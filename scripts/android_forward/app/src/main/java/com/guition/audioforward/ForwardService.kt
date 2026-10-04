package com.guition.audioforward

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.net.wifi.WifiManager
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 前台服务：承载 announce/接收/播放/A2DP 全部线程，锁屏与切后台持续工作。
 * START_STICKY：进程被杀后由系统重建，转发自动恢复。
 */
class ForwardService : Service() {

    companion object {
        const val CHANNEL_ID = "audio_forward"
        const val NOTIFICATION_ID = 1

        /** Activity 轮询入口（服务实例的弱引用式静态指针）。 */
        @Volatile
        var instance: ForwardService? = null
            private set
    }

    private val running = AtomicBoolean(true)
    lateinit var receiver: AudioReceiver  // MainActivity 轮询状态
        private set
    lateinit var pump: AudioPump
        private set
    private var a2dp: A2dpStatus? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    @Volatile
    var headsetText: String = ""
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        startForeground(NOTIFICATION_ID, buildNotification("启动中..."))

        val queue = LinkedBlockingQueue<AudioFrame>(8)
        receiver = AudioReceiver(running, queue)
        pump = AudioPump(running, queue)
        receiver.start()
        pump.start()

        // 组播接收锁：防止省电策略过滤组播包
        try {
            val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            multicastLock = wifi?.createMulticastLock("xz_forward")?.apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (_: Exception) {
        }
        AnnounceSender(running) {}.start()
        a2dp = A2dpStatus(this) { s ->
            headsetText = s
        }
        a2dp?.start()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        return START_STICKY  // 被杀后自动重建服务
    }

    override fun onDestroy() {
        running.set(false)
        receiver.socket?.close()
        a2dp?.stop()
        try {
            if (multicastLock?.isHeld == true) multicastLock?.release()
        } catch (_: Exception) {
        }
        instance = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun buildNotification(text: String): Notification {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26) {
            val channel = NotificationChannel(
                CHANNEL_ID, "音频中转", NotificationManager.IMPORTANCE_LOW
            )
            channel.description = "数智方舟智通音频中转站运行状态"
            manager.createNotificationChannel(channel)
        }
        val builder = if (Build.VERSION.SDK_INT >= 26) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("数智方舟智通音频中转站")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .build()
    }
}
