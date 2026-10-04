#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include <sys/stat.h>
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/event_groups.h"
#include "apps/image_display/PPACompositor.h"
#include "apps/live2d/moc3_parser.h"
#include "apps/live2d/lv2_render.h"
#include "apps/loading/loading_ui.h"

static const char *TAG = "SD_TEST";

// Global Live2D model + eye tracking
static Lv2RenderModel* g_lv2_model = NULL;
static Lv2RenderModel* g_lv2_theresia = NULL;
static Lv2RenderModel* g_lv2_furina = NULL;
static Lv2RenderModel* g_lv2_zll0 = NULL;
static int g_active_character = 0;  // 0=Amiya, 1=Theresia, 2=Furina, 3=Zll0
float g_eye_target_x = 0.5f;
float g_eye_target_y = 0.5f;
float g_shy_trigger = 0;
bool g_touching_head = false;
int g_lv2_expression = 0;  // 0=auto

/* PPD 交互等大内存场景:释放 Live2D 常驻(模型+纹理+帧缓冲,约 8~11MB),
   腾 PSRAM 给 PPD 大图层。再次进 Live2D 交互/切角色时按需重载(do_switch 全量重载)。 */
void lv2_free_all(void) {
    if (g_lv2_theresia == g_lv2_model) g_lv2_theresia = NULL;
    if (g_lv2_furina    == g_lv2_model) g_lv2_furina    = NULL;
    if (g_lv2_zll0      == g_lv2_model) g_lv2_zll0      = NULL;
    if (g_lv2_model)     { lv2_free_render(g_lv2_model);     g_lv2_model = NULL; }
    if (g_lv2_theresia)  { lv2_free_render(g_lv2_theresia);  g_lv2_theresia = NULL; }
    if (g_lv2_furina)    { lv2_free_render(g_lv2_furina);    g_lv2_furina = NULL; }
    if (g_lv2_zll0)      { lv2_free_render(g_lv2_zll0);      g_lv2_zll0 = NULL; }
    extern uint16_t* g_lv2_fb;
    extern int g_lv2_fb_w, g_lv2_fb_h;
    if (g_lv2_fb) { heap_caps_free(g_lv2_fb); g_lv2_fb = NULL; g_lv2_fb_w = 0; g_lv2_fb_h = 0; }
    ESP_LOGI(TAG, "Live2D resident freed (PPD memory headroom)");
}

void lv2_set_expression(int expr) {
    if (expr < 0) expr = 0;
    if (expr > 12) expr = 12;
    g_lv2_expression = expr;
    if (g_lv2_model) g_lv2_model->expression = expr;
    ESP_LOGI(TAG, "LV2 expr: %d", expr);
}

static int g_pending_switch = -1;  // -1=none, 0=Amiya, 1=Theresia

// ── 切换完成事件组（加载动画等它，替代旧代码的固定 vTaskDelay(500)）──
static EventGroupHandle_t s_lv2_switch_evt = NULL;
#define LV2_SWITCH_DONE BIT0

void lv2_switch_character(int chr) {
    if (!s_lv2_switch_evt) s_lv2_switch_evt = xEventGroupCreate();
    xEventGroupClearBits(s_lv2_switch_evt, LV2_SWITCH_DONE);
    g_pending_switch = chr;  // defer switch to animation task (avoids race condition)
    ESP_LOGI(TAG, "LV2 switch queued: %d", chr);
}

void lv2_wait_switch_done(int timeout_ms) {
    if (!s_lv2_switch_evt) return;
    xEventGroupWaitBits(s_lv2_switch_evt, LV2_SWITCH_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
}

bool lv2_is_model_ready(void) { return g_lv2_model != NULL; }

// Emoji name → expression ID (0=auto, 1-12)
int lv2_emoji_to_expression(const char* emoji) {
    if (!emoji || !emoji[0]) return 12;  // neutral
    if (strstr(emoji,"happy")    || strstr(emoji,"开心")) return 1;
    if (strstr(emoji,"sad")      || strstr(emoji,"悲伤") || strstr(emoji,"crying")) return 2;
    if (strstr(emoji,"surpris")  || strstr(emoji,"惊讶") || strstr(emoji,"shock")) return 3;
    if (strstr(emoji,"confident")|| strstr(emoji,"自信") || strstr(emoji,"wink")||strstr(emoji,"cool")) return 4;
    if (strstr(emoji,"confus")   || strstr(emoji,"困惑")) return 5;
    if (strstr(emoji,"think")    || strstr(emoji,"思考")) return 6;
    if (strstr(emoji,"laugh")    || strstr(emoji,"大笑") || strstr(emoji,"funny")) return 7;
    if (strstr(emoji,"sleep")    || strstr(emoji,"困倦") || strstr(emoji,"tired")) return 8;
    if (strstr(emoji,"silly")    || strstr(emoji,"傻气") || strstr(emoji,"goofy")) return 9;
    if (strstr(emoji,"angry")    || strstr(emoji,"生气")) return 10;
    if (strstr(emoji,"shy")      || strstr(emoji,"害羞")) return 11;
    if (strstr(emoji,"neutral")  || strstr(emoji,"idle")||strstr(emoji,"idle")||strstr(emoji,"待机")) return 12;
    return 12;  // default: neutral/idle
}

static void do_switch(int chr) {
    loading_set_stage("释放旧模型", -1);
    // Prevent double-free: g_lv2_model may == g_lv2_theresia or g_lv2_furina or g_lv2_zll0
    if (g_lv2_theresia == g_lv2_model) g_lv2_theresia = NULL;
    if (g_lv2_furina    == g_lv2_model) g_lv2_furina    = NULL;
    if (g_lv2_zll0      == g_lv2_model) g_lv2_zll0      = NULL;
    if (g_lv2_model)     { lv2_free_render(g_lv2_model);     g_lv2_model = NULL; }
    if (g_lv2_theresia)  { lv2_free_render(g_lv2_theresia);  g_lv2_theresia = NULL; }
    if (g_lv2_furina)    { lv2_free_render(g_lv2_furina);    g_lv2_furina = NULL; }
    if (g_lv2_zll0)      { lv2_free_render(g_lv2_zll0);      g_lv2_zll0 = NULL; }

    /* 文件规范（2026-08）：LIVE2D 数据位于 operator/<职业>/<星级>/<干员>/LIVE2D/，
       非干员角色（Furina/zll0）临时放 /sdcard/tempLIVE2D/ */
    if (chr == 0) {
        g_lv2_model = lv2_load("/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/model.l2d",
                               "/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/amiya_tex.raw", NULL);
        if (g_lv2_model) { g_lv2_model->ppu = 3600; g_lv2_model->expression = g_lv2_expression; lv2_load_keyforms(g_lv2_model, "/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/keyforms.bin"); }
    } else if (chr == 1) {
        g_lv2_theresia = lv2_load("/sdcard/Arknights/main/operator/SUPPORTER/6STAR/Civilight_Eterna/LIVE2D/model.l2d",
                                  "/sdcard/Arknights/main/operator/SUPPORTER/6STAR/Civilight_Eterna/LIVE2D/tex_00.raw", NULL);
        if (g_lv2_theresia) {
            g_lv2_theresia->ppu = 5768;
            static const int theresia_needed[]={0,1,2,3,5,12,4,6,11,7,8};
            lv2_load_keyforms_ex(g_lv2_theresia, "/sdcard/Arknights/main/operator/SUPPORTER/6STAR/Civilight_Eterna/LIVE2D/keyforms.bin", theresia_needed, 11);
            g_lv2_theresia->expression = g_lv2_expression;
        }
        g_lv2_model = g_lv2_theresia;
    } else if (chr == 2) {
        g_lv2_furina = lv2_load("/sdcard/tempLIVE2D/Furina/model.l2d", "/sdcard/tempLIVE2D/Furina/tex_00.raw", NULL);
        if (g_lv2_furina) {
            g_lv2_furina->ppu = 3000;
            static const int furina_skip[]={403,404,208,209,210,211,245,246,248,249};
            for(int i=0;i<10;i++){int di=furina_skip[i]; if(di<g_lv2_furina->drawable_count)g_lv2_furina->mask_info[di]=-999;}
            static const int furina_needed[]={8,9,10,2,0,7,3,1,6,4,5,16,15};
            bool kf_ok = lv2_load_keyforms_ex(g_lv2_furina, "/sdcard/tempLIVE2D/Furina/keyforms.bin", furina_needed, 13);
            ESP_LOGI(TAG, "Furina KF loaded: %s", kf_ok?"OK":"FAILED");
            g_lv2_furina->expression = g_lv2_expression;
        }
        g_lv2_model = g_lv2_furina;
    } else if (chr == 3) {
        g_lv2_zll0 = lv2_load("/sdcard/tempLIVE2D/zll0/model.l2d", "/sdcard/tempLIVE2D/zll0/tex_00.raw", NULL);
        if (g_lv2_zll0) {
            g_lv2_zll0->ppu = 3000;
            static const int zll0_needed[]={0,1,2,3,5,12,4,6,11,7,8};  // 与 tuner load_keyforms_zll0 一致
            bool kf_ok = lv2_load_keyforms_ex(g_lv2_zll0, "/sdcard/tempLIVE2D/zll0/keyforms.bin", zll0_needed, 11);
            ESP_LOGI(TAG, "Zll0 KF loaded: %s", kf_ok?"OK":"FAILED");
            // ── 足力零专属 base 修正 ──
            // 模型制作者把 def=1.0 的上眼睑画得过高（眼睛上方露出无贴图缝隙），
            // 原作者作品实际形态 = EyeOpen≈0.35 的上眼睑 + 1.0 的下眼睑。
            // 闭眼 delta（槽3=EyeLOpen, 槽4=EyeROpen）中 dy<0 的顶点=上眼睑：
            //   1. base 静态偏置 +0.40×delta（睁眼态=参数 0.60 形态）
            //   2. 闭眼 delta 幅度 ×0.60（闭眼时 0.40+0.60=1.0 正好标准闭眼，不过冲）
            // 下眼睑(dy>0)完全不动。
            if (kf_ok) {
                float ppu_z = g_lv2_zll0->ppu;
                if (g_lv2_zll0->kf_offsets) {
                    // Dense 路径
                    int kfv = g_lv2_zll0->kf_verts;
                    if (kfv > g_lv2_zll0->total_verts) kfv = g_lv2_zll0->total_verts;
                    for (int slot = 3; slot <= 4; slot++) {
                        float* kf = g_lv2_zll0->kf_offsets + slot * g_lv2_zll0->kf_verts * 2;
                        for (int vi = 0; vi < kfv; vi++) {
                            if (kf[vi*2+1] < -0.006f) {  // dy<0 = 上眼睑（闭眼下移）
                                float dx = 0.40f * kf[vi*2]   * ppu_z;
                                float dy = 0.40f * kf[vi*2+1] * ppu_z;
                                g_lv2_zll0->kf_base_pos[vi*2]   += dx;
                                g_lv2_zll0->kf_base_pos[vi*2+1] += dy;
                                g_lv2_zll0->positions[vi*2]     += dx;
                                g_lv2_zll0->positions[vi*2+1]   += dy;
                                kf[vi*2]   *= 0.60f;
                                kf[vi*2+1] *= 0.60f;
                            }
                        }
                    }
                    ESP_LOGI(TAG, "Zll0 upper-eyelid bias + delta scale applied (dense)");
                } else {
                    // Sparse 路径: 交错 [vi:u16|dx:i16|dy:i16], dx/dy 已量化 ×10000
                    for (int slot = 3; slot <= 4; slot++) {
                        uint16_t* sp = g_lv2_zll0->kf_sparse[slot];
                        int n = (int)g_lv2_zll0->kf_sparse_n[slot];
                        for (int e = 0; e < n; e++) {
                            int vi = (int)sp[e*3];
                            float dx = (float)(int16_t)sp[e*3+1] / 10000.0f;
                            float dy = (float)(int16_t)sp[e*3+2] / 10000.0f;
                            if (dy < -0.006f) {
                                float bx = 0.40f * dx * ppu_z;
                                float by = 0.40f * dy * ppu_z;
                                g_lv2_zll0->kf_base_pos[vi*2]   += bx;
                                g_lv2_zll0->kf_base_pos[vi*2+1] += by;
                                g_lv2_zll0->positions[vi*2]     += bx;
                                g_lv2_zll0->positions[vi*2+1]   += by;
                                sp[e*3+1] = (uint16_t)(int16_t)(dx * 0.60f * 10000.0f);
                                sp[e*3+2] = (uint16_t)(int16_t)(dy * 0.60f * 10000.0f);
                            }
                        }
                    }
                    ESP_LOGI(TAG, "Zll0 upper-eyelid bias + delta scale applied (sparse)");
                }
            }
            g_lv2_zll0->eye_cx = -0.5f;   // 模型默认瞳孔偏左：X 居中需 0（-0.5）
            g_lv2_zll0->eye_cy = 0.0f;
            // 视线 Y 重映射：追踪输入 g=-1(上)→0.3, 0.5(默认)→0.6, +1(下)→1.0
            g_lv2_zll0->eye_gm = 0.5f; g_lv2_zll0->eye_vm = 0.6f;
            g_lv2_zll0->eye_vup = 0.3f; g_lv2_zll0->eye_vdown = 1.0f;
            g_lv2_zll0->expression = g_lv2_expression;
        }
        g_lv2_model = g_lv2_zll0;
    }
    g_active_character = chr;
    ESP_LOGI(TAG, "LV2 char: %d model=%p", chr, (void*)g_lv2_model);
    if (s_lv2_switch_evt) xEventGroupSetBits(s_lv2_switch_evt, LV2_SWITCH_DONE);
}

// Event group for signaling SD card + preload completion
static EventGroupHandle_t s_sd_event_group = NULL;
#define SD_READY_BIT BIT0

// SD卡引脚（Slot 0）
#define SD_CLK_GPIO  GPIO_NUM_43
#define SD_CMD_GPIO  GPIO_NUM_44
#define SD_D0_GPIO   GPIO_NUM_39
#define SD_D1_GPIO   GPIO_NUM_40
#define SD_D2_GPIO   GPIO_NUM_41
#define SD_D3_GPIO   GPIO_NUM_42
#define SD_MOUNT_POINT "/sdcard"

static esp_err_t sdmmc_host_init_dummy(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }

static void sd_mount_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Initializing SD card via SDMMC...");
    ESP_LOGI(TAG, "Pins: CLK=%d, CMD=%d, D0=%d, D1=%d, D2=%d, D3=%d",
             SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO, SD_D1_GPIO, SD_D2_GPIO, SD_D3_GPIO);

    // ESP32-P4 SDMMC Slot 0 需要使用内部LDO供电
    // LDO通道4用于SDMMC Slot 0
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,  // ESP32-P4 SDMMC Slot 0 专用LDO通道
    };
    
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create LDO power control: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without LDO power control...");
    } else {
        ESP_LOGI(TAG, "LDO power control initialized successfully");
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_SDR50;
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CLK_GPIO;
    slot_config.cmd = SD_CMD_GPIO;
    slot_config.d0 = SD_D0_GPIO;
    slot_config.d1 = SD_D1_GPIO;
    slot_config.d2 = SD_D2_GPIO;
    slot_config.d3 = SD_D3_GPIO;
    slot_config.width = 4;
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 30,
        .allocation_unit_size = 16 * 1024,
    };
    
    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, 
                                   &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Please check:");
        ESP_LOGW(TAG, "  1. SD card is inserted");
        ESP_LOGW(TAG, "  2. SD card is formatted as FAT32");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "SD card mounted successfully!");
    sdmmc_card_print_info(stdout, card);

    // 提前预加载默认 cover 到 cover 专用槽（WiFi 前，SD 独占，速度快）
    ESP_LOGI(TAG, "Early preload: Kaltsit cover...");
    int loaded = ppa_preload_cover("/sdcard/Arknights/main/operator/MEDIC/6STAR/Kaltsit/cover/kaltsit_cover.mjpeg");
    ESP_LOGI(TAG, "Early preload done: %d frames", loaded);

    // ── Live2D MOC3 parser integration (early preload for fast expression switching) ──
    ESP_LOGI(TAG, "Preloading MOC3 model: /sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/Amiya.moc3");
    FILE* fp = fopen("/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/Amiya.moc3", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (buf) {
            fread(buf, 1, sz, fp);
            Lv2Model* m = lv2_parse_moc3(buf, sz);
            if (m) {
                ESP_LOGI(TAG, "Live2D model loaded: %lu params, %lu drawables, %.0fx%.0f",
                    (unsigned long)m->counts.parameters, (unsigned long)m->counts.art_meshes,
                    (double)m->canvas_w, (double)m->canvas_h);
                lv2_free_model(m);
            }
            heap_caps_free(buf);
        }
        fclose(fp);
    }

    // ── Live2D render ──
    ESP_LOGI(TAG, "Loading Amiya...");
    g_lv2_model = lv2_load("/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/model.l2d",
                           "/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/amiya_tex.raw", NULL);
    if (g_lv2_model) {
        lv2_load_keyforms(g_lv2_model, "/sdcard/Arknights/main/operator/CASTER/5STAR/Amiya/LIVE2D/keyforms.bin");
        int rw = 480, rh = 800;
        uint16_t* fb = (uint16_t*)heap_caps_malloc(rw*rh*2, MALLOC_CAP_SPIRAM);
        if (fb) {
            memset(fb, 0, rw*rh*2);
            lv2_render_frame(g_lv2_model, fb, rw, rh);
            ESP_LOGI(TAG, "Amiya render OK! %dx%d, %d drawables, tex=%dx%d",
                rw, rh, g_lv2_model->drawable_count, g_lv2_model->tex_w[0], g_lv2_model->tex_h[0]);
            extern uint16_t* g_lv2_fb;
            extern int g_lv2_fb_w, g_lv2_fb_h;
            if (g_lv2_fb) heap_caps_free(g_lv2_fb);
            g_lv2_fb = fb; g_lv2_fb_w = 480; g_lv2_fb_h = 800;
        }
    } else ESP_LOGW(TAG, "Amiya load skipped");

    // Theresia loaded on-demand via lv2_switch_character(1)

    // 2026-09-17:空卡基底构建——新 SD 卡预先建好统一文件系统骨架
    // (operator/INDEX 缩略图、background、music;公共背景/音乐随设备三件套下载补齐)
    {
        static const char *base_dirs[] = {
            "/sdcard/Arknights",
            "/sdcard/Arknights/main",
            "/sdcard/Arknights/main/operator",
            "/sdcard/Arknights/main/operator/INDEX",
            "/sdcard/Arknights/main/background",
            "/sdcard/Arknights/main/music",
        };
        for (auto d : base_dirs) {
            mkdir(d, 0755);
            ESP_LOGI(TAG, "基底目录: %s", d);
        }
    }

    // 通知主任务：SD卡就绪
    if (s_sd_event_group) xEventGroupSetBits(s_sd_event_group, SD_READY_BIT);

    vTaskDelete(NULL);
}

void sdcard_init(void)
{
    if (!s_sd_event_group) s_sd_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_sd_event_group, SD_READY_BIT);
    xTaskCreate(sd_mount_task, "sd_mount", 8192, NULL, 5, NULL);
}

bool sdcard_wait_ready(int timeout_ms)
{
    if (!s_sd_event_group) return false;
    EventBits_t bits = xEventGroupWaitBits(s_sd_event_group, SD_READY_BIT,
                                            pdTRUE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & SD_READY_BIT) != 0;
}

// Called periodically for Live2D animation (loads keyforms on first call)
void lv2_update_animation(float time_sec) {
    // Process pending character switch (safe: runs in animation task context)
    if (g_pending_switch >= 0) {
        do_switch(g_pending_switch);
        g_pending_switch = -1;
        loading_hide();  // 场景 B（测试页切角色）的隐藏点；场景 A 幂等
    }
    if (!g_lv2_model) { static int rpt=0; if((rpt++&63)==0) ESP_LOGW("LV2","anim: model NULL"); return; }
    // Keyforms loaded in lv2_switch_character on model load
    extern uint16_t* g_lv2_fb;
    if (!g_lv2_fb) { static int rpt2=0; if((rpt2++&63)==0) ESP_LOGW("LV2","anim: fb NULL"); return; }
    // Render to temp buffer, then copy to display buffer
    static uint16_t* tmp_fb = NULL;
    if (!tmp_fb) tmp_fb = (uint16_t*)heap_caps_malloc(480*800*2, MALLOC_CAP_SPIRAM);
    if (!tmp_fb) { static int rpt3=0; if((rpt3++&63)==0) ESP_LOGE("LV2","tmp_fb alloc FAILED"); return; }
    {
        // Load background as base layer
        extern bool ppa_has_background(void);
        extern bool ppa_load_background(const char*);
        extern const uint8_t* ppa_get_background_buffer(void);
        if (!ppa_has_background()) ppa_load_background("/sdcard/Arknights/main/background/background.jpg");
        const uint8_t* bg = ppa_get_background_buffer();
        if (bg) memcpy(tmp_fb, bg, 480*800*2);
        else memset(tmp_fb, 0, 480*800*2);
        g_lv2_model->eye_x = g_eye_target_x;
        g_lv2_model->eye_y = g_eye_target_y;
        g_shy_trigger = g_touching_head ? 1.0f : 0.0f;
        g_lv2_model->shy = g_shy_trigger;
        g_lv2_model->expression = g_lv2_expression;
        static int scnt=0;
        if((scnt++&15)==0) ESP_LOGI("SHY","touch:%d shy:%.2f", g_touching_head, g_shy_trigger);
        static int ecnt=0;
        if((ecnt++&63)==0) ESP_LOGI("LV2_EYE","eyes:(%.2f,%.2f) shy:%.2f", g_eye_target_x, g_eye_target_y, g_shy_trigger);
        lv2_render_animated(g_lv2_model, tmp_fb, 480, 800, time_sec);
        // Swap display/render buffers (canvas rebind done in anim task under LVGL lock)
        {uint16_t* swp=tmp_fb; tmp_fb=g_lv2_fb; g_lv2_fb=swp;}
    }
}


