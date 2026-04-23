package com.audio.stream

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    private lateinit var ipEdit: EditText
    private lateinit var portEdit: EditText
    private lateinit var startBtn: Button
    private lateinit var stopBtn: Button
    private lateinit var statusView: TextView

    private lateinit var projectionManager: MediaProjectionManager

    // Android 13+ требует явного разрешения на уведомления
    private val notifPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* no-op */ }

    // RECORD_AUDIO нужен для AudioPlaybackCapture
    private val recordPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) requestProjection()
        else statusView.text = "Нет разрешения RECORD_AUDIO"
    }

    // Диалог Android "поделиться экраном/звуком"
    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK && result.data != null) {
            val intent = Intent(this, AudioCaptureService::class.java).apply {
                action = AudioCaptureService.ACTION_START
                putExtra(AudioCaptureService.EXTRA_RESULT_CODE, result.resultCode)
                putExtra(AudioCaptureService.EXTRA_RESULT_DATA, result.data)
                putExtra(AudioCaptureService.EXTRA_TARGET_IP, ipEdit.text.toString())
                putExtra(AudioCaptureService.EXTRA_TARGET_PORT, portEdit.text.toString().toIntOrNull() ?: 3333)
            }
            ContextCompat.startForegroundService(this, intent)
            statusView.text = "Стрим идёт -> ${ipEdit.text}:${portEdit.text}"
            startBtn.isEnabled = false
            stopBtn.isEnabled = true
        } else {
            statusView.text = "Отменено"
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ipEdit     = findViewById(R.id.ipEdit)
        portEdit   = findViewById(R.id.portEdit)
        startBtn   = findViewById(R.id.startBtn)
        stopBtn    = findViewById(R.id.stopBtn)
        statusView = findViewById(R.id.statusView)

        ipEdit.setText("192.168.4.1")
        portEdit.setText("3333")

        projectionManager = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager

        // Запросим разрешение на уведомления (Android 13+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
                notifPermLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }

        startBtn.setOnClickListener {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
                requestProjection()
            } else {
                recordPermLauncher.launch(Manifest.permission.RECORD_AUDIO)
            }
        }

        stopBtn.setOnClickListener {
            val intent = Intent(this, AudioCaptureService::class.java).apply {
                action = AudioCaptureService.ACTION_STOP
            }
            startService(intent)
            statusView.text = "Остановлено"
            startBtn.isEnabled = true
            stopBtn.isEnabled = false
        }

        stopBtn.isEnabled = false
    }

    private fun requestProjection() {
        projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
    }
}
