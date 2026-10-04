#pragma once
/* PPDQ / PPD-Mesh v2 设备端渲染(2026-09-14):
 * 云端编译 mesh.json v2 → 二进制 mesh.ppdq + atlas RGBA raw。
 * 本模块加载并渲染:每帧按 draw 顺序三角形光栅(normal/additive blend)。
 * 存在 mesh.ppdq 时由 pd_render 分流到此;否则旧 PPD 路径不受影响。 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdq_model PdqModel;

/* 加载 <dir>/mesh.ppdq(+<tex>.raw)。无 ppdq/解析失败返回 NULL。 */
PdqModel *pdq_load(const char *dir);
void pdq_free(PdqModel *m);

/* 渲染动画 anim 在 t_ms 时刻的帧到 RGB565 fb(fw×fh)。
 * 不绘制背景(背景由调用方沿用旧路径);角色 draw 列表自覆盖区域。
 * anim=NULL/找不到 → 用 "Relax"(无则第一个动画)循环做待机。 */
void pdq_render(PdqModel *m, uint16_t *fb, int fw, int fh, const char *anim, uint32_t t_ms, bool force_loop);

/* 动画名是否存在(设备端动作列表过滤用) */
int pdq_has_anim(PdqModel *m, const char *anim);

/* 动画名列表(2026-09-17:PDQ 动作按钮直接读 mesh 动画名,免载大体积 anims.json) */
int pdq_anim_count(PdqModel *m);
const char *pdq_anim_name(PdqModel *m, int i);

#ifdef __cplusplus
}
#endif
