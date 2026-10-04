# Auto Live2D 项目日志

> **给新终端 Claude 的接手说明**：这是"明日方舟干员 Live2D 自动化"项目的持久化文档。
> 对话上下文可能超限换终端，**读本文件即可了解全部背景、计划、进度**。
> 规则：每次修改计划或完成阶段都在本文件更新；关键发现必须记录。

---

## 一、项目定位与当前状态（2026-08-19）

**目标**：明日方舟全部干员（300+）从静态立绘 PSD 自动生成可在 P4 设备展示的 Live2D 模型。

**当前主线**：THA（LivePortrait）稠密运动场 → 自动生成 Live2D 嘴部 keyform（张嘴/口型变形数据），解决 stretchystudio 自动绑骨无法生成嘴部的问题。同学的模板工程（手动绑骨标准版）并行推进中，用于骨架结构参照。

**P4 设备链**（已验证，勿重复验证）：`moc3 → PC Demo(DumpGeometry/DumpKeyforms) → model.l2d + keyforms.bin → tuner.py → P4 lv2_render`。全链路已跑通多次。

---

## 二、背景与关键结论（勿重蹈覆辙）

### 2.1 工具链

```
seethrough 分层 PSD（tag 命名：face/front hair/eyelash-l/…）
  → stretchystudio（网页版，自动绑骨）→ .cmo3
  → Cubism Editor 5.0（打开验证/修头饰）→ 导出 .moc3（输出版选 SDK 5.0）
  → PC Demo（CubismSDKForNative-5-r.5 OpenGL Demo，已改造：L/K 键导出）
  → model.l2d + keyforms.bin
  → tuner.py（PC 预览调参）→ P4（sd_test.cc + lv2_render.c）
```

### 2.2 stretchystudio 的关键结论

- **网页版（editor.stretchy.studio）与 GitHub master 代码分叉**：网页版是未推送的完整版（157 warp、mesh 坐标为 0..1 warp-local、Editor 打开正常）；master 是重构半成品（89 warp、mesh 坐标 fallback 成画布像素、Editor 打开人物消失）。**放弃在 master 上修复**（重构未完成，工程量不可控）。
- 网页版导出在 Editor 中正常显示的前提：Editor 打开时会**自动把画布像素的 ArtMesh 坐标重算为 warp 局部 0..1**——任何改变 FaceParallax 网格位置的修改都可能破坏此兼容路径（血泪教训：改了 facePivot → Editor 不重算 → 人物消失）。
- 绑骨质量问题的根因链：seethrough 分割输出含**散布全画布的噪声像素** → 网格生成碎片顶点 → bbox 拉爆全画布 → facePivot 校准落到画布外 (671, 1406.9) → AngleX 位移场中心在画布外 → 头部平移/部件方向混乱。
- **已探明的正确修复方向**（拿到完整源码后应用）：① bbox 计算改 2%-98% 分位数裁剪；② 骨架启发式用最大连通块 bbox（非层矩形）；③ neck bbox 异常时跳过 NeckWarp。这些修复在 master 上验证过方向正确，但 master 本身有回归无法使用。

### 2.3 Mon3tr 现状

- Mon3tr1.cmo3（网页版导出 + Editor 保存）：静态正常、闭眼正常（mesh 级 keyform 绑 ParamEyeLOpen/ROpen）、AngleZ 绕脚下转（FaceRotation pivot 错）、AngleX 头部平移 60px+、脖子在 AngleX 下平移（neck mesh 直接绑 ParamAngleX）、headwear 与头发反向、**嘴部无张嘴变形（mouth 是贴图，无 keyform，无嘴内部/牙齿）**。
- fixed2/fixed14 后处理（cmo3 XML 修改，脚本方式已验证）：FaceRotation origin 改到下巴 (43,-1033)、Neck Warp deformer keyform 禁用、neck mesh keyform 锁定 rest——**这三个修复有效**。
- 参数映射（keyforms.bin 顺序，Editor 导出后 13 个 key 参数）：AngleX, AngleY, AngleZ, EyeLOpen, EyeROpen, EyeBallX, EyeBallY, MouthForm, MouthOpenY, BodyAngleX, BodyAngleY, BodyAngleZ, Breath。P4 的 11 参数 needed[] = {0,1,2,3,4,8,-1,-1,7,5,6}（EyeLSmile/EyeRSmile/BrowLAngle/BrowRAngle 缺失）。

### 2.4 嘴部问题

- stretchystudio 无法自动生成张嘴：mouth 层是贴图（无 keyform 变形、无牙齿口腔内部）。
- **用户要求**：纯 Live2D 方案（不在 P4 端叠加贴图）。
- 同学的模板工程将包含标准嘴部结构（mouth inside 部件），用于参照。

---

## 三、当前计划：THA 嘴部 keyform 方案（审核通过，执行中）

### 技术选型
**LivePortrait**（快手开源）：稠密运动场可外接提取、有动漫版权重（社区 liveportrait-anime）、嘴部说话是强项。

### 资源需求
GPU 显存 2-4GB（RTX 4060 Laptop 8GB 够）、内存 ~4GB、磁盘 ~2GB。离线单角色推理（每角色几分钟）。

### 流程
```
Mon3tr 立绘（PSD 合成 1280×1280）
  → [1] 嘴部区域定位（mouth ArtMesh 顶点 → bbox，已有解析工具）
  → [2] LivePortrait 推理：张嘴驱动帧 → 帧序列 + 稠密运动场
  → [3] 运动场采样：mouth 顶点 → 位移向量
  → [4] keyform 生成：最大张嘴帧位移 = ParamMouthOpenY=1 的 keyform
        （可选多口型 a/i/u/e/o → ParamMouthForm）
  → [5] 写入 cmo3（工具链已有：解密/修改/打包）
  → [6] Editor 验证张嘴
```

### 阶段与验收
- **阶段 1（半天）**：环境搭建（conda brain 环境 + PyTorch + LivePortrait + 动漫权重）→ Mon3tr 立绘生成闭嘴→张嘴动画帧 → 验收：嘴部自然、牙齿口腔可见、画风一致（**风险最大，先验证，不达标及时止损**）
- **阶段 2（半天）**：提取 dense motion（warping 模块 deformation 输出）→ 坐标系映射（crop 空间 → 画布空间）→ mouth 顶点采样位移 → 验收：闭嘴帧位移≈0、张嘴帧下唇向下
- **阶段 3（1 天）**：最大张嘴帧位移 → mouth 的 CArtMeshForm keyform；THA 生成帧抠牙齿口腔 → mouth inside 部件纹理+网格 → 一起写入 cmo3 → 验收：Editor 拖 MouthOpenY 张嘴自然
- **阶段 4（可选）**：推广到头部转头 AngleX（THA 转头帧 → 运动场 → FaceParallax keyform）

### 风险与对策
| 风险 | 对策 |
|---|---|
| 动漫权重对干员画风效果差 | 阶段 1 先验证；备选：用户已有张嘴参考图 + 光流提取 |
| 运动场嘴部边界不准 | 只用嘴部内部 80% 区域位移，边缘内插 |
| 牙齿口腔生成模糊 | 降级：THA 帧做参考，素材由同学模板嘴内部适配 |
| LivePortrait 检测动漫人脸失败 | 已有 mouth bbox，手动 crop，不依赖自动检测 |

---

## 四、关键文件路径清单

| 用途 | 路径 |
|---|---|
| P4 项目根 | E:\Passport\espp4\sparepart\JC4880P443C_I_W\1-Demo\idf_examples\ESP-IDF\xiaozhi-esp32sp1 |
| PC Demo（L/K 导出改造版） | E:\Passport\pet\CubismSdkForNative\CubismSdkForNative-5-r.5\Samples\OpenGL\Demo\proj.win.cmake\src\LAppModel.cpp |
| Demo Resources | ...\Demo\proj.win.cmake\build\bin\Demo\Release\Resources\ |
| stretchystudio 本地源码（master 半成品，勿用） | E:\Passport\source\live2d\strechyStudio\stretchystudio-master |
| stretchystudio 全新解压副本（纯净 master） | E:\Passport\source\live2d\SS\stretchystudio-master |
| Mon3tr 原始工程（网页版+Editor 保存） | E:\Passport\source\live2d\Mon3tr\Mon3tr1.cmo3 |
| Mon3tr 后处理版本（fixed2~fixed14） | E:\Passport\source\live2d\Mon3tr\Mon3tr_fixed*.cmo3 |
| 网页版 vs 本地导出对比 | E:\Passport\source\live2d\Mon3tr\cmp\（modelwebsite=正常，modellocalhost=坏） |
| Mon3tr 分层 PSD | E:\Passport\source\live2d\seethrough_00001.psd |
| tuner（PC 预览器） | E:\Passport\pet\live2d_tuner\tuner.py |
| 网页版 stretchystudio | editor.stretchy.studio（完整版代码未推送，勿再折腾 GitHub master） |

## 五、已有工具与脚本（复用，勿重写）

| 工具 | 位置 |
|---|---|
| cmo3 解密（CAFF 容器） | C:\Users\HP\AppData\Local\Temp\ss_src\docs__live2d-export__scripts__cmo3_decrypt.py |
| cmo3 打包 | 同目录 caff_packer.py |
| cmo3 XML 后处理脚本（fixed 系列修复） | C:\Users\HP\AppData\Local\Temp\fix_v14.py（含 neck 锁定+FaceRotation 修复） |
| moc3 解析/对比脚本 | C:\Users\HP\AppData\Local\Temp\ 下多个 check_*/diag_*/l2d_bbox.py |
| PSD 层 alpha bbox 分析 | 用 psd_tools（conda brain 环境已装） |
| stretchystudio 源码关键文件快照 | C:\Users\HP\AppData\Local\Temp\ss_src\（cmo3writer.js、faceParallax.js、bodyRig.js 等） |

**注意**：Temp 目录工具脚本可能被清理——重要脚本应复制到项目内（下一步动作）。

## 六、当前进度与下一步

### THA 阶段 1 进度（2026-08-19 第三次更新，重启后 GPU 验证完成）
- ✅ 环境就绪（PyTorch CUDA、onnxruntime、pykalman；RTX 4060）；重启后 GPU 干净（426MiB/8188MiB），推理 3 秒跑通
- ✅ 冒烟测试通过（自驱动差 1.10/255）
- ✅ **跨身份张嘴驱动有效**：flag_relative_motion=False + flag_lip_retargeting=True 时嘴部区域变化 31.9（默认相对模式仅 3.9）
- ❌ 动漫小嘴问题：Mon3tr 嘴仅 ~15×5px，insightface 把下巴当嘴；运动场 64×64 分辨率（8px/格）无法表达小嘴张开
- ✅ **画大嘴增强方案有效**（在源图画 2px 细线嘴后，检测到嘴 (652,237) ≈ 真嘴 (660,242)）
- ⚠️ landmark.onnx 细化会把嘴点重新放回下巴 → 绕过方案（直接信 insightface 修正点 + M_o2c 转 crop 空间）已写入 mouthbig_run2.py
- ✅ **阶段 1 验收结论（重要，2026-08-19 15:56）**：
  - **deformation 稠密运动场不可用**：嘴部四周（上/下/左/右带）均匀 +25px 向下、gap=0——只有下巴下沉运动，无双唇分离（源嘴太小，8px/格插值抹平）
  - **kp 隐式关键点（21 点中嘴部 14-17）也不可用**：4 点全部向下，stitching retarget 压缩幅度后分离消失
  - **insightface 106 点嘴 landmark（48-67）是唯一干净信号**：驱动图（真人张嘴）嘴高 432.8px vs 源图 108.9px；对齐嘴中心后上唇点上移 -222px、下唇点下移 +102px，开合模式清晰
  - 注意：源图 landmark 嘴中心 (650,267) 与真嘴 (660,242) 错位 25px，且 109px 高是检测假象（把画的线和下巴阴影当嘴轮廓）→ 不能直接用源图 landmark 的绝对位置
- 关键脚本：E:\Passport\source\live2d\liveportrait\ 下 mouthbig_run2.py（v3 全图统计）、mouthbig_run3.py（v4 嘴中心采样，输出 deform_v4_mouth.png）、kp_diag.py（kp+landmark 诊断，输出 out/mouth_lmk_data.npz）
- 运动场语义：deformation (1,16,64,64,3) = 加权总场（16=深度切片、3=(x,y,mask)），位移 = (grid−identity)×32（64 网格单位）

### 阶段 2 修正方案（基于诊断结论）
不再从稠密运动场采样，改为 **landmark 开合模式映射**：
1. 驱动图嘴 landmark（48-67）张开轮廓 → 提取模式：各点相对嘴中心的偏移向量（上点上移/下点下移/嘴角内收）
2. 源图嘴 landmark（48-67）闭合轮廓 → 同法提取
3. 模式差 = 张开位移场，按 **真实嘴部几何**（mouth ArtMesh 顶点 bbox，来自 PSD ≈ (653-668, 239-244)）归一化缩放
4. 对 mouth 顶点做 RBF/仿射变形：顶点相对嘴中心偏移 × 模式方向 × 幅度系数
5. 幅度系数是审美参数（嘴高 5px → 目标 12-15px，约 3 倍），tuner 可调
6. 优点：不依赖源图 landmark 绝对位置（25px 错位无害）、不需要运动场、insightface CPU 检测即可

### 待办
1. ✅ 阶段 1 验证完成（结论：运动场不可用，landmark 模式可用）
2. 阶段 2：landmark 开合模式提取 + mouth 顶点 RBF 变形（按修正方案）
3. 阶段 3：keyform 生成 + cmo3 写入
4. 等同学的模板工程（骨架结构参照）

### 路线 A 进度（2026-08-19 下午，THA 止损后转入）
- 2026-08-19 15:5x：用户审核批准止损 LivePortrait 路线，转 **路线 A：几何规则张嘴 keyform**（用户确认"尝试路线A"）
- 用户关键问题："张嘴后嘴部展示什么内容"→ 方案：程序化通用口腔模板（深色口腔底+双排牙齿+舌头，参数化生成，写入 cmo3 纹理图集=纯 Live2D，不叠真人纹理）
- ✅ **路线 A 阶段 1 完成（fixed16）**：
  - 生成器 E:\Passport\source\live2d\Mon3tr\mouth_keyform_gen.py（参数 OPEN_TOTAL/UP_RATIO/DN_RATIO/CORNER_PULL 顶部可调）
  - 输出 E:\Passport\source\live2d\Mon3tr\Mon3tr_fixed16.cmo3（从 fixed15 基线）
  - 原理：嘴部 16 顶点（index 99-114，warp-local 空间）上唇上移/下唇下移/嘴角内收，抛物线权重（中央最大）
  - cmo3 修改：新 CFormGuid #1030 (note=mouth_open) + 新 KeyformBindingSource #1031（绑 ParamMouthOpenY #798, key=1.0）+ KeyformGridSource #414 加 KeyformOnGrid/keyformBindings + keyforms count 1→2
  - 验证脚本 verify_fixed16.py：9 项结构检查全 OK、嘴部顶点位移 0.0078、非嘴部零改动
  - **待用户在 Editor 拖 Mouth Open 参数视觉验证**
  - 踩坑记录：①keyforms carray_list 插入必须锚定 mouth #416（全局第一个 count=1 列表不是 mouth）；②count=272 是 float 数=136 顶点；③CArtMeshForm 的 positions 是 warp-local 绝对坐标（非偏移）
- cmo3 关键结构知识（供后续阶段复用）：
  - 参数表：CParameterSource/CParameterId idstr/CParameterGuid ref；ParamMouthOpenY=#798 (0-1, def 0)、ParamMouthForm=#1015 (-1-1)
  - mouth ArtMesh #416：136 顶点（272 float），前 99 顶点是 seethrough 噪声碎片（不动），嘴部轮廓顶点 99-114
  - keyform 绑定链：KeyformGridSource.keyformsOnGrid[KeyformOnGrid{accessKey{KeyOnParameter{binding ref, keyIndex}}, keyformGuid ref}] + keyformBindings[ref] + KeyformBindingSource{parameterGuid, keys[f]}
  - 新对象分配：xs.id #1030+/xs.idx 5896+（max idx=5895, max id=1029），uuid 用 uuid4
  - mouth 区域锚定：xml.find('<CArtMeshSource xs.id="#416"') 到其后第一个 </CArtMeshSource>
- 下一步：Editor 验证张嘴 → 阶段 2 口腔模板 ArtMesh（任务 #48）

### 关键进展与转折（2026-08-19 晚，重要）
- **fixed17 Editor 验证失败**：嘴下拉不张开、头掉在脚下、headwear 反向——全部问题仍在
- **决定性 diff 结论**：Editor 打开另存（fixed17_editor）后逐字节对比——31 个 mesh 全部 positions 数组零变化、mouth keyform 数据原样保留 → **Editor 没有重烘焙任何数据，fixed 系列的脚本修复在 Editor 渲染坐标系里本来就无效**（之前"修复有效"只在 moc3→Demo→P4 链路验证过）
- **头掉根因**：FaceRotation origin = (43,-1033)（fixed2 写入的"下巴"值）在 Editor 渲染里指向画布外远点，头绕它旋转视觉上就是掉下来
- **坐标系一致性是最大障碍**：同一 cmo3 里 ArtMesh 顶点（warp-local 0..1）、deformer origin（模型空间负值）等多套空间混杂，脚本修改在 Editor/导出/SDK 三路径表现不一致，无法可视化验证
- **用户决策**：不再盲目试错，整理技术咨询文档向老师请教（docs\数智方舟_Live2D自动化技术咨询.md + 微信文案已给用户）
- 请教要点：①cmo3 坐标空间定义/Editor 兼容化 ②批量 Live2D 嘴部自动化行业做法 ③比 stretchystudio 更成熟的自动绑骨方案 ④批量质检
- 并行线：同学手动绑骨模板工程（骨架/嘴部标准参照）继续等
- 待老师反馈后继续：路线 A 阶段 2 口腔模板（不依赖坐标系问题，可独立推进）

### 重大发现（2026-08-20）：AutoLive2d + Cubism 5.4 External API
- **AutoLive2d**（E:\Passport\source\live2d\AutoLive2d\AutoLive2d-main，用户已下载）：
  - 自研 Live2D 风格运行时（非 Cubism 导出器），但**自动绑定算法是现成参照**：统一 0..1 归一化坐标（无 stretchystudio 多空间混乱）、每层一个规则网格+参数 keyform（非 157 warp）、支点规则（手臂在肩根）、**嘴张开=围绕嘴中心缩放+抑制垂直平移**（验收清单原文 "scales around its fixed face position without vertical drift"——正是 fixed17 下拉 bug 的答案，已修正）、头 X/Y=椭圆壳投影（无需 FaceParallax 网格）、头 Z=绕颈支点平面旋转、虹膜眼眶裁剪
  - 源码关键文件：src/lib/mesh.ts（网格生成）、classify.ts（图层语义分类）、deform3d.ts（变形）、defaultPivotForKind（支点规则）
- **Cubism Editor 5.4 alpha1 外部集成 API**（AutoLive2d 的 docs/cubism-5.4-alpha1-external-api.zh-CN.md 已存）：
  - WebSocket ws://127.0.0.1:22033，JSON 协议，EditBegin/EditEnd 事务式编辑
  - 能：创建/删除参数、参数组、关键形槽位、Part、旋转/弯曲变形器、重组层级、SetParameterValues 驱动参数、**GetObject 读取物体坐标**（可反测 Editor 坐标约定！）、**NotifyMocFileExported 自动导出 moc3**
  - 不能：创建 ArtMesh、写顶点/UV、写 Warp 控制点（顶点数据仍需 cmo3 脚本注入）
  - 三合一路线：AutoLive2d 算法（绑定规则）→ cmo3 脚本（顶点/keyform 注入）→ Cubism 5.4 API（Editor 结构操作保证兼容 + 坐标反测 + 自动导出）
- 嘴 keyform 已按 AutoLive2d 规则修正：对称 50/50 分离 + FIX_CENTER（嘴中心严格不动，实测 drift=0.000000），重新生成 fixed17（3181290 bytes）
- **AI 张嘴图流水线已就绪**：网页工具 E:\Passport\source\live2d\mouth_inpaint_web\app.py（Flask，127.0.0.1:7860，框选蒙版+提示词+denoise+种子），AOM3 模型 E:\Passport\source\live2d\models\AOM3_v3.0.safetensors（2.1GB），SD15 config 在 models\sd15_cfg（ModelScope 下载），transformers 4.46.3 + diffusers 0.31.0 版本组合已调通，8GB 显存 fp16 跑通（21 步 2 秒）
- 关键参数经验：denoise 0.6、蒙版比例 0.45（上下文大框+蒙版中心小块，防瞳孔变色）、负面词加 changed eye color
- 待办：用户 Editor 验证 fixed17 嘴效果 → AI 张嘴图 → seethrough 分割 → 12 节点绑定 → 修 AngleXYZ（参考 fixed2）

## 七、决策记录（时间倒序）

- 2026-09-15：RH 头饰 SAM 分割"0px"根因定位——**平台侧工作流被改坏,服务器代码零责任**。手动重放一次完整调用:提交 payload(nodeId 1=text='horns'、5=image=fileName)正常、RH 接受并返回 taskId;任务在 ComfyUI 执行时失败,failedReason 完整 JSON 给出决定性证据:① 失败节点是 `ComfyUI-Easy-Sam3/nodes.py` 的 `easy sam3ImageSegmentation`(node_id 7),而服务器 9-03 导出的 headwear_sam_workflow.json 还是 GroundingDinoSAMSegment——平台图已被编辑替换;② `"current_inputs": "{}"`——节点执行瞬间输入为空对象,text/image 连线断了。即:ai-app 对外输入定义未动(提交照常被接受),但工作流图内部 SAM3 节点悬空,报 "At least one prompt must be provided"。与用户"昨天开始失败"吻合=有人昨天在 RH 平台改了图。修复只能由 RH 账号持有者(资源组同学)在平台把 text/image 重新连线到 SAM3 节点并**重新发布 API**;服务器 runninghub_client.py/rh_sam_engine.py 无需改动。排查手段留存:/tmp/rh_debug.py(打印提交 payload+最终响应完整 JSON)。
- 2026-09-16(续):**Key 池共享通知 + 30 分钟在线窗口 + 88 心跳**。用户拍板:在线窗口 5→30 分钟(ONLINE_WINDOW_MIN=30,任何 88/7861 页面操作都算活着,30 分钟无操作强制离线);点退出登录立即清心跳(88 /admin/api/logout、admin.html doLogout、login_box.js logout 三处调用);88 后台加心跳(_touch_rh 统一走 rh_key_pool.touch_owner,修"只登录 88 被误判离线")。共享通知(用户需求闭环):成员离线→上线瞬间,touch_owner 检测 was_offline,若 Key 正被他人借用(rh_key_runs owner≠本人且运行中),双向发 rh_notices 表通知——借用者收到「成员XX已上线,任务结束后该Key将释放归还」,上线者收到「你的Key正在被XX共享使用,预计约X分钟后释放(按 KIND_AVG_SEC 类型平均时长减已运行时间估算),期间新任务自动走别的Key」;10 分钟内同借用对不重复;展示=7861 总页横幅+88 后台横幅(30s 轮询,/rh/notices GET+POST read)。4 项端到端测试全过(双向通知/估算分钟/去重)。用户报:下载SD卡与自动导入云端仓库"根本不行"。① /export/<name> 500 UnboundLocalError:export_device 开头 `_char = _u(CHAR_DIR, request)` 引用 request,但函数内中部 `from flask import send_file, after_this_request, request` 使 request 成为局部变量(模块顶部第 6 行其实已 import)——删函数内 import 的 request 即修。② /repo/import 404"产物不存在":cloud_repo._product_root 按 uid 隔离(base/u<uid>),但静图转 PPD 产物写在共享根(characters/根)且 characters/u1 非空(有旧产物),目录空判断的回退不生效 → 改为产物名级回退:新增 _product_locate(kind, uid, name=None) 个人目录优先、产物名不在个人目录回退共享根;repo_products 列表=个人∪共享去重(openRepoPicker 能看到共享产物);repo_import 用 _product_locate 定位 src。实测:export 200(8.8MB zip)、products 合并列表含 Ascalon2、import 200(25 文件)。sim.js 对 anims.json/forms.json/bg.raw 已有 try/catch 容错,控制台 404 是可选文件噪音,非故障。7861 重启需逐次用户批准(权限分类器强制,记忆规则同步)。归属链路:80 反代 spine2ppd_proxy 解析 token 注入 X-Szfz-User 头(app_cloud.py)→ 8088 do_POST 读头传 import_character(value, user)→ 落位公共库时写角色目录 .owner(local_server.py);88 submit 校验:owner 存在且非本人且非 admin → 403「该角色由 xx 制作」,无归属旧角色首个提交者成为归属人,admin 豁免;summary roles 带 owner,验收树显示 👤 制作者。声音分类:voices_map 键 = ip/voc/star/name(IP 一级,默认 Arknights,扩展其他 IP 直接填新名字),wav 落 voices_dir/<ip>/<voc>/<star>/<name>.wav(_do_clone_admin 拆 map_key 定 vdir);列表返回 cats 树+unclassified(旧平铺音色归未分类)+平铺 voices(兼容角色下拉);删除改 POST /admin/api/voices/delete(分类 key 含 / 无法走 {name} 路由);新接口 /admin/api/ops/cats(operators 表 DISTINCT voc/star,音色表单下拉);前端音色页表单加 IP/职业/稀有度,列表三级树渲染。验证 11 项全过(他人角色 403/豁免/首提归属/summary owner/cats 数据/树结构/旧音色未分类/上传分类链路/删除/反代)。服务重启:88+80+8088(80 重启命令 sudo setsid sh -c 'SZ_RSC_SD=... PORT=80 python3 /opt/szfz/app_cloud.py >> app_cloud.log';8088 必须 SZ_RSC_SD env)。用户需求:资源组成员面板的「验收」改为「提交给管理员」,提交后角色在公共仓库待验收并通知管理员,验收权收归管理员。设计(用户确认:仅状态标记,不复制文件——角色本就落位公共库;通知=88 后台站内角标):状态流转 制作中(无标记)→ 待验收(.submitted,内容=提交人+时间)→ 已验收(.accepted + OSS 打包)。后端 admin_handler.py:新增 handle_agents_submit(仅 resource/admin,已验收拒绝重复提交)、handle_agents_notices(仅 admin,扫公共库 .submitted 无 .accepted,按时间倒序)、_user_role helper(username→role 走 agent_manager.list_users);handle_agents_accept 权限收紧为仅 admin(此前成员能给自己验收!)+ 验收时删 .submitted;summary roles 加 submitted 字段;路由 /admin/api/agents/submit|notices。前端 admin.html:ME_ROLE 全局变量(登录时存);验收树按钮按角色渲染(admin=验收/取消验收,resource=提交给管理员/已提交⏳/已验收✅);管理员角色管理页顶部「🔔 待验收 N」通知条(chip 点击直接验收);refreshAgents 联动刷新通知。测试:12 项端到端全过(user 403、resource 提交/验收 403、通知出现与消失、重复提交 400;xmmy 临时改 resource 测试后已改回)。已知边界:公共库角色无归属信息,成员间可互相提交他人未验收角色(团队信任,后续需要可加归属)。用户需求:每号并发 3,资源组加入后需要更高的整体并行——成员离线时其 Key 可被借用。架构:新模块 /opt/szfz/rh_key_pool.py(pick_and_acquire/release/touch_owner/pool_status,9 项单测全过);rh_keys 表扩展 share(默认 1 可关)/last_seen;新表 rh_key_runs(每 Key 并发计数,孤儿 3h 清理)。调度:自己优先 → 借离线成员(5 分钟无活动=离线,最久未活动优先,share=1)→ 虚拟 key_uid=0(服务器默认 Key,同样 3 并发登记)→ 全满返回 (None,None) 由 RH 排队。软锁语义:拥有者上线(touch 心跳)其 Key 立即从可借池摘除;已提交的 RH 任务不可取消、跑完自然归还。接线:workflow.py(拆图 run/rerun worker try/finally release、嘴部 inpaint)、headwear_pick.py(extract/inpaint_gap,proc 按 key 缓存 _procs dict)、app.py(auth_gate 放行后 touch 心跳,GET /rh/key 加 username+pool,POST 支持 share 开关)、home.html(Key 池卡片:填 Key、共享复选框、池状态列表)。坑:① try/finally/except 顺序语法错误(Python 要求 except 在 finally 前)② 测试造数据误用 sqlite datetime('now')=UTC 与模块本地时间比较,时区混用致误判离线——生产路径全由 touch_owner 写本地时间,无此问题 ③ heredoc 嵌套 bash 双引号会吃掉 Python 字符串引号,复杂补丁一律本地写文件 scp 上传。端点:7861 重启注入 RH_SAM_CUT_ENDPOINT=2099706001816551426(用户新工作流)+RH_SAM_CUT_ENDPOINT_TYPE=workflow。用户在项目根目录找到两个工作流 JSON(`SAM-3语义分割抠图 - 接入SD-PPP实现 自动抠图选择主体.json` 9-03 版与 ` (1).json` 今天下载版),diff 结果:唯一差异是节点画布坐标——图根本没被改。该工作流文本链路 = CR Text(1) → `DeepTranslatorTextNode`(4,service=GoogleTranslator,auto→english)→ easy showAnything(2) → SAM3 prompt(7)。9-14 起 GoogleTranslator(免 key 调 Google 免费接口)在 RH 云失效:翻译节点执行"成功"但输出空(不抛异常,故 traceback 只显示 SAM3 的 ValueError)→ SAM3 收空 prompt 报 "At least one prompt must be provided"。**修复:用户自己重建工作流(只改翻译部分)并发布为新 workflow 端点 2099706001816551426**,服务器实测通过(30s/12 币,mask 4922px)。服务器切换:runninghub_client.py 的 `SAM_CUT_ENDPOINT_TYPE` 加环境变量支持(RH_SAM_CUT_ENDPOINT_TYPE,默认 ai-app);7861 重启注入 `RH_SAM_CUT_ENDPOINT=2099706001816551426` + `RH_SAM_CUT_ENDPOINT_TYPE=workflow`。7861 重启必须保留的 env:PPD_HOST=0.0.0.0、SPINE2PPD_TOOLS=/opt/szfz/tools/spine2ppd、SPINE_IMPORTS=/opt/szfz/sdcard/Arknights/spine_imports、SZ_RSC_SD=/opt/szfz/sdcard/Arknights/main/operator。
- 2026-08-27：Agent 遥控链路全链路实测通过 + 完成通知注入（阶段 3）。实测证据：设备语音"帮我做一个扫雷游戏" → LLM 调 agent.start_task（"IMPORTANT: 不要拒绝"工具描述加固后不再被人设拒绝；此前凯尔希人设拒绝 2048："罗德岛不执行未经核验的娱乐程序"）→ 设备 HTTP 桥 → claude -p 后台执行 561 秒（权限修复：headless -p 无 UI 弹窗 → 写文件默认被拒"尚未授予写入权限" → 桥加 --dangerously-skip-permissions 全放行，安全性=用户语音发起+桥 token 鉴权）→ 真写文件（minesweeper.html 25KB，14 项逻辑测试全过）→ 弹窗带摘要 + task_output.txt。完成通知注入（阶段 3 最后一环）：设备端 agent_evt 轮询任务（3s GET /agent/events，since 游标；Alert 提示音屏幕保底 + WakeWordInvoke 注入）。实验结论：注入**可行**（坏 JSON 都能触发服务器开 LLM 新回合，日志见 LLM 连调 get_device_status）但暴露两 bug：① 重启后 since=0 重放历史事件 → 未唤醒就进入对话；② 注入文本含换行未转义 → SendWakeWordDetected（protocol.cc:44 无 JSON 转义）构造的 JSON 被拆两半 → 服务器 "Missing message type"。修复：桥 /agent/events since=-1 只给当前 seq 不回放；设备端注入文本 JsonEscape+截断 100 字、Alert 截断 200 字、游标以响应 seq 推进。设备烧录前双实例坑：桥 7863 曾被我后台测试实例占用（Windows SO_REUSEADDR 双 LISTENING，用户窗口显示横幅但请求被另一实例收走，弹窗来自后台实例）——教训：改桥后必须确保单实例。
- 2026-08-27：设备语音遥控电脑 Agent 方案 v3 定稿并实施阶段 1+2。架构（用户决策）：**设备端本地 MCP 工具**（mcp_server.cc AddTool 三个工具 agent.start_task/get_progress/stop_task，回调同步 HTTP 电脑桥）——LLM（服务器端）通过 xiaozhi 原生 MCP 通道调设备工具，设备转发电脑桥，**服务器端零改动**（无论自建/公服）；MCP 只做同步调用，"完成通知"走带外（电脑桥弹窗+UDP/轮询事件→设备注入，阶段 3 实验选型）。电脑桥（E:\Passport\source\live2d\agent_bridge.py，7863 监听 0.0.0.0）：Flask HTTP + claude -p 子进程管理（--output-format json 抓 session_id 存 automode_session.txt，automode 续跑 --resume；**Windows 下 CreateProcess 不能跑 npm .cmd shim → resolve_claude() 优先 node+cli.js、回退 shell=True**）+ automode_status.json 状态文件（automode 会话按 prompt 要求写进度）+ tkinter 弹窗 + token 鉴权（agent_token.txt 自动生成）。已验证：start→claude -p（走用户 DeepSeek 配置）→完成事件 seq/弹窗/状态全链路 OK。设备端：agent_tools.{h,cc}（Http 接口来自 esp-ml307 组件 network_interface，SetTimeout 5s 同步调用）+ Kconfig AGENT_BRIDGE_URL/TOKEN + CMakeLists 显式 SOURCES 追加（main 顶层非 glob）。automode 任务描述 automode_prompt.txt（批量 wiki 导入干员模板，用户可编辑）+ automode_operators.txt 干员列表。待办：用户 menuconfig 填 IP/token 编译烧录 → 阶段 3 语音联调（完成通知注入：SendWakeWordDetected listen/detect text 实验，三条兜底路径见方案 §4）。
- 2026-08-27：电脑重启服务恢复——一键启动 E:\Passport\source\live2d\启动资源服务.bat：paperdoll_sim 7861（仿真器/转换器/workflow/tune/arkitech/spine2ppd+wiki 导入）、mouth_inpaint_web 7862（AI 张嘴图，注意端口已从 7860 改为 7862 默认参数）、agent_bridge 7863（阶段 1 起加）。系统 Python 3.10 + Flask 3.1.1 全局可用（无 venv）。
- 2026-08-27：PRTS wiki 干员 Spine 资源自动爬取（逆向数据源）。链路：角色名 → `https://m.prts.wiki/api.php?action=opensearch`（主站 prts.wiki 有 Tengine 反爬，curl/Python 一律 403——UA/TLS 指纹+频率风控，m 子域放行但高频请求也会被锁；Claude WebFetch 出口不受限）→ `m.prts.wiki/index.php?title=<页面名>&action=raw` 取 wikitext `|干员id=char_XXX`（{{spineId}} 模板 → Widget:SpineViewer，干员id 由 cargo 表 chara 查询）→ `https://torappu.prts.wiki/assets/char_spine/<charid>/meta.json`（阿里云 OSS 无风控；曾误试 static.prts.wiki/spine/ 路径 NoSuchKey，实际端点从 SpineViewer source map 提取——common.js 常量 TORAPPU_ENDPOINT）→ meta.skin["默认"].{正面,背面,基建}.file（如 defaultskin/front/char_003_kalts）→ `<prefix><file>.skel/.atlas/.png` 下载（骨骼 404 回退 .json）。实现：paperdoll_sim/wiki_fetch.py（search_operators/get_meta/download_set，charId 直输兼容）+ app.py 两路由（/spine2ppd/wiki/search 候选列表、/spine2ppd/wiki/import 三套串行下载转换，每套独立子目录——正/背面文件同名如 char_003_kalts.* 混放会转错）+ spine2ppd.html 搜索导入区（候选按钮→导入→三场景预览按钮）。转换核心重构为 _convert_spine_dir 复用。端到端：凯尔希 三套成功（正面 83 层 4 动作/背面 31 层 3 动作/基建 87 层 5 动作）。已知边界：英文代号（Amiya）搜不到（opensearch 对英文支持差），拟干员一览页全量中英映射缓存解决（待 m 子域风控冷却）；junction 冲突备份 = os.rename 改名 junction 本身（数据无损）。
- 2026-08-27：基建「散的」真正根因——下载渠道把图集 png 缩了：基建 png 344x344 vs atlas 声明 516x516（2/3 缩放，region 坐标按 516 设计，uv 归一化基准错误 → x>344 的区域渲染为空 → 20 个槽 skip、只剩 18 层）。验证：全部区域 x0.667 后落回图集（缩放完整）。修复：三个渲染脚本（render_layers/render_ref/render_anim_frames）加载 png 时对比 atlas 声明尺寸，实际更小则放大到声明尺寸。配套：exportLayer 的 Default 姿势渲染为空回退 setup 姿势（基建 Default 动画把部分链 scale 归零隐藏）+ export_anims 读 scene.json visible 判定 hidden。修复后基建 43 层（35 槽+8 变体）、5 动画，静态合成 vs 官方整身 diff 13.12（正面历史 ~12）。通用修复同样覆盖背面等被缩图的模型。
- 2026-08-27：基建"乱七八糟"根因——用户 zip 同时含 char_002_amiya.skel（背面）与 build_char_002_amiya.skel（基建），web 转换静默取第一个 .skel 转了背面。修复：多 .skel 检测 → 前端候选按钮选择（chosen_skel 参数），atlas/png 按 skel 同 stem 匹配。结构确认：基建 skel 与正面同源（F_ 前缀、F_Head/F_Eye/F_Mouth 齐、动画 Default/Interact/Move/Relax/Sit/Sleep）；背面 skel 是 B_ 前缀 38 槽、无眼/嘴（group/special 识别失败也是背面转换乱的原因之一）。仿真器横屏方向：设备端 LVGL 无旋转（DISPLAY_SWAP_XY false 竖帧直通，用户横持=物理旋转）→ 无法从代码确定方向，改为"旋转 90°"按钮 4 态循环（0=顺时针90 1=180 2=逆时针90 3=不转）+ localStorage 记忆，用户一键对齐设备端。
- 2026-08-27：spine2ppd 同名冲突策略——characters/<名> 已存在真实目录且含 scene.json（旧转换产物）→ 自动备份为 <名>_bak 后覆盖重转；无法识别的目录才拒绝；冲突检查提前到转换前（曾转完 1-2 分钟才报错）。用户转"阿米娅基建"与旧 PPD 数据同名时即触发。
- 2026-08-27：spine2ppd 上传页支持 zip 压缩包输入（与三文件二选一）：后端解压+递归查找 .skel/.atlas/.png（跳过 __MACOSX、防路径穿越）、子目录内文件自动移到顶层（render_layers 只读顶层）；zip 模式场景名默认取 .skel 文件名。端到端测试通过（嵌套目录 zip → 转换成功）。
- 2026-08-27：横屏预览方向修正（转置 (y,x) 反了→顺时针 90° setTransform(0,1,-1,0,799,0)，触摸逆映射 (Y,799-X)——设备端横持显示同方向）。Web 端 Spine→PPD 流程：/spine2ppd 上传页（拖/选 .skel/.atlas/.png）→ POST /spine2ppd/convert（node 子进程跑 render_layers+export_anims，输出 E:\虚拟SD卡\spine_imports\<名>，junction 挂仿真器）→ 自动跳转 /?char=<名>；仿真器"下载 SD 卡包"对 landscape 场景走 /spine2ppd/download/<名>（zip 打包原始 PPD 数据）。
- 2026-08-27：右鬓发幅度错位分析——F_R_Hair 挂 F_Head 骨骼（无独立骨骼动画），Start 飞入阶段 PCA 主轴 -84.6° vs 骨骼角 -23°（尖端弯曲滞后带偏主轴）；尝试 clamp PCA 与骨骼偏差 ≤25°（f4 区域 diff 68→77 恶化）→ 回退：PCA 主轴即官方渲染内容的实际形状方向，clamp 把发根也带偏；设备端整块刚性旋转的观感差异来自尖端弯曲无法表达（仿射边界）+ sin 包络渐入渐出。仿真器横屏预览：scene.landscape → 画布 800×480、CTM setTransform(0,1,1,0,0,0)（场景(x,y)→屏幕(y,x) 头朝上）、触摸 evtToScene 逆映射、HUD 移出变换。convert_ppd.py 一键转换器：render_layers(--landscape)+export_anims+junction 挂仿真器+index 注册+预览 URL 提示，确认后目录即 PPD 导出（冒烟测试通过）。
- 2026-08-27：Start 眼睛角度根因 = **Spine 附件切换（换附件，非换链）**：F_Eye 槽 3 附件（F_Eye 睁眼 43x17 / F_Eye_2 半闭 / F_Eye_3 闭眼线 40x7），Start t=0.267-0.633 切到 F_Eye_3——转换器对附件切换无感知，继续用睁眼纹理按闭眼线几何摆放。修复：render_layers 导出变体附件为隐藏层（名=附件名+'_v'，Default 姿势渲染；'_v' 防与换链槽名重名——F_Emoticon_2/F_Scarf 既是槽名又是附件名，曾同名导致 frames 轨道键冲突、长度翻倍）；export_anims 生成变体轨道（vis 互斥：主层变体区间 vis=0、变体层 vis=1+几何，基准=Default 姿势下该附件）；换链槽多附件（F_L_Arm_2 等 26 个）未逐附件导层（layerGeom 无层则回退单层纹理近似，设备端 tracks[64] 余量仅剩 4）。仿真器矩阵修复：设备端 mat_mul 是 canvas 后乘语义（注释明写）→ ctx 序 = translate(x+w/2+adx)→rotate(adrot)→translate(-w/2)，曾把层原点 translate 重复加两次（R 内一次 R 外一次）→ 零件四散；实验确认 Canvas rotate 正角=视觉顺时针、PIL 正角=逆时针（synth_diff 用 -adrot 与之自洽）。
- 2026-08-27：Spine→PPD 动画头发跟随修复——mesh 层放弃 region 四角近似质心+骨骼矩阵角，改真实顶点几何（MeshAttachment.computeWorldVertices）：质心=顶点均值（头部运动时权重形变的实际位移被捕获）、旋转=PCA 协方差主轴角（λ2/λ1<0.5 才用，圆形退化骨骼角）；无向主轴沿时间轴连续展开（每帧与上一帧取最近方向，首帧对齐基准——曾每帧独立对齐基准导致大幅甩动 >90° 被错 wrap 成回摆 165°）；隐藏帧（vis=0）的 dx/dy/rot 填最近可见帧值（曾填 0，换链激活瞬间 drot 从 0 插值到姿势角产生甩动伪影）。合成 diff（新脚本 tools/spine2ppd/synth_diff.py + render_anim_frames.mjs，mean|dRGB|/3）：Stun 14.67→12.03、Die 14.5→11.95、Start 14.42→12.23、Attack 13.17。PC 仿真器 paperdoll_sim 已接入 anims.json 播放（照抄设备端 draw_band 数学：线性插值×sin 包络+绕 bbox 中心旋转平移+vis 轨道；raw 层 u16 头+RGBA→createImageBitmap、bg.raw RGB565），Amiya_Q junction 进 static/characters（E:\虚拟SD卡 PPD_Q 实时同步），浏览器 http://127.0.0.1:7861/ 选 Amiya_Q 即看动作。
- 2026-08-19：决定放弃本地 stretchystudio master 修复（代码分叉、master 半成品）；同学做模板工程并行；主线转为 THA 嘴部 keyform 方案（已审核通过）
- 2026-08-19 凌晨：本地 stretchystudio 修复尝试（bbox 分位数/facePivot/骨架连通块）方向正确但 master 有回归；血泪教训：facePivot 修改破坏 Editor 自动坐标重算
- 2026-08-18：fixed2→fixed14 迭代修 Mon3tr（FaceRotation 下巴/脖子锁定有效；FaceParallax 位移场 6 种模型全部不满意，判定为坏数据打补丁的死路）
- 2026-08-18：发现 stretchystudio moc3 顶点中心原点（±0.5）问题 → Demo DumpGeometry 加 centerOrigin 检测修复（已验证）
- 2026-08-17：Mon3tr 从 stretchystudio+Editor 导出成功（moc3 v5.0）；发现 moc3 顶点损坏、头部链 pivot 错误
