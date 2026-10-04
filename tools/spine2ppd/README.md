# spine2ppd — Spine 3.8.99 → 纸偶(PPD) 转换器

把明日方舟 Q 版小人（Spine 3.8.99：`.skel` + `.atlas` + `.png`）转换为设备端纸偶引擎
（`lv2_paperdoll`）的 `scene.json + *.raw` 格式，**设备端零新代码**，直接走已验证的
PPD 渲染管线（待机动画/触摸互动/表情联动全部继承）。

## 用法

```bash
# 依赖：Node 22+（npm canvas）、Python 3 + PIL
cd tools/spine2ppd && npm install

python convert.py <spine_dir> <out_dir> [scene_name] [--landscape]
# 示例（阿米娅正面 → 虚拟 SD 卡，横屏 Q 版互动布局）
python convert.py "E:/Passport/source/立绘/产出/Qlive2d/Amiya/阿米娅正面" \
       "E:/虚拟SD卡/main/operator/CASTER/5STAR/Amiya/PPD_Q" Amiya_Q --landscape
```

产出：`<out_dir>/scene.json` + 每层 `<槽名>.raw` + `preview.png`（复合预览，与设备显示
方向一致，转换后先肉眼看这张图）。

**`--landscape`（横屏 Q 版互动）**：Spine 内容顺时针旋转 90° 画入 480×800 竖帧
（角色头朝画布左 = 用户横持设备的视觉上方，与 standee 预旋转同思路，设备端渲染零旋转）。
fit 公式交换 CW/CH（角色世界高度映射画布宽度）。preview 输出 800×480 正立图。

分步调试时可单独跑：

```bash
node render_layers.mjs <spine_dir> <out_dir> [scene_name] [--landscape]   # 逐层导出
python preview.py <out_dir> <out_dir>/preview.png [--landscape]           # 复合预览
node render_ref.mjs <spine_dir> ref.png [--landscape]                     # 整身参考（对照用）
```

若 `preview.png` ≠ `ref.png`，说明逐层隔离有问题（图层级渲染 ≠ 整身渲染）。

## 原理（关键决策，别在重构时丢掉）

- **官方渲染器导出**：每个槽单独挂附件 → `spine-canvas` 官方渲染器画到 480×800 缓冲
  → 像素扫描 bbox → 切出 raw。旋转/mesh 形变/裁剪全部由官方渲染器处理，
  **零手写几何**（初版手算旋转角每层错 90°，已废弃）。
- **必须应用 Default 动画第 0 帧**：setup pose 同时含 5 套姿势骨骼链与表情变体，
  不应用动画会全部叠加（5 只手 + 痛苦表情）。`defaultAnim.apply(skel, 0, 0, ...)` 后
  隐藏链骨骼被移到屏幕外、槽附件置空，得到正确的单姿势单表情初始态。
- **bbox 只算当前挂载的可见附件**：隐藏链骨骼移到 x≈634 处，遍历皮肤全部附件会把
  远点算进 bbox → 模型缩成一小块。
- **图层名 = 槽名**（不是附件名）：两个槽可能挂同名附件，用附件名会 raw 文件名冲突。
- **解析与渲染必须用同一套 spine 类**：本目录只 vendored `spine-canvas-3.8.99.js`
  （自带核心类集）。npm 的 spine-core 4.x 读不了 3.8.99 二进制；3.8.99 core 与 canvas
  混用时 `instanceof` 跨类集失败 → 渲染器静默跳过所有附件（全空白）。**不要再引
  spine-core 进来。**
- **方向约定**：Spine 世界 y 向上；node-canvas 负 scale 会裁剪 drawImage（坑），所以
  正 scale 渲染（缓冲中人物倒立），导出时对整幅画面做 **180° 旋转**：raw 行列都反转
  + 图层 x/y/bbox/cx/cy 换算 `x' = W - x - w`、`y' = H - y - h`（初版只翻 y 漏翻 x，
  图层内容左右颠倒）。设备端场景空间是 y 向下屏幕系（`blit`/`sX`/`sY` 无翻转），
  pivot 与图层同系——`head_pivot`/`mouth_pivot` 直接取自 180° 旋转后的质心坐标。
  **验收标准：`preview.png` 与 `ref.png` 像素级一致（mean diff < 0.1/通道）。**
- 加载脚本注意：`spine-canvas-3.8.99.js` 尾部有 `//# sourceMappingURL` 注释，拼接导出
  语句前必须先加 `\n`，否则被注释吞掉。

## 输出格式（与设备端 lv2_paperdoll.c 约定一致）

- `<name>.raw`：u16 小端 w、u16 小端 h + RGBA8888（行序 = 屏幕行序，行 0 = 顶部）
- `scene.json`：`name` / `head_pivot` / `mouth_pivot` / `face_tilt` / `no_cavity` /
  `mouth_compress` / `mouth_gain` / `mouth_floor` / `mouth_width_open` /
  `mouth_open_gain` / `hair_factor` / `layers[{name,x,y,w,h,cx,cy,bbox,z,group,special}]`
- `special`：`mouth`（嘴层，锚 mouth_pivot 做张嘴动画）；`eye-l`（Q 版 `F_Eye` 单层
  双眼条，设备端走 eyelash 同款沿脸轴缩放 = 眨眼）。
- `arm-l`/`arm-r` 设备端已定义但渲染侧未使用，Q 版手臂也不标。

## 设备端适配（已实现，2026-08-26）

- `lv2_paperdoll.c` 的 `head_part()` 已加 Q 版前缀匹配（F_Head/F_Front_Hair/F_Hair/
  F_Ear/F_Eye/F_Eyebrow/F_Mouth/F_Ponytail/F_Hood）→ 摸头判定正常。
- 眨眼：`special == 2` 分支已加 `strncmp(name, "F_Eye", 5)` 走 eyelash 同款缩放。
- 场景参数（mouth_gain 等）当前是经验默认值，设备上说话效果需要调参。

## 动作导出（Q 版动作测试）

```bash
node export_anims.mjs <spine_dir> <out_dir> [fps]
```

每个 Spine 动画逐帧渲染（横屏变换，与 `--landscape` 场景一致）→ `<out_dir>/anim/<动画名>.mjpeg`
（u32 帧数 + 偏移表 + 连续 JPEG，与设备端 `load_mjpeg_into` 约定一致）+ `anim.json` 清单。
**每动画独立 fit**（动画内所有帧 bbox 并集）：Start 等入场动画首帧角色在远处，
全动画 union 会把所有动画的 scale 污染掉。设备端动作测试页（横屏菜单进入）列表播放。

## 拆眼

Q 版 `F_Eye` 是单层双眼条（两眼睛同一纹理）。整层锚中心眨眼会把双眼拉向中央——
`render_layers.mjs` 导出后处理：沿长轴找最大中间空隙切成 `F_Eye_L`/`F_Eye_R`
（special=eye-l/eye-r，各自锚自己的质心），双眼原地闭合。

`scene.json` 含 `landscape` 字段：设备端横屏场景眨眼沿画布 x 轴（视觉竖直方向），
竖屏场景沿 y 轴。

## 子模型批量转换

正面验证通过后，同法转换「背面」「基建」：

```bash
python convert.py "E:/Passport/source/立绘/产出/Qlive2d/Amiya/阿米娅背面" \
       "E:/虚拟SD卡/main/operator/CASTER/5STAR/Amiya/PPD_Q_back" Amiya_Q_back --landscape
node export_anims.mjs "E:/Passport/source/立绘/产出/Qlive2d/Amiya/阿米娅背面" \
       "E:/虚拟SD卡/main/operator/CASTER/5STAR/Amiya/PPD_Q_back"
```

每个子模型独立目录（独立 scene.json），设备端按互动模式切换加载。

## 已知限制（设备端 eye_mask WARNING）

设备日志里的 `scene.json 无 eye_mask 字段 → hair=0 将退回矩形兜底` 与 `hair punch SKIPPED`
是**头发打洞掩膜缺失**（竖屏仿真器导出带 eye_mask.raw，Q 版转换器未导出）——与眨眼无关，
只影响"前发对眼睛的遮挡打洞"效果（Q 版第一版退回矩形兜底，可接受）。

## 目录

| 文件 | 作用 |
|---|---|
| `convert.py` | 一键：node 导出 + python 预览 |
| `render_layers.mjs` | **转换器本体**：逐层官方渲染导出 |
| `render_ref.mjs` | 整身参考渲染（与 preview 对照排错） |
| `preview.py` | 按 z 序复合 scene.json 全部图层 → PNG |
| `spine-canvas-3.8.99.js` | vendored 官方运行时（spine-runtimes 3.8.99.1） |
