# 表情动画播放 — 变更记录

## 2026-08-01 #29 — 三槽缓存+模式切换秒开

### 看门狗崩溃修复
**问题**: 按钮切换 cover↔expression 模式时，AFE ringbuffer 溢满 → 看门狗重置
**根因**: `video_play` 和 `audio_detection`（AFE fetch）同为 prio 3 挤在 core 0，
        视频帧处理抢走全部时间片，AFE 拿不到 CPU → ringbuffer 溢满
**修复**:
1. `video_play` 钉 core 0 / prio 2，让 audio_detection(prio 3) 随时抢占
2. `mode_switch_task` 独立 10KB 栈（SD I/O 不用挤 4096 栈）
3. 按钮切换 cover↔expression 的显示逻辑跑在独立 task，不阻塞主循环/LVGL

### 三槽 PPA 缓存（Active + Pending + Cover）
**问题**: expression 模式时 cover 帧被 emotion 换出 pending，切回 cover 须重新 SD 加载 5.7MB
**架构**:
```
Active:  s_jpg_cache[200]  ← 当前播放
Pending: s_pending_cache[200] ← emotion 专用（LLM 驱动）
Cover:   s_cover_cache[200] ← 永久保留（不被 emotion 换出）
```
**关键逻辑**:
1. `s_cover_location`: 三态追踪 (0=无, 1=在slot, 2=在active)
2. `ppa_swap_to_cover()`: 双向交换 active↔cover_slot，自动翻转 location
3. Cover→Expression: save-cover swap → slot 旧帧（neutral）弹回 active → 直接复用，零 SD I/O
4. Expression→Cover: swap cover 回 active → 秒切
5. LLM emotion swap (active↔pending): cover slot 完全不受影响
6. 换角色时 `ppa_unload_cover()` 释放旧 cover

### 按钮文字随状态切换
- cover_display_start → 按钮显示 "对话模式"
- expression_display_start → 按钮显示 "通行证模式"
- 模式切换和对话结束统一更新按钮文字

### 字体修复
- `chat_overlay_init()` 直接传入 `display->GetTextFont()` 中文字体
- `s_btn_labels[]` 数组跟踪所有按钮 label，`chat_overlay_set_font()` 统一更新

### Cover 预加载优化
- cover_display_start 启动时异步预加载 neutral 到 pending
- 首次 expression 切换可通过 swap 直接命中

### 对话断连
- `Application::CloseAudioChannel()`: 直接关闭协议通道+设 idle
- 点击"通行证模式"即断对话，不走 Schedule 链等待

### 杂项
- snprintf 缓冲区扩大到 520 字节（agent_path+d_name 组合可能超 300）
- `expression_display_start` 等待旧 video task 退出后再启动新 task（防 PPA 冲突）

## 2026-08-01 #28 — GT911 触摸修复 + 聊天覆盖层

### GT911 触摸驱动修复
**问题**: JC4880P443 触屏完全不工作，GT911 初始化报 "GT911 read error"
**根因**:
1. `InitializeGT911()` 从未被构造函数调用
2. GT911 上电后需手动复位时序（拉低 20ms → 拉高 100ms）
3. `int_gpio_num` 错误设为 GPIO 21，Waveshare 参考板用 `GPIO_NUM_NC`
4. I2C 时钟 100kHz 偏低，参考板用 400kHz
5. `lvgl_port_add_touch` 返回值是 `lv_indev_t*` 不是 `esp_err_t`

**修复**:
- 构造函数中调用 `InitializeGT911()`
- 手动 `gpio_set_level(GPIO 22)` 复位 → `gpio_config` 拉低/拉高
- `rst_gpio_num = GPIO_NUM_NC`（驱动不接管复位）
- `int_gpio_num = GPIO_NUM_NC`（轮询模式，参考 Waveshare）
- `scl_speed_hz = 400000`
- 非致命初始化：失败时 return 不崩溃
- `#include <driver/gpio.h>` + `#include "esp_lvgl_port.h"`

### 聊天覆盖层（半透明置顶）
- LVGL 两段式覆盖层，`lv_layer_top()` 置顶
- 用户框 y=520 (440×100)，助理框 y=630 (440×140)
- 半透明灰底（`LV_OPA_50`），白字中文，可滚动
- LLM `sentence_start` → `chat_overlay_append_assistant()` 追加文本
- STT → `chat_overlay_set_user()` + 清空助理框
- 唤醒时 `chat_overlay_show(true)`，回 cover 时隐藏
- 右上角 LVGL 按钮 "隐藏/显示"（触摸修复后可用）
- `Display::GetTextFont()` 暴露中文字体

### 自动回退 neutral
- 非 neutral 表情播完 1 轮自动 `expression_switch_emotion("neutral")`
- `ppa_swap_emotion` 后 `s_pending_ready = true`（旧活跃已变后备，直接可用）
- Swap 后正确更新 `s_pending_emotion`

### 文件结构
- `build_mjpeg_mask.py` — 一站式生成 MJPEG + .mask（Q 可选，MARGIN_BOTTOM=20）
- `build_mask.py` — 独立 .mask 生成
- `compress_mjpeg.py` — JPEG 降质
- `pack_jpg.py` — JPG 序列打包 MJPEG
- `split_mjpeg.py` — MJPEG 拆分 part
- `deploy_sd.py` — SD 卡一键部署
- `patch_managed_components.py` — fullclean 后一键修复 3 补丁

### Cover 流式预加载
- `load_mjpeg_into()` 512KB chunk 顺序读取，零 fseek
- MjpegPlayer 顺序帧检测跳过 fseek
- `ppa_preload_mjpeg()` 复用 `load_mjpeg_into()`（去重）

---

## 2026-07-31 #27 — LLM 情绪抢占式表情 + RLE 遮罩 Alpha 混合 + fullclean 恢复指南

### RLE 遮罩 Alpha 混合
- **问题**: JPEG Q<85 时色键抠图边缘锯齿严重
- **方案**: 每帧附带 RLE 压缩 1bit 遮罩（~10KB/帧），JPEG 解码后转 ARGB8888，PPA Alpha 混合逐像素完美边缘
- **效果**: Q=20 也无锯齿，单表情 2-3MB（+遮罩 ~1MB），PSRAM 仅存 2 个
- **工具**: `build_mjpeg_mask.py` 一键生成 .mjpeg + .mask
- **格式**: `.mask` 文件 [4B fc][N×4B offsets][RLE: 2B count, 1B value...]
- **颜色空间**: ARGB8888（ARGB1555 在 ESP-IDF v5.5 PPA 中不可用）

### JPEG 质量对比（凯尔希新表情 480×800, 120帧）

| Q | MJPEG | Mask | 合计 |
|---|-------|------|------|
| 20 | 2.0 MB | 0.9 MB | **2.9 MB** |
| 40 | 2.8 MB | 0.9 MB | **3.7 MB** |
| 60 | 3.4 MB | 0.9 MB | **4.3 MB** |
| 85 | 6.2 MB | 0.9 MB | **7.1 MB** |

### Cover 早期预加载
- SD 挂载后立即预加载 cover（WiFi 前，SD 独占 ~2.9MB/s）
- `cover_display_start` 首次调用直接复用，零等待

### LLM 情绪对接（抢占式）
- LLM 消息 `{"type":"llm","emotion":"happy"}` → `expression_switch_emotion()` → 后台预加载 → `s_force_swap` → 视频任务每帧检查 → 就绪立即交换
- 终端打印: `🎭 LLM → expression: happy` + `🎭 Preemptive swap: 120 frames`
- 移除定时轮播，LLM 独占驱动

### SD 卡加载优化
- `load_mjpeg_into()` 改为流式顺序读取（512KB chunk，零 fseek）
- MjpegPlayer 顺序帧检测，跳过不必要的 fseek

### fullclean 后一键恢复（重要！）
`idf.py fullclean` 会重置 3 个 managed_components 文件，运行修复脚本：
```powershell
python patch_managed_components.py
```
修复内容：
1. `lvgl__lvgl/esp.cmake` → REQUIRES esp_timer **fatfs**
2. `esp-wifi-connect/wifi_configuration_ap.h` → IsExitRequested() + exit_requested_
3. `esp-wifi-connect/wifi_configuration_ap.cc` → /exit 端点 + /reboot 不重启 + SmartConfig P4 包裹

### 环境恢复
重启/断电后需重新激活 ESP-IDF：
```powershell
. E:\Passport\esp32\v5.5.1\esp-idf\export.ps1
```
如果报错 Python 环境丢失：
```powershell
python E:\Passport\esp32\v5.5.1\esp-idf\tools\idf_tools.py install-python-env
python E:\Passport\esp32\v5.5.1\esp-idf\tools\idf_tools.py install
```
如果 fullclean 后 Python 路径变了：
```powershell
idf.py fullclean
idf.py build
```

---

## 2026-07-30 #25 — 多唤醒词防误触发 + 智能体切换准备

### 多唤醒词注册（5条命令）
- `ni hao kai er xi` (command_id=1) → "你好凯尔希"
- `kai er xi` (command_id=2) → "凯尔希"(短)
- `ni hao xiao zhi` (command_id=3) → "你好小智"(保底)
- `ni hao a mi ya` (command_id=4) → "你好阿米娅" ✨新增
- `a mi ya` (command_id=5) → "阿米娅"(短) ✨新增
- 显示名映射数组 `names[]`，为智能体切换做准备

### 防误触发三层防护
**问题**: 对话结束后长时间静音，设备会自动唤醒（假阳性）  
**根因分析**:
1. 对话期间 AFE 持续接收麦克风数据，检测 task 被 Stop 不消费 → 音频堆积
2. 对话结束后 `Start()` 直接放行 → `fetch_with_delay()` 秒吐堆积音频 → MultiNet 误匹配
3. `get_results()` 在 detect 未匹配时返回残留旧数据（空字符串, prob=0.19, command_id=4）

**三层防护**:

| 层级 | 机制 | 文件 | 解决问题 |
|------|------|------|----------|
| 1 | `Start()` 中 `reset_buffer()` + `clean()` | custom_wake_word.cc | 清除对话期间 AFE 堆积的残留音频 |
| 2 | 阈值 0.05→0.10 | custom_wake_word.cc | 过滤环境噪声低概率假阳性（正常唤醒~0.12） |
| 3 | 5秒冷却时间 | custom_wake_word.cc | 防止连续误触发 |

### ESP_MN_STATE_DETECTED 状态检查
- **关键修复**: 加回 `mn_state == ESP_MN_STATE_DETECTED` 判断
- **原因**: 之前绕过此检查是因为默认阈值0.5太高，自定义唤醒词永远进不了 DETECTED
- **现在**: 阈值降到 0.10 后，正常唤醒词(prob≈0.12~0.20)能触发 DETECTED 状态
- **效果**: 过滤掉 `get_results()` 在未检测到匹配时返回的残留旧数据

### 双缓冲异步表情切换修复
- Bug fix: `emotion_idx` 只在 swap 成功后才递增（之前提前递增导致显示错误表情名）
- 预期行为: thinking↔neutral 每3秒自动切换，零阻塞（<1ms）

---

## 2026-07-29 #24 — 阿米娅唤醒词 + 双缓冲表情切换

### 新增唤醒词
- `ni hao a mi ya` (你好阿米娅, command_id=4)
- `a mi ya` (阿米娅短唤醒, command_id=5)
- 检测范围扩展至 1~5

### 双缓冲异步表情切换
**问题**: 表情切换时需从 SD 卡加载 4.4MB MJPEG → 阻塞显示 ~200ms → 画面冻结  
**方案**: 活跃缓存 + 后备缓存双缓冲

- `s_jpg_cache[]` — 活跃帧（当前播放）
- `s_pending_cache[]` — 后备帧（后台异步预加载）
- `ppa_preload_mjpeg_async(path)` — FreeRTOS 后台任务（prio=2, stack=8KB）
- `ppa_swap_emotion()` — 瞬间交换两个缓存数组（<1ms，零阻塞！）
- 首次预加载后自动启动下一个表情的异步加载
- 修复: 预加载前先释放旧帧，防止 PSRAM 碎片化

### 30FPS 表情播放 + 对话共存
- 视频播放 task prio=5，独立于音频 pipeline
- 125帧 thinking/neutral MJPEG，30+ FPS 稳定播放
- 对话期间表情持续播放不中断

---

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
