# 数智方舟智通音频中转站（Android）

接收 P4 终端通过局域网 UDP 转发的 LLM 回复音频，经手机系统路由到蓝牙耳机播放。
**前台服务**运行：锁屏/切换 App 后中转持续。

## 工作原理

```
P4 设备 ──XZAD 音频帧 UDP──▶ 本 App（AudioReceiver → AudioPump → AudioTrack）
本 App ──XZFA announce 广播(每2s)──▶ P4 设备（自动发现手机 IP）
```

- 端口：announce 50000（App→设备），音频 50001（设备→App），keepalive XZAL 每 1s
- 音频格式：16kHz mono 16bit PCM，60ms/帧；每帧拆 2 个 ≤1020B 的 UDP 包（避免 IP 分片被手机丢弃）
- 蓝牙耳机：**手机系统负责配对与路由**，耳机连上后音频自动进耳机，App 只显示状态
- 详细对接文档见 `数智方舟智通音频中转站-对接说明.md`

## 构建

```bash
# 环境：JDK 17+、Android SDK（platform-34 + build-tools 34）
export ANDROID_HOME=<sdk路径>
gradle assembleDebug          # 或 Android Studio 打开本目录 Build APK
```

产物：`app/build/outputs/apk/debug/app-debug.apk`

## 安装与使用

1. USB 拷 APK 到手机 → 点开安装（允许"未知来源"）
2. 首次打开授权：通知、蓝牙（Android 12+）、网络
3. **家庭/室内**：手机与设备同一 WiFi → 设备"设置"页开"转发到手机"即可对话
4. **外出（设备连手机热点）**：设备"设置"页再开"**组播模式**"（音频走组播穿透热点 NAT，已实测可用）
5. 手机蓝牙设置配对耳机（系统操作，音频自动路由到耳机）
6. 停止中转：App 内"停止中转"按钮（App 为前台服务，锁屏/切后台持续工作）

## 调试

- PC 协议探针：`scripts/audio_forward_probe.py`（模拟 App 收发，无需手机）
- 国产 ROM 后台限制：如华为/小米在"电池管理"里允许本 App 后台运行/自启动
