# Live2D 新角色添加工作流 — Claude 自动执行

> 当用户说"添加新角色 XXX"或"把这个模型加到 Live2D"时，按本文档执行。

---

## 用户需要提供的

1. Live2D 模型文件夹路径（包含 .moc3 / .model3.json / 纹理 PNG / .cdi3.json）
2. SD 卡盘符（如 `G:`）

## Claude 自动执行步骤

### 第 1 步：分析模型

```bash
# 用 Python 读取 .model3.json，获取纹理文件列表
# 用 Python 读取 .moc3 头部，获取 drawable/param 数量（可选）
```

**输出给用户看**：角色名、部件数、纹理数、纹理尺寸。

### 第 2 步：生成纹理 raw 文件

```python
from PIL import Image
# 对每个纹理 PNG → resize 到 1024×1024 → 输出 tex_00.raw, tex_01.raw 等
# 格式：2 字节 header (width, height uint16 LE) + RGBA8888 像素数据
```

### 第 3 步：拷入 Demo Resources + 隐藏其他模型

```bash
# 1. 创建 Resources/角色名/ 文件夹
# 2. 拷入 .moc3, .model3.json, .cdi3.json, 纹理 PNG
# 3. 把 Resources 下其他模型改名加 _bak 后缀（只留目标角色）
```

### 第 4 步：用户编译运行 Demo

告诉用户执行：
```bash
cd E:\Passport\pet\CubismSdkForNative\CubismSdkForNative-5-r.5\Samples\OpenGL\Demo\proj.win.cmake\build
cmake --build . --config Release
cd bin\Demo\Release
.\run_demo.bat
```

用户按 **L** 导出 model.l2d，按 **K** 导出 keyforms.bin。

### 第 5 步：收集模型参数

让用户把 Demo 控制台里 `DumpKeyforms` 段的输出贴给我。从中提取：

| 参数 | 从哪里看 |
|------|---------|
| `ppu` | `Canvas: ... ppu=XXXX` |
| keyform 参数列表 | `Key param: [N] ParamXXX def=... max=...` |
| 顶点数 | `DumpKeyforms: ... XXXX verts` |

**自动计算 keyform 映射 `needed[]` 数组**：从参数列表中找到 AngleX/Y/Z、EyeLOpen/ROpen、MouthOpenY、EyeLSmile/RSmile、MouthForm、EyeBallX/Y、BrowLAngle/RAngle 的 SDK 索引，生成映射表。

### 第 6 步：拷文件到 SD 卡 + Export 文件夹

```bash
# 创建 Release/角色名_export/ 文件夹
# 拷入 model.l2d, keyforms.bin, tex_*.raw
# 拷同样文件到 SD 卡 /sdcard/角色名/
```

### 第 7 步：改 PC Tuner（l2d_loader.py + tuner.py）

1. **l2d_loader.py**：新增 `load_keyforms_角色名()` 和 `load_角色名()` 函数
   - needed[] 数组 = 第 5 步计算的映射
   - ppu = 第 5 步获取的值

2. **tuner.py**：在加载区添加新角色路径搜索和加载

3. **ui_panel.py**：如需要可加跳过部件列表

### 第 8 步：改 P4（sd_test.cc + lv2_render.h）

1. **sd_test.cc**：`do_switch()` 新增 `chr == N` 分支
   - lv2_load 路径
   - ppu 赋值
   - keyform needed[] 数组
   - 跳过部件列表（如有）
   - 新增 lv2_switch_character 按钮编号

2. **ImageDisplay.cpp**：修改角色切换按钮的循环数量

### 第 9 步：PC Tuner 分部件截图（如需要跳过部件）

```bash
cd E:\Passport\pet\live2d_tuner
python -c "
# 渲染分组截图到 FurinaParts/ 文件夹
"
```

用户查看截图，告诉哪些编号需要跳过。
然后更新 l2d_loader.py 和 sd_test.cc 的跳过列表。

### 第 10 步：PC Tuner 测试 + P4 编烧

用户运行 `python tuner.py` 验证，确认后 `idf.py build flash monitor`。

---

## 添加新表情工作流

用户描述即可，格式：

> "加一个 XXX 表情：面部参数=值, 身体动画=描述"

Claude 同时改：
1. `l2d_animate.py` — PC 端动态函数 + DYNAMIC_PRESETS
2. `lv2_render.c` — P4 端 lv2_expr_override 的 case 分支

---

## 导出 C / JSON 说明

| 按钮 | 输出 | 用途 |
|------|------|------|
| **导出 C 代码** | `presets/tuned_params.h` | PC Tuner 上调好参数 → 导出 float vals[] → 直接粘贴到 P4 lv2_render.c |
| **保存 JSON** | `presets/custom.json` | 保存当前滑块值为预设文件，下次加载 |
