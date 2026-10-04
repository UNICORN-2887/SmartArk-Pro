package com.guition.audioforward

import java.net.DatagramPacket
import java.net.InetAddress
import java.net.MulticastSocket
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * UDP 接收线程：XZAD 音频帧（含 fragment 重组）入队，XZAL 刷新在线时间戳。
 * 队列满丢最旧帧。
 *
 * XZAD 头 20 字节（小端）：magic'XZAD' seq(u32) ts_ms(u32) sample_rate(u16)
 * channels(u16) samples(u16, 整帧样本数) frag_index(u8) frag_count(u8) + PCM
 * P4 每包 ≤500 样本（≤1020B），避免 IP 分片被手机丢弃；frag_count>1 时需重组。
 */
class AudioReceiver(
    private val running: AtomicBoolean,
    private val queue: LinkedBlockingQueue<AudioFrame>
) : Thread("AudioReceiver") {

    class Stats {
        @Volatile var frames = 0L
        @Volatile var dropped = 0L
        @Volatile var keepalives = 0L
        @Volatile var lastPacketMs = 0L
        @Volatile var lastSeq = -1
        @Volatile var seqGaps = 0L
    }

    val stats = Stats()

    @Volatile
    var socket: MulticastSocket? = null
        private set

    private class Pending(
        val tsMs: Int,
        val sampleRate: Int,
        val channels: Int,
        val samples: Int,
        val fragCount: Int,
        val parts: Array<ByteArray?>
    )

    private val pending = HashMap<Int, Pending>()

    override fun run() {
        val sock = try {
            MulticastSocket(Protocol.AUDIO_PORT).also {
                it.soTimeout = 500
                socket = it
                joinGroups(it)
            }
        } catch (e: Exception) {
            // 端口被占用：announce 端口写死，异常状态由 UI 显示离线
            return
        }
        val buf = ByteArray(2048)
        while (running.get()) {
            try {
                val pkt = DatagramPacket(buf, buf.size)
                sock.receive(pkt)
                handle(pkt.data, pkt.length)
            } catch (_: SocketTimeoutException) {
                continue
            } catch (_: Exception) {
                break
            }
        }
        sock.close()
        socket = null
    }

    /** 加入组播组（所有可用网络接口）：组播模式下设备发 239.255.1.1，接收端必须 joinGroup。 */
    private fun joinGroups(sock: MulticastSocket) {
        try {
            val group = InetAddress.getByName(Protocol.MULTICAST_GROUP)
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val nif = interfaces.nextElement()
                if (!nif.isUp || nif.isLoopback) continue
                try {
                    sock.joinGroup(java.net.InetSocketAddress(group, Protocol.AUDIO_PORT), nif)
                } catch (_: Exception) {
                    // 该接口不支持组播，忽略
                }
            }
            // 默认接口兜底
            try {
                sock.joinGroup(group)
            } catch (_: Exception) {
            }
        } catch (_: Exception) {
        }
    }

    private fun handle(data: ByteArray, length: Int) {
        val packet = data.copyOf(length)
        stats.lastPacketMs = System.currentTimeMillis()
        when {
            Protocol.startsWithMagic(packet, Protocol.MAGIC_AUDIO) && packet.size >= 20 -> {
                val bb = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
                bb.position(4)
                val seq = bb.int
                val ts = bb.int
                val sr = bb.short.toInt() and 0xFFFF
                val ch = bb.short.toInt() and 0xFFFF
                val samples = bb.short.toInt() and 0xFFFF
                val fragIndex = (packet[18].toInt() and 0xFF)
                val fragCount = (packet[19].toInt() and 0xFF).coerceAtLeast(1)
                val pcmBytes = packet.size - 20
                if (pcmBytes < 0) return

                if (fragCount == 1) {
                    val frame = makeFrame(seq, ts, sr, ch, samples, packet, 20, pcmBytes)
                    if (frame != null) enqueue(frame, seq)
                } else {
                    // fragment 重组
                    var p = pending[seq]
                    if (p == null) {
                        p = Pending(ts, sr, ch, samples, fragCount, arrayOfNulls(fragCount))
                        pending[seq] = p
                    }
                    if (fragIndex < fragCount) {
                        p.parts[fragIndex] = packet.copyOfRange(20, packet.size)
                    }
                    if (p.parts.all { it != null }) {
                        pending.remove(seq)
                        val total = p.parts.sumOf { it!!.size }
                        val merged = ByteArray(total)
                        var off = 0
                        for (part in p.parts) {
                            System.arraycopy(part!!, 0, merged, off, part.size)
                            off += part.size
                        }
                        val frame = makeFrame(seq, p.tsMs, p.sampleRate, p.channels, p.samples, merged, 0, merged.size)
                        if (frame != null) enqueue(frame, seq)
                    }
                    // 清理过期 fragment（防泄漏）
                    if (pending.size > 64) {
                        val oldest = pending.keys.minOrNull()
                        if (oldest != null && oldest < seq - 64) pending.remove(oldest)
                    }
                }
            }
            Protocol.startsWithMagic(packet, Protocol.MAGIC_KEEPALIVE) -> {
                stats.keepalives++
            }
        }
    }

    private fun makeFrame(seq: Int, ts: Int, sr: Int, ch: Int, samples: Int,
                          data: ByteArray, offset: Int, pcmBytes: Int): AudioFrame? {
        val actualSamples = pcmBytes / 2
        if (actualSamples == 0) return null
        val pcm = ShortArray(actualSamples)
        ByteBuffer.wrap(data, offset, pcmBytes).order(ByteOrder.LITTLE_ENDIAN)
            .asShortBuffer().get(pcm)
        return AudioFrame(seq, ts, sr, ch, actualSamples.coerceAtMost(samples), pcm)
    }

    private fun enqueue(frame: AudioFrame, seq: Int) {
        if (!queue.offer(frame)) {
            queue.poll()
            queue.offer(frame)
            stats.dropped++
        }
        stats.frames++
        if (stats.lastSeq >= 0) {
            val gap = (seq - stats.lastSeq - 1) and 0xFFFF
            if (gap > 0) stats.seqGaps += gap
        }
        stats.lastSeq = seq
    }
}
