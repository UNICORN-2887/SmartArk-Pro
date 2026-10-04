/** Live2D P4 software triangle renderer — .l2d + RGBA8888 texture + alpha blend. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int drawable_count;
    int total_verts, total_indices;
    int*   drawable_vc;
    int*   drawable_ic;
    int*   drawable_vo;
    int*   drawable_io;
    float* positions;
    float* uvs;
    uint16_t* indices;
    int tex_count;          // number of textures (1 or 2)
    int tex_w[4], tex_h[4];
    uint32_t* textures[4];  // RGBA8888 pixels (PSRAM), up to 4 atlases
    int16_t* tex_index;     // [dc] per-drawable texture index (v2 only, NULL=v1)
    int16_t* mask_info;     // [dc] -2=mask, -1=normal, >=0=masked ref
    uint16_t* mask_extra_n; // [dc] number of extra masks per drawable
    int16_t*  mask_extras;  // flat array of extra mask indices
    uint8_t* mask_buf;      // 1-bit mask buffer (temp for v3 extra masks)
    int16_t* mask_slots;    // [dc] mask drawable → slot index (-1 = not a mask)
    uint8_t* mask_bufs[16]; // per-slot mask buffers (independent masks survive)
    // Keyform animation
    int kf_verts, kf_param_count;
    float* kf_base_pos;
    float* kf_offsets;          // dense: single block (Amiya/Theresia)
    // Sparse storage (for large models like Furina):
    uint16_t* kf_sparse[32];    // per-param sparse data (vi:u16|dx:i16|dy:i16 interleaved)
    uint32_t kf_sparse_n[32];   // number of non-zero entries per param
    float kf_param_range[32][3];
    float eye_x, eye_y;    // gaze target (-1..1, 0=center)
    float eye_cx, eye_cy;  // model-specific gaze center offset (zll0: -0.5, 0)
    float eye_gm, eye_vm, eye_vup, eye_vdown;  // gaze Y remap: g=-1→v_up, gm→vm, +1→v_down (0,0,0,0=disabled)
    float shy;             // 1.0 = hair touched, tremble effect
    int expression;        // 0=auto, 1-12=expression preset
    float ppu;             // pixels per unit (model-specific)
} Lv2RenderModel;

bool lv2_load_keyforms(Lv2RenderModel* m, const char* kf_path);
bool lv2_load_keyforms_ex(Lv2RenderModel* m, const char* kf_path, const int* needed, int n_needed);
void lv2_animate(Lv2RenderModel* m, float* out_pos, float time_sec, float eye_x, float eye_y, float shy);
void lv2_render_animated(Lv2RenderModel* m, uint16_t* fb, int fb_w, int fb_h, float t);

Lv2RenderModel* lv2_load(const char* l2d_path, const char* tex0, const char* tex1);
void lv2_render_frame(Lv2RenderModel* m, uint16_t* fb, int fb_w, int fb_h);
void lv2_free_render(Lv2RenderModel* m);

// ── 加载进度回调（加载动画用）：stage 阶段名，percent 0-100（-1=不确定阶段）──
typedef void (*Lv2ProgressCb)(const char* stage, int percent);
void lv2_set_progress_cb(Lv2ProgressCb cb);

#ifdef __cplusplus
}
#endif
