# 设备语音遥控电脑 Agent 方案 v3（定稿待审核）

> 目标：对着设备说话，云端 LLM 通过**设备端本地 MCP**（`mcp_server.cc` AddTool）调用电脑上的 Claude Agent 会话（automode），
> LLM 同时正常对话、随时查进度、Claude 完成后 LLM 转达用户。
> 核心优势：**服务器端零改动**（MCP 通道 xiaozhi 原生支持），电脑桥只需局域网可达。
> 状态：**定稿待审核**。审核通过后按 §6 阶段实施。

## 1. 交互流程（最终形态）

```
用户: "小智小智，启动 automode"
服务器 ASR → LLM 决策 → MCP tools/call {tool: agent.start_task, args:{task:"automode"}}
   ↓ 设备端 mcp_server.cc 回调
设备 UDP → 电脑桥 → claude -p "继续执行 automode" --resume <会话>（后台进程 + 状态文件）
   ↓ 工具返回值（同步）
LLM: "好的，automode 已启动"（设备 TTS 播报）     ← LLM 继续正常服务用户
用户（几小时后）: "进度怎么样了"
LLM → tools/call agent.get_progress → 设备问电脑桥 → "12/300，当前凯尔希，预计 40 分钟"
LLM: 自然语言播报进度
[Claude 全部完成]
电脑桥 → ① 电脑弹窗（用户已有提示窗口实现，电脑端不动）② UDP 完成事件 → 设备
设备 → 通知注入（见 §4）→ LLM 新回合 → "automode 全部完成啦！"（TTS 主动播报）
```

## 2. 为什么这样设计（用户问题：MCP 合适吗？）

- **设备端本地 MCP = xiaozhi 原生机制的正用**：`mcp_server.cc` 的 `AddTool(name, description, properties, callback)` 就是干这个的——LLM 通过服务器↔设备 MCP 通道调设备工具，callback 返回 JSON 即工具结果。**服务器端（无论自建还是公服）都不需要任何改动**，因为 MCP 客户端是 xiaozhi 服务器内置能力
- "调用规则和具体流程"：写在工具 description 里（LLM 会读），回调里做转发——完全按用户说的"在 AddTool 里设定"
- 电脑桥不需要做成 MCP server（省掉公网可达/隧道问题）——它是设备可连的局域网 UDP 服务，比独立 MCP server 简单一个量级

## 3. 三组件设计

### 3.1 设备端（固件，~150 行，用户编译）

在 `mcp_server.cc` AddCommonTools 附近注册 3 个工具：

| 工具 | 参数 | 回调行为 | 返回值（给 LLM） |
|---|---|---|---|
| `agent.start_task` | `task`(string, 如 automode/自由文本) | UDP 发电脑桥"启动" | "已启动（任务 id xxxx）"或错误 |
| `agent.get_progress` | 无 | UDP 问电脑桥状态 | 状态一句话（进度/ETA/完成） |
| `agent.stop_task` | 无 | UDP 发电脑桥"停止" | "已请求停止" |

- 转发通道：UDP（复用 XZ 协议模式：XZAA announce 发现 + XZAR 请求帧 + XZAI 信息帧——电脑桥回应 announce，设备自动发现，无需手动输 IP；协议细节照抄 `audio_forwarder.cc` 的 announce/keepalive/超时模式）
- 完成事件接收：设备监听电脑桥的 XZEV 事件帧（完成/失败）→ §4 注入

### 3.2 电脑 Agent 桥（新 Python 服务，~300 行，放 paperdoll_sim 或独立目录）

- UDP 服务：XZAA/XZAR/XZAI/XZEV 协议 + token 校验
- Claude 调用：`claude -p "<prompt>" --resume <automode会话id>`（subprocess 后台，stdout 尾行 = 结果摘要）
- 状态文件 `automode_status.json`：automode 会话把进度写这里（automode 工作流加一步"落盘进度"）；桥读它答 get_progress
- 完成时：调用用户已有的电脑弹窗 + UDP XZEV 推设备
- 指令白名单：start 只接受"automode"/"进度"/"停止"等白名单词 + 可选自由文本（走独立会话防污染 automode 会话）

### 3.3 服务器端

**零改动**。LLM 看到新工具后自然使用（工具 description 写清"何时用"）。

## 4. 完成通知的注入机制（实施时实验选型，备选三条）

Claude 完成后要触发 LLM 新回合主动播报，按优先级实验：

1. **设备→服务器 listen/detect 注入**：固件已有 `SendWakeWordDetected`（`{"type":"listen","state":"detect","text":...}`，protocol.cc:43）——设备收到完成事件后发 detect 消息（text=完成摘要），看服务器端是否把 text 当用户输入开启 LLM 回合（服务器实现各异，需实测）
2. **服务器端 API 直推**（若自建服务器）：电脑桥直接调服务器 API 给设备推 TTS——最可靠，但要服务器端路由（阶段 1 后用）
3. **兜底（确定性可行）**：设备提示音 + 屏幕显示"电脑任务完成"，用户说话问进度时 LLM 查 get_progress 自然告知

## 5. 既有资产复用清单

- `mcp_server.cc`：AddTool 模式 + MCP JSON-RPC 全链路（已通）
- `audio_forwarder.{h,cc}` + `scripts/audio_forward_probe.py`：UDP announce 发现/keepalive/超时的成熟协议模板
- `protocol.cc:SendWakeWordDetected`：设备主动发消息给服务器的现成入口
- 用户已有电脑弹窗：完成提示复用
- `E:\Project\Realtime_PC`：将来公网阶段 localtunnel 参考

## 6. 实施阶段（审核后按序执行）

1. **电脑 Agent 桥**：UDP 协议 + Claude CLI 封装 + 状态文件 + 弹窗（先在电脑上用探针脚本模拟设备验证）
2. **设备端工具**：mcp_server.cc 加 3 工具 + UDP 转发（用户编译烧录）
3. **联调**：语音启动 automode → 查进度 → 完成播报；完成通知按 §4 三条实验选型
4. **automode 进度落盘**：automode 工作流加"写 automode_status.json"一步
5. **（可选）公网**：localtunnel + token 加固

## 7. 风险

- 局域网内伪造 UDP 指令 → token 校验（阶段 1 就加）
- automode 会话被自由文本污染 → 白名单工具 + 自由文本走独立会话
- 完成通知注入机制依赖服务器实现 → §4 三条兜底，最坏退化为"提示音+下次对话查询"
- MCP 工具调用失败（电脑桥离线）→ 回调返回错误文本，LLM 自然告知用户"电脑不在线"
