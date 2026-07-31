#!/usr/bin/env python3
"""fullclean 后一键修复 managed_components 的 3 个补丁"""
import os, re

BASE = os.path.dirname(os.path.abspath(__file__))

# ── Patch 1: lvgl esp.cmake 添加 fatfs ──
def patch_lvgl():
    path = os.path.join(BASE, 'managed_components/lvgl__lvgl/env_support/cmake/esp.cmake')
    with open(path) as f: content = f.read()
    if 'REQUIRES esp_timer fatfs' in content:
        print('[OK] lvgl fatfs — already patched'); return
    content = content.replace('REQUIRES esp_timer)', 'REQUIRES esp_timer fatfs)')
    with open(path, 'w') as f: f.write(content)
    print('[✓] lvgl: added fatfs to REQUIRES')

# ── Patch 2: wifi_configuration_ap.h 添加 IsExitRequested ──
def patch_wifi_h():
    path = os.path.join(BASE, 'managed_components/78__esp-wifi-connect/include/wifi_configuration_ap.h')
    with open(path) as f: content = f.read()
    if 'IsExitRequested' in content:
        print('[OK] wifi_ap.h — already patched'); return

    # 添加 exit_requested_ 成员
    content = content.replace(
        'bool is_connecting_ = false;',
        'bool exit_requested_ = false;\n    bool is_connecting_ = false;')
    # 添加 IsExitRequested 方法
    content = content.replace(
        'std::string GetWebServerUrl();',
        'std::string GetWebServerUrl();\n    bool IsExitRequested() const { return exit_requested_; }')
    with open(path, 'w') as f: f.write(content)
    print('[✓] wifi_ap.h: added IsExitRequested + exit_requested_')

# ── Patch 3: wifi_configuration_ap.cc /exit + /reboot修改 + SmartConfig P4 ──
def patch_wifi_cc():
    path = os.path.join(BASE, 'managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc')
    with open(path) as f: content = f.read()

    modified = False

    # 3a: /exit endpoint
    if '"/exit"' not in content:
        content = content.replace(
            'auto captive_portal_handler',
            '''httpd_uri_t exit_uri = {
        .uri = "/exit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto* this_ = static_cast<WifiConfigurationAp*>(req->user_ctx);
            this_->exit_requested_ = true;
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "OK", 2);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &exit_uri));

    auto captive_portal_handler''')
        modified = True
        print('[✓] wifi_ap.cc: added /exit endpoint')

    # 3b: /reboot 改 exit_requested_（不重启）
    if 'self->exit_requested_ = true' not in content and 'exit_requested_ = true' not in content:
        content = content.replace(
            'esp_restart();\n            }, "reboot_task"',
            'self->exit_requested_ = true;\n            }, "reboot_task"')
        modified = True
        print('[✓] wifi_ap.cc: /reboot sets exit_requested_ (no restart)')

    # 3c: SmartConfig P4 — 包裹 #if !CONFIG_IDF_TARGET_ESP32P4
    if '#endif\n\n#if !CONFIG_IDF_TARGET_ESP32P4\nvoid WifiConfigurationAp::SmartConfigEventHandler' not in content:
        content = content.replace(
            'void WifiConfigurationAp::StartSmartConfig()',
            '#if !CONFIG_IDF_TARGET_ESP32P4\nvoid WifiConfigurationAp::StartSmartConfig()')
        content = content.replace(
            'ESP_LOGI(TAG, "SmartConfig started");\n}',
            'ESP_LOGI(TAG, "SmartConfig started");\n}\n#endif')
        content = content.replace(
            'void WifiConfigurationAp::SmartConfigEventHandler',
            '#if !CONFIG_IDF_TARGET_ESP32P4\nvoid WifiConfigurationAp::SmartConfigEventHandler')
        content = content.replace(
            '    esp_smartconfig_stop();\n            break;\n        }\n    }\n}\n\nvoid WifiConfigurationAp::Stop()',
            '    esp_smartconfig_stop();\n            break;\n        }\n    }\n}\n#endif\n\nvoid WifiConfigurationAp::Stop()')
        content = content.replace(
            '    // 停止SmartConfig服务\n    if (sc_event_instance_) {',
            '#if !CONFIG_IDF_TARGET_ESP32P4\n    // 停止SmartConfig服务\n    if (sc_event_instance_) {')
        content = content.replace(
            '    esp_smartconfig_stop();\n\n    // 停止定时器',
            '    esp_smartconfig_stop();\n#endif\n\n    // 停止定时器')
        modified = True
        print('[✓] wifi_ap.cc: SmartConfig wrapped for P4')

    if modified:
        with open(path, 'w') as f: f.write(content)
    else:
        print('[OK] wifi_ap.cc — already patched')

if __name__ == '__main__':
    patch_lvgl()
    patch_wifi_h()
    patch_wifi_cc()
    print('\nAll patches applied.')
