# spine2ppdsimulator — Spine→PPD Q 版仿真器（重写版）

按 `docs\Spine获取与仿真器交接文档.md` 第三部分重写的网页仿真器：回放 PPD 转换产物（`scene.json` + `.raw` + `anims.json` + `forms.json`），在 480×800 画布上逐层渲染并播放动作。

无构建工具：纯原生 JS + Canvas 2D，`index.html` + `style.css` + `simulator.js` 三个文件。

## 快速开始

**直接双击 `index.html`**（file:// 即可）——已内置 **缄默德克萨斯**（三形态：正面 / 背面 / 基建，含 Sleep/Die/Idle/Skill 全动画）：

- 形态下拉切"基建" → 动画选 `Sleep`：人物平躺、闭眼线跟随头部
- 动画选 `Die`：按官方节奏倒下
- 内置数据打包于 `data_tx.js`（`tools/pack.py` 生成，10.7MB，base64 内嵌，绕开 file:// 的 fetch 限制）

也可点 **演示数据** 看合成小人自检（走真实 `.raw` 解码路径）。

## 本地工具链（tools/）

全链路已在本地 Windows 跑通（Node 22 + Python）：

```bash
cd tools
python fetch_char.py 缄默德克萨斯          # 1. 搜索名字 → 下载三形态 .skel/.atlas/.png 到 spine_raw/<名>/
node convert.mjs "spine_raw/缄默德克萨斯/正面" "ppd_out/缄默德克萨斯"                # 2. spine → PPD 产物
node convert.mjs "spine_raw/缄默德克萨斯/背面" "ppd_out/缄默德克萨斯/forms/背面"
node convert.mjs "spine_raw/缄默德克萨斯/基建" "ppd_out/缄默德克萨斯/forms/基建"
python pack.py "ppd_out/缄默德克萨斯" "../data_tx.js" 缄默德克萨斯                  # 3. 打包进 simulator
node verify.mjs "ppd_out/缄默德克萨斯" "spine_raw/缄默德克萨斯/正面"                # 4. 像素级验收
```

- `wiki_fetch.py`：prtswiki 搜索/下载链路（opensearch → 干员id → torappu meta → 三元组）
- `convert.mjs`：官方 spine-canvas 3.8.99 渲染（`triangleRendering=true`）逐槽渲染导出贴图 + 逐帧像素扫描（bbox 左上 + PCA 主轴）导出轨道。关键实现点：
  - 相机吸收进骨骼根变换（`skeleton.x/y/scaleX/scaleY`），ctx 恒等——@napi-rs/canvas 在 ctx.scale+clip 组合下行为异常，此方案绕过且渲染输出即屏幕坐标
  - 槽对象从 `skeleton.slots` 按名字查找（**不能用 drawOrder 下标**：Die 等动画有 DrawOrderTimeline 会重排 drawOrder）
  - 半像素对齐：bbox 的 minX+maxX 为偶数时右侧/下侧补 1 透明像素，保证回放公式 `translate(x+w/2)` 落在整数像素（否则整层被 Canvas 重采样）
  - 参考帧语义：setup 有内容的层参考 = setup 姿势（贴图同源）；setup 无内容的层参考 = 全局首个有内容帧（贴图也从该帧裁剪）
  - PCA 无向主轴与参考角对齐（±180 取最近）；近圆形层（λ2/λ1≥0.5）drot=0
  - 帧数 n = clamp(round(duration×20), 8, 80)；循环判定 = 名字含 Loop/Idle/Default；空动画（duration=0）跳过
- `verify.mjs`：验收脚本——PPD 回放 vs spine 官方整身渲染逐采样点像素 diff。**当前成绩：Die 4.4%、Start 4.4%（过 5% 线）、基建 Move/Relax 0.6%；Skill 系 15~25%、基建 Sleep 8.7% 为 mesh 形变 + PCA 残余（交接文档第四节已知难题，服务器版靠 rot_calib 人工校准兜底）**
- `diag_anim.mjs`：单动画单帧逐层 diff 定位（`node diag_anim.mjs <动画> <t> <ppd目录> <spine目录>`）
- 依赖：`npm i @napi-rs/canvas`；`spine-canvas-3.8.99.js` 来自官方 spine-runtimes 3.8 分支 build 产物（spine-all.js，含 canvas 后端）

## 加载真实数据

服务器上的转换产物：`/opt/szfz/ppd_assets/spine_imports/u3/缄默德克萨斯/`（根=正面、`forms/基建/`=基建形态）；Spine 三元组在 `/tmp/spine_diag/`。

**方式一 · 本地文件夹（无需服务器）**：把角色目录拷/下载到本地 → 点"选择本地文件夹" → 选**角色目录本身**（含 `scene.json` 的那一层）。浏览器需支持目录选择（Chrome/Edge 可）。

**方式二 · HTTP 服务**：

```bash
python serve.py --data <角色数据根目录> --port 8080
# 浏览器打开 http://127.0.0.1:8080/ ，URL 框填 /characters/缄默德克萨斯/
```

也可把本目录放到任何静态服务器下，URL 填角色目录的相对/绝对地址（需同源或允许 CORS）。

## 功能

- 角色目录加载 + 形态切换（`forms.json` 驱动，`forms/<形态>/` 子目录）
- 动画下拉框（`anims.json` 全部动作）+ 播放 / 停止 / 进度条拖动 seek；单次动画播完停在末帧，再点播放从头开始
- 画布旋转 0°/90°/180°/270°（适配横屏场景）、缩放 50%~100%、背景白/灰/黑/棋盘格
- 图层列表（z 升序）：勾选隐藏单层，调试层翻转/遮挡问题用
- 导出当前帧 PNG（白底合成，480×800 原分辨率）

## 渲染语义（与交接文档逐条对应，勿改）

```js
// 每层（z 升序）：
ctx.translate(L.x + L.w/2 + dx, L.y + L.h/2 + dy);
ctx.rotate(drot * Math.PI / 180);
ctx.translate(-L.w/2, -L.h/2);
ctx.drawImage(tex, 0, 0, L.w, L.h);
```

- 动画插值**纯线性**，无任何过渡包络（sin 渐入渐出会扭曲轨迹、造成黑屏）
- `vis` 是**硬切换**：`k0[3] + (k1[3]-k0[3])*frac >= 0.5`，不做淡入淡出
- **无 head/body 分组变换**（sway 等）——Q 版数据无 head 层
- 无轨道的层用 scene.json 默认值（`visible` 决定显隐），不参与动画
- 帧数/时长由轨道本身决定：`n = track.length`，插值 `pos = t/duration*(n-1)`

## 数据格式摘要

- `scene.json`：`{landscape, landscape_rot, layers:[{name, x, y, w, h, z, group, visible}]}`（渲染只用 `x/y/w/h/z/visible/name`，`cx/cy/bbox` 保留未用）
- `<槽名>.raw`：u16 w + u16 h + RGBA8888（byte0=R, byte2=B）；字节序自动检测（LE 尺寸不合时回退 BE）
- `anims.json`：`{动作名: {duration, loop, layers: {槽名: [[dx, dy, drot, vis], ...]}}}`
- `forms.json`：`{"forms": [{"name": "基建", "dir": "forms/基建"}]}`

## 已知局限

本仿真器只忠实回放 PPD 数据。Spine mesh 顶点形变已由转换工具烘焙进轨道（整层刚体近似：平移+旋转），mesh 层的 180° 方向翻转、近圆形层方向退化等问题属于**数据侧**问题（见交接文档第四节），不在渲染器范围内。
