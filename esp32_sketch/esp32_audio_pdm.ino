/*
 * ESP32-S3 WiFi Audio Receiver - PDM output (без внешнего DAC)
 * SoftAP + UDP -> I2S в PDM режиме -> RC фильтр -> XH-M177
 *
 * Плата: ESP32-S3 Super Mini
 * Arduino core: esp32 by Espressif 3.0+
 *
 * Подключение к XH-M177 через RC-фильтр:
 *
 *   ESP32 GPIO7 ──[1 кОм]──┬─── AUX L вход XH-M177
 *                          │
 *                        [100 нФ]
 *                          │
 *                         GND ──── GND XH-M177
 *
 * ОБЯЗАТЕЛЬНО: GND ESP32 и GND усилителя соединить!
 *
 * Питание ESP32 и XH-M177 — лучше от разных источников, иначе
 * помехи от усилителя лезут в слабый PDM-сигнал. Если от одного —
 * поставь электролит 470-1000 мкФ по питанию ESP32.
 *
 * Громкость — крути потенциометры на XH-M177 (начни с минимума!).
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>

// ===== настройки =====
const char* AP_SSID     = "ESP32-Audio";
const char* AP_PASSWORD = "12345678";
const uint16_t UDP_PORT = 3333;

// Один пин для PDM вывода
#define PDM_DOUT_PIN    7        // любой свободный GPIO
#define PDM_CLK_PIN     I2S_PIN_NO_CHANGE  // не нужен наружу

// Входной формат от телефона (не менять — Android шлёт именно так)
#define SAMPLE_RATE     44100
#define UDP_RX_BUFFER_SIZE 1460

// Буферы
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     256

// ======================

WiFiUDP udp;
uint8_t rxBuf[UDP_RX_BUFFER_SIZE];
int16_t monoBuf[UDP_RX_BUFFER_SIZE / 2];  // после стерео->моно микса

void setupI2S_PDM() {
    // I2S в PDM TX режиме (на S3 поддерживается)
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // моно
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_PIN_NO_CHANGE,   // для PDM не нужен
        .ws_io_num    = PDM_CLK_PIN,          // внутренний, наружу не выводим
        .data_out_num = PDM_DOUT_PIN,         // ВОТ ЭТОТ пин идёт в RC-фильтр
        .data_in_num  = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("i2s_driver_install err=%d\n", err);
    }
    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("i2s_set_pin err=%d\n", err);
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
}

// Конвертация стерео 16-бит -> моно 16-бит (среднее L и R)
// На входе: PCM interleaved L0,R0,L1,R1,...
// На выходе: M0,M1,M2,...
size_t stereoToMono(const uint8_t* in, size_t inBytes, int16_t* out) {
    const int16_t* samples = (const int16_t*)in;
    size_t stereoSamples = inBytes / 4;  // каждая пара L+R = 4 байта
    for (size_t i = 0; i < stereoSamples; i++) {
        int32_t L = samples[i * 2];
        int32_t R = samples[i * 2 + 1];
        out[i] = (int16_t)((L + R) / 2);
    }
    return stereoSamples * 2;  // bytes
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== ESP32-S3 WiFi Audio - PDM out ===");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("AP: %s / %s\n", AP_SSID, AP_PASSWORD);
    Serial.printf("IP: %s   UDP port: %u\n", apIP.toString().c_str(), UDP_PORT);

    setupI2S_PDM();
    Serial.printf("PDM output on GPIO%d\n", PDM_DOUT_PIN);
    Serial.println("Schema: GPIO7 -[1k]- out -[100nF]- GND");
    Serial.println("        out -> AUX L входа XH-M177");

    udp.begin(UDP_PORT);
    Serial.println("Ready.");
}

unsigned long lastStatMs = 0;
uint32_t rxBytes = 0;
uint32_t rxPackets = 0;

void loop() {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        int len = udp.read(rxBuf, sizeof(rxBuf));
        if (len >= 4) {  // минимум одна стерео-пара
            // Стерео -> моно
            size_t monoBytes = stereoToMono(rxBuf, len, monoBuf);
            size_t written = 0;
            i2s_write(I2S_NUM_0, monoBuf, monoBytes, &written, portMAX_DELAY);
            rxBytes += written;
            rxPackets++;
        }
    }

    unsigned long now = millis();
    if (now - lastStatMs >= 1000) {
        if (rxPackets > 0) {
            float kbps = (rxBytes * 8.0f) / 1000.0f;
            Serial.printf("[stat] %u pkt/s, %.1f kbps (mono), clients=%u\n",
                          rxPackets, kbps, WiFi.softAPgetStationNum());
        } else {
            Serial.printf("[idle] clients=%u\n", WiFi.softAPgetStationNum());
        }
        rxBytes = 0;
        rxPackets = 0;
        lastStatMs = now;
    }
}
