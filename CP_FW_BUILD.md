# CP(C6 协处理器)官方固件构建指南

> 背景:esphome 预编译固件 3.0.9(SDIO SW_AGGR 可用、速度正常)但 WiFi 不稳定
> (信号 -53 也 4-way 握手超时 reason=15、反复断连、MAC 读成垃圾值)。
> 官方没有 3.0.9 预编译固件,需从组件注册表自行构建(官方源码,无 ESP-NOW 附加)。

## 前置条件

- ESP-IDF 5.5.1(本机已有:`E:\Passport\esp32\v5.5.1\esp-idf`)
- ESP32-C6 烧录方式(板载 USB 转串口,或板厂提供)

## 构建步骤

```bat
:: 1. 新建项目(官方示例)
cd /d E:\Passport\espp4\sparepart\JC4880P443C_I_W\1-Demo
idf.py create-project-from-example "espressif/esp_hosted=3.0.9:wifi/sta/cp"
cd wifi_sta_cp

:: 2. 目标芯片
idf.py set-target esp32c6

:: 3. 菜单配置(默认即可:CP transport = SDIO,CP feature = WiFi)
idf.py menuconfig
:: 确认:
::   ESP Hosted → Coprocessor 相关 → Communication bus = SDIO(默认)
::   WiFi 相关保持默认(可关省电相关选项,与 host 侧 WIFI_PS_NONE 呼应)

:: 4. 构建
idf.py build

:: 5. 烧录(C6 的串口;板厂出厂烧录用的口)
idf.py -p <C6_COM口> flash
```

## 构建产物

`build/merged-binary.bin`(或各分区 bin)—— 烧录后 CP 版本为 3.0.9。

## 验证

设备开机日志:
```
esp-hosted fw versions: host=3.0.9 coprocessor=3.0.9 (match)
SDIO SW_AGGR negotiated
```

## 注意事项

- 构建出的固件替代 esphome 版(无需再走设备端 CP OTA;CP 版本匹配后自动跳过)
- 若 C6 无板载串口,需板厂协助烧录(或提供测试点)
- 烧录后建议先长时间观察 WiFi 稳定性(断连次数)再验收
