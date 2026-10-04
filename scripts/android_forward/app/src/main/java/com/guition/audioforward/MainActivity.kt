package com.guition.audioforward

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.TextView

/** 主界面：启动前台服务 + 500ms 轮询服务状态。退出界面不影响转发。 */
class MainActivity : Activity() {

    private lateinit var tvLocalIp: TextView
    private lateinit var tvDevice: TextView
    private lateinit var tvHeadset: TextView
    private lateinit var tvStats: TextView
    private lateinit var tvService: TextView
    private lateinit var btnStop: Button
    private lateinit var btnStart: Button

    private val handler = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvLocalIp = findViewById(R.id.tv_local_ip)
        tvDevice = findViewById(R.id.tv_device)
        tvHeadset = findViewById(R.id.tv_headset)
        tvStats = findViewById(R.id.tv_stats)
        tvService = findViewById(R.id.tv_service)
        btnStop = findViewById(R.id.btn_stop)
        btnStart = findViewById(R.id.btn_start)

        // Android 12+ 蓝牙权限；Android 13+ 通知权限（前台服务通知需要）
        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31 &&
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) {
            needed.add(Manifest.permission.BLUETOOTH_CONNECT)
        }
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (needed.isNotEmpty()) {
            requestPermissions(needed.toTypedArray(), 100)
        }

        btnStart.setOnClickListener { startForward() }
        btnStop.setOnClickListener { stopForward() }

        startForward()

        handler.post(object : Runnable {
            override fun run() {
                updateStatus()
                handler.postDelayed(this, 500)
            }
        })
    }

    private fun startForward() {
        val intent = Intent(this, ForwardService::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    private fun stopForward() {
        stopService(Intent(this, ForwardService::class.java))
    }

    private fun updateStatus() {
        if (isDestroyed) return
        val svc = ForwardService.instance
        if (svc == null) {
            tvService.text = "中转服务: 未运行"
            tvDevice.text = "设备: --"
            tvHeadset.text = "耳机: --"
            tvStats.text = "帧 0 | 丢帧 0 | seq缺口 0 | keepalive 0 | 欠载 0"
            tvLocalIp.text = "本机 IP: " + AnnounceSender.localIps().joinToString()
            return
        }
        tvService.text = "中转服务: 运行中"
        val stats = svc.receiver.stats
        val online = System.currentTimeMillis() - stats.lastPacketMs < Protocol.OFFLINE_TIMEOUT_MS
        tvDevice.text = if (online) "设备: 在线" else "设备: 离线（等待 P4）"
        tvHeadset.text = when {
            svc.headsetText.isEmpty() -> "耳机: 未连接（请先在系统蓝牙设置配对）"
            svc.headsetText == "蓝牙未开启" -> "耳机: 蓝牙未开启"
            else -> "耳机: ${svc.headsetText}"
        }
        tvStats.text = "帧 ${stats.frames} | 丢帧 ${stats.dropped} | seq缺口 ${stats.seqGaps} | " +
                "keepalive ${stats.keepalives} | 欠载 ${svc.pump.underruns}"
        tvLocalIp.text = "本机 IP: " + AnnounceSender.localIps().joinToString()
    }
}
