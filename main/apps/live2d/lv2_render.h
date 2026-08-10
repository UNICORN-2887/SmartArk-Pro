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
    int tex_w, tex_h;
    uint32_t* texture;     // RGBA8888 pixels (PSRAM)
    int16_t* mask_info;    // [dc] -2=mask, -1=normal, >=0=masked ref
    uint8_t* mask_buf;     // 1-bit mask buffer
    // Keyform animation
    int kf_verts, kf_param_count;
    float* kf_base_pos;
    float* kf_offsets;
    float kf_param_range[16][3];
} Lv2RenderModel;

bool lv2_load_keyforms(Lv2RenderModel* m, const char* kf_path);
void lv2_animate(Lv2RenderModel* m, float* out_pos, float time_sec);
void lv2_render_animated(Lv2RenderModel* m, uint16_t* fb, int fb_w, int fb_h, float t);

Lv2RenderModel* lv2_load(const char* l2d_path, const char* tex_path);
void lv2_render_frame(Lv2RenderModel* m, uint16_t* fb, int fb_w, int fb_h);
void lv2_free_render(Lv2RenderModel* m);

#ifdef __cplusplus
}
#endif
