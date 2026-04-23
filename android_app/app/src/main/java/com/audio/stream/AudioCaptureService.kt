package com.audio.stream

import android.app.*
import android.content.Context
import android.content.Intent
import android.media.*
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Захват системного звука через AudioPlaybackCaptureConfiguration (Android 10+)
 * и стриминг на ESP32 по UDP. PCM 16-bit signed LE, stereo, 44100 Hz — ровно то,
 * что ожидает скетч.
 *
 * Важно: захватываются только приложения, которые НЕ установили
 * setAllowedCapturePolicy(ALLOW_CAPTURE_BY_NONE). YouTube, браузеры, Spotify,
 * большинство плееров — захватываются. Некоторые защищённые (DRM) — нет.
 */
class AudioCaptureService : Service() {

    companion object {
        const val ACTION_START = "START_CAPTURE"
        const val ACTION_STOP  = "STOP_CAPTURE"
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
        const val EXTRA_TARGET_IP   = "targetIp"
        const val EXTRA_TARGET_PORT = "targetPort"

        private const val NOTIF_CHANNEL_ID = "audio_stream"
        private const val NOTIF_ID = 1001

        private const val SAMPLE_RATE  = 44100
        private const val CHANNEL_MASK = AudioFormat.CHANNEL_IN_STEREO
        private const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT
        private const val TAG = "AudioCapture"
    }

    private var projection: MediaProjection? = null
    private var audioRecord: AudioRecord? = null
    private var captureThread: Thread? = null
    private val running = AtomicBoolean(false)
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
                val data: Intent? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(EXTRA_RESULT_DATA)
                }
                val ip   = intent.getStringExtra(EXTRA_TARGET_IP) ?: "192.168.4.1"
                val port = intent.getIntExtra(EXTRA_TARGET_PORT, 3333)

                startForeground(NOTIF_ID, buildNotification("Стрим на $ip:$port"))

                if (data != null) {
                    startCapture(resultCode, data, ip, port)
                } else {
                    stopSelf()
                }
            }
            ACTION_STOP -> {
                stopCapture()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
        }
        return START_NOT_STICKY
    }

    private fun startCapture(resultCode: Int, data: Intent, ip: String, port: Int) {
        val pm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = pm.getMediaProjection(resultCode, data)

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            Log.e(TAG, "Требуется Android 10+")
            stopSelf()
            return
        }

        val config = android.media.AudioPlaybackCaptureConfiguration.Builder(projection!!)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        val audioFormat = AudioFormat.Builder()
            .setEncoding(AUDIO_FORMAT)
            .setSampleRate(SAMPLE_RATE)
            .setChannelMask(CHANNEL_MASK)
            .build()

        val minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_MASK, AUDIO_FORMAT)
        val bufSize = maxOf(minBuf, 4096)

        audioRecord = AudioRecord.Builder()
            .setAudioFormat(audioFormat)
            .setBufferSizeInBytes(bufSize * 2)
            .setAudioPlaybackCaptureConfig(config)
            .build()

        // Wi-Fi lock чтобы система не душила соединение при блокировке экрана
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "audio-stream")
        wifiLock?.acquire()

        audioRecord?.startRecording()
        running.set(true)

        captureThread = Thread {
            streamLoop(ip, port)
        }.apply {
            name = "AudioStream"
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun streamLoop(ip: String, port: Int) {
        val socket = DatagramSocket()
        socket.sendBufferSize = 64 * 1024

        // Пакет 1440 байт = 360 стерео-сэмплов ~ 8.16 мс при 44.1 кГц
        // Укладываемся в MTU и даём ESP32 нормально успевать
        val pktSize = 1440
        val buffer = ByteArray(pktSize)
        val addr = InetAddress.getByName(ip)

        Log.i(TAG, "Streaming to $ip:$port, pkt=$pktSize")

        try {
            while (running.get()) {
                var offset = 0
                // Собираем пакет из нескольких чтений (AudioRecord может вернуть меньше)
                while (offset < pktSize && running.get()) {
                    val n = audioRecord?.read(buffer, offset, pktSize - offset) ?: -1
                    if (n <= 0) break
                    offset += n
                }
                if (offset > 0) {
                    val p = DatagramPacket(buffer, offset, addr, port)
                    try { socket.send(p) } catch (e: Exception) { /* сеть мигнула — игнор */ }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "stream error", e)
        } finally {
            try { socket.close() } catch (_: Exception) {}
        }
    }

    private fun stopCapture() {
        running.set(false)
        try { captureThread?.join(500) } catch (_: Exception) {}
        captureThread = null

        try { audioRecord?.stop() } catch (_: Exception) {}
        try { audioRecord?.release() } catch (_: Exception) {}
        audioRecord = null

        try { projection?.stop() } catch (_: Exception) {}
        projection = null

        try { wifiLock?.release() } catch (_: Exception) {}
        wifiLock = null
    }

    override fun onDestroy() {
        stopCapture()
        super.onDestroy()
    }

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                NOTIF_CHANNEL_ID, "Audio streaming",
                NotificationManager.IMPORTANCE_LOW
            )
            nm.createNotificationChannel(ch)
        }
        return NotificationCompat.Builder(this, NOTIF_CHANNEL_ID)
            .setContentTitle("Audio Stream")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOngoing(true)
            .build()
    }
}
