# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

xiaozhi-esp32 (小智 AI 聊天机器人) — an ESP32-based AI voice chatbot that uses MCP (Model Context Protocol) for device control. It connects to a cloud server via WebSocket or MQTT+UDP, streams audio with OPUS codec, performs wake-word detection (ESP-SR), and drives OLED/LCD displays with LVGL for emotion表情表达. Licensed under MIT, supporting 70+ open-source hardware boards.

## Build & Flash

ESP-IDF **v5.4+** is required. All commands use `idf.py`.

```bash
# One-time target setup (esp32, esp32s3, esp32c3, esp32c6, esp32p4)
idf.py set-target esp32s3

# Configure board type and options via menuconfig
idf.py menuconfig
# Key menu: "Xiaozhi Assistant" → Board Type, language, wake word, AEC, etc.

# Build
idf.py build

# Flash (single command)
idf.py -p /dev/ttyACM0 flash

# Monitor serial output
idf.py -p /dev/ttyACM0 monitor

# Build and release (within project)
python scripts/release.py [board_type]
# e.g.: python scripts/release.py guition-jc4880p443
# "all" builds every board that has a config.json
```

`scripts/flash.sh` is a legacy quick-flash script using esptool directly (hardcoded `/dev/ttyACM0` and a specific firmware path).

## Core Architecture

### Startup Sequence (`main/main.cc` → `Application::Start()`)

1. `app_main` creates the default event loop, initializes NVS flash, then calls `Application::Start()`
2. `Application::Start()` initializes the display, audio service, starts the network, checks OTA for firmware updates / activation, then initializes the protocol (MQTT or WebSocket), and enters `MainEventLoop()`

### Key Singletons (accessed via `GetInstance()`)

| Class | File | Role |
|---|---|---|
| `Application` | `main/application.cc` | Central state machine, event loop, device lifecycle |
| `Board` | `main/boards/common/board.h` | Hardware abstraction — created by `DECLARE_BOARD` macro per board |
| `AudioService` | `main/audio/audio_service.cc` | Audio pipeline: mic → processing → encode → send / receive → decode → playback |
| `McpServer` | `main/mcp_server.cc` | Device-side MCP protocol — registers tools for LLM to control hardware |
| `DeviceStateEventManager` | `main/device_state_event.cc` | State change events dispatched to listeners |

### Device State Machine

`Application` manages states defined in `device_state_event.h`: `kDeviceStateStarting → kDeviceStateIdle ⇄ kDeviceStateConnecting → kDeviceStateListening ⇄ kDeviceStateSpeaking`. State transitions are posted to `DeviceStateEventManager` and drive display updates, LED changes, and audio pipeline toggling.

### Audio Pipeline

Two independent data flows (see `main/audio/audio_service.h`):
1. **Upload**: MIC → AudioProcessor (AEC/NS) → Encode Queue → Opus Encoder → Send Queue → Protocol → Server
2. **Download**: Server → Protocol → Decode Queue → Opus Decoder → Playback Queue → Speaker

Three FreeRTOS tasks handle this: `audio_input_task`, `audio_output_task`, and `opus_codec_task`. Wake word detection (AFE, ESP-SR, or custom) can interrupt the pipeline.

### Protocol Layer

Abstracted behind `main/protocols/protocol.h`. Two implementations:
- `MqttProtocol` — MQTT for signaling + UDP for real-time audio
- `WebsocketProtocol` — single WebSocket for both

Selected at startup based on server OTA config response (`ota.HasMqttConfig()` / `ota.HasWebsocketConfig()`).

### Board Abstraction

Every board lives in `main/boards/<board-type>/` and must provide:
- `config.h` — GPIO pin definitions, display parameters, audio sample rates
- `xxx_board.cc` — class inheriting from `WifiBoard`, `Ml307Board`, or `DualNetworkBoard`, implementing `GetAudioCodec()`, `GetDisplay()`, `GetBacklight()`, etc.
- `config.json` — `{"target": "esp32s3", "builds": [{"name": "...", "sdkconfig_append": [...]}]}`
- `DECLARE_BOARD(ClassName)` macro registers the concrete board class

Common board code in `main/boards/common/` provides base classes (`WifiBoard`, `Ml307Board`, `DualNetworkBoard`), button handling, backlight/PWM, power management ICs (AXP2101, SY6970), battery monitoring, and sleep timers.

### Display System

`main/display/` provides `LcdDisplay` (SPI/MIPI-DSI panels via LVGL 9.x) and `OledDisplay` (I2C SSD1306/SH1106). Displays show status bar, chat messages (user/assistant/system), emotions, and notifications.

### MCP (Model Context Protocol)

`McpServer` (`main/mcp_server.cc`) registers device capabilities as MCP tools. The LLM controls hardware (volume, brightness, LEDs, GPIO, motors) by sending MCP JSON payloads through the protocol channel.

## Configuration System

`main/Kconfig.projbuild` defines the "Xiaozhi Assistant" menu with:
- **Board Type** — 80+ options, each gated on a chip target (ESP32, ESP32S3, ESP32C3, ESP32C6, ESP32P4)
- **Language** — ZH_CN, ZH_TW, EN_US, JA_JP
- **Wake Word** — AFE (default, needs PSRAM + S3/P4), ESP-SR (C3/C5/C6), or Custom
- **Audio Processor / AEC** — device-side, server-side, or off
- **Display type** — varies per board (OLED vs LCD with specific driver)

Board selection in `main/CMakeLists.txt` maps each `CONFIG_BOARD_TYPE_*` to a `BOARD_TYPE` string, then globs `main/boards/${BOARD_TYPE}/*.cc` and `*.c` into the build.

Language assets are auto-generated: `scripts/gen_lang.py` reads `assets/<lang>/language.json` and produces `assets/lang_config.h`.

## Adding a New Board

See `main/boards/README.md` (中文). Summary:
1. Create `main/boards/<name>/` with `config.h`, `<name>.cc`, `config.json`, `README.md`
2. Add a `BOARD_TYPE_*` choice in `main/Kconfig.projbuild`
3. Add the mapping in `main/CMakeLists.txt` (`elseif(CONFIG_BOARD_TYPE_*)`)
4. The board class inherits from `WifiBoard` / `Ml307Board` / `DualNetworkBoard` and uses `DECLARE_BOARD` to register

## Key Dependencies (from idf_component.yml)

- **Audio**: `78/esp-opus-encoder`, `espressif/esp_codec_dev`, `espressif/esp-sr` (wake word)
- **Display**: `lvgl/lvgl` (~9.2.2), `esp_lvgl_port`, various `esp_lcd_*` drivers
- **Network**: `78/esp-wifi-connect`, `78/esp-ml307` (4G module)
- **Fonts**: `78/xiaozhi-fonts`
- **Peripherals**: `espressif/button`, `espressif/knob`, `espressif/led_strip`, `espressif/esp32-camera`
- **Protocol**: No external MQTT/WebSocket libs — implemented directly in `main/protocols/` using espressif socket APIs

## Code Style

Google C++ code style. Source files use `.cc` extension (not `.cpp`). Headers use `.h`.

## CI/CD

`.github/workflows/build.yml` runs `idf.py build` on push/PR to `main` using `espressif/esp-idf-ci-action@v1.1.0` with `release-v5.4` and `esp32s3` target. This is a basic smoke build; it does not cover all board types.

## Release Process

`scripts/release.py` automates:
1. `idf.py set-target <target>`
2. Appends `sdkconfig_append` options to `sdkconfig`
3. `idf.py -DBOARD_NAME=<name> build`
4. `idf.py merge-bin`
5. Zips `merged-binary.bin` into `releases/v<version>_<name>.zip`

OTA URL defaults to `https://api.tenclass.net/xiaozhi/ota/` (configurable in Kconfig).
