/*
 * ESP32-S3 WiFi Audio Receiver
 * SoftAP + UDP -> I2S (MAX98357A)
 *
 * Плата: ESP32-S3 Super Mini
 * Arduino core: esp32 by Espressif (3.0+)
 *
 * Подключение MAX98357A:
 *   BCLK -> GPIO4
 *   LRC  -> GPIO5
 *   DIN  -> GPIO6
 *   GND  -> GND
 *   VIN  -> 5V  (с USB, пин 5V на плате)
 *   SD   -> не подключать (авто: L+R mix)
 *          или на GND = выкл, >1.4V = вкл
 *   GAIN -> не подключать = +9дБ (или GND=+6, VIN=+15, 100k to GND=+12, 100k to VIN=+3)
 *
 * Пины можно менять — любые свободные GPIO подходят.
 *
 * Работа:
 *  1. ESP32 поднимает AP "ESP32-Audio" / пароль "12345678"
 *  2. Телефон подключается к этой WiFi сети
 *  3. Android приложение стримит PCM 16бит/44.1кГц стерео на 192.168.4.1:3333
 *  4. ESP32 выводит на MAX98357A
 *
 * Формат UDP пакета: сырой PCM, 16-bit signed LE, interleaved L/R, 44100 Hz
 *                    размер пакета — кратен 4 байтам (один стерео-сэмпл)
 *                    рекомендуется 512..1460 байт на пакет (< MTU)
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>

// ===== настройки =====
const char* AP_SSID     = "ESP32-Audio";
const char* AP_PASSWORD = "12345678";          // минимум 8 символов
const uint16_t UDP_PORT = 3333;

// I2S пины для MAX98357A
#define I2S_BCLK    4
#define I2S_LRC     5
#define I2S_DIN     6

// Параметры аудио
#define SAMPLE_RATE     44100
#define CHANNELS        2       // стерео (MAX98357A микширует в моно сам)
#define BITS_PER_SAMPLE 16

// Размер UDP-буфера. Максимум ~1472 (MTU 1500 - заголовки)
#define UDP_RX_BUFFER_SIZE 1460

// ======================

WiFiUDP udp;
uint8_t rxBuf[UDP_RX_BUFFER_SIZE];

void setupI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // стерео
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRC,
        .data_out_num = I2S_DIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_0);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== ESP32-S3 WiFi Audio Receiver ===");

    // 1. Поднимаем AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("AP started: SSID=\"%s\" PASS=\"%s\"\n", AP_SSID, AP_PASSWORD);
    Serial.printf("IP: %s   UDP port: %u\n", apIP.toString().c_str(), UDP_PORT);

    // 2. I2S init
    setupI2S();
    Serial.println("I2S initialized (MAX98357A)");

    // 3. UDP
    udp.begin(UDP_PORT);
    Serial.println("UDP listening");
    Serial.println("Ready. Подключай телефон к WiFi и запускай приложение.");
}

unsigned long lastStatMs = 0;
uint32_t rxBytes = 0;
uint32_t rxPackets = 0;

void loop() {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        int len = udp.read(rxBuf, sizeof(rxBuf));
        if (len > 0) {
            size_t written = 0;
            // Блокирующая запись на I2S (DMA подхватит сэмплы)
            i2s_write(I2S_NUM_0, rxBuf, len, &written, portMAX_DELAY);
            rxBytes += written;
            rxPackets++;
        }
    }

    // Лог раз в секунду
    unsigned long now = millis();
    if (now - lastStatMs >= 1000) {
        if (rxPackets > 0) {
            float kbps = (rxBytes * 8.0f) / 1000.0f;
            Serial.printf("[stat] %u pkt/s, %.1f kbps, clients=%u\n",
                          rxPackets, kbps, WiFi.softAPgetStationNum());
        } else {
            Serial.printf("[idle] clients=%u\n", WiFi.softAPgetStationNum());
        }
        rxBytes = 0;
        rxPackets = 0;
        lastStatMs = now;
    }
}
