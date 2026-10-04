# Spine 模型获取 + 仿真器需求（交接文档）

> 用途：交给新的 AI/开发者重写 Q 版（Spine→PPD）仿真器。
> 本文件只陈述事实与需求，不包含失败尝试的细节。

---

## 一、Spine 模型如何获取

### 1. 来源

明日方舟角色页（PRTS wiki）的 Live2D 资源，实际是 **Spine 3.8 格式**。

- 元数据：`https://torappu.prts.wiki/assets/torappu/db/char/<charid>/meta.json`
  - 返回 `{prefix, name, skin: {"默认": {"正面": {file}, "背面": {file}, "基建": {file}}}}`
- 资源下载：`https://torappu.prts.wiki/assets/` + `<prefix><file>` + 扩展名
  - 每个形态 3 个文件：`.skel`（骨骼，404 时回退 `.json`）、`.atlas`、`.png`
- charid 格式：`char_1028_texas2`（缄默德克萨斯）、`char_1016_amiya2` 等
- 现成下载代码：`/opt/szfz/paperdoll_sim/wiki_fetch.py`
  - `get_meta(charid)` → `download_set(meta, group_key, dest_dir)`（返回文件 stem）

### 2. 三个形态

| 形态 | 含义 | 说明 |
|---|---|---|
| 正面 | 默认皮肤主场景 | 主场景放角色目录根 |
| 背面 | 背面立绘 | 放 `forms/背面/` |
| 基建 | 基建小场景 | 放 `forms/基建/`（含 Sleep 等特殊动画） |

每个形态是**独立的 .skel/.atlas/.png 三元组**，转换产物是独立目录。

### 3. 已知特殊点

- 基建形态的 Sleep 动画包含 **IkConstraintTimeline ×4、TransformConstraintTimeline ×1、129 条 AttachmentTimeline**（换附件驱动闭眼等）
- 图集 png 可能被下载渠道缩放（如基建 png 940×940 vs atlas 声明 940×940 一般一致；凯尔希等部分角色 png 比声明小）——需要按比例缩放 atlas 坐标
- atlas 换行可能是 CRLF

---

## 二、PPD 数据格式（转换产物）

角色目录（如 `/static/characters/缄默德克萨斯/`）结构：

```
scene.json            # 层几何
<槽名>.raw            # 每层贴图:u16 w + u16 h + RGBA8888(每像素 4 字节,byte0=R,byte2=B)
anims.json            # 动作轨道
forms.json            # {"forms":[{"name":"基建","dir":"forms/基建"},...]}
forms/<形态>/          # 形态子目录(同上结构)
```

### scene.json

```json
{
  "landscape": true,
  "landscape_rot": 1,
  "layers": [
    {
      "name": "F_Chest",
      "x": 229, "y": 423, "w": 68, "h": 68,
      "cx": 263, "cy": 457,
      "bbox": [229, 423, 297, 491],
      "z": 37, "group": "body", "special": "", "visible": true
    }
  ]
}
```

- `x/y/w/h`：贴图在 480×800 画布上的位置与尺寸
- `z`：绘制顺序（升序）
- `group`：head/body（Q 版大多无 F_Head 骨骼 → 全 body）
- `visible`：默认可见性（false 的层靠动画 vis 轨道激活）

### anims.json

```json
{
  "Sleep": {
    "duration": 4.0, "loop": false,
    "layers": {
      "F_Chest": [[dx, dy, drot, vis], ...每帧...]
    }
  }
}
```

- 帧数 n = clamp(round(duration×fps), 8, 80)，fps 默认 20
- 每帧 `[dx, dy, drot, vis]`：平移增量、旋转增量（度）、可见性（≥0.5 渲染）
- 无轨道的层：用 scene.json 默认（不参与动画）
- 层名 = 槽名（与 scene.json 对应）

---

## 三、仿真器需求

### 1. 基本要求

- 网页（无构建工具，原生 JS + Canvas 2D）
- 加载角色目录（可切换形态 forms/），显示 480×800 画布
- 逐层渲染（按 z 升序）
- 动作下拉框：列出 anims.json 的全部动画，可播放/停止
- 支持画布旋转（90°/180°/270°/不转）以适配横屏场景

### 2. 渲染公式（唯一正确语义）

```js
// 每层:
ctx.translate(L.x + L.w/2 + dx, L.y + L.h/2 + dy);
ctx.rotate(drot * Math.PI / 180);
ctx.translate(-L.w/2, -L.h/2);
ctx.drawImage(tex, 0, 0, L.w, L.h);
```

动画插值（线性，无任何包络）：

```js
pos = t / duration * (n-1);
i0 = floor(pos); frac = pos - i0;
vis = k0[3] + (k1[3]-k0[3])*frac >= 0.5;
dx  = k0[0] + (k1[0]-k0[0])*frac;  // dy/drot 同
```

### 3. 关键注意（踩过的坑）

1. **不要加任何"过渡包络"**（sin 渐入渐出等）——官方 Spine 动画是直接播放的，包络会扭曲轨迹、造成黑屏
2. **vis 是硬切换**（≥0.5），不要做淡入淡出
3. **不要做 head/body 分组变换**（sway 等）——Q 版数据无 head 层，加分组变换会破坏画面
4. 参考实现（官方渲染正确性的金标准）：`/opt/szfz/tools/spine2ppd/render_ref.mjs`——spine-canvas 官方渲染器整身渲染，用户已确认"完全正确"
5. spine-canvas 3.8.99（`/opt/szfz/tools/spine2ppd/spine-canvas-3.8.99.js`）使用前必须：
   ```js
   spine.Texture.prototype.setFilters = function () {};
   spine.Texture.prototype.setWraps = function () {};
   ```
   TextureAtlas 构造回调：`p => new spine.Texture(image, w, h)`
   渲染器：`new spine.canvas.SkeletonRenderer(ctx)` + `renderer.triangleRendering = true`
   透明背景用 `clearRect`（不透明背景会被像素扫描误判）
6. 骨骼动画时间轴：`anim.apply(skel, 0, time, ...)`（**0 是 lastTime、time 是当前**——参数反写会让所有帧错乱）

### 4. 验收标准

- 播放 Sleep：人物**平躺**（与 x 轴平行），闭眼线、头发、耳朵跟随头部
- 播放 Die：按官方节奏倒下、无黑屏
- 与 `render_ref.mjs` 的整身渲染逐帧一致（像素 diff 目标 <5%）
- PRTS 官方预览对比：姿势、方向、层间关系一致

### 5. 可用素材

| 资源 | 位置 |
|---|---|
| 官方渲染参考（用户验证正确） | `render_ref.mjs`（spine 官方整身渲染） |
| 转换工具 | `/opt/szfz/tools/spine2ppd/`（render_layers.mjs 层导出、export_anims_ppd.mjs 轨道导出） |
| 测试数据 | 服务器 `/tmp/spine_diag/`（基建 spine 三元组）、`/opt/szfz/ppd_assets/spine_imports/u3/缄默德克萨斯/forms/基建/`（转换产物） |
| spine runtime | `/opt/szfz/tools/spine2ppd/spine-canvas-3.8.99.js` |

---

## 四、已知未解决的难题（供新开发者参考）

1. **mesh 层顶点形变**：官方 Spine 是顶点级渲染（mesh 权重形变），PPD 是整层刚体（平移+旋转）。贴图 = Default 姿势像素——动画中 mesh 形变（头发弯曲等）无法用刚体变换表达。
   - 现状：轨道按"帧姿势逐层渲染→像素扫描"计算（bbox 左上对齐 + 像素 PCA 方向），主体正确
   - 残余问题：细长 mesh 层的 180° 方向翻转（无向主轴歧义）与近圆形层的方向退化
2. **无向主轴 180° 翻转**：像素 PCA 主轴无向（±180° 等价），细长层（头发）头尾互换可见
3. **近圆形层方向退化**：λ2/λ1 ≥ 0.5 时 PCA 不可信，内容方向需另找判据

当前兜底：`/opt/szfz/tools/spine2ppd/rot_calib.json` 校准表（角色/形态/动画 → 层 → dx/dy/dr 修正量），由拖拽页人工校准生成。
