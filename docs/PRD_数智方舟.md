
# 数智方舟（Digital Ark）产品定义文档

> 版本：v1.2 | 日期：2026-08-07 | 状态：草案

---

## 一、产品概述

**数智方舟** 是一块搭载 ESP32-P4 芯片的智能通行证设备（480×800 LCD 触摸屏，ES8311 音频 Codec），作为 AI 语音助手的物理载体。用户通过语音或键盘与绑定的智能体（Agent）对话，设备显示角色动态立绘和播放语音回复。

### 硬件规格

| 项目 | 参数 |
|---|---|
| 主控 | ESP32-P4 RISC-V 400MHz |
| 内存 | 32MB PSRAM + 384KB SRAM |
| 屏幕 | 480×800 MIPI-DSI LCD（GT911 触摸） |
| 音频 | ES8311 Codec（I2S，单声道 16kHz/16bit） |
| 网络 | ESP32-C6 WiFi 协处理器（2.4GHz） |
| 存储 | SD 卡（FATFS，16MB Flash） |
| 扩展 | 无蓝牙（P4 和 C6 均不支持 Classic BT） |

### 应用场景

| 场景 | 描述 | 核心需求 |
|---|---|---|
| 🏠 **私人在家使用** | 单人安静环境，深度对话 | 多角色切换、长期记忆、自定义角色/音色、背景音乐 |
| 🎪 **漫展/展会** | 嘈杂环境，多人互动 | 声纹识别防串音、蓝牙耳机防社死、互换智能体 |
| 🚶 **路上使用** | 移动场景，不愿外放 | 文本输入+本地TTS、蓝牙耳机输出 |
| 🔄 **同好互换** | 用户间交换自制智能体 | 智能体增删改查、音色克隆、知识库导入导出 |

---

## 二、完整功能矩阵

| 序号 | 功能 | 场景 | 优先级 | 客户端 | 后端 |
|---|---|---|---|---|---|
| F1 | **画面模式：Cover 动态立绘** | 全部 | P0 | PPA MJPEG 播放 | 无需 |
| F2 | **画面模式：Expression 表情对话** | 全部 | P0 | PPA 色键合成 | 无需 |
| F3 | **画面模式：个性主页（静图+动图）** | 🏠 | P1 | PPA 第4槽独立播放 | 无需 |
| F4 | **语音记录** | 🏠 | P1 | 双层下拉框+WAV 播放 | 无需 |
| F5 | **背景音乐** | 🏠🎪 | P1 | 单层下拉框+WAV 播放 | 无需 |
| F6 | **角色切换（立绘+Agent）** | 🏠🎪 | P0 | 罗德岛索引页 | Agent 管理 API |
| F7 | **文本输入→音频输出（键盘+TTS）** | 🚶 | P0 | 26/9 键键盘+ESP-TTS | 无需 |
| F8 | **声纹识别（可选开关+多声纹）** | 🎪 | P1 | 设备端开关+声纹匹配 | 声纹注册/验证 API |
| F9 | **音频输出到蓝牙耳机** | 🚶🎪 | P2 | 外挂蓝牙模块 | 无需 |
| F10 | **音色克隆（零样本）** | 🏠🎪 | P1 | 小程序录音上传 | 声纹克隆 API |
| F11 | **长期记忆** | 🏠 | P1 | 无需 | 记忆存储 API |
| F12 | **知识库** | 🏠🎪 | P1 | 小程序编辑 | 知识库 CRUD API |
| F13 | **OTA 固件更新** | 全部 | P0 | 设备端 OTA 流程 | OTA 服务端 |
| F14 | **智能体增删改查** | 🔄 | P1 | 小程序管理 | Agent CRUD API |
| F15 | **MCP 拍照** | 🏠 | P2 | 设备端 MCP | 无需 |
| F16 | **DIY 角色创建（Live2D/表情迁移）** | 🔄 | P1 | 生成工具+P4 渲染 | 无需 |
| F17 | **背景场景切换（+背景音乐联动）** | 🏠🎪 | P1 | 设备端滑动+下拉框 | 无需 |
| F18 | **NFC 交友名片** | 🎪 | P2 | NFC 模块 | 小程序 |

---

## 三、功能分项详述

### F1：画面模式——Cover 动态立绘

#### 场景
设备空闲时展示角色的全屏动态立绘，类似手机待机画面。

#### 客户端操作流程
1. 设备启动 → SD 卡读取当前 Agent 的 cover MJPEG
2. PPA 硬件引擎以 **30fps** 循环播放 cover 帧（480×800，JPG 序列）
3. 右侧按钮列显示：「隐藏/显示」、「罗德岛」、「对话模式」、「语音记录」、「背景音乐」
4. 用户点击 **「对话模式」** 按钮 → 切换到 Expression 模式
5. 用户点击 **「罗德岛」** → 打开角色索引页切换角色
6. 用户点击 **「隐藏/显示」** → 切换 cover 文字文本框可见性

#### 实现
- PPA (Pixel Processing Accelerator) 硬件引擎，色键抠图模式关闭，直接解码 JPG 帧
- 双槽缓存 + 预加载：切换角色时先新后旧，避免鬼图
- **一键制作脚本**：`converter/jpg2mjpeg.py`（JPG 序列 → MJPEG，自动缩放至 480×800）
- SD 卡路径：`/sdcard/main/operator/{职业}/{星级}/{英文名}/cover/{英文名}_cover.mjpeg`

---

### F2：画面模式——Expression 表情对话

#### 场景
设备唤醒后进入对话模式，展示角色动态表情配合对话情绪。

#### 客户端操作流程
1. 用户说出唤醒词（如「凯尔希」）→ 设备切换到 Expression 模式
2. 背景加载静态 JPG 背景图
3. 前层 PPA 抠图合成：角色 PNG 表情逐帧播放（~120 帧 MJPEG）
4. 色键抠除纯色背景（R=[200,255], G=[0,80], B=[0,80]），合成到背景上
5. 服务器发送 LLM 情绪指令 → 设备切换对应情绪表情（happy/sad/angry/...）
6. 对话结束 → 自动切回 Cover 模式（从缓存秒切）

#### 实现
- PPA BLEND 引擎：背景 RGB565 + 前景 RGB565 + RLE Mask → 合成输出
- RLE Mask 格式：`[4B frame_count][N×4B offsets][RLE: 2B count, 1B value...]`
- 三槽缓存：active / pending / cover（独立无竞争）
- LLM 情绪抢占：服务端下发 emotion 指令 → 立即切换对应表情（不等循环结束）
- **一键制作脚本**：`converter/png2emoji.py`（PNG RGBA 序列 → MJPEG + .mask）
- SD 卡路径：`/sdcard/main/operator/{职业}/{星级}/{英文名}/emoji/{情感名}.mjpeg` + `.mask`

---

### F3：画面模式——个性主页（静图+动图）

#### 场景
用户展示自定义个人主页，可包含自拍、OC 立绘、视频等。项目社区昵称「蟑螂派对」。

#### 客户端操作流程
1. 用户在 Cover 或 Expression 模式点击 **「蟑螂派对！」** 按钮
2. 设备暂停视频播放 → 检查 SD 卡：
   - `Profile.raw` 存在 → 直接显示静态图（RGB565 raw，**秒显**）
   - 否则 `Profile.jpg` → 解码显示（约 2 秒）
3. 静态图全屏显示后，右上角出现 **「动图」** 按钮（半透明灰色，竖排文字）
4. 用户点击 **「动图」** → 设备在 PPA 第 4 槽加载 `Profile.mjpeg` → 切换播放源
5. 点击屏幕任意处 → 退出个人主页，恢复之前画面模式

#### 实现
- PPA 第 4 槽（profile 槽）：**完全不 swap**，与 active/pending/cover 独立
- `ppa_use_profile_cache(bool)` 切换播放源，零数据搬运
- static image 优先 raw 文件：480×800 RBG565，768KB，直接写 LVGL canvas
- 后台任务版本号防竞态（gen counter 替代 vTaskDelete）
- JPG 缩略图 DMA 内存手动释放防泄露
- **一键制作脚本**：
  - `converter/img2jpg.py`（图片 → Profile.jpg + Profile.raw）
  - `converter/video2mjpeg.py`（视频 → Profile.mjpeg）
- SD 卡路径：`/sdcard/User/Ur_Info/Profile.jpg` / `.raw` / `.mjpeg`

---

### F4：语音记录

#### 场景
用户收听角色的全部语音台词（类似游戏内语音鉴赏功能）。

#### 客户端操作流程
1. 用户在 Cover 或 Expression 模式点击 **「语音记录」** 按钮
2. 半屏遮罩弹出，顶部标题「语音记录」，下方 **分类下拉框**（日常 / 作战中 / 晋升）
3. 用户选择分类 → 第二下拉框出现，列出该分类下所有语音条目（**中文名**，如「交谈1」「部署2」）
4. 用户点击条目 → 设备从 SD 卡播放对应 WAV 文件（16kHz mono 16-bit PCM）
5. 同时屏幕底部 **显示语音文本**（从 `text.yaml` 读取）
6. 播放中再次点击其他条目 → 干净中断旧播放，启动新播放
7. 播放完毕自动停止，封面立绘恢复播放

#### 实现
- 中文名→英文文件名映射表（38 条目硬编码）：`{ "交谈1" → "conver1", "部署1" → "alloc1", ... }`
- WAV 播放器：标准 44 字节头解析 → 扫描 chunk 找到 `data` → 立体声折叠单声道 → `codec->OutputData()`
- WAV 文件要求：16kHz mono 16-bit PCM（通过 `wav_converter.py` 转换）
- 取消标志安全退出（避免 vTaskDelete 遗留资源）
- 文本关联：`kb_select_candidate` 触发 `voice_text_update()`，从 SD 卡读 `text.yaml` 查找文本
- **语音文本格式**：`{英文key} : {中文文本}`（一行一条）
- **一键导入脚本**：`converter/import_voice.py --page "角色名/语音记录" --dest <voice目录>`
  - 自动调用 PRTS wiki API 下载语音 → 中文→英文重命名 → 分类到 daily/fight/promotion → 16kHz 转换 → 生成 text.yaml
- SD 卡路径：
  ```
  /sdcard/main/operator/{职业}/{星级}/{英文名}/voice/
    daily/    {conver1.wav, birthday.wav, ...}
    fight/    {alloc1.wav, combating1.wav, ...}
    promotion/{elitepm1.wav, pmconver1.wav, ...}
    text.yaml
  ```

---

### F5：背景音乐

#### 场景
用户播放游戏原声背景音乐，作为对话背景或单独欣赏。

#### 客户端操作流程
1. 用户在 Cover 或 Expression 模式点击 **「背景音乐」** 按钮
2. 半屏遮罩弹出，顶部标题「背景音乐」，下方 **音乐列表下拉框**（中文名，如「覆尘-Operation_Ashring」）
3. 音乐列表从 SD 卡 `backgroundmusic.yaml` 读取：`{中文显示名} : {文件名.wav}`
4. 用户点击条目 → 设备播放对应 WAV 文件（16kHz mono）
5. 播放中再次选择 → 干净中断旧播放，启动新播放
6. 点击遮罩空白处 → 取消，关闭列表，恢复之前画面

#### 实现
- 音乐路径：`/sdcard/main/music/{文件名}.wav`
- 独立音乐播放任务（music_play_task），与语音播放互斥（一方播放时另一方被取消）
- 音乐不暂停视频播放（后台播放）
- **一键导入脚本**：`converter/import_music.py`
- **一键转换脚本**：`converter/music_converter.py`（24k/48k → 16kHz）
- SD 卡路径：
  ```
  /sdcard/main/music/
    Under_Dust!Operation_Ashring.wav
    Star_Trail!Ad_Astra.wav
    ...
    backgroundmusic.yaml
  ```

---

### F6：角色切换（立绘 + 智能体）

#### 场景
用户有多个偏好角色，希望在不同角色之间切换对话。

#### 客户端操作流程
1. 在 Cover 模式点击右侧 **「罗德岛」** 按钮
2. 进入全屏角色索引页：
   - 顶部两个下拉框：**职业**（先锋/近卫/重装/狙击/术师/医疗/辅助/特种）和 **稀有度**（6★~1★）
   - 选择筛选条件后，卡片网格自动刷新
   - 每页 12 张卡片（4 列×3 行），支持左右滑动手势翻页
3. 点击角色卡片 → 「加载中」动画 → PSRAM 载入角色 cover MJPEG
4. 立绘加载完成后，自动切换到该角色的 Cover 模式
5. 用户说出唤醒词（如「凯尔希」）→ 设备切换到该角色的 Expression 模式 → 开始对话
6. 对话中设备的 AgentID 自动切换，服务器使用该角色的音色和知识库

#### 角色创建流程（完整导入流水线）

**前置准备：**
- Cover 动图：PR 合成输出，`{英文名}XXX.jpg`（XXX=000~123，480×800）
- 表情图：PNG RGBA 序列，按情感分文件夹（neutral/happy/angry...22 种情感）
- 语音文件：从 PRTS wiki 获取（或手动下载）
- 缩略图：108×228 JPG（任意比例自动缩放白底居中）

**一键脚本：**
1. `python jpg2mjpeg.py {JPG文件夹} {英文名}_cover` → 生成 cover MJPEG
2. `python png2emoji.py {表情根目录} --dest {SD卡emoji目录}` → 生成 22 组 .mjpeg + .mask
3. `python import_voice.py --page "角色名/语音记录" --dest {voice目录}` → 下载+重命名+分类+16kHz+text.yaml
4. `python make_thumbnail.py {原图} {英文名}` → 生成 108×228 INDEX 缩略图

**固件注册（一次性）：** 修改 2 个文件：
- `custom_wake_word.cc`：注册唤醒词拼音 + 显示名
- `application.cc`：映射 唤醒词→AgentID+SD路径 和 AgentID→cover路径

SD 卡最终结构：
```
/sdcard/main/operator/{职业}/{星级}/{英文名}/
  cover/{英文名}_cover.mjpeg
  emoji/{情感}.mjpeg + {情感}.mask
  voice/daily/ fight/ promotion/ text.yaml
/sdcard/main/operator/INDEX/{职业}_108x228/{星级}/{英文名}.jpg
```

#### API（后端提供）

**搜索智能体（全量）**：
```
GET /v1/agents?user_id={USER_ID}
Authorization: Bearer {USER_TOKEN}
```
返回：
```json
{
  "agents": [
    {
      "agent_id": "272076f9d2d34503b350220d59f82a60",
      "name": "凯尔希",
      "voice_id": "voice_kaltsit_001",
      "knowledge_base_ids": ["kb_001", "kb_002"]
    }
  ]
}
```

**切换智能体**（设备端携带 AgentID 打开音频通道）：
```
WebSocket Header: X-Agent-ID: {agent_id}
```

---

### F7：文本输入→音频输出（键盘+TTS）

#### 场景
用户在公共场合不便开口说话，通过键盘输入文字，设备本地合成语音并发送。

#### 客户端操作流程

1. 用户在 Expression 模式点击右侧按钮 **「弹出键盘」**
2. 底部半屏（480×420，y=380）弹出键盘覆盖层，**上半屏保留表情动画**
3. 顶部 **预览栏**（460×36）显示当前输入内容，同时实时同步到 Dr.XM 输入框
4. 预览栏下方 **候选栏**（6 按钮，73×38）显示匹配的汉字（5 个/页 + `>` 翻页）
5. 键盘区域：
   - **26 键**（默认）：QWERTY 布局，43×39 按键，直接输入字母
   - **9 键**（切换）：多击法，同键 **0.7 秒**内连按循环字母（含数字键 1-9）
6. 用户输入拼音 → 候选栏实时更新 → 点击候选字替换拼音
7. 连续输入：中文边界自动识别分词，无需手动空格
8. 功能键：**删除**（完整删 UTF-8 字符，3 字节中文字符一次删掉）、**清空**、**空格/0**、**隐藏**
9. 布局：Row 2 末尾为 `[Send]`，Row 4 为 `[9Key] [Hide] [Space] [Clear]`
10. 点击 **「Send」**：
    - 设备将输入文字复制到堆 → 关闭键盘（恢复表情动画）
    - 启动 TTS 任务（栈 40KB）
    - 清空音频编码队列（`ClearSendQueue`）→ ESP-TTS 合成 PCM（16kHz mono 16-bit）
    - 缓冲至 **960-sample 帧**（对齐 Opus 编码器帧大小）
    - 推入 `PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, pcm)`
    - WebSocket/MQTT 发送至服务器 → 服务器 LLM 处理 → 返回音频 → 设备扬声器播放

> ⚠️ **已知缺陷**：当前方案通过本地 ESP-TTS 将文本转为音频，再走音频通道发送——服务器收到的是**合成音频**而非原始文本。因合成音质为机械拼接音，服务器的语音识别（STT）可能无法准确识别，导致「发送了但智能体没反应」。
>
> **期望改进**：请后端提供 **文本输入接口**（如 `POST /v1/chat/text`），设备可直接发送文本字符串，跳过本地 TTS→STT 的迂回。API 格式建议：
> ```json
> POST /v1/chat/text
> Authorization: Bearer {USER_TOKEN}
> { "agent_id": "...", "user_id": "...", "text": "你好凯尔希" }
> ```
> 服务器收到文本后走 LLM → TTS → 返回音频。在此之前，保留本地 TTS 方案作为过渡。

#### 实现
- 拼音→汉字映射表：394 音节 3035 字（93KB C header，二分搜索查找），生成脚本 `gen_pinyin_table.py`
- ESP-TTS：flash 分区加载语音数据（`voice` 分区，3MB，避免 P4 PSRAM XIP 崩溃）
  - 语音数据文件：`esp_tts_voice_data_xiaole.dat`（从 ESP-SR GitHub 下载）
  - 烧录命令：`esptool write_flash 0xD00000 voice_data_xiaole.dat`
- TTS 任务栈 40KB，编码帧对齐 960-sample，发送前清空麦克风遗留队列
- 输入 buffer 从 PSRAM 分配 320 字节（避免 BSS 栈溢出覆盖）
- **无需新 API**（当前方案），期望后端提供文本接口后移除 TTS 依赖

---

### F8：声纹识别（可选开关+多声纹）

#### 场景
漫展等嘈杂环境，需要识别当前说话者是否为已授权用户。支持**多声纹**——一台设备中的一个智能体，能够与所有已录入声纹的人交互。比如，用户 A 拥有设备，他在小程序中录入了自己、朋友 B、朋友 C 的声纹，三人都可以对设备说话，设备根据声纹识别出是谁在说话，但都由**同一个智能体**（如凯尔希）回复。未录入声纹的路人 D 说话则被忽略。

#### 客户端操作流程

**注册阶段（小程序）：**
1. 用户在微信小程序 →「声纹管理」→ 点击 **「添加声纹」**
2. 为当前声纹命名（如「我自己」「小明」「小红」）
3. 点击「录制」→ 朗读 20-40 秒文本（小程序显示引导文字，确保高质量录音：采样率≥16kHz、环境安静、音量适中）
4. 录制完成 → 上传至服务器 → 返回 `role_id`
5. 重复步骤 1-4 添加多个人的声纹

**设备端开关：**
1. 在 Expression 模式，右侧按钮列增加 **「声纹开关」** 按钮（默认关闭，灰色半透明）
2. 用户点击 → 按钮高亮（蓝色），文字变为「声纹：开」
3. 设备请求服务器获取已注册声纹列表（`GET /v1/voiceprint/voices?user_id=xxx&device_id=xxx`）定义的API方法在后续有写。
4. 开启后，用户说话时：设备将**音频直接传给服务器**，由服务器端实时比对声纹（设备端不做本地预处理）：
   - 服务器匹配成功 → 接受输入，标注匹配到的 `role_id`，智能体正常回复
   - 服务器匹配失败 → **忽略输入**，屏幕短暂提示「未识别声纹」
5. 关闭声纹：点击按钮 → 恢复为任何人均可对话（不做声纹过滤）

**漫展多声纹场景：**
- 用户 A 提前在小程序注册自己 + B、C 共 3 人的声纹（均归属于同一 `user_id`）
- 设备开启声纹 → A、B、C 轮流说话均被识别，由**同一智能体（凯尔希）**回复
- 路人 D 说话 → 服务器返回 `match: false` → 设备忽略（防串音）

#### 声纹 API（后端提供，参考 https://linx.qiniu.com/docs/xrobot/api/voiceprint-v2）

**注册声纹：**
```
POST https://{host}/v1/voiceprint/voices/create
Authorization: Bearer {USER_TOKEN}
Content-Type: application/json

{
  "user_id": "{USER_ID}",
  "device_id": "{DEVICE_UUID}",
  "role_id": "role_alice_main",
  "threshold": 0.5,
  "audio_url": "https://storage.example.com/alice_voice.wav"
}
```
| 参数 | 类型 | 说明 |
|---|---|---|
| `user_id` | string | 用户唯一标识（必填） |
| `device_id` | string | 设备 UUID（必填） |
| `role_id` | string | 声纹标签，用于区分不同人（必填） |
| `threshold` | float | 匹配阈值 0-1，越低越宽松（默认 0.5） |
| `audio_url` | string | 注册音频的云端地址（20-40 秒，16kHz+，安静环境） |

响应：
```json
{ "success": true, "role_id": "role_alice_main" }
```

**获取已注册声纹列表**（设备端开关开启时调用）：
```
GET https://{host}/v1/voiceprint/voices?user_id={USER_ID}&device_id={DEVICE_UUID}
Authorization: Bearer {USER_TOKEN}
```
响应：
```json
{
  "voices": [
    { "role_id": "role_alice_main", "name": "我自己" },
    { "role_id": "role_bob_friend", "name": "小明" },
    { "role_id": "role_carol_friend", "name": "小红" }
  ]
}
```

**实时声纹验证**（设备端发送完整音频，服务器端比对，**多声纹匹配**——遍历用户所有已注册声纹，返回置信度最高的匹配）：
```
POST https://{host}/v1/voiceprint/voices/verify
Authorization: Bearer {USER_TOKEN}
Content-Type: application/json

{
  "user_id": "{USER_ID}",
  "device_id": "{DEVICE_UUID}",
  "audio_url": "https://storage.example.com/live_audio.wav"
}
```
响应：
```json
{
  "match": true,
  "matched_role_id": "role_alice_main",
  "confidence": 0.92
}
```
若 `match: false`，设备忽略此次输入。设备端可设置最低置信度阈值（默认 0.5）。

---

### F9：音频输出到蓝牙耳机

#### 场景
用户在公共场合（地铁、漫展）不希望扬声器外放，希望 LLM 的语音回复通过蓝牙耳机播放。

#### 当前限制
- ESP32-P4：无 Classic Bluetooth（仅 RISC-V 核心，不带 BT 射频）
- ESP32-C6（WiFi 协处理器）：有 BLE 5.3，但**无 Classic Bluetooth**，不支持 A2DP 音频协议
- 结论：**无法通过纯软件实现蓝牙音频输出**

#### 解决方案
**外挂蓝牙音频模块**，通过 I2S 接入音频链路：

```
ESP32-P4  →  I2S1  →  CSR8675  →  ━━ 蓝牙耳机
                ↘  I2S0  →  ES8311  →  内置扬声器
```

P4 拥有 2 个 I2S 控制器，I2S0 已被 ES8311 占用，I2S1 空闲。

| 模块 | 接口 | 价格 | 协议 | 音质 |
|---|---|---|---|---|
| **CSR8675 / QCC3034**（推荐） | I2S | ¥15-40 | A2DP + aptX/aptX-HD | 高（接近有线） |
| JDY-68 | UART PCM | ¥5-8 | A2DP + SBC | 低（语音可接受） |
| JDY-67 | UART 透传 | ¥3-5 | SPP（不是 A2DP） | 极低（仅数据） |

**推荐 CSR8675**：I2S 接口，支持 aptX-HD 高音质编码，价廉。P4 固件侧 `codec->OutputData()` 同时写入两个 I2S 通道即可实现双输出（扬声器 + 蓝牙）。

#### 客户端操作流程
1. 设备上电后，CSR8675 上电并进入**配对模式**（首次需手动触发，后续自动重连）
2. 用户在蓝牙耳机端搜索并配对（设备蓝牙名可自定义，如 `Xiaozhi-ARK`）
3. 配对成功后，音频同时输出到：**内置扬声器** + **蓝牙耳机**
4. 未来可通过屏幕设置界面切换输出模式（仅扬声器 / 仅蓝牙 / 同时）
5. 小程序端可管理已配对设备列表

**暂缓实施**，待硬件验证后确定最终方案。优先级：P2（等需求紧迫时启动）。

---

### F10：音色克隆（零样本）

#### 场景
用户希望为自己的 OC（自设角色）或导入的 IP 角色创建专属声音。**零样本**——只需一段音频即可克隆，无需大量训练数据。

#### 技术方案
后端使用 **3D-Speaker** 声纹识别模型进行声纹克隆。灵犀平台已内置此功能，需配置 API Key。参考项目：[xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)（已内置克隆功能）。

> 老师说："克隆的我也确认看看默认是有的，应该需要配置下 API Key。"

#### 客户端操作流程

**小程序端：**
1. 用户打开小程序 → 底部 Tab「音色克隆」→ 进入克隆页面
2. 页面显示 **录音引导**：
   - 提示文字：「请朗读以下文本，保持语速平稳，环境安静」
   - 显示一段 50-100 字的中文引导文本（覆盖常用音节）
3. 点击 **「开始录音」** → 系统请求麦克风权限
4. 录音中：实时显示波形和时长（需 ≥20 秒），用户可点击 **「停止」** 提前结束
5. 录制完成后：
   - 前端先进行**本地质量评估**（检测音量、底噪、时长）→ 不通过则提示重录
   - 通过后点击 **「上传」** → 音频上传至服务器
6. 服务器处理后返回结果：
   - ✅ 成功 → 显示「克隆成功！」，返回 `voice_id`（如 `voice_custom_abc123`），用户可试听样本
   - ⚠️ 质量不足 → 提示「请重新录制，建议在安静环境下朗读」（显示具体原因：如「音量过低」「背景噪音过大」）
7. 克隆成功后，用户在**创建智能体**或**编辑智能体**时，在音色列表中选择「我的音色」→ 选择已克隆的音色

**高质量录音要求（参考灵犀文档）：**
- 时长 ≥ 20 秒，建议 30-40 秒
- 采样率 ≥ 16kHz（小程序默认 44.1kHz）
- 无明显背景噪音（SNR ≥ 20dB）
- 音量适中，无爆音
- 单人朗读，无其他人声

#### API（后端提供）

```
POST https://{host}/v1/voiceprint/voices/create
Authorization: Bearer {USER_TOKEN}
Content-Type: application/json

{
  "user_id": "{USER_ID}",
  "device_id": "{DEVICE_UUID}",
  "role_id": "role_custom_001",
  "threshold": 0.5,
  "audio_url": "https://storage.example.com/user_voice.wav"
}
```

| 参数 | 类型 | 说明 |
|---|---|---|
| `user_id` | string | 用户唯一标识（必填） |
| `device_id` | string | 设备 UUID（必填） |
| `role_id` | string | 自定义声纹标签（必填，用于后续 Agent 绑定） |
| `threshold` | float | 匹配阈值（默认 0.5） |
| `audio_url` | string | 用户录制音频的云端地址（20-40 秒，16kHz+） |

响应：
```json
{
  "success": true,
  "voice_id": "voice_custom_abc123",
  "message": "声纹克隆成功",
  "sample_url": "https://storage.example.com/sample_abc123.wav"
}
```

**错误响应（质量不达标）：**
```json
{
  "success": false,
  "error": "AUDIO_QUALITY_LOW",
  "message": "音频质量不达标：背景噪音过高（SNR=12dB）",
  "suggestion": "请在安静环境下重新录制"
}
```

> 注：灵犀平台目前内置的克隆服务可能已不可用（第三方平台 `linkerai.cn` 已挂）。可切换至**火山引擎**或其他第三方 TTS 克隆服务。需后端老师确认并配置对应的 API Key。

---

### F11：长期记忆

#### 场景
智能体能记住用户的偏好、习惯、之前对话中的重要信息，实现**跨会话连续性对话**。例如，用户上周说过「喜欢喝热牛奶」，这周对话中智能体可以主动提及。

#### 实现方案
后端使用 **Chroma** 向量数据库存储对话记忆。每次对话结束后，后端自动提取关键信息存入向量库。下次对话时，根据上下文检索相关记忆注入 LLM prompt。

> 注意：灵犀官方提供的记忆条目**限制为 20 条**。对于深度个性化场景，建议提高存储上限。

#### 客户端操作流程
无需客户端额外操作。对话过程中后端自动记录：

1. **存储**：每次对话结束后，后端 LLM 提取对话中的关键信息（偏好、事实、用户状态），存入 Chroma
2. **检索**：下次对话开始时，根据当前对话上下文在 Chroma 中检索 top_k 条最相关记忆
3. **注入**：检索到的记忆文本拼入 LLM system prompt，使智能体「记得」之前的互动

**小程序管理（可选）**：用户可在小程序中查看/删除已存储的记忆条目。

#### API（后端提供）

**存储记忆**（后端自动调用，设备无需感知）：
```
POST https://{host}/v1/memories
Authorization: Bearer {USER_TOKEN}
Content-Type: application/json

{
  "user_id": "{USER_ID}",
  "agent_id": "{AGENT_ID}",
  "content": "用户喜欢喝热牛奶，不喜欢咖啡",
  "category": "偏好",
  "timestamp": "2026-08-07T12:00:00Z"
}
```
| 参数 | 类型 | 说明 |
|---|---|---|
| `user_id` | string | 用户唯一标识 |
| `agent_id` | string | 智能体 ID（记忆按智能体隔离） |
| `content` | string | 记忆内容（自然语言） |
| `category` | string | 分类标签（偏好/事实/状态/其他） |
| `timestamp` | string | ISO 8601 时间戳 |

**检索记忆**（对话开始时自动调用）：
```
GET https://{host}/v1/memories?user_id={USER_ID}&agent_id={AGENT_ID}&query={当前对话上下文}&top_k=5
Authorization: Bearer {USER_TOKEN}
```
响应：
```json
{ "memories": [ { "content": "...", "score": 0.89 }, ... ] }
```

**管理记忆条目**（小程序端）：
```
POST https://{host}/v1/memories/{memory_id}/attentions
Authorization: Bearer {USER_TOKEN}
Content-Type: application/json

{
  "name": "用户饮食偏好",
  "description": "用户喜欢和不喜欢的食物类型，包括口味偏好、过敏信息、饮食习惯等",
  "defaultValue": "暂无记录"
}
```
支持 `GET`（查询）、`PUT`（修改）、`DELETE`（删除）。

---

### F12：知识库

#### 场景
智能体基于特定世界观回复。用户可创建/编辑/绑定知识库。知识库以 RAG 方式注入 LLM prompt。

#### 客户端操作流程
1. 小程序 →「知识库」→ 点击 **「创建知识库」**
   - 输入名称（如「泰拉世界知识库」）、描述
   - 设置检索参数：`top_k`（3-8）、`score_threshold`（0-1）
2. 点击知识库 → **上传文档**（TXT/MD/PDF，≤10MB），支持批量上传
3. 系统自动分段→向量化→存入向量数据库
4. 创建/编辑智能体时，选择绑定知识库（**可多选**）
5. 对话时后端自动检索→注入 prompt

#### API（后端提供）

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/v1/datasets?user_id={}` | 创建知识库 |
| `POST` | `/v1/datasets/{id}/documents?user_id={}` | 上传文档（multipart） |
| `PUT` | `/v1/agents/{id}?user_id={}` | 绑定知识库到智能体 |

创建请求体：`{ "name": "...", "desc": "...", "retrieval_model": { "top_k": 5, "score_threshold": 0.5 } }`

参考：[知识库使用最佳实践指南](https://linkerai.cn/docs/knowledge-base-guide)

---

### F13：OTA 固件更新

#### 场景
固件发布新版本后，用户通过 OTA 远程更新。

#### 流程
1. 设备上电→POST OTA URL（`board_type`, `MAC`, `UUID`, 版本号）
2. 服务器比对→无更新/有新版本/新设备激活
3. 有更新→下载 `.bin`→写入 `ota_1`→重启→生效（失败自动回滚 `ota_0`）
4. 新设备→返回 `activation.code`（6 位码），屏幕显示

**请求**：
```json
POST http://{host}/xiaozhi/ota/
{ "application": {"version":"1.8.2"}, "board": {"type":"guition-jc4880p443","name":"guition-jc4880p443"},
  "mac_address":"9c:13:9e:dc:0d:6c", "uuid":"524c4c23-4664-4d67-a772-ea81a875ac44",
  "chip_model_name":"esp32p4", "flash_size":16777216 }
```

**响应（有更新）**：`{ "firmware": {"version":"1.9.0","url":"https://..."}, "mqtt": {...}, "websocket": {...} }`
**响应（激活）**：`{ "activation": {"code":"A1B2C3","message":"请在智控台输入验证码"} }`

设备端配置：`CONFIG_OTA_URL="http://47.92.32.95:19224/xiaozhi/ota/"`。固件托管任意公网 HTTP 地址。`firmware.url` 由服务器返回，更换服务器即可切换来源。

---

### F14：智能体增删改查

#### 说明
小程序管理用户的智能体列表。设备端通过 **WebSocket Header `X-Agent-ID`** 切换智能体（不直接调用 CRUD API）。参考：[Agent API](https://linx.qiniu.com/docs/xrobot/api/agent)

#### API（后端提供）

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/v1/agents?user_id={}` | 全量列表（含 name, agent_id, voice_id, knowledge_base_ids） |
| `POST` | `/v1/agents?user_id={}` | 创建（body: name, voice_id, kb_ids, llm_model, greeting） |
| `PUT` | `/v1/agents/{id}?user_id={}` | 编辑 |
| `DELETE` | `/v1/agents/{id}?user_id={}` | 删除 |

**创建请求示例**：
```json
{
  "name": "凯尔希",
  "voice_id": "voice_kaltsit_001",
  "knowledge_base_ids": ["kb_001"],
  "llm_model": "default",
  "greeting_message": "博士，我出现在这里，说明局势不容乐观。"
}
```

**全量列表响应**：
```json
{ "agents": [{ "agent_id": "...", "name": "凯尔希", "voice_id": "...", "knowledge_base_ids": [...], "created_at": "..." }] }
```

---

### F15：MCP 拍照
同 xiaozhi 原版，设备端 MCP 调用，无需改动。

---

### F16：DIY 角色创建（静态图 → 动态表情）

#### 场景
用户想用自己的 OC（自设角色）或导入 IP 角色的单张立绘，自动生成 Cover 动态立绘和 22 组表情动画。**无需组员手工拆分部件**。

#### 方案 A：Live2D 路径（推荐，前提：P4 能带动）

**原理**：将一张静态图的各部件（眉毛、眼睛、嘴巴、头发、身体）自动拆分，通过绑定骨骼和参数曲线驱动部件移动/旋转/变形，生成动态表情。

**自动化拆分**：
- GitHub 已有项目（如 `live2d-crop-utils`、`Skeleton-based-Animation-Transfer`）可从单张立绘自动识别和裁剪部件
- 输入：1 张角色正面立绘（480×800 PNG RGBA）
- 输出：头发层、眉毛层、眼睛层、嘴巴层、身体层等（每层 PNG RGBA，带透明通道）
- 拆分精度取决于原图质量，目前开源项目对"精细拆分"（发丝、睫毛）的支持有限

**表情轨迹**：
- 22 组表情共用同一套部件，仅改变参数（如「happy」= 眉毛上挑 + 嘴角上扬 + 眼睛微眯）
- 轨迹文件为 JSON 格式：`{ "happy": [ { "part": "eyebrow_L", "rotate": 15, "move_y": -5 } ... ] }`
- 轨迹模板**一套通用**，所有角色共用——只需替换部件图片

**P4 渲染性能评估**：

| 方案 | 层数 | 每帧操作 | 预估帧率 |
|---|---|---|---|
| 当前 Expression | 1 层（角色）+ 1 层（背景） | PPA BLEND 1 次 | **30fps** ✅ |
| Live2D（保守） | 5 层（头发+眉毛+眼+嘴+身体） | PPA BLEND 4 次（逐层合成） | **25fps** ✅ |
| Live2D（复杂） | 10 层 | PPA BLEND 9 次 | **15-18fps** ⚠️ |

PPA 硬件 BLEND 引擎 480×800×16bit ≈ 768KB/帧，单次合成为硬件加速（~0.5ms）。5 层合成 ≈ 2-3ms，完全可在 33ms（30fps）内完成。**P4 能带动 5-8 层 Live2D**。

**可行性**：✅ 推荐。P4 硬件足以支撑 5-8 层 Live2D 渲染。22 组表情轨迹模板可一套复用。

**风险**：自动化部件拆分的精度（开源项目对复杂立绘的发丝/配件拆分效果待验证）。**人为拆分太耗时**，优先推进自动化方案。

---

#### 方案 B：表情迁移（备选，如果 Live2D 渲染层数不够）

**原理**：制作一套**通用面部表情动画模板**（22 组，仅面部区域），用 AI 表情迁移模型将动作映射到目标角色脸上。

**流程**：
1. 预制作：22 组面部表情动画（通用模板，如「标准女性脸型」的 happy/sad/angry 等）
2. 用户上传 1 张角色正面照 → 服务器运行表情迁移模型 → 输出 22 组 MJPEG
3. 设备端直接播放（与当前 Expression 模式完全一致）

**优点**：与现有架构零改动，设备端无需额外渲染能力。

**缺点**：需服务器 GPU（推理成本），网络依赖，不够"本地化"。

**推荐**：先尝试方案 A（Live2D），如部件拆分效果不佳或层数超限，再回退方案 B。

---

### F17：背景场景切换（+背景音乐联动）

#### 场景
游戏提供多种场景背景（如罗德岛办公室、切尔诺伯格废墟、多索雷斯海滩），用户可切换不同背景，每个场景默认绑定一首背景音乐。场景图片为横屏（如 1920×800），设备竖屏显示时左右滑动浏览。

#### 客户端操作流程
1. 在 Cover 或 Expression 模式，新增 **「场景」** 按钮
2. 点击 → 下拉框列出所有场景（中文名，如「罗德岛办公室」「切尔诺伯格废墟」）
3. **场景与音乐绑定**：选择场景后，自动播放该场景默认的背景音乐（可手动切换音乐）
4. 场景图片为横屏，设备竖屏显示时：
   - 方案：将横屏图片**直接载入**，用户手指**左右滑动**平移视口，每帧显示 480×800 的裁剪窗口
   - 支持惯性滑动和边界回弹
5. 场景配置存储在 `background_scenes.yaml`：
   ```yaml
   罗德岛办公室 : Rhides_Office
   切尔诺伯格废墟 : Chernobog_Ruins
   ```
   每行格式：`{中文显示名} : {文件夹名}`
6. 场景图片存储：`/sdcard/main/background/{文件夹名}/` 下放置横屏图片（如 `Rhides_Office.jpg`，1920×800）

#### 实现
- LVGL 图片控件 + 触摸手势（LV_EVENT_GESTURE）
- 横屏图片直接载入 LVGL canvas，用户滑动改变 x 偏移量
- 每屏 480×800 视口显示大图的局部区域
- 场景切换时自动查找同文件夹名的 `backgroundmusic.yaml` 条目，播放默认音乐
- **无需后端 API**

#### 与背景音乐的区别
- 背景音乐：音频播放，下拉框选择
- 场景切换：图片显示 + 滑动浏览，下拉框选择，**默认绑定背景音乐**，可单独切换

---

### F18：NFC 交友名片

#### 场景
漫展或聚会时，用户打开自己的个性主页（Profile），用设备 NFC 碰一下好友手机，好友手机自动弹出指定游戏页面（如明日方舟好友页面、原神 UID 页）。

#### 客户端操作流程
1. 用户在个性主页（Profile）显示状态下，设备 NFC 模块处于待机模式
2. 好友手机（支持 NFC）靠近设备 NFC 区域
3. 设备通过 NFC 发送预设 URL（如 `https://ak.hypergryph.com/user/{user_game_id}`）
4. 好友手机收到 NFC 数据 → 自动打开浏览器跳转到对应页面

#### 预研
ESP32-P4 官方 SDK 不支持 NFC 主机模式。需外挂 NFC 模块（如 **PN532**，I2C/UART 接口），成本约 ¥10-15。设备端通过 I2C 写入 NDEF 记录（URL 格式），手机上电后 NFC 模块自动广播。

**暂缓实施**，待个性主页功能稳定后再评估 NFC 模块集成。

---

## 六、后端 API 对接清单

以下区分**设备端需要调用**的 API 和**后端管理用**的 API。

### 设备端必须对接的 API

| API | 用途 | 文档 |
|---|---|---|
| **OTA 设备注册/更新检查** | 设备激活、固件更新 | `POST /xiaozhi/ota/` |
| **智能体列表** | 获取用户所有 Agent（切换角色用） | [Agent API](https://linx.qiniu.com/docs/xrobot/api/agent) |
| **LLM 对话** | 发送音频/文本，接收 LLM 回复 | [LLM API](https://linx.qiniu.com/docs/xrobot/api/llm) |
| **声纹注册/验证** | 注册声纹、实时验证 | [声纹 API v2](https://linx.qiniu.com/docs/xrobot/api/voiceprint-v2) |
| **音色列表** | 获取可用音色（创建 Agent 时选择） | [音色 API](https://linx.qiniu.com/docs/xrobot/api/others) |

### 后端管理/小程序端 API

| API | 用途 | 文档 |
|---|---|---|
| **智能体 CRUD** | 创建/编辑/删除智能体 | [Agent API](https://linx.qiniu.com/docs/xrobot/api/agent) |
| **聊天记录** | 查询历史对话 | [聊天记录 API](https://linx.qiniu.com/docs/xrobot/api/chat-history) |
| **设备管理** | 绑定/解绑设备、查看设备列表 | [设备 API](https://linx.qiniu.com/docs/xrobot/api/device) |
| **音色管理** | 上传/删除自定义音色 | [音色 API](https://linx.qiniu.com/docs/xrobot/api/others) |

### 声纹识别 v2 更新（2026-08-07）

灵犀平台声纹 API 已发布 v2 版本：[voiceprint-v2](https://linx.qiniu.com/docs/xrobot/api/voiceprint-v2)

**与设备端相关的变化**（需同步给后端老师）：
- 支持**多声纹注册**（一个用户可注册多个人声纹）
- `threshold` 参数可自定义匹配阈值
- 新增 `user_id` 字段隔离不同用户声纹

设备端直接调用上述 API，**无需中间层**。设备请求格式和服务器响应格式见各文档页面。

---

## 四、用户认证体系

所有 API 统一携带用户标识：
```
user_id: "{user_id}"
Authorization: Bearer {USER_TOKEN}
```

`user_id` 在用户注册时分配，小程序和设备共享同一 ID。声纹、记忆、知识库、智能体均按 `user_id` 隔离。

---

## 七、任务分工

| 模块 | 负责人 | 状态 |
|---|---|---|
| 固件开发（ESP32-P4） | 星马梦缘 | 🔧 进行中 |
| 后端（OTA + Agent + LLM + 声纹 API） | 忠龙老师 | 🔧 搭建中 |
| 小程序（前端：智能体管理、音色克隆、知识库、声纹注册） | 忠龙老师 | ⏳ 待启动 |
| Live2D 自动化部件拆分 + P4 渲染验证 | 星马梦缘及梦懿老师 | ⏳ 预研 |
| 背景场景素材（1920×800 横屏图片） | 资源组 | 制作中 |
| 蓝牙模块选型评估 | 元帅老师 | ⏳ 调研中 |
| NFC 模块选型评估 | 资源组 | 完成 |
