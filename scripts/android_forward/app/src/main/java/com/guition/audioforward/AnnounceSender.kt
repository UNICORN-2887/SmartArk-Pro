package com.guition.audioforward

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.NetworkInterface
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 周期广播 XZFA announce：全局广播 + 每个网卡的 /24 子网定向广播
 * （部分路由器会过滤 255.255.255.255，双路发送兜底）。
 */
class AnnounceSender(
    private val running: AtomicBoolean,
    private val onLocalIps: (List<String>) -> Unit
) : Thread("AnnounceSender") {

    override fun run() {
        val sock = try {
            DatagramSocket().also { it.broadcast = true }
        } catch (e: Exception) {
            return
        }
        var seq = 0
        while (running.get()) {
            val pkt = Protocol.buildAnnounce(seq++)
            onLocalIps(localIps())
            for (target in announceTargets()) {
                try {
                    sock.send(DatagramPacket(pkt, pkt.size, InetAddress.getByName(target), Protocol.ANNOUNCE_PORT))
                } catch (_: Exception) {
                    // 单目标失败忽略，下一轮重试
                }
            }
            try {
                Thread.sleep(2000)
            } catch (_: InterruptedException) {
                break
            }
        }
        sock.close()
    }

    companion object {
        /** 本机所有 site-local IPv4 地址。 */
        fun localIps(): List<String> {
            val ips = mutableListOf<String>()
            try {
                val interfaces = NetworkInterface.getNetworkInterfaces()
                while (interfaces.hasMoreElements()) {
                    val nif = interfaces.nextElement()
                    if (!nif.isUp || nif.isLoopback) continue
                    val addrs = nif.inetAddresses
                    while (addrs.hasMoreElements()) {
                        val addr = addrs.nextElement()
                        if (addr is java.net.Inet4Address && addr.isSiteLocalAddress) {
                            ips.add(addr.hostAddress ?: continue)
                        }
                    }
                }
            } catch (_: Exception) {
            }
            return ips
        }

        /** 全局广播 + 各网卡 /24 定向广播。 */
        private fun announceTargets(): List<String> {
            val targets = mutableListOf("255.255.255.255")
            for (ip in localIps()) {
                val parts = ip.split(".")
                if (parts.size == 4) {
                    targets.add("${parts[0]}.${parts[1]}.${parts[2]}.255")
                }
            }
            return targets
        }
    }
}
