package com.guition.audioforward

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max

/**
 * AudioTrack 播放线程：16kHz mono PCM_16BIT MODE_STREAM。
 * 首次（含欠载后）预滚 4 帧（240ms）再写；阻塞 write 天然背压。
 * 音频经手机系统自动路由到蓝牙耳机（系统级，App 不指定输出设备）。
 */
class AudioPump(
    private val running: AtomicBoolean,
    private val queue: LinkedBlockingQueue<AudioFrame>
) : Thread("AudioPump") {

    @Volatile var playedFrames = 0L
    @Volatile var underruns = 0L

    override fun run() {
        var track: AudioTrack? = null
        var rate = 0
        while (running.get()) {
            val frame = try {
                queue.poll(500, TimeUnit.MILLISECONDS)
            } catch (_: InterruptedException) {
                null
            } ?: continue

            // 采样率变化（防御）或首次 → 重建 track
            if (track == null || frame.sampleRate != rate) {
                track?.release()
                rate = frame.sampleRate
                track = buildTrack(rate)
                track.play()
                // 预滚 4 帧（≈240ms）填满抖动缓冲，避免开播即欠载
                var primed = 0
                var f: AudioFrame? = frame
                while (f != null && primed < 4 && f.sampleRate == rate) {
                    track.write(f.pcm, 0, f.pcm.size)
                    playedFrames++
                    primed++
                    f = if (primed < 4) {
                        try {
                            queue.poll(200, TimeUnit.MILLISECONDS)
                        } catch (_: InterruptedException) {
                            null
                        }
                    } else {
                        null
                    }
                }
                continue
            }

            val before = track.underrunCount
            track.write(frame.pcm, 0, frame.pcm.size)
            playedFrames++
            if (track.underrunCount > before) {
                underruns++
                // 欠载后由下一次采样率重建分支重新预滚；此处直接释放强制重建
                track.release()
                track = null
            }
        }
        track?.release()
    }

    private fun buildTrack(sampleRate: Int): AudioTrack {
        val minBuf = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        return AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build()
            )
            .setBufferSizeInBytes(max(minBuf * 2, 7680))  // ≥240ms
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
    }
}
