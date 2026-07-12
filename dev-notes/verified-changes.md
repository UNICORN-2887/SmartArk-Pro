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
