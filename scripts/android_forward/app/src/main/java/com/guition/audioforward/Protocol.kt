package com.guition.audioforward

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** XZFA/XZAD/XZAL 协议常量与编解码（全部小端）。 */
object Protocol {
    const val ANNOUNCE_PORT = 50000   // App → P4 广播 announce
    const val AUDIO_PORT = 50001      // P4 → App 音频帧
    const val MULTICAST_GROUP = "239.255.1.1"  // 组播模式（热点实验）：P4 发组播到此地址
    const val OFFLINE_TIMEOUT_MS = 8000L  // 双方失联判定（对齐 P4 侧）

    val MAGIC_ANNOUNCE = "XZFA".toByteArray(Charsets.US_ASCII)
    val MAGIC_AUDIO = "XZAD".toByteArray(Charsets.US_ASCII)
    val MAGIC_KEEPALIVE = "XZAL".toByteArray(Charsets.US_ASCII)

    fun startsWithMagic(data: ByteArray, magic: ByteArray): Boolean {
        if (data.size < magic.size) return false
        for (i in magic.indices) {
            if (data[i] != magic[i]) return false
        }
        return true
    }

    /** XZFA announce：magic(4) + ver(1) + flags(1) + audio_port(u16) + seq(u32) = 12B */
    fun buildAnnounce(seq: Int, audioPort: Int = AUDIO_PORT): ByteArray {
        val buf = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(MAGIC_ANNOUNCE)
        buf.put(0x01)  // version
        buf.put(0x00)  // flags
        buf.putShort(audioPort.toShort())
        buf.putInt(seq)
        return buf.array()
    }
}

/** 一帧解码后的 PCM（P4 已保证 16kHz mono，sampleRate 字段做防御性传递）。 */
data class AudioFrame(
    val seq: Int,
    val tsMs: Int,
    val sampleRate: Int,
    val channels: Int,
    val samples: Int,
    val pcm: ShortArray
)
