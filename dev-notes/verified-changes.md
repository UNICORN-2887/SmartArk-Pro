# 已验证成功的修改

> 此文件记录已经过实际验证、确认可用的代码修改。
> 每项修改包含：起因、原因、步骤。

---

## 1. 修复 menuconfig 组件依赖解析失败

**日期**: 2026-07-08

**起因**: 在 VSCode 中使用 ESP-IDF 插件运行 menuconfig 时报错，CMake 配置失败。

**错误信息**:
```
ERROR: Because project depends on espressif2022/image_player (==1.1.0~1)
which doesn't match any versions, version solving failed.

WARNING: The following versions of the "txp666/otto-emoji-gif-component"
component have been yanked: - 1.0.2 (reason: "1.0.2")
```

**原因**:
- `espressif2022/image_player` 版本 `1.1.0~1` 在 ESP-IDF 组件注册表中已不存在（可能被下架）
- `txp666/otto-emoji-gif-component` 版本 `1.0.2` 已被作者 yanked（撤回）
- 这两个组件只用于特定板子（ESP-HI 机器狗、Otto Robot/Electron Bot），但 `idf_component.yml` 中写成了无条件依赖
- 当前项目使用 ESP32-P4 芯片，根本不需要这两个组件

**修改步骤**:
在 `main/idf_component.yml` 中，为两个组件添加 `rules` 条件限制：

```yaml
# 修改前（无条件依赖，所有芯片平台都会尝试解析）
espressif2022/image_player: ==1.1.0~1
txp666/otto-emoji-gif-component: ~1.0.2

# 修改后（仅相关芯片平台才解析）
espressif2022/image_player:
  version: ==1.1.0~1
  rules:
  - if: target in [esp32c3]          # ESP-HI 机器狗使用 ESP32-C3

txp666/otto-emoji-gif-component:
  version: ~1.0.2
  rules:
  - if: target in [esp32s3]          # Otto/Electron Bot 使用 ESP32-S3
```

**验证结果**: ✅ 成功 — menuconfig 可以正常打开，CMake 配置通过。

---

## 2. 修复 ESP32-P4 烧录后无限重启（ESP-Hosted 内存耗尽）

**日期**: 2026-07-08

**起因**: 固件烧录成功后，板子无法启动，反复重启。

**错误信息**:
```
I (603) host_init: ESP Hosted : Host chip_ip[18]
I (607) H_API: ESP-Hosted starting. Hosted_Tasks: prio:23, stack: 5120 RPC_task_stack: 5120
I (615) H_API: ** add_esp_wifi_remote_channels **

assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)
```

**原因**:
- ESP32-P4 无内置 WiFi，通过 SDIO 接口调用板载 C6 芯片提供 WiFi（ESP-Hosted 方案）
- ESP-Hosted 初始化时创建多个任务和内存池，默认配置下全部从**内部 RAM**（~500KB）分配
- PSRAM（32MB）未被 ESP-Hosted 利用，导致内部 RAM 耗尽
- `app_main` 任务创建时 `xTaskCreatePinnedToCore` 分配栈内存失败 → assert 崩溃
- 关键未启用的配置：
  - `CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM` — 任务栈不用 PSRAM
  - `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` — 内存池不用 PSRAM

**修改步骤**:
在 `sdkconfig` 和 `sdkconfig.defaults.esp32p4` 中启用两项配置：

```ini
# sdkconfig & sdkconfig.defaults.esp32p4
# 修改前（未启用）
# CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM is not set
# CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM is not set

# 修改后
CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM=y    # ESP-Hosted 任务栈使用 PSRAM
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y    # ESP-Hosted 内存池优先用 PSRAM
```

**涉及文件**:
- `sdkconfig` — 运行时配置（立即生效）
- `sdkconfig.defaults.esp32p4` — 默认配置种子（防止 menuconfig 重新生成时丢失）

**验证结果**: ✅ 成功 — 板子正常启动，WiFi 功能正常。

---

## 3. 修复 SD 卡 SDMMC 初始化失败（WiFi 与 SD 卡共享冲突）

**日期**: 2026-07-13

**起因**: SD 卡通过 SDMMC Slot 0 初始化时反复报 `ESP_ERR_TIMEOUT (0x107)`。

**错误信息**:
```
E (911) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (911) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
E (911) jc4880p443: SD card mount failed: ESP_ERR_TIMEOUT
```

**原因**:
- ESP32-P4 只有一个 SDMMC 外设，分为两个槽位：
  - **Slot 0**: 连接 SD 卡（GPIO 43 CLK, 44 CMD, 39-42 D0-D3）
  - **Slot 1**: 连接 C6 WiFi 协处理器（ESP-Hosted SDIO，GPIO 18 CLK, 19 CMD, 14-17 D0-D3）
- ESP-Hosted 先初始化了 `sdmmc_host_init()`，后续对 Slot 0 的初始化被跳过
- ESP32-P4 的 SDMMC Slot 0 需要**片内 LDO 通道 4** 供电，之前未启用
- FATFS 扇区大小需要配置为 4096（不是 512）

**解决方案（参考工作版 `xiaozhi-esp32_v3picOK`）**:

1. **LDO 供电控制** — 使用 `sd_pwr_ctrl_new_on_chip_ldo()` 为 Slot 0 供电:
   ```c
   sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
   sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
   host.pwr_ctrl_handle = pwr_ctrl_handle;
   ```

2. **Dummy init/deinit** — ESP-Hosted 已初始化 SDMMC host，跳过重复初始化:
   ```c
   host.init = &sdmmc_host_init_dummy;
   host.deinit = &sdmmc_host_deinit_dummy;
   ```

3. **异步任务** — 在 FreeRTOS 任务中执行挂载，避免阻塞主流程:
   ```c
   xTaskCreate(sd_mount_task, "sd_mount", 4096, NULL, 5, NULL);
   ```

4. **调用时机** — 在 `Application::Start()` 中 display 初始化之后立即调用 `sdcard_init()`

**涉及文件**:
- `main/sd_test.cc` — **新增** SD 卡初始化与目录遍历代码
- `main/application.cc` — 添加 `extern void sdcard_init(void)` 声明与调用
- `main/CMakeLists.txt` — 添加 `sd_test.cc` 到编译列表
- `sdkconfig.defaults.esp32p4` — 从 v3picOK 同步（含 LVGL FATFS/LODEPNG 配置）
- `sdkconfig` — FATFS 扇区恢复为 4096
- `config.h` + `jc4880p443.cc` — 保持原版，板级文件无需改动

**验证结果**: ✅ 成功 — SD 卡正常挂载，目录遍历正常，WiFi 同时正常工作。

---

## 4. SD卡JPG图片解码显示在屏幕上

**日期**: 2026-07-13

**起因**: 需要在配网阶段将SD卡中的JPG图片显示在屏幕上。

**解决方案（移植自 `xiaozhi-esp32_v3picOK/main/apps/image_display/`）**:
- 使用 ESP32-P4 硬件 JPEG 解码器 (`driver/jpeg_decode.h`)
- 解码为 RGB565 格式，通过 LVGL `lv_canvas` 直接渲染

**涉及文件**:
- `main/apps/image_display/ImageDisplay.cpp` — **新增** JPEG解码+LVGL显示
- `main/apps/image_display/ImageDisplay.hpp` — **新增** 接口头文件（移除extern "C"修复链接）
- `main/CMakeLists.txt` — 添加源文件和 include 路径
- `main/sd_test.cc` — SD卡挂载成功后调用 `image_display_init()`

**验证结果**: ✅ 成功 — HAPPY001.JPG 正常显示在屏幕上。

---

## 5. 视频循环播放（硬件JPEG解码 + MJPEG序列）

**日期**: 2026-07-13

**起因**: 需要实现SD卡图片序列的视频循环播放，验证硬件解码帧率。

**方案**: 
- 基于已有的硬件 JPEG 解码器（`driver/jpeg_decode.h`）
- 新增 `video_playback_task` FreeRTOS 任务循环调用 `image_display_next()`
- 双缓冲 LVGL canvas 解码+显示
- 每秒统计并打印实际 FPS

**涉及文件**:
- `main/apps/image_display/ImageDisplay.cpp` — 新增视频播放任务、FPS统计
- `main/apps/image_display/ImageDisplay.hpp` — 新增 `video_playback_start/stop/get_fps` API
- `main/sd_test.cc` — 挂载后自动启动30FPS循环播放

**验证结果**: ✅ 成功 — 3张JPG以23 FPS循环播放（目标30 FPS），硬件JPEG解码流水线验证通过。

---

## 6. PPA硬件抠图合成（色键抠红 + 图层叠加）

**日期**: 2026-07-14

**起因**: 角色帧含红色背景，需要抠掉红色叠到静态背景上。软件PNG解码太慢（100-200ms/帧），需要硬件方案。

**解决方案**:
- **完全取代PNG方案**：所有素材统一JPEG格式
- **硬件流水线**：JPEG解码器 → PPA BLEND(色键抠红) → RGB565合成 → LVGL canvas
- PPA `fg_ck_en` 前景色键：红色范围 `BGR=(200,0,0)~(255,80,80)` 的像素替换为背景
- 背景缺失时自动降级为直接显示
- 开启 FATFS 长文件名支持（`FATFS_LFN_HEAP`）

**涉及文件**:
- `main/apps/image_display/PPACompositor.cpp` — **新增** PPA BLEND客户端+背景加载+JPEG解码+色键合成
- `main/apps/image_display/PPACompositor.h` — PPA 接口（init/load_bg/composite_frame/deinit）
- `main/apps/image_display/ImageDisplay.cpp` — 重构为PPA合成通道，删除lodepng/PNG代码
- `main/CMakeLists.txt` — 添加PPACompositor.cpp
- `sdkconfig` — 开启FATFS长文件名

**验证结果**: ✅ 成功 — 30+ FPS稳定播放，WiFi+SD卡同时工作正常。

---

## 8. MJPEG表情切换 + 原始v3picOK SDMMC恢复

**日期**: 2026-07-15

**起因**:
1. 引入MJPEG格式后需要交叉播放 thinking/neutral 两套表情
2. SD卡在WiFi连接后重启时挂载失败，需恢复原始v3picOK的sd_test.cc

**关键发现**:
- **sd_test.cc必须使用原始v3picOK版本**（`SDMMC_FREQ_SDR50`、无gpio_reset_pin、无重试）
- SDMMC探测频率(400kHz)会导致SD卡操作时间过长，与WiFi SDIO冲突
- SDR50(50MHz)高速模式数据传输快，占用总线时间短，WiFi不受影响
- MJPEG文件通过fseek定位帧 → fread读取 → 硬件JPEG解码 → PPA合成
- 表情切换：`ppa_open_mjpeg("/sdcard/{name}.mjpeg")` 即可切换

**涉及文件**:
- `main/sd_test.cc` — 恢复为v3picOK原始版本
- `main/apps/image_display/PPACompositor.cpp` — 帧缓存+MJPEG双模式
- `main/apps/image_display/PPACompositor.h` — 新增ppa_preload_frames/ppa_open_mjpeg
- `main/apps/image_display/ImageDisplay.cpp` — MJPEG优先+表情切换+终端显示表情名
- `main/apps/image_display/MjpegPlayer.cpp` — MJPEG文件fseek读取
- `main/CMakeLists.txt` — 添加MjpegPlayer.cpp

**验证结果**: ✅ 成功 — SD卡+WiFi共存，thinking↔neutral每3秒切换，FPS稳定，终端显示表情名。

---

## 7. 七牛云OTA接入 + 帧预加载 + 30FPS稳定播放

**日期**: 2026-07-14

**起因**:
1. 接入七牛云OTA后帧率降至3FPS
2. SD卡fopen/fread每帧耗时~300ms拖垮性能
3. 唤醒词检测抢占CPU加剧帧率下降

**解决方案**:

### 帧预加载到PSRAM
- 启动时一次性读取全部120张JPEG（~7MB）到PSRAM缓存
- 每帧解码直接从内存读取，消除SD卡IO瓶颈
- **关键bug修复**: `char[256][300]` 不是 `const char**`，需要构造指针数组

### 视频优先级提升
- `video_playback_task` 优先级从 5 → 15，高于唤醒词检测

### 背景图过滤
- 搜索帧文件时跳过 `background.jpg`

**涉及文件**:
- `main/apps/image_display/PPACompositor.cpp` — 新增 `ppa_preload_frames()`/`ppa_free_cache()`，`ppa_composite_frame` 改为索引参数
- `main/apps/image_display/PPACompositor.h` — 更新接口
- `main/apps/image_display/ImageDisplay.cpp` — 预加载调用+过滤背景图+指针数组修复

**验证结果**: ✅ 成功 — **30+ FPS稳定播放**，唤醒词和对话同时正常工作。

---

## 7. 七牛云OTA接入 + 动态人物延迟显示 + SD卡挂载/显示分离

**日期**: 2026-07-14

**起因**: 
1. 需要将ESP32-P4板接入七牛云OTA服务（`xrobo.qiniuapi.com`）
2. 配网阶段动图会遮挡"手机连接热点XXX"界面
3. 需要在对话模式启动后才展示动态人物和背景

**解决方案**:

### 7.1 七牛云OTA接入
- 从参考工程 (`xiaozhi-esp32_aicam_1.8.5_MCPpro`) 移植完整 OTA 实现
- `ota.cc`/`ota.h` 替换：新增 `BuildOtaRequestBody()` 构建七牛云协议请求体
- 请求体包含：application版本/elf_sha256、board类型/名称、mac_address、uuid、chip_model、flash_size、psram_size、chip_info、partition_table
- 响应解析：activation、mqtt、websocket、server_time、firmware 五段
- OTA URL：`https://xrobo.qiniuapi.com/v1/ota/`（在 `Kconfig.projbuild` → `OTU_URL` 配置）
- 修复 `Upgrade()` 中 `esp_ota_begin` 返回值判断错误（`!= ESP_OK`）

### 7.2 SD卡挂载与显示分离
**核心教训**：SD卡必须在 WiFi 之前挂载，但**显示必须延迟到对话模式后**。

- **为什么分离**：
  - ESP32-P4 的 SDMMC 外设被 SD卡（Slot 0, GPIO39-44）和 ESP-Hosted WiFi（Slot 1, GPIO14-19）共享
  - 配网阶段需要显示"手机连接热点XXX"界面，不能被图片遮挡
  - 进入对话模式后才展示数字人动画

- **启动顺序**：
  ```
  I2C → LCD → 背光 → [SD卡异步挂载] → 显示 → 音频 → WiFi → OTA → 协议 → 对话模式 → [图片/视频加载]
  ```
  - `sdcard_init()`（异步任务）：`Application::Start()` 中 display 初始化后立即调用
  - `image_display_init()` + `video_playback_start(30)`：`MainEventLoop()` 之前，协议启动成功之后

### 7.3 WiFi连接失败处理
- `wifi_board.cc`：连接失败后 `WifiStation::Stop()` 然后 `EnterWifiConfigMode()` 会因 WiFi 已关闭而崩溃
- 当前保留原逻辑，但标注此已知问题

**涉及文件**:
- `main/ota.cc` — 从参考工程完整替换（七牛云协议 BuildOtaRequestBody + Upgrade修复）
- `main/ota.h` — 新增 `GetFirmwareUrl()`、`StartUpgradeFromUrl()`、`BuildOtaRequestBody()`
- `main/application.cc` — sdcard_init()移回早期位置；image_display_init()+video_playback_start()延迟到对话模式
- `main/sd_test.cc` — 重构为 do_sd_mount()/sdcard_mount_sync()/sdcard_init() 三态接口
- `main/boards/common/wifi_board.cc` — WiFi连接失败后重启进入配网模式

**验证结果**: ✅ 成功 — 七牛云OTA正常通信，配网阶段屏幕干净，联网成功后背景+120帧角色动画自动播放。
