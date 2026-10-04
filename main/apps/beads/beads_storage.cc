/** 拼豆存储 — SD 卡读写，纯像素数据。 */
#include "beads.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"

#define TAG "BEADS"
#define BEADS_DIR "/sdcard/User/Beads"

// ── 36 色水彩笔色板（RGB565），6 行 × 6 列，每行深→浅 ──
const uint16_t g_beads_palette[BEADS_COLORS] = {
    // 行1 红系
    0x8800, 0x7808, 0xF800, 0xF8C0, 0xF812, 0xFD16,
    // 行2 橙黄系
    0x5AC2, 0x4282, 0x6A63, 0xFC60, 0xFD00, 0xFFE0,
    // 行3 绿系
    0x0320, 0x4AA4, 0x0400, 0x3666, 0x97D2, 0xAFA5,
    // 行4 蓝系
    0x188E, 0x0011, 0x001F, 0x05FF, 0xADDC, 0x07FF,
    // 行5 紫粉系
    0x4810, 0x8010, 0xBAB6, 0xDB92, 0xE73F, 0xFFFF,
    // 行6 灰黑系
    0x0000, 0x4208, 0x8410, 0xC618, 0xB31B, 0xFFDF,
};

static bool ensure_dir(void) {
    struct stat st;
    if (stat(BEADS_DIR, &st) == 0) return true;
    return mkdir(BEADS_DIR, 0777) == 0 || stat(BEADS_DIR, &st) == 0;
}

BeadArt* beads_create(int w, int h) {
    BeadArt* a = (BeadArt*)calloc(1, sizeof(BeadArt));
    if (!a) return NULL;
    a->w = w; a->h = h;
    a->pixels = (uint8_t*)calloc(1, (size_t)w * h);
    if (!a->pixels) { free(a); return NULL; }
    return a;
}

void beads_free(BeadArt* art) {
    if (!art) return;
    if (art->pixels) free(art->pixels);
    free(art);
}

// 文件格式：magic "BEAD"(4B) + level(1B) + w(2B) + h(2B) + 像素数据
static const uint8_t BEADS_MAGIC[4] = {'B', 'E', 'A', 'D'};

bool beads_save(BeadArt* art, const char* name, int level) {
    if (!art || !name || !name[0]) return false;
    if (!ensure_dir()) { ESP_LOGE(TAG, "mkdir %s failed", BEADS_DIR); return false; }
    char path[128];
    snprintf(path, sizeof(path), "%s/B%d_%s.bead", BEADS_DIR, level, name);
    FILE* f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "save open failed: %s", path); return false; }
    fwrite(BEADS_MAGIC, 1, 4, f);
    uint8_t lv = (uint8_t)level;
    fwrite(&lv, 1, 1, f);
    uint16_t w = (uint16_t)art->w, h = (uint16_t)art->h;
    fwrite(&w, 2, 1, f);
    fwrite(&h, 2, 1, f);
    fwrite(art->pixels, 1, (size_t)art->w * art->h, f);
    fclose(f);
    ESP_LOGI(TAG, "saved %s (%dx%d)", path, art->w, art->h);
    return true;
}

BeadArt* beads_load(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    uint8_t hdr[9];
    if (fread(hdr, 1, 9, f) != 9) { fclose(f); return NULL; }
    if (memcmp(hdr, BEADS_MAGIC, 4) != 0) { fclose(f); return NULL; }
    int level = hdr[4];
    int w = hdr[5] | (hdr[6] << 8);
    int h = hdr[7] | (hdr[8] << 8);
    if (level < 1 || level > 3) { fclose(f); return NULL; }
    if (w != BEADS_L1_SIZE * (1 << (level > 1 ? 1 : 0)) * (level > 2 ? 3 : 1)) {
        // 尺寸校验：L1=24, L2=72, L3=216
        if (!(w == BEADS_L1_SIZE || w == BEADS_L2_SIZE || w == BEADS_L3_SIZE)) {
            fclose(f); return NULL;
        }
    }
    BeadArt* a = beads_create(w, h);
    if (!a) { fclose(f); return NULL; }
    if (fread(a->pixels, 1, (size_t)w * h, f) != (size_t)w * h) {
        beads_free(a); fclose(f); return NULL;
    }
    fclose(f);
    return a;
}

int beads_list_files(int level, char out[][64], int max_n) {
    int n = 0;
    DIR* d = opendir(BEADS_DIR);
    if (!d) return 0;
    char prefix[4];
    snprintf(prefix, sizeof(prefix), "B%d_", level);
    struct dirent* e;
    while ((e = readdir(d)) && n < max_n) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
        if (!strstr(e->d_name, ".bead")) continue;
        if (strlen(e->d_name) >= 64) continue;   /* 超长文件名跳过（防 -Wstringop-truncation） */
        strncpy(out[n], e->d_name, 63);
        out[n][63] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

bool beads_delete(const char* filename) {
    if (!filename || !filename[0]) return false;
    // 只允许纯文件名（来自 beads_list_files），防止路径穿越
    if (strchr(filename, '/') || strchr(filename, '\\')) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", BEADS_DIR, filename);
    if (remove(path) != 0) { ESP_LOGE(TAG, "delete failed: %s", path); return false; }
    ESP_LOGI(TAG, "deleted %s", path);
    return true;
}
