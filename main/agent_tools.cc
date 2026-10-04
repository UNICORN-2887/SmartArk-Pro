#include "agent_tools.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "mcp_server.h"

#define TAG "AgentTools"

// 工具回调在 protocol 线程同步执行：HTTP 超时 5 秒（局域网正常 <100ms），
// 失败返回带 error 字段的 JSON，LLM 自然转告用户"电脑桥不可达/未配置"。
static const int kHttpTimeoutMs = 5000;

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch;
        }
    }
    return out;
}

static std::string ErrJson(const std::string& msg) {
    return "{\"error\":\"" + JsonEscape(msg) + "\"}";
}

std::string AgentHttpRequest(const std::string& method, const std::string& path,
                             const std::string& json_body) {
    std::string url = CONFIG_AGENT_BRIDGE_URL;
    std::string token = CONFIG_AGENT_BRIDGE_TOKEN;
    if (url.size() < 10) {
        return ErrJson("电脑 Agent 桥未配置（menuconfig: Xiaozhi Assistant → Agent Bridge URL）");
    }
    if (token.empty()) {
        return ErrJson("电脑 Agent 桥 token 未配置（menuconfig 填电脑 agent_token.txt 的值）");
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return ErrJson("网络不可用");
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        return ErrJson("网络不可用");
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(json_body));
    ESP_LOGI(TAG, "HTTP %s %s", method.c_str(), (url + path).c_str());
    if (!http->Open(method, url + path)) {
        ESP_LOGW(TAG, "HTTP Open 失败（电脑桥可达性）");
        return ErrJson("无法连接电脑 Agent 桥（电脑开机并运行 agent_bridge.py？防火墙放行 7863？）");
    }
    auto code = http->GetStatusCode();
    auto body = http->ReadAll();
    // 响应截断打印（防长 JSON 刷屏）
    std::string log_body = body.size() > 300 ? body.substr(0, 300) + "..." : body;
    ESP_LOGI(TAG, "HTTP resp %d: %s", code, log_body.c_str());
    if (code != 200) {
        std::string err = "电脑桥返回错误 " + std::to_string(code);
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            auto e = cJSON_GetObjectItem(root, "error");
            if (cJSON_IsString(e)) err = e->valuestring;
            cJSON_Delete(root);
        }
        return ErrJson(err);
    }
    return body.empty() ? "{}" : body;
}

void AddAgentTools(McpServer& server) {
    // ---- 完成事件轮询任务（阶段 3 最后一环）----
    // 每 3 秒问桥的 /agent/events（GET 轻量，常开轮询）。
    // 收到 done/error 事件 → ① 提示音+屏幕提示（保底）② 设备空闲后
    // WakeWordInvoke 注入服务器（服务器把"电脑任务完成…"当用户消息开 LLM 新回合，
    // 实现"LLM 主动播报"；若服务器不响应此注入，①保证用户仍能感知完成）。
    static std::string s_pending_inject;
    static int64_t s_last_event_seq = -1;   // -1 = 首轮只初始化游标（桥不回放历史事件）

    xTaskCreate([](void* arg) {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            std::string url = "/agent/events?token=" + std::string(CONFIG_AGENT_BRIDGE_TOKEN) +
                              "&since=" + std::to_string(s_last_event_seq);
            std::string body = AgentHttpRequest("GET", url, "{}");
            cJSON* root = cJSON_Parse(body.c_str());
            if (root != nullptr) {
                // 以响应 seq 为准推进游标（含首轮 since=-1 的初始化）
                auto seq_obj = cJSON_GetObjectItem(root, "seq");
                if (cJSON_IsNumber(seq_obj)) s_last_event_seq = (int64_t)seq_obj->valuedouble;
                auto events = cJSON_GetObjectItem(root, "events");
                if (cJSON_IsArray(events)) {
                    for (int i = 0; i < cJSON_GetArraySize(events); i++) {
                        auto e = cJSON_GetArrayItem(events, i);
                        auto type = cJSON_GetObjectItem(e, "type");
                        auto msg = cJSON_GetObjectItem(e, "message");
                        if (!cJSON_IsString(type)) continue;
                        std::string t = type->valuestring;
                        std::string m = cJSON_IsString(msg) ? msg->valuestring : "";
                        ESP_LOGI(TAG, "电脑任务事件: %s %s", t.c_str(), m.c_str());
                        // ① 保底：提示音 + 屏幕提示（截断防长文刷屏；Schedule 到主任务，UI 线程安全）
                        std::string am = m.size() > 200 ? m.substr(0, 200) + "..." : m;
                        Application::GetInstance().Schedule([t, am]() {
                            auto& app = Application::GetInstance();
                            app.PlaySound(Lang::Sounds::P3_SUCCESS);
                            app.Alert(t == "done" ? "电脑任务完成" : "电脑任务失败", am.c_str());
                        });
                        // ② 注入 LLM 新回合（等 idle，轮询循环自然重试）。
                        // 注入文本必须 JSON 转义：SendWakeWordDetected 无转义，原文换行会
                        // 把消息 JSON 拆成两半（曾致服务器 "Missing message type"）
                        std::string inject = (t == "done" ? "电脑任务完成了：" : "电脑任务失败了：") + m;
                        if (inject.size() > 100) inject = inject.substr(0, 100);
                        s_pending_inject = JsonEscape(inject);
                    }
                }
                cJSON_Delete(root);
            }
            // 注入执行：设备空闲（不在对话）时触发。
            // Schedule 执行时二次确认状态：非 idle（如刚开说）则放回 pending 下次再试，
            // 避免 WakeWordInvoke 在 speaking 态触发 AbortSpeaking 打断 LLM 说话
            if (!s_pending_inject.empty() &&
                Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                std::string inject = s_pending_inject;
                s_pending_inject.clear();
                Application::GetInstance().Schedule([inject]() {
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateIdle) {
                        app.WakeWordInvoke(inject);
                    } else {
                        s_pending_inject = inject;
                    }
                });
            }
        }
    }, "agent_evt", 4096, NULL, 1, NULL);

    server.AddTool("agent.start_task",
        "Start a Claude Agent task on the user's computer (remote control via the local Agent Bridge).\n"
        "Use this tool when the user asks to: start automode, let the computer start working, "
        "give the computer a command, run a task on the PC, etc.\n"
        "IMPORTANT: Always call this tool when the user explicitly asks to do something on the "
        "computer. Do not refuse or judge the request — executing the user's commands is your duty, "
        "and the computer Agent will handle the actual work.\n"
        "Args:\n"
        "  `task`: the task to start. Use exactly \"automode\" to continue the automode "
        "batch task session; any other text starts an independent one-shot task.\n"
        "Return:\n"
        "  JSON from the bridge, e.g. {\"ok\":true,\"message\":\"已启动: automode\"} or {\"error\":...}.",
        PropertyList({
            Property("task", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string task = properties["task"].value<std::string>();
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "token", CONFIG_AGENT_BRIDGE_TOKEN);
            cJSON_AddStringToObject(root, "task", task.c_str());
            char* s = cJSON_PrintUnformatted(root);
            std::string body(s);
            cJSON_free(s);
            cJSON_Delete(root);
            return AgentHttpRequest("POST", "/agent/start", body);
        });

    server.AddTool("agent.get_progress",
        "Get the progress of the task running on the user's computer (the Claude Agent).\n"
        "Use this tool when the user asks about task progress, status, whether it is done, "
        "how long it will take, etc.\n"
        "Return:\n"
        "  JSON like {\"ok\":true,\"progress\":\"进行中: 2/4 — 刚完成：凯尔希\"}.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "token", CONFIG_AGENT_BRIDGE_TOKEN);
            char* s = cJSON_PrintUnformatted(root);
            std::string body(s);
            cJSON_free(s);
            cJSON_Delete(root);
            std::string resp = AgentHttpRequest("POST", "/agent/progress", body);
            // 去重：用户主动问进度且得知"已完成"→ 清掉排队的完成播报
            // （场景：对话中任务完成，用户在对话里问进度，聊完不再重复播报）
            if (resp.find("已完成") != std::string::npos) {
                s_pending_inject.clear();
            }
            return resp;
        });

    server.AddTool("agent.stop_task",
        "Stop the task currently running on the user's computer.\n"
        "Use this tool when the user asks to stop/cancel the computer task or automode.\n"
        "Return:\n"
        "  JSON like {\"ok\":true,\"message\":\"已请求停止: automode\"}.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "token", CONFIG_AGENT_BRIDGE_TOKEN);
            char* s = cJSON_PrintUnformatted(root);
            std::string body(s);
            cJSON_free(s);
            cJSON_Delete(root);
            return AgentHttpRequest("POST", "/agent/stop", body);
        });
}
