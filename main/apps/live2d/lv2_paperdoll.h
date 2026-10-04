/* 纸偶渲染器（sprite 层 + 支点 + 仿射变换，无网格变形）
 *
 * 与 PC 仿真器 static/sim.js 逐行对应：
 *   - 11 参数（AngleX/Y/Z、EyeL/R、MouthOpenY、EyeL/R smile、MouthForm、EyeBallX/Y）
 *   - 全局 z 序逐层绘制；头组全变换；脖子 0.85 阻尼；待机晃动+呼吸
 *   - 脸轴张嘴矩阵 B·S·Bᵀ；统一眨眼；前发眼区打洞（hair_factor）
 *   - 摸头（按住头部 1.4s 包络）；视线追踪（触点位置 → 眼球缓动）
 *   - 待机演示模式（sway/breath/blink/mouth pulse/眼球游走）
 *
 * 数据源：/sdcard/main/operator/<职业>/<星级>/<干员>/PPD/scene.json + <layer>.raw(u16w,u16h+RGBA8888) + eye_mask.raw
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t w, h;
    uint32_t *rgba;      // RGBA8888（PSRAM）
    uint16_t cbx0, cby0, cbx1, cby1;   // 内容 bbox（alpha>8，纹理坐标，含端点）
    bool has_cb;
    uint16_t *span;                    // 每行内容起止表 [min,max]×h（0xFFFF=空行），逐行跨度裁切用
} pd_tex_t;

typedef struct {
    char name[40];
    pd_tex_t tex;
    int16_t x, y;        // 场景画布坐标（左上角）
    uint16_t w, h;
    float cx, cy;        // 层中心
    float bbox[4];       // x0,y0,x1,y1
    float z;
    uint8_t group;       // 0=body 1=head
    uint8_t special;     // 0 无,1 mouth,2 eye-l,3 eye-r,4 mouth-open,5 arm-l,6 arm-r
    bool visible;        // 默认可见性（隐藏槽 false；动作播放时可见性轨道覆盖）
    float bone_px, bone_py;   // 骨骼点在层内坐标(动作旋转中心;缺省=w/2,h/2 兼容旧数据)
    uint32_t prof_us;    // 剖面：累计渲染耗时（μs）
    uint32_t prof_px;    // 剖面：累计绘制像素
    pd_tex_t ppa_tex;    // PPA 预缩放纹理（全身帧 1:1，BGRA 字节序=硬件 ARGB8888）
    int16_t ppa_x, ppa_y;  // 烘焙的屏幕位置（导出时按设备数学精确计算）
    bool has_ppa;
} pd_layer_t;

/* ---- Spine 动画 → PPD 原生动画（anims.json）：每层旋转/位移增量时间轴 ---- */
typedef struct {
    float dx, dy, rot, vis, sx, sy;   // sx/sy 缺省 1，vis>=0.5 渲染
    float a, b, c, d;                 // 可选完整 2x2 仿射矩阵
    uint8_t affine;
} pd_anim_key_t;
typedef struct {
    char name[32];
    float duration;
    bool loop;
    int n_tracks;
    /* 256 ≥ 场景层数上限——曾 128 截断:缄默德克萨斯基建 Sleep 129 条轨道,
       第 129 条的闭眼/睫毛层被丢,闭眼线停在原地。
       256 条 × 每动画 ≈3KB(仅索引,keys 另算),PSRAM 可忽略。 */
    struct { int16_t layer_idx; int16_t n_keys; pd_anim_key_t *keys; } tracks[256];
} pd_anim_t;

typedef struct {
    char name[64];                    // 显示名（UTF-8，仅展示用）
    float head_pivot[2], mouth_pivot[2];
    float face_tilt;                  // 度
    float eye_tilt;                   // 度（眼轴倾角：眨眼压缩沿眼睛竖直方向；缺省=face_tilt）
    float mouth_tilt;                 // 度（嘴轴倾角：张嘴/嘴型沿嘴的垂直方向；缺省=face_tilt）
    uint8_t no_cavity, mouth_compress;
    uint8_t landscape;                 // 横屏布局场景（Q 版互动）：眨眼沿画布 x 轴（视觉竖直）
    float mouth_gain, mouth_floor, mouth_width_open, mouth_open_gain;
    float hair_factor;
    pd_tex_t eye_mask; int16_t em_x, em_y; bool has_mask;
    pd_tex_t hair_punched; bool hair_done;   // 前发打洞缓存
    uint16_t *bg; int bg_w, bg_h;            // 可选背景（<dir>/bg.raw，u16w+u16h+RGB565；无则深色填充）
    /* 长背景（横屏长图偏移渲染，外部注入、不归模型释放）：
       long_bg = 解码槽(RGB565, 行 stride=long_bg_stride 像素), long_bg_w/h = 可见宽高
       （解码 pad 区在 long_bg_w 之外）, long_bg_off_x = 当前视口左缘偏移(0..w-480)。
       非空时 fill_bg 优先铺它（每帧逐行拷贝），触摸/切换只需改 off_x 或换指针 */
    uint16_t *long_bg; int long_bg_w, long_bg_h, long_bg_stride, long_bg_off_x;
    uint16_t *hfb;                           // 半分辨率帧缓冲（240×400 RGB565）
    bool half_res;                           // true=快速模式（半分辨率+2×放大），false=全分辨率（默认）
    int n_layers; pd_layer_t *layers;        // 已按 z 升序
    /* 取景（fitToScreen + 全身/特写） */
    float base_scale, cX, cY, zoom, pan_y;
    float scale;                     // 全局渲染缩放（0.8=展示 80%，渲染与触摸共用）
    float ppa_zoom, ppa_scale;       // PPA 预缩放对应的取景参数（匹配时启用硬件混合）
    /* 摸头判定区域（场景坐标） */
    float head_rect[4], head_center[2];
    /* ---- 运行时状态 ---- */
    float P[15];                      // AngleX,AngleY,AngleZ,EyeLOpen,EyeROpen,
                                      // MouthOpenY,EyeLSmile,EyeRSmile,MouthForm,EyeBallX,EyeBallY,
                                      // BrowYL,BrowYR,BrowAngL,BrowAngR
    bool demo;                        // 待机演示模式
    int expr;                         // 表情 0..11（0=无表情，与 sim.js EXPRESSIONS 对应）
    bool dragging; int last_x, last_y;
    bool press_set; int press_x, press_y;   // 按下起点（拖动阈值判定：位移超阈值才转拖动）
    bool pat_active; uint32_t pat_t0;
    float track_x, track_y;           // 视线目标 -1..1
    bool track_set;                   // 首次触摸后才启用追踪（无触摸时保留演示游走）
    /* Spine 原生动画（anims.json 增量时间轴；-1 = 未播放） */
    pd_anim_t *anims; int n_anims;
    int anim_idx; uint32_t anim_t0;
    float anim_speed;                 // 播放速度（0.6 = 慢放便于看清动作）
    uint32_t speak_t0;                // 进入说话表情的时刻（嘴型循环从闭合相位起步，防跳变闪烁）
    float hf_x, hf_y;                 // 头部随视线微随动（±4°/±3°，渲染时叠加）
    float sway_amp;                   // 待机晃动淡入淡出
    uint32_t last_ms;
    void *pdq;                        // PPDQ 模型(有 mesh.ppdq 时加载;NULL=旧路径,2026-09-14)
    char pdq_anim[40]; uint32_t pdq_anim_t0;   // 2026-09-17:mesh 动画播放(名+起始时刻)
    bool pdq_force_loop;   // 2026-09-27:兜底动画非循环时强制循环(Relax 播 4 秒停的修复)
} pd_model_t;

/* 从 <dir>/scene.json + *.raw 加载角色(失败返回 NULL,pd_load_error 可读)。
 * mesh_only=1(2026-09-17):mesh.ppdq 优先——加载成功后跳过层纹理与 anims.json,
 * 渲染走三角形光栅,省 15MB+ PSRAM(PPD_Q 大层数角色)。 */
pd_model_t *pd_load(const char *dir, int mesh_only);
void pd_free(pd_model_t *m);
/* 渲染一帧到 RGB565 帧缓冲（fw×fh，通常 480×800） */
void pd_render(pd_model_t *m, uint16_t *fb, int fw, int fh, uint32_t t_ms);
/* 触摸事件：down=true 按下/按住，false 抬起。坐标=屏幕像素（480×800） */
void pd_touch(pd_model_t *m, int sx, int sy, bool down);
/* 待机演示开关（任何触摸自动关） */
void pd_set_demo(pd_model_t *m, bool on);
/* 表情 0..11（0=无表情恢复演示；与 PC 仿真器 EXPRESSIONS 表一致） */
void pd_set_expression(pd_model_t *m, int ex);
void pd_set_anim(pd_model_t *m, const char *name);   // 播放原生动画（NULL/空 = 停止回待机）

#ifdef __cplusplus
}
#endif
