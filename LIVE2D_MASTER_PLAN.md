# 数智方舟 Live2D 项目总计划（MASTER PLAN）

> **本文档是项目唯一事实源。上下文超限后，下一个 Claude 必须先读本文档再动手。**
> 配套文档：`LIVE2D_WORKFLOW.md`（设备端角色导入流程）、`auto_live2d_log.md`（技术决策日志，按时间倒序）、`docs\数智方舟_Live2D自动化技术咨询.md`（发给老师的咨询文档）、**`PAPERDOLL_PLAN.md`（纸偶替代路线：方案+试验记录+进度，与 Live2D 招募线并行）**。

---

## 一、项目目标

《数智方舟》项目：为 **300+ 明日方舟干员** 从静态立绘**自动**生成 Live2D 模型，显示在 ESP32-P4 通行证设备上（480×800 MIPI-DSI 屏、16MB Flash、PSRAM HEX 模式），并支持与 AI 聊天机器人交互（情绪→表情、事件→动作）。团队**没有专业绑骨美术**。

## 二、已验证事实基线（禁止重新验证）

1. **设备端链路（moc3→P4）已跑通很多次**：见 `LIVE2D_WORKFLOW.md`——模型放 Demo Resources → 编译运行 → 按 L 导出 model.l2d、按 K 导出 keyforms.bin → 拷入 P4。用户原话："不用执行从错误的moc3到p4了，这个流程我们已经跑通很多次了"。
2. **P4 端渲染器 `main/apps/live2d/lv2_render.c`**：纯 C 软光栅，RGBA8888 + Alpha 混合 + 遮罩裁剪 + Keyform 变形，全部 PSRAM 分配，**多纹理 per-drawable 已支持（tex_index v2）**。已实现实时渲染 + 待机动画。
3. **P4 端 11 参数通道**（lv2_render.c:280 expr_override）：AngleX/Y/Z、EyeLOpen/R、MouthOpenY、MouthForm、EyeBallX/Y、EyeLSmile/RSmile。BodyAngleX/Y/Z 与 Breath 不驱动。
4. **用户红线（不可违反）**：① 不要重新验证 moc3→P4；② 不要在 P4 端叠加贴图，用 Live2D 就纯 Live2D；③ 停止盲目试错迭代；④ 不浪费 token 猜测，先给结论再动作。
5. **LivePortrait 路线已死（结论性）**：立绘嘴部仅 15×5px，运动场只有 +25px 均匀下移，嘴唇 gap=0 无法分离——不是调参问题，是输入分辨率问题。
6. **脚本修 cmo3 的教训**：Editor 打开另存后逐字节 diff——31 个 mesh 的 positions 全部零变化 → **Editor 不重烘焙，但脚本改的数据在 Editor 渲染坐标系里表现不一致**。头掉根因：FaceRotation origin 被写成 (43,-1033)，画布外远点。
7. **固定基线文件**：`E:\Passport\source\live2d\M3psd\modelaaab_fixed2.cmo3`——唯一用户确认"位置对了"的版本（AngleXY 位移场移植成功）。

## 三、最终架构决策（2026-08-21 确定，QQ秀 架构三原则）

腾讯 QQ秀 无瑕疵的真相（已调研）：不开源、私有格式（3D 厘米秀=glTF+face.json blendshape、超级QQ秀=UE4）、**没有任何技术可搬**，但其架构原则可抄：

1. **AI 只做低方差活**（识别/匹配/参数回归），不让 AI 生成结构——坏输出在结构上不存在
2. **运行时确定性渲染**：设备端零概率，概率全部集中在 PC 生产侧
3. **人在环摊销到资产层**：每角色人工修一次 → 之后永久确定性有效

我们的设备端（cmo3 + lv2_render + 11 参数 + 22 表情映射）在架构上**就是 2D QQ秀 的同构物**，无需接任何 QQ秀 组件。

**生产侧概率管理方案（核心）**：质检门 + 自动重试 + 人工兜底 + 修复反哺规则库。
关键认知：之前踩的坑一大半是**确定性 bug**（intc JSON、PSD 尺寸、坐标偏移），修一次永久解决；真正的概率失败（张嘴质量、拆图噪声）才有质检器的用武之地。

## 四、具体实现路径

### 阶段 0（当前）：人工兜底跑通单角色（Mon3tr）

- 招募 Live2D 接单人。任务说明已写好：`E:\Passport\source\live2d\M3psd\接单任务说明.txt`（headwear 重建网格+重绑、五官关键形调整、11 参数验收清单、下半身不动、交付修复后 cmo3）
- 群发/私聊文案：`docs\群发_Live2D群.txt`、`docs\私聊_Live2D群友.txt`
- **收到修复文件后最重要的一步：diff 学习**——对比人工修复 vs stretchystudio 自动生成的差异，总结正确规则，反哺流水线。这是此前欠用户的取证承诺。

### 阶段 1：生产流水线质检门（三个概率模型各配一个质检器）

| 阶段 | 失败模式 | 质检方式 | 处理 |
|---|---|---|---|
| SD 张嘴重绘 | 嘴没张开/崩坏/变色 | **LivePortrait 关键点 mouth-gap 检测**（确定性判定，gap≈0 即失败，不需要 LLM） | 自动换 seed 重试 N 次 → 仍失败标人工 |
| see-through 拆图 | bbox 爆炸、层错乱、部件丢失 | 确定性检查：bbox 尺寸、层数、alpha 覆盖率 | 同上 |
| 绑骨产物 cmo3 | 转头部件脱节/穿模 | P4 11 参数极值测试（程序判定网格爆掉/穿模） | 重绑或标人工 |

质检器全部做成独立脚本、可插拔。**SD 张嘴质检器是第一步**：接在 `E:\Passport\source\live2d\mouth_inpaint_web\app.py`（Flask 127.0.0.1:7860）流程后自动判定。

### 阶段 2：规则库沉淀（滚雪球）

每修一个角色 → diff 结果写规则库 → 修复脚本化 → 人工介入率随角色数递减收敛。预计修 10~20 个角色后多数失败模式可程序化修复。

### 阶段 3：300+ 角色批量

流水线全自动跑批 + 异常队列人工处理。

### 设备端增量（可选，QQ秀 式交互，纯内容添加零引擎改动）

- **动作片段库**：拍一拍/关键词彩蛋 → 动作时间轴播放。基础设施（待机动画）已存在，加动作 = 加内容（每个片段几 KB~几十 KB，Flash 轻松容纳）
- **事件映射表**：LLM 情绪标签→11 参数（已有）；触摸/按键→反应动画；聊天关键词→彩蛋动作
- **换装**：同角色换皮肤 = 换贴图文件（多纹理已支持；512×512 贴图 1MB/张，16MB Flash 可装 8~10 套）

## 五、当前进度快照（2026-08-21）

- **Mon3tr（modelaaab）状态**：fixed2 = 唯一验证基线。fixed3（headwear 重绑 #691）失败、fixed4（neck 锁定）有效、fixed5（嘴 keyform 锁定）被用户否决、fixed6（FaceParallax 重建）头缩到极小——**全部作废**。等接单人人工修复。
- **遗留问题清单**（给接单人）：① headwear 不跟随头/头发 ② 嘴 AngleX 转头时跟随慢 ③ neck 层级在 AngleX -15°~-16° 交换 ④ 嘴 form/open 无法变化
- **see-through 重测**：输出 bug 已全部修完（np.intc JSON 序列化、PSDImage 宽高交换、alpha 阈值 0.02），等待用户重测结果
- **AI 张嘴工具就绪**：mouth_inpaint_web（Flask）+ AOM3_v3.0.safetensors（E:\Passport\source\live2d\models\）+ sd15_cfg，transformers 4.46.3 + diffusers 0.31.0 组合已调通，8GB 显存跑通
- **待老师反馈**：docs\数智方舟_Live2D自动化技术咨询.md（坐标空间定义、批量自动化行业做法、更成熟自动绑骨方案、批量质检）
- **等同学**：手动绑骨模板工程（骨架/嘴部标准参照）
- **Codex CLI 已装好**（0.148.0，device-auth 已登录老师账号）：作为 Claude 深夜涨价的备用。运行前需设代理环境变量（HTTPS_PROXY=http://127.0.0.1:9674，Tyty VPN）。项目根已有 `AGENTS.md`（禁哈希/审计/消融/multi-seed，聚焦开发）。

## 六、关键文件地图

| 路径 | 内容 |
|---|---|
| `E:\Passport\espp4\sparepart\JC4880P443C_I_W\1-Demo\idf_examples\ESP-IDF\xiaozhi-esp32sp1\` | **固件仓库根**（LIVE2D_WORKFLOW.md / auto_live2d_log.md / AGENTS.md / 本文档） |
| `固件仓库\main\apps\live2d\lv2_render.c/.h` | P4 端 Live2D 渲染器（11 参数驱动） |
| `固件仓库\tools\cmo3\` | cmo3 解密/打包工具（docs__live2d-export 系列脚本） |
| `E:\Passport\source\live2d\M3psd\` | 用户重导出的 Mon3tr 工程（modelaaab.cmo3 系列、接单任务说明.txt） |
| `E:\Passport\source\live2d\Mon3tr\` | 分析/修复脚本区（sync_anglexy_v2.py=fixed2 生成器、fix_*.py=已作废的尝试、mouth_keyform_gen.py=几何张嘴 keyform） |
| `E:\Passport\source\live2d\mouth_inpaint_web\app.py` | AI 张嘴重绘网页工具（127.0.0.1:7860） |
| `E:\Passport\source\live2d\models\` | AOM3_v3.0.safetensors + sd15_cfg |
| `E:\Passport\source\live2d\AutoLive2d\AutoLive2d-main\` | AutoLive2d 源码（绑定算法参照：统一 0..1 坐标、嘴张开=绕中心缩放+抑制垂直漂移、椭圆壳头部投影） |
| `AutoLive2d\docs\cubism-5.4-alpha1-external-api.zh-CN.md` | Cubism Editor 5.4 External API 文档（WebSocket 22033，能建参数/变形器/读坐标/自动导出，不能写顶点） |
| `E:\Passport\source\live2d\see-through\`（AutoLive2d\third_party） | see-through 拆图（本地部署，输出 bug 已修） |
| `E:\Passport\source\live2d\strechyStudio\` | stretchystudio 自动绑骨（12 节点→JS 生成全部 deformers；已知问题：多坐标空间混乱、FaceParallax 6×6 网格） |
| `docs\群发_Live2D群.txt` / `docs\私聊_Live2D群友.txt` | 招募文案 |

## 七、交接注意事项（给下一个 Claude）

1. **禁止**：盲目脚本修 cmo3（用户三次否决）；重新验证 moc3→P4；P4 端叠加贴图；在没有人工修复样本前再猜坐标系规则
2. **动作前先给结论**：用户明确不满"浪费 token 的猜测"。每个动作先说明依据和预期结果
3. **交互逻辑定义**：设备端交互 = LLM 情绪标签→11 参数 + 事件（触摸/按键/关键词）→动作片段，全应用层查表，与模型文件格式无关
4. **等待中的外部输入**（任一到达都推进阶段 0）：接单人修复后的 cmo3、老师对咨询文档的反馈、同学的模板工程、see-through 重测结果
5. 项目记忆目录：`C:\Users\HP\.claude\projects\E--Passport-...\memory\`（见 MEMORY.md 索引）
