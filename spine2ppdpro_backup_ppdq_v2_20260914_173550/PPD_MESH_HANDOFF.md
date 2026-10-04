# PPDQ / PPD-Mesh v2 对接交接说明

## 结论

本地 `spine2ppdpro` 已完成 Spine 到 PPDQ 的验证版转换链路。核心变化是新增 `mesh.json v2`：把 Spine 的 `MeshAttachment` 和 `RegionAttachment` 都导出为三角形贴图，并按 Spine 官方 `drawOrder` 整体绘制。

这不是 MJPEG，也不是整帧 raw。数据粒度是 attachment 级三角网格，保留官方骨骼/mesh 变形后的逐帧顶点。

旧 PPD 文件仍保留：

- `scene.json`
- `anims.json`
- `*.raw`

因此旧硬件路径可以继续忽略 `mesh.json`；新 PPDQ 路径检测到 `mesh.json.version >= 2` 后走完整三角形渲染。

## 解决的问题

旧 PPD 只能做矩形 sprite 的位移/旋转/缩放，遇到复杂 mesh 会出现：

- sleep 时尾巴断裂、尾焰和尾巴分离
- 五官和脸比例/位置错位
- 睁眼和闭眼贴图同时存在或遮挡错误
- 后发、耳朵、角、衣服在 sleep 里错位
- 年、夕、余、令、塞雷娅、维什戴尔、新约能天使这类复杂基建动作需要大量补丁

PPD-Mesh v2 的处理方式是：不再用旧 raw 层去近似 mesh，而是直接导出官方 Spine 当前帧的三角形顶点、UV、drawOrder。用户本地验证反馈：之前的主要错位问题已消失。

## 当前实现文件

- `spine2ppdpro/tools/export_mesh_ppd.mjs`
  - 读取 Spine `.skel/.atlas/.png`
  - 导出 `mesh.json v2`
  - 同时复制 atlas PNG 到 PPD 目录
  - 支持 `MeshAttachment` 和 `RegionAttachment`
  - 支持 `normal/additive` blend 标记
  - 输出完整 `draw` 列表，key 为 `slot__attachment`

- `spine2ppdpro/simulator.js`
  - 加载 `mesh.json`
  - `mesh.json.version >= 2` 时默认走完整 mesh 渲染
  - URL 加 `?mesh=0` 可强制回退旧 raw 路径
  - Canvas 预览中 additive 用 `lighter`
  - 三角裁剪做了 0.75px 扩边，避免浏览器预览出现黑色三角缝
  - 已修复形态切换 bug：进入 `forms/背面` 或 `forms/基建` 后仍会回读根目录 `forms.json`，下拉框不会变成单选“主场景”

- `spine2ppdpro/tools/preview_mesh.mjs`
  - 命令行离线预览工具
  - 用于不打开浏览器时快速出 PNG 检查 mesh 渲染

- `spine2ppdpro/local_server.py`
  - PRTS 导入/本地转换流程中已经调用 `export_mesh_ppd.mjs`
  - 新导入角色应自动生成三形态 PPD + PPDQ mesh

## mesh.json v2 格式

```json
{
  "version": 2,
  "canvas": { "w": 480, "h": 800, "landscape": true },
  "textures": [
    { "id": "build_char_xxx.png", "file": "build_char_xxx.png", "w": 416, "h": 416 }
  ],
  "attachments": {
    "F_Head__F_Head": {
      "slot": "F_Head",
      "attachment": "F_Head",
      "type": "mesh",
      "texture": "build_char_xxx.png",
      "blend": "normal",
      "alpha": 1,
      "triangles": [0, 1, 2],
      "uvs": [12.0, 34.0]
    }
  },
  "base": {
    "draw": [
      { "key": "F_Head__F_Head", "vertices": [x0, y0, x1, y1] }
    ]
  },
  "animations": {
    "Sleep": {
      "duration": 4.0,
      "loop": false,
      "frames": [
        {
          "draw": [
            { "key": "F_Head__F_Head", "vertices": [x0, y0, x1, y1] }
          ]
        }
      ]
    }
  }
}
```

字段说明：

- `uvs`：atlas PNG 像素坐标，不是 0..1。
- `vertices`：PPD 场景坐标，已经完成 Spine 世界坐标到 480x800 场景坐标的转换。
- `triangles`：顶点索引三元组。
- `draw`：当前帧官方绘制顺序。设备端必须按这个顺序绘制，不能再按 `scene.json.layers[].z` 混排 mesh。
- `key`：`slot__attachment`，同一 slot 切换不同 attachment 时不会覆盖。
- `blend`：目前有 `normal` 和 `additive`。设备端至少要支持这两种。

## 生成命令

以夕基建为例：

```powershell
node spine2ppdpro\tools\render_layers.mjs spine2ppdpro\data\spine_raw\char_2015_dusk\基建 spine2ppdpro\data\ppd_out\char_2015_dusk\forms\基建 char_2015_dusk_build --landscape
node spine2ppdpro\tools\export_anims_ppd.mjs spine2ppdpro\data\spine_raw\char_2015_dusk\基建 spine2ppdpro\data\ppd_out\char_2015_dusk\forms\基建 20
node spine2ppdpro\tools\export_mesh_ppd.mjs spine2ppdpro\data\spine_raw\char_2015_dusk\基建 spine2ppdpro\data\ppd_out\char_2015_dusk\forms\基建 20
```

预览：

```powershell
node spine2ppdpro\tools\preview_mesh.mjs spine2ppdpro\data\ppd_out\char_2015_dusk\forms\基建 spine2ppdpro\preview_dusk_sleep_v2.png Sleep 0.55 --landscape
```

本地服务：

```powershell
cd spine2ppdpro
python -B local_server.py --port 8089
```

打开：

```text
http://127.0.0.1:8089/
```

## 三形态体积

形态只有三个：

- 主场景/正面：角色根目录
- 背面：`forms/背面`
- 基建：`forms/基建`

夕 `char_2015_dusk` 三形态完整 PPDQ 兼容目录统计：

- 干净展开大小：18.60 MB
- zip 分发包大小：5.43 MB
- `mesh.json` 合计：12.25 MB
- 旧 raw 合计：4.43 MB
- atlas PNG 合计：0.31 MB
- JSON 合计：13.86 MB
- 文件数：256

说明：

- 这是兼容包口径，保留旧 PPD raw。
- 真正给设备端的 PPDQ 包不应该直接使用 JSON；应在云端把 `mesh.json` 编译成二进制 `mesh.ppdq`。
- 设备端 PPDQ 包可以不携带旧 raw，除非需要回退兼容。
- 三形态 zip 约 5MB 级别可以接受。

## 性能数据

复杂基建角色平均每帧大致：

- draw item：73-102
- triangles：840-1347
- vertices：900-1400

代表样本：

```text
char_2015_dusk   avgDraw=73   avgTri=1037  avgVert=965
char_2014_nian   avgDraw=85   avgTri=884   avgVert=882
char_2026_yu     avgDraw=102  avgTri=1347  avgVert=1391
char_2023_ling   avgDraw=89   avgTri=1046  avgVert=1082
char_1035_wisdel avgDraw=87   avgTri=1225  avgVert=1179
char_1041_angel2 avgDraw=100  avgTri=1180  avgVert=1141
char_202_demkni  avgDraw=42   avgTri=540   avgVert=559
char_2027_wang   avgDraw=86   avgTri=840   avgVert=937
```

旧 PPD 曾能到约 21 fps。PPDQ 是否能接近 21 fps 取决于设备端三角光栅实现。建议主 agent 不要让设备端解析 JSON，也不要做 PNG 解码，必须走二进制和 raw atlas。

## 设备端接入建议

不要破坏旧 PPD 路径。新增 PPDQ 可选路径：

1. 云端转换阶段继续输出旧 PPD，附加生成 `mesh.json v2`。
2. 云端打包阶段把 `mesh.json` 编译成二进制 `mesh.ppdq`。
3. atlas PNG 在云端转成设备已有 raw 纹理格式。
4. 设备端加载时：
   - 若存在 `mesh.ppdq`：走 PPDQ 三角形路径。
   - 否则：走旧 PPD raw sprite 路径。
5. PPDQ 渲染时按当前动画帧 `draw` 顺序遍历：
   - 查 attachment spec。
   - 查 texture。
   - 按 triangles + uvs + vertices 做 textured triangle。
   - `blend=normal` 做普通 alpha blend。
   - `blend=additive` 做 additive/lighter。

建议二进制结构：

```c
header:
  magic "PPDQ"
  version 2
  canvas_w, canvas_h
  texture_count
  attachment_count
  animation_count

texture:
  id/index
  raw file path or texture offset
  w, h

attachment:
  texture_index
  blend
  alpha
  vertex_count
  triangle_count
  uvs_offset
  triangles_offset

animation:
  name
  duration_ms
  loop
  frame_count
  frames_offset

frame:
  draw_count
  draw_item_offset

draw_item:
  attachment_index
  vertices_offset
```

建议数据类型：

- vertices：`int16_t` 或定点 `q8.8/q12.4`，坐标范围 480x800，保留小数即可。
- uvs：`uint16_t` 像素坐标。
- triangles：`uint16_t` 索引。
- alpha：`uint8_t`。
- blend：`uint8_t`。

设备端性能要点：

- 不在帧循环里查字符串 key，云端编译时把 `key` 全部映射成 attachment index。
- 不在设备端解析 JSON。
- 不在设备端解 PNG。
- 顶点帧数据按当前动画帧直接读，不做 Spine 骨骼计算。
- 可以复用 `main/apps/live2d/lv2_render.c` 里的 textured triangle 软光栅思路。
- 先用塞雷娅或望做小样，再测夕/余/维什戴尔。

## 当前限制

- 当前 v2 未实现 Spine `ClippingAttachment`。目前已测试的问题角色主要靠完整 drawOrder + mesh/region 全三角化解决；若后续遇到真实 clipping 角色，需要追加 mask/clipping。
- 浏览器 Canvas 三角边缘和设备端软光栅细节可能不同，设备端以实际屏幕验收为准。
- 本地目录里有很多历史调试 PNG，不应进入正式包。正式包只收：
  - `scene.json`
  - `anims.json`
  - `forms.json`
  - `mesh.json` 或编译后的 `mesh.ppdq`
  - atlas raw/png
  - 必要旧 raw

## 已验证角色

已重点验证/导入：

- 夕
- 年
- 余
- 令
- 塞雷娅
- 维什戴尔
- 新约能天使
- 望
- 阿米娅
- 凯尔希
- 拉普兰德
- 缄默德克萨斯

用户确认：PPD-Mesh v2 后，之前 sleep/尾巴/后发/五官错位的主要问题已经消失。
