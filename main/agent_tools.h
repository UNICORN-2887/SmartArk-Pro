#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <string>

class McpServer;

// 注册语音遥控电脑 Agent 的 MCP 工具（agent.start_task / get_progress / stop_task）。
// 工具回调同步 HTTP 请求电脑 Agent 桥（Kconfig: AGENT_BRIDGE_URL / AGENT_BRIDGE_TOKEN），
// 返回的 JSON 直接作为工具结果给 LLM。
void AddAgentTools(McpServer& server);

// 电脑桥 HTTP 请求封装（供完成事件轮询任务复用）
std::string AgentHttpRequest(const std::string& method, const std::string& path, const std::string& json_body);

#endif
