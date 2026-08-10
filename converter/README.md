# 角色导入标准化流程

## 一、所需资料清单

| 序号 | 内容 | 格式 | 尺寸 | 说明 |
|---|---|---|---|---|
| 1 | **Cover 动图** | JPG 序列 | 480×800 | PR 合成输出，命名 `{英文名}XXX.jpg`（XXX=三位序号从000起） |
| 2 | **表情动图** | PNG 序列（RGBA） | 480×800 | 按情感分文件夹，每帧透明背景。文件夹名即情感名（neutral/happy/angry...） |
| 3 | **缩略图** | JPG | 108×228 | 罗德岛索引页用的卡片图 |
| 4 | **语音文件** | WAV（16kHz mono 16-bit PCM） | — | 分 daily/fight/promotion 三个文件夹 |
| 5 | **语音文本** | YAML | — | key : value 格式，一行一条 |
| 6 | **背景音乐** | WAV（16kHz mono 16-bit PCM） | — | 放在 `/sdcard/main/music/` |
| 7 | **背景音乐列表** | YAML | — | 显示名 : 文件名.wav 格式 |

## 二、基本信息

| 项目 | 内容 |
|---|---|
| 英文名 | `Civilight_Eterna`（示例） |
| 职业 | `SUPPORTER`（VANGUARD/GUARD/REINSTALL/SNIPER/CASTER/MEDIC/SUPPORTER/SPECIALIST） |
| 星级 | `6STAR`（6STAR/5STAR/4STAR/3STAR/2STAR/1STAR） |
| Agent ID | `ef84ee6764ce44e78ec131aa9d5ebb1d`（服务器侧分配） |
| 唤醒词拼音 | `te lei xi ya` + `ni hao te lei xi ya` |
| 唤醒词中文 | `特蕾西亚` + `你好特蕾西亚` |

## 三、执行步骤

### Step 1 — 准备表情（PNG → MJPEG + RLE Mask）

```bash
python png2emoji.py "{源/表情根目录}"
```

生成文件：`{情感}.mjpeg` + `{情感}.mask`（每情感一组，在对应情感子文件夹中）

### Step 2 — 准备 Cover（JPG → MJPEG）

```bash
python jpg2mjpeg.py "{源/Cover_JPG文件夹}" {英文名}_cover
```

生成文件：`{英文名}_cover.mjpeg`

### Step 3 — 准备语音（WAV → 16kHz）

将 `wav_converter.py` + `音频转换器.bat` 复制到每个 voice 子文件夹（daily/fight/promotion），双击 bat 运行。

### Step 4 — 准备背景音乐（WAV → 16kHz）

将 `music_converter.py` + `music_converter.bat` 复制到 `/sdcard/main/music/`，双击 bat 运行。

### Step 5 — 复制到 SD 卡

SD 卡目录结构：
```
/sdcard/main/operator/{职业}/{星级}/{英文名}/
  cover/
    {英文名}_cover.mjpeg          ← Step 2 产物
  emoji/
    neutral.mjpeg + neutral.mask  ← Step 1 产物
    happy.mjpeg   + happy.mask
    ...（全部情感）
  voice/
    daily/
      *.wav                       ← Step 3 产物
    fight/
      *.wav                       ← Step 3 产物
    promotion/
      *.wav                       ← Step 3 产物
    text.yaml                     ← 语音文本

/sdcard/main/operator/INDEX/{职业}_108x228/{星级}/
  {英文名}.jpg                    ← 缩略图

/sdcard/main/music/
  *.wav                           ← Step 4 产物
  backgroundmusic.yaml            ← 音乐列表
```

### Step 6 — 固件注册

修改 2 个文件：

**① `main/audio/wake_words/custom_wake_word.cc`** — 注册唤醒词
```cpp
esp_mn_commands_add(N, "ni hao {拼音}");   // 你好{中文}
esp_mn_commands_add(N+1, "{拼音}");        // {中文}(短)
// Name array 也要同步加
```

**② `main/application.cc`** — 两处映射
- 唤醒词中文 → agentId + SD路径（约 641-647 行）
- agentId → cover_display_start 路径（约 417-422 行）

### Step 7 — 编译烧录

```bash
idf.py build flash monitor -p COM5
```

## 四、脚本速查

| 脚本 | 命令 | 输出 |
|---|---|---|
| `jpg2mjpeg.py` | `python jpg2mjpeg.py <JPG文件夹> [输出名]` | `{输出名}.mjpeg` |
| `png2emoji.py` | `python png2emoji.py <表情根目录>` | `{情感}.mjpeg` + `{情感}.mask` |
| `import_voice.py` | `python import_voice.py --page "角色名/语音记录" --dest <voice目录>` | 下载+重命名+分类+16kHz+text.yaml |
| `wav_converter.py` | 双击同目录的 `音频转换器.bat` | 原地替换为 16kHz |
| `make_thumbnail.py` | `python make_thumbnail.py <输入图> [输出名]` | `{输出名}.jpg` (108×228) |
| `music_converter.py` | 双击同目录的 `music_converter.bat` | 原地替换为 16kHz |

## 五、音频一键导入

### `import_voice.py` — PRTS Wiki → SD 卡语音全自动

```bash
python import_voice.py --page "魔王/语音记录" --dest "E:\虚拟SD卡\main\operator\SUPPORTER\6STAR\Civilight_Eterna\voice"
```

**自动完成 5 步：**
1. 调用 MediaWiki API 获取页面 HTML
2. 解析 38 条语音数据（中文名、音频 URL、文本内容）
3. 下载 WAV 文件（`torappu.prts.wiki` 直链）
4. 中文名 → 英文文件名映射 + 分类到 daily/fight/promotion
5. 调用 `wav_converter.py` 转为 16kHz
6. 生成 `text.yaml`（英文 key : 中文文本）

**参数：**
| 参数 | 说明 |
|---|---|
| `--page` | PRTS wiki 页面标题，如 `"魔王/语音记录"`、`"凯尔希/语音记录"` |
| `--dest` | 目标 voice/ 文件夹路径 |
| `--dry-run` | 仅解析不下载，测试用 |
| `--no-download` | 跳过下载（文件已在 dest） |
| `--no-convert` | 跳过 16kHz 转换 |

**中文→英文映射表内嵌在脚本中**，覆盖全部 38 条语音行，各角色通用。如需新增映射条目，编辑脚本中 `CN2EN` 字典。
