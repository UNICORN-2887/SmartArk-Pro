# Live2D P4 自研引擎开发日志

> 目标：ESP32-P4 端实时渲染 Live2D Cubism 4/5 模型。
> 技术路线：自研 C 解析器 + 软件光栅 → PPA 合成。
> 创建日期：2026-08-08 | 更新：2026-08-09

---

## 架构

```
SD 卡:  {角色}.moc3 + texture.png + {角色}.model3.json
           ↓
Phase 1: moc3_parser.c ✅ — 解析 .moc3 → 计数/参数名/默认值
Phase 2: deformer.c    🔧 — 参数值 → 仿射变换 → 变形顶点
Phase 3: sw_raster.c   ⏳ — 三角面片 + 纹理采样 → RGB565 帧
Phase 4: lv2_render.c  ⏳ — 集成 PPA 输出到屏幕
```

## 进度

| Phase | 状态 | 关键结果 |
|---|---|---|
| 1 解析器 | ✅ 完成 | 100 params, 126 drawables, 29358 UVs, 66354 posIdx |
| 2 变形器 | 🔧 开发中 | — |
| 3 光栅化 | ⏳ | — |
| 4 渲染集成 | ⏳ | — |

## 关键发现

- **CountInfoTable 不在固定偏移**——SectionOffsetTable (offset 64) 的第一个 uint32 指向它
- hexpat 的 `padding[256] [[no_unique_address]]` 是**虚拟填充**，不占文件空间
- Amiya v5 模型的 CountInfoTable 在 offset 1984

## 文件清单

| 文件 | 大小 | 状态 |
|---|---|---|
| `main/apps/live2d/moc3_parser.h` | 136行 | ✅ |
| `main/apps/live2d/moc3_parser.c` | 200行 | ✅ |
| `main/sd_test.cc` | 测试代码 | ✅ |

---

## 开发日志

### 2026-08-09
- Phase 1 完成！100 params 加载成功
- 发现 .moc3 双层指针结构极复杂（152 pointer table → index tables → data）
- 决定双轨并行：方案 B（PC Demo 导出 .l2d）先跑通，方案 A（P4 自解析）作为后备
- PC Demo 加 `--dump` 模式开发中

### 2026-08-08
- 创建开发日志，搭建基础架构
- moc3_parser 从固定偏移→指针导航，经历 4 次迭代
- 编译验证通过，CMakeLists 挂载
