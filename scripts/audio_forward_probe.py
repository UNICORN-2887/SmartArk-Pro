#!/usr/bin/env python3
"""音频转发协议探针：模拟手机 App，验证 P4 的 XZFA/XZAD/XZAL 协议。

用法：
    python audio_forward_probe.py               # 默认端口 50000/50001
    python audio_forward_probe.py --no-announce # 只收不发（验证手动目标时用）

行为：
    - 每 2s 发 XZFA announce（全局广播 + 各网卡 /24 子网定向广播）
    - 收 XZAD 音频帧（支持 fragment 重组）与 XZAL keepalive，
      打印 seq 连续性 / 帧间隔 / 丢帧统计

XZAD 头 20 字节（小端）：magic seq(u32) ts_ms(u32) sr(u16) ch(u16)
samples(u16 整帧) frag_index(u8) frag_count(u8) + PCM。
P4 每包 ≤500 样本（≤1020B）避免 IP 分片；frag_count>1 时需重组。
"""

import argparse
import socket
import struct
import sys
import threading
import time

ANNOUNCE_PORT = 50000
AUDIO_PORT = 50001

XZFA = b"XZFA"
XZAD = b"XZAD"
XZAL = b"XZAL"


def get_broadcast_addrs():
    """本机所有 IPv4 子网定向广播地址 + 全局广播（/24 假设，局域网常见）。"""
    addrs = ["255.255.255.255"]
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip.startswith("127."):
                continue
            parts = ip.split(".")
            addrs.append(f"{parts[0]}.{parts[1]}.{parts[2]}.255")
    except OSError:
        pass
    return addrs


class Receiver:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, 0)  # placeholder
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", AUDIO_PORT))
        self.sock.settimeout(0.5)
        self.frames = 0
        self.frags = 0
        self.keepalives = 0
        self.gaps = 0
        self.dropped_frags = 0
        self.last_seq = None
        self.last_frame_time = None
        self.last_packet_time = time.time()
        self.running = True
        self.pending = {}  # seq -> dict(parts, frag_count, ts, sr, ch, samples)

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                self._cleanup_pending()
                continue
            except OSError:
                break
            self.last_packet_time = time.time()
            if data[:4] == XZAL and len(data) >= 8:
                self.keepalives += 1
                seq = struct.unpack("<I", data[4:8])[0]
                self._track_seq(seq)
            elif data[:4] == XZAD and len(data) >= 20:
                seq, ts, sr, ch, samples = struct.unpack("<IIHHH", data[4:18])
                frag_index = data[18]
                frag_count = max(data[19], 1)
                pcm = data[20:]
                self.frags += 1
                if frag_count == 1:
                    self._frame_done(seq, ts, sr, ch, samples, pcm, addr)
                else:
                    p = self.pending.get(seq)
                    if p is None:
                        p = {
                            "parts": [None] * frag_count,
                            "ts": ts, "sr": sr, "ch": ch, "samples": samples,
                            "count": frag_count,
                        }
                        self.pending[seq] = p
                    if frag_index < frag_count:
                        p["parts"][frag_index] = pcm
                    if all(x is not None for x in p["parts"]):
                        del self.pending[seq]
                        merged = b"".join(p["parts"])
                        self._frame_done(seq, p["ts"], p["sr"], p["ch"], p["samples"], merged, addr)

    def _frame_done(self, seq, ts, sr, ch, samples, pcm, addr):
        self.frames += 1
        self._track_seq(seq)
        now = time.time()
        if self.last_frame_time is not None:
            interval_ms = (now - self.last_frame_time) * 1000
            if interval_ms > 90:
                print(f"[WARN] 帧间隔 {interval_ms:.0f}ms (>90ms, 期望≈60ms)")
        self.last_frame_time = now
        if self.frames <= 3 or self.frames % 50 == 0:
            print(f"XZAD #{self.frames} seq={seq} ts={ts} {sr}Hz {ch}ch "
                  f"samples={samples} pcm={len(pcm)}B 来自 {addr[0]}")

    def _cleanup_pending(self):
        for seq in list(self.pending.keys()):
            if seq < (self.last_seq or 0) - 64:
                del self.pending[seq]
                self.dropped_frags += 1

    def _track_seq(self, seq):
        if self.last_seq is not None:
            gap = (seq - self.last_seq - 1) & 0xFFFFFFFF
            if gap:
                self.gaps += gap
                print(f"[WARN] seq 跳变: {self.last_seq} -> {seq} (丢 {gap})")
        self.last_seq = seq

    def status(self):
        online = time.time() - self.last_packet_time < 8
        return (f"帧 {self.frames} | frag {self.frags} | keepalive {self.keepalives} | "
                f"丢包 {self.gaps} | 残帧 {self.dropped_frags} | "
                f"{'在线' if online else '离线(>8s)'}")


def main():
    parser = argparse.ArgumentParser(description="XZFA/XZAD/XZAL 协议探针")
    parser.add_argument("--no-announce", action="store_true", help="只收不发")
    args = parser.parse_args()

    print(f"广播地址: {get_broadcast_addrs()}")
    print(f"监听 0.0.0.0:{AUDIO_PORT}，announce 目标 {ANNOUNCE_PORT}")

    rx = Receiver()
    rx_thread = threading.Thread(target=rx.run, daemon=True)
    rx_thread.start()

    if args.no_announce:
        print("announce 已关闭（--no-announce），等待音频...")
    else:
        def announce_loop():
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            addrs = get_broadcast_addrs()
            seq = 0
            while rx.running:
                pkt = struct.pack("<4sBBHI", XZFA, 1, 0, AUDIO_PORT, seq)
                for addr in addrs:
                    try:
                        sock.sendto(pkt, (addr, ANNOUNCE_PORT))
                    except OSError as e:
                        print(f"[WARN] announce {addr} 失败: {e}")
                seq += 1
                time.sleep(2)

        threading.Thread(target=announce_loop, daemon=True).start()

    try:
        while True:
            print(f"[status] {rx.status()}")
            time.sleep(2)
    except KeyboardInterrupt:
        rx.running = False
        print("\n探针退出")


if __name__ == "__main__":
    main()
