# 表情动画播放 — 变更记录

## 2026-07-29 #23 — 0x107 真因 + FATFS LFN + 表情切换稳定方案

### 0x107 (SDMMC超时) 根因分析

**持续半个月的问题最终定位：SD卡文件系统损坏，不是C6 WiFi冲突。**

| 误判历程 | 实际原因 |
|----------|----------|
| 以为是C6 SDIO抢占Slot 0 | C6静默时也有0x107（t=1072ms，C6在t=4083ms才初始化） |
| 以为是SDMMC时钟未初始化 | dummy init结合C6 auto_init可以正常工作 |
| 以为是MJPEG大文件fseek问题 | 格式化后同样文件完美读取 |

**真实验证**：
- 同一文件 `thinking.mjpeg`：旧卡卡在32KB，格式化新卡后125/125帧完整读取
- `neutral.mjpeg`：旧卡卡在32KB，`awake.mjpeg`：114/125帧（尾部丢失）
- 逐JPG文件：旧卡前5个OK，第6个开始全部0x107
- **全部不同的文件在不同位置失败 → 典型的FAT文件系统损坏**

**解决方案**：格式化SD卡为FAT32，重新拷贝文件。

### MJPEG预加载 + 表情切换（稳定方案）

- `ppa_preload_mjpeg(path)`: **一次性顺序fread整个MJPEG文件到PSRAM**，零fseek，然后在内存中memcpy解析帧
- `mjpeg_set_emotion()`: 同机制，WiFi前预加载默认表情
- `sd_mount_task`中预加载，`sdcard_wait_ready()`阻塞等完成 → 再启动WiFi
- 播放时零SD访问，30+ FPS稳定

### FATFS 长文件名 (LFN) — menuconfig陷阱

**问题**: `idf.py menuconfig` 会覆盖 `sdkconfig`，导致以下配置丢失：
1. `CONFIG_FATFS_LFN_STACK=y` → 变回 `LFN_NONE`，`.mjpeg` 扩展名找不到
2. `lvgl__lvgl/esp.cmake` 的 `REQUIRES esp_timer fatfs` → fatfs依赖丢失
3. `78__esp-wifi-connect` 的 `IsExitRequested` + `/exit` 端点 → 被覆盖

**解决方案**：menuconfig后需要手动恢复这三个patch，或直接改sdkconfig文件而不开menuconfig。

### 当前架构

```
上电 → C6 auto_init(t=614ms) → SD挂载(t=950ms)
      → ppa_preload_mjpeg("thinking.mjpeg") → 125帧入PSRAM
      → sdcard_wait_ready() → 启动WiFi
      → 播放: PSRAM指针直接返回，零SD卡访问，30FPS
      → 每3秒切换: ppa_preload_mjpeg("neutral.mjpeg") 重载PSRAM
```

---

## 2026-07-28 #22 — CAPTIVE_PORTAL_MODE 编译开关

**新增**: `#define CAPTIVE_PORTAL_MODE` 控制配网模式
- **1 = 弹窗模式** — 302重定向到配网页，手机弹出浏览器手动输入WiFi（开发调试）
- **0 = 静默模式** — 伪装"能上网"信号，不弹页面（小程序配网）
- 修改位置: `wifi_configuration_ap.cc` 顶部 `#define CAPTIVE_PORTAL_MODE 1`
- 当前: 弹窗模式（因为小程序还没开发好）

---

## 2026-07-28 #21 — WiFi前预打开MJPEG (SDMMC冲突解决方案)
**修改文件**: `MjpegPlayer.h/cpp`, `ImageDisplay.cpp`, `application.cc`, `PPACompositor.h`

**方案**: 在C6 WiFi启动前，独占SDMMC总线预打开所有MJPEG文件
- `MjpegPlayer` 新增多文件预打开 API:
  - `mjpeg_preopen_all()` — 扫描/sdcard，fopen所有.mjpeg，读偏移表→PSRAM
  - `mjpeg_set_emotion(name)` — 切换表情（仅改指针，无文件IO）
  - `mjpeg_get_emotion_count()/name()` — 查询已加载的表情
- `image_display_preopen()` — WiFi前调用，预打开22个表情
- `application.cc` — 在 `sdcard_init()` 后、`board.StartNetwork()` 前调用预打开
- 播放时表情切换用 `mjpeg_set_emotion()` 代替 `ppa_open_mjpeg()`
- PSRAM: 22表情×~1KB = ~22KB，远小于全帧加载

**预期效果**: 预打开后的fseek+fread为原子操作（~1ms），不会与WiFi的SDIO传输冲突

---

## 2026-07-28 #20 — Captive Portal 免弹窗 + 长文件名修复
**修改文件**: `sdkconfig`, `wifi_configuration_ap.cc/h`, `wifi_board.cc`

### FATFS 长文件名修复
- **根因**: `CONFIG_FATFS_LFN_NONE=y` → 8.3 短名，`thinking.mjpeg` 变 `THINKI~1.MJP`
- **修复**: 改为 `CONFIG_FATFS_LFN_STACK=y`
- **结果**: ✅ SD卡上 `.mjpeg` 和 `.jpg` 文件全部用全名正确识别

### Captive Portal 免弹窗
- **根因**: 手机连热点后会探测"有无互联网"，ESP32 原返回 302 重定向到配网页 → 浏览器自动弹出
- **需求**: 小程序配网不需要网页，手机不应弹出浏览器
- **修复**:
  - Apple `/hotspot-detect.html` → 返回 `Success` (200) — iOS 认为有网，不弹窗
  - Android `/generate_204*` → 返回 204 No Content — Android 认为有网，不切换
  - 其他探测地址 → 返回 204
- **结果**: ✅ 手机连热点后不会弹浏览器

### 小程序配网端点
- **新增** `POST /exit` 端点 — 接收小程序"配网完成"确认
- **新增** `exit_requested_` 标志+`IsExitRequested()` — 跨端点通信
- **改造** `/reboot` → 设 `exit_requested_=true`，不重启
- **改造** `EnterWifiConfigMode()` → 两阶段轮询（Phase1: 检测SSID → Phase2: 等/exit信号）

---

## 2026-07-28 #19 — esp-wifi-connect SmartConfig P4 兼容修复
**修改文件**: `wifi_configuration_ap.cc`
- **根因**: `Stop()` 中直接调用 `esp_smartconfig_stop()`/`SC_EVENT`，P4 无 smartconfig 组件
- **修复**: 用 `#if !CONFIG_IDF_TARGET_ESP32P4` 包裹 smartconfig 相关代码（Stop + StartSmartConfig + SmartConfigEventHandler）
- **结果**: ✅ 编译通过 ##

### 配网免重启改造 (#18)
**修改文件**: `wifi_configuration_ap.cc`, `wifi_board.cc`
- `/reboot` 端点不再调用 `esp_restart()`，只返回 success
- `EnterWifiConfigMode()` 轮询 SSID 变化（500ms×240次），检测到新 SSID 后停 AP 切 Station
- `StartNetwork()` 配网模式返回后继续 WiFi 连接流程
- **结果**: ✅ 配网后不再重启，直接连接

### 编译环境修复
- 升级 `78/esp-wifi-connect` 版本 → 后回退至 `~2.4.3`（v3.0 API 不兼容）
- 启用 `CMAKE_NINJA_FORCE_RESPONSE_FILE=ON` 解决 Windows 命令行超长
- 添加 `lvgl__lvgl` 的 `fatfs` 依赖
- OTA URL 设为七牛云 `https://xrobo.qiniuapi.com/v1/ota/`
- 板型配置 `guition-jc4880p443`

---

## 问题描述
ESP32-P4 SD卡(Slot0) + C6 WiFi(Slot1) 共享 SDMMC 硬件，ESP-IDF v5.5.1 下两者冲突：
- 首次上电(无WiFi凭证)：SD卡正常，预加载120帧→31FPS 正常
- WiFi配对后重启：C6抢占SDMMC总线→SD卡挂载失败或仅5/120帧

**根因**：ESP-IDF issue #17889，commit `c2b8ea07d0` 将 `SDMMC_LL_HOST_CTLR_NUMS` 硬编码为 1，且 DMA 状态全局共享(Slot0/Slot1 ISR 互相破坏)。v5.4可用，v5.5.1+不可用。

---

## 变更记录

### 2026-07-26 #14 — LDO电压显式设置3300mV
**文件**: `main/sd_test.cc`
**改动**: 
- `sd_pwr_ctrl_new_on_chip_ldo({.ldo_chan_id=4})` 
- → `esp_ldo_acquire_channel({.chan_id=4, .voltage_mv=3300})`
**依据**: GitHub issue #17889 comment #39 — 同款Guition板用户确认需要显式设3300mV
**预期**: SD卡供电稳定，预加载全120帧成功
**结果**: 待测试

### 2026-07-26 #13 — 构造器提权+预加载 (prio 24)
**文件**: `jc4880p443.cc`
**改动**: `vTaskPrioritySet(NULL,24)` → SD挂载+预加载120JPG → `vTaskPrioritySet(NULL,1)`
**结果**: ❌ 仍只有5/120帧

### 2026-07-26 #12 — max_files: 5→128
**结果**: ❌ 无效

### 2026-07-26 #11 — sd_mount_task prio:5→24 + 内嵌预加载
**结果**: ❌ 仍5/120帧（transport虽不能抢占，但SDMMC驱动本身有限制）

### 2026-07-26 #10 — ESP_SYSTEM_INIT_FN 极早期初始化
**结果**: ❌ 撤回（CORE阶段FreeRTOS未启动，无法用VFS）

### 2026-07-26 #9 — GPIO 54硬复位C6
**结果**: ❌ C6复位后WiFi连接失败，撤回

### 2026-07-26 #8 — MJPEG→PSRAM整块读取
**文件**: `MjpegPlayer.cpp`
**改动**: mjpeg_open时一次性fread全部125帧到PSRAM(7.5MB)
**结果**: ❌ SD卡挂载失败（LDO被误删导致）

### 2026-07-26 #7 — 恢复备份"保持视频且能正常对话"(ESP-IDF v5.4)
**文件**: 全部 image_display + sd_test.cc + application.cc
**结果**: ✅ 备份版在v5.4下全120帧31FPS正常；当前v5.5.1仅5帧

### 2026-07-26 #6 — MJPEG多文件预打开 (mjpeg_register x22)
**文件**: `MjpegPlayer.cpp/h`
**改动**: 在WiFi前一次fopen 22个MJPEG文件
**结果**: 18/22→10/22→5/22 不稳定，且播放时fseek+fread仍被WiFi阻断

### 2026-07-26 #5 — 预加载回退到逐JPG文件+PSRAM
**文件**: `ImageDisplay.cpp`
**改动**: search_image_files() + ppa_preload_frames(121帧到PSRAM)
**结果**: ✅ 31FPS但仅5/120帧

### 2026-07-26 #4 — SD 卡挂载后阻塞预打开
**文件**: `application.cc`
**改动**: sdcard_wait_mounted()后立即预打开全部MJPEG
**结果**: 18/22成功(WiFi前窗口)，播放时fread仍失败

### 2026-07-26 #3 — SD 卡构造器同步挂载(~650ms)
**文件**: `jc4880p443.cc`
**改动**: 在构造器中调用 sdcard_mount_sync()
**结果**: SD挂载成功但WiFi驱动初始化崩溃(esp_wifi_init失败)

### 2026-07-26 #2 — 双重防挂载 + 预加载容错重试
**文件**: `sd_test.cc`, `PPACompositor.cpp`
**改动**: s_sd_mounted标志 + 3次重试50ms
**结果**: SD挂载稳了但预加载仍5/120帧

### 2026-07-26 #1 — 回归MJPEG+PSRAM全加载方案
**文件**: `PPACompositor.cpp`
**改动**: ppa_open_mjpeg→逐帧读入PSRAM
**结果**: SD卡0x107失败

---

### 2026-07-26 #16 — 40MHz高速SD + WiFi后挂载
**结果**: ❌ 无效

### 2026-07-26 #17 — **回退到"成功切换表情！"备份**
**文件**: 全部恢复
**说明**: 14次尝试后，确认 ESP-IDF v5.5.1 无法稳定支持 SDMMC Slot 0+1 共存。回退到 MJPEG fseek 流式方案（已调通的备份版），将来需通过升级 C6 固件或降级 ESP-IDF 解决

---

## 待尝试方案

| 方案 | 来源 | 状态 |
|---|---|---|
| LDO 3300mV显式设置 | Issue #17889 comment #39 | **测试中** |
| Bruce297 SDMMC re-init patch | Issue #17889 comment #41 | 已下载，待应用 |
| 降级ESP-IDF v5.4 | Issue确认v5.4可用 | 备份方案 |
| C6固件OTA升级 v2.3→v2.12 | esp_hosted slave OTA example | 备选 |
