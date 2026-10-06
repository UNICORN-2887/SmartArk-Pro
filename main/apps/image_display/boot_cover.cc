/* 开机背景(2026-09-29 用户方案:bitmap 位图显示):公共库 cover 图轮播。
 * RGB565 raw(480x800=768KB/张)放 SD 卡,直接读内存构造 lv_image_dsc_t 显示,
 * 零解码(PNG 解码在启动早期不稳,已避开)。独立编译单元(布局敏感教训)。 */
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_random.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#define TAG "BootCover"
#define BW 480
#define BH 800

static char s_boot_paths[8][128];
static int s_boot_count = 0;
static int s_boot_cur = 0;
static lv_obj_t *s_boot_img = NULL;
static lv_timer_t *s_boot_timer = NULL;
static uint8_t *s_buf[2] = {NULL, NULL};   // 双缓冲:上一张/当前张
static int s_buf_cur = 0;

static uint8_t *boot_load_raw(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "加载失败(打开): %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != BW * BH * 2) {   // RGB565 固定大小(半截下载/损坏文件会到这)
        fclose(f);
        ESP_LOGW(TAG, "加载失败(大小 %ld != %d): %s", sz, BW * BH * 2, path);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        ESP_LOGW(TAG, "加载失败(内存): %s", path);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "加载失败(读取 %d/%ld): %s", (int)got, sz, path);
        return NULL;
    }
    return buf;
}

static void boot_free_slot(int i) {
    /* 释放槽位(dsc 描述符 + raw 数据一起) */
    if (!s_buf[i]) return;
    lv_image_dsc_t *od = (lv_image_dsc_t *)s_buf[i];
    if (od->data) heap_caps_free((void *)od->data);
    heap_caps_free(od);
    s_buf[i] = NULL;
}

static void boot_cover_show(void) {
    if (s_boot_count == 0) return;
    uint8_t *raw = boot_load_raw(s_boot_paths[s_boot_cur]);
    if (!raw) return;
    lv_image_dsc_t *dsc = (lv_image_dsc_t *)heap_caps_malloc(sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dsc) {
        heap_caps_free(raw);
        return;
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = BW;
    dsc->header.h = BH;
    dsc->header.stride = BW * 2;
    dsc->data_size = BW * BH * 2;
    dsc->data = raw;

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) {
        heap_caps_free(raw);
        heap_caps_free(dsc);
        return;
    }
    if (!s_boot_img) {
        // 2026-09-29 放 lv_layer_top 底部:主屏层被启动全屏黑 canvas 盖住
        // (垫底看不到);top 层底部 = 启动界面按钮之下、后续全屏页(索引/配网)之下
        s_boot_img = lv_image_create(lv_layer_top());
        lv_obj_set_size(s_boot_img, BW, BH);
        lv_obj_set_pos(s_boot_img, 0, 0);
        lv_obj_move_to_index(s_boot_img, 0);
    }
    lv_image_set_src(s_boot_img, dsc);
    lvgl_port_unlock();

    // 双缓冲换槽:释放即将被覆盖的槽(保留上一张防 LVGL 渲染引用)
    s_buf_cur = 1 - s_buf_cur;
    boot_free_slot(s_buf_cur);
    s_buf[s_buf_cur] = (uint8_t *)dsc;
    ESP_LOGI(TAG, "Boot cover shown: %s", s_boot_paths[s_boot_cur]);
}

static void boot_cover_timer_cb(lv_timer_t *t) {
    if (s_boot_count < 2) return;
    s_boot_cur = (s_boot_cur + 1) % s_boot_count;
    boot_cover_show();
}

static void boot_cover_create_cb(lv_timer_t *t) {
    /* 延迟 3 秒:等启动流程/显示链路完全就绪(启动早期创建 LVGL 对象实测会崩) */
    boot_cover_show();
    extern void boot_music_play(void);   /* 2026-10-04 开机音乐:封面显示后播生命流 */
    boot_music_play();
    if (s_boot_count > 1 && s_boot_img) {
        s_boot_timer = lv_timer_create(boot_cover_timer_cb, 3000, NULL);
    }
    lv_timer_delete(t);
}

void boot_cover_stop(void) {
    /* 2026-09-29 轮播约束:点击开始唤醒后停止切换并隐藏背景图
       (垫底会在配网页→索引页跳转间隙闪现,用户要求彻底不出现) */
    if (s_boot_timer) {
        lv_timer_delete(s_boot_timer);
        s_boot_timer = NULL;
    }
    if (s_boot_img) {
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            lv_obj_add_flag(s_boot_img, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
    }
    ESP_LOGI(TAG, "Boot cover stopped and hidden");
}

void boot_cover_init(void) {
    // 2026-09-29 后台同步无条件启动:首次开机 SD 无 cover 目录时,
    // opendir 失败不能早退——目录要靠同步任务下载创建
    extern int role_download_fetch_public(const char *rel,
                                          bool (*step_cb)(int pct, const char *file, void *ud),
                                          void *ud);
    xTaskCreate([](void *arg) {
        vTaskDelay(pdMS_TO_TICKS(8000));   // 让位启动流程与网络
        for (int i = 0; i < 3; i++) {   // 2026-09-29 网络未通时重试(共 3 次)
            ESP_LOGI(TAG, "boot cover sync: 检查公共库 cover 目录(第 %d 次)", i + 1);
            int r = role_download_fetch_public("cover", nullptr, nullptr);
            if (r == 0) break;
            vTaskDelay(pdMS_TO_TICKS(20000));   // 失败等 20s 再试(等 WiFi 连上)
        }
        vTaskDelete(NULL);
    }, "bootsync", 4096, NULL, 2, NULL);

    const char *dir = "/sdcard/Arknights/main/cover";
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_boot_count < 8) {
        size_t nl = strlen(e->d_name);
        if (nl > 4 && strcmp(e->d_name + nl - 4, ".bin") == 0) {
            snprintf(s_boot_paths[s_boot_count], sizeof(s_boot_paths[0]),
                     "/sdcard/Arknights/main/cover/%.*s",
                     (int)strnlen(e->d_name, 120), e->d_name);
            s_boot_count++;
        }
    }
    closedir(d);
    if (s_boot_count == 0) return;   // 本次开机无图(同步后下次开机可见)
    s_boot_cur = esp_random() % s_boot_count;
    ESP_LOGI(TAG, "Boot cover: %d 张,起点 %d,3s 后显示", s_boot_count, s_boot_cur);
    lv_timer_create(boot_cover_create_cb, 3000, NULL);
}
