/* 角色下载页:云端角色清单 → 点选 → 确认框 → 逐文件下载写入 SD 卡。
 *
 * 交互(极简,给小学生用):
 *   菜单点"角色下载" → 自动拉取云端角色列表 → 屏幕点角色名 → 确认框"下载 xxx?"
 *   → [下载]/[取消] → "下载中 xx%" → 完成提示(去罗德岛选角色)
 * 失败自动重试 3 次;全部成功后写 .done 标记(半成品目录会被角色页过滤掉)。
 * 打开本页自动关闭干员索引页(互斥,防两页叠加事件串扰)。
 *
 * 下载核心 role_download_fetch 同时供干员索引页"未缓存弹窗下载"复用(见 .h)。
 *
 * 服务器 API(与 paperdoll_sim/device_api.py 对应):
 *   GET /api/roles                                  → [{"name","vocation","star","has":[..],"incomplete"}]
 *   GET /api/roles/<voc>/<star>/<name>/manifest     → {"files":[{"rel","size"}]}
 *   GET /api/roles/file?v=&s=&n=&p=                 → 单文件二进制
 */
#include "role_downloader.h"

#include <string>
#include <vector>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_heap_caps.h>   // 2026-10-07 cp_ota 固件 PSRAM 缓冲
#include <esp_system.h>      // 2026-10-07 cp_ota 完成后 esp_restart
#include <cJSON.h>
#include <freertos/FreeRTOS.h>   // 2026-10-07 xSemaphoreCreateMutex(下载并发互斥)
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_lvgl_port.h>
#include <wifi_station.h>
#include "system_info.h"

#define TAG "RoleDownload"

#define MAX_ROLES 200
#define MAX_HTTP_BUF 8192
#define BUF_SIZE 16384   /* 2026-10-06 下载加速:4K→16K(esp_http_client 内部缓冲) */

/* 2026-10-07 全局累计下载字节(ImageDisplay 速度显示用;多文件下载 pct 按单文件重置,
   按总字节×pct 会算出假速度 100MB/s,真实字节计数才准) */
int64_t g_dl_bytes_done = 0;

struct RoleInfo {
    std::string name, vocation, star;
    bool incomplete = false;
};
struct FileItem {
    std::string rel;
    int64_t size = 0;
};

static lv_obj_t *s_page = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_progress = nullptr;
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_confirm_box = nullptr;   // 确认框(下载/取消)
static lv_obj_t *s_confirm_lbl = nullptr;
static std::vector<RoleInfo> s_roles;
static std::vector<FileItem> s_files;   // 2026-10-07 仅作 parse 输出缓冲(锁内写、拷局部读)
static std::vector<FileItem> s_music_missing;   // 背景音乐缺失列表(check 写 / fetch 读,锁保护)
static bool s_busy = false;       // 列表加载/下载中
static bool s_quit = false;       // 页面关闭/下载取消
static int s_confirm_idx = -1;    // 确认框指向的角色下标
static const lv_font_t *s_font = NULL;

/* ImageDisplay.cpp 提供:下载页打开前关闭干员索引页(互斥) */
extern void agent_index_hide_for_app(void);

static const lv_font_t *efont(void) { return s_font ? s_font : &lv_font_montserrat_14; }

void role_downloader_set_font(const lv_font_t *font) {
    s_font = font;
}

/* ---------- 小工具 ---------- */
static std::string s_url_host;
static std::string url_host() {
    if (s_url_host.empty()) {
#if defined(CONFIG_RESOURCE_SERVER)
        s_url_host = CONFIG_RESOURCE_SERVER;   // 如 http://124.221.186.33
#endif
    }
    /* 2026-10-05 裸前缀 "http://" 视为未配置:拼接出的 "http:///api/…" 解析出
       空 host → getaddrinfo("") → lwip assert 整机崩溃 */
    if (s_url_host == "http://" || s_url_host == "https://")
        s_url_host.clear();
    return s_url_host;
}

/* 用户仓库模式:接口走 /api/user_repo(服务器按 mac 定位 uid),
 * rel = 用户根下相对路径,落位 /sdcard/_users/u<uid>/<rel>/。
 * 2026-10-07 并发防护:cover 同步/角色下载/主页下载多任务曾共用全局
 * s_files/s_user_mode/s_public_rel,互相覆盖引发三类故障:
 *  - fetch 循环持 s_files[i] 引用,他任务 clear+重建 → 悬空引用 → 乱码文件名(URL 里二进制)
 *  - probe_public("cover") 改全局 rel,角色 fetch 的 file_url_for 读到 "cover" → 404
 *  - 双下载任务并发 → 取消一个另一个仍在跑
 * 方案:mode/rel 全部显式传参(impl 系列,不读全局);s_files 仅作解析缓冲,
 * parse 临界区加锁 + 调用方立即拷贝局部列表;fetch 同一时刻只允许一个任务 */
static std::string url_encode(const std::string &v);   // 定义在本文件稍后

static SemaphoreHandle_t s_dl_mutex = NULL;
static void dl_lock(void) {
    if (!s_dl_mutex) s_dl_mutex = xSemaphoreCreateMutex();
    if (s_dl_mutex) xSemaphoreTake(s_dl_mutex, portMAX_DELAY);
}
static void dl_unlock(void) {
    if (s_dl_mutex) xSemaphoreGive(s_dl_mutex);
}
static bool s_fetching = false;   // 防重入:同一时刻只允许一个下载任务

static std::string manifest_url_for(const char *voc, const char *star, const char *name,
                                    bool umode, int uid, const std::string &urel,
                                    bool pmode, const std::string &prel) {
    if (umode) {
        return url_host() + "/api/user_repo/manifest?mac=" +
               url_encode(SystemInfo::GetMacAddress()) + "&rel=" + url_encode(urel);
    }
    if (pmode) {
        return url_host() + "/api/public/manifest?rel=" + url_encode(prel);
    }
    return url_host() + "/api/roles/" + url_encode(voc) + "/" +
           url_encode(star) + "/" + url_encode(name) + "/manifest";
}

static std::string file_url_for(const char *voc, const char *star, const char *name,
                                const std::string &frel,
                                bool umode, int uid, const std::string &urel,
                                bool pmode, const std::string &prel) {
    if (umode) {
        return url_host() + "/api/user_repo/file?mac=" + url_encode(SystemInfo::GetMacAddress()) +
               "&rel=" + url_encode(urel) + "&p=" + url_encode(frel);
    }
    if (pmode) {
        return url_host() + "/api/public/file?rel=" + url_encode(prel) +
               "&p=" + url_encode(frel);
    }
    return url_host() + "/api/roles/file?v=" + url_encode(voc) +
           "&s=" + url_encode(star) + "&n=" + url_encode(name) + "&p=" + url_encode(frel);
}

static std::string role_local_base(const char *voc, const char *star, const char *name,
                                   bool umode, int uid, const std::string &urel,
                                   bool pmode, const std::string &prel) {
    if (umode) {
        if (urel.rfind("Arknights/", 0) == 0) {
            /* 统一文件系统(2026-09-10):Arknights 类落位 /sdcard/<rel>/,不分用户 */
            return "/sdcard/" + urel + "/";
        }
        /* OC(other/...)保留用户隔离 */
        return "/sdcard/_users/u" + std::to_string(uid) + "/" + urel + "/";
    }
    if (pmode) {
        return "/sdcard/Arknights/main/" + prel + "/";
    }
    return "/sdcard/Arknights/main/operator/" + std::string(voc) + "/" +
           std::string(star) + "/" + std::string(name) + "/";
}

/* __index__ 条目落位:公共库重定向到 INDEX 缩略图目录;用户库直接落在角色目录内 */
static std::string file_dst_for(const char *voc, const char *star, const char *name,
                                const std::string &rel,
                                bool umode, int uid, const std::string &urel,
                                bool pmode, const std::string &prel) {
    if (!umode && !pmode && rel.rfind("__index__/", 0) == 0) {
        return "/sdcard/Arknights/main/operator/INDEX/" + std::string(voc) + "_108x228/" +
               std::string(star) + "/" + std::string(name) + ".jpg";
    }
    return role_local_base(voc, star, name, umode, uid, urel, pmode, prel) + rel;
}

/* URL 编码(设备/角色名/路径可能含中文与特殊字符) */
static std::string url_encode(const std::string &v) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static std::string http_get(const std::string &url, std::string &body, int timeout_ms = 20000) {
    /* 返回空字符串 = 成功;否则返回错误描述。
     * 注意:读循环缓冲只用 1KB 栈(esp_http_client 内部 8KB 解析缓冲是堆分配),
     * 调用本函数的任务栈 ≥10KB 即安全。 */
    /* 2026-10-05 网络未就绪不发请求:新设备 WiFi 未配置时,lwip 的 TCP/IP
       mbox 尚未创建,getaddrinfo 直接 assert(Invalid mbox)整机崩溃。
       旧设备开机自动连 WiFi 所以正常,新设备必崩——这是根因。 */
    if (!WifiStation::GetInstance().IsConnected()) {
        return "network not ready";
    }
    /* 2026-10-05 空/非法 URL 防御:新设备未激活时 OTA 服务器地址为空,
       getaddrinfo("") 触发 lwip assert(Invalid mbox)整机崩溃。
       校验到 host 段:裸前缀 "http:///…" 也拦截(host 为空同样崩) */
    bool _ok = false;
    size_t _scheme = url.find("://");
    if (_scheme != std::string::npos) {
        size_t _hs = _scheme + 3;
        size_t _he = url.find_first_of("/?#", _hs);
        std::string _host = url.substr(_hs, _he == std::string::npos
                                                ? std::string::npos : _he - _hs);
        _ok = !_host.empty();
    }
    if (url.empty() || url.size() < 8 || !_ok) {
        return "empty url";
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = MAX_HTTP_BUF;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "szfz-device/1.0";
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "http client init fail";

    body.clear();
    char buf[1024];

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return "open fail"; }
    int64_t total = esp_http_client_fetch_headers(client);
    if (total < 0) { esp_http_client_cleanup(client); return "fetch headers fail"; }

    while (true) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r <= 0) break;
        body.append(buf, r);
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status < 200 || status >= 300) {
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP %d", status);
        return msg;
    }
    return "";
}

/* 下载到文件(边下边写,返回空=成功;返回 "cancelled" = 回调要求取消)。
 * offset>0 时断点续传:发 HTTP Range,206 → 追加写;服务器不支持(200)→ 从头重写。
 * 取消时不删已写数据(保留 .part 供下次续传)。step_cb 每 1% 调用一次,返回 false 中止。 */
static std::string http_download_ex(const std::string &url, const std::string &dst,
                                    bool (*step_cb)(int pct, const char *file, void *ud),
                                    void *ud, int64_t offset = 0) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 60000;
    cfg.buffer_size = BUF_SIZE;
    cfg.user_agent = "szfz-device/1.0";
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "client init fail";
    if (offset > 0) {
        char rng[48];
        snprintf(rng, sizeof(rng), "bytes=%d-", (int)offset);
        ESP_LOGI(TAG, "http_dl 续传: %s (Range: %s)", url.c_str(), rng);
        esp_http_client_set_header(client, "Range", rng);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return "open fail"; }
    int64_t total = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_cleanup(client);
        char msg[48]; snprintf(msg, sizeof(msg), "HTTP %d", status);
        return msg;
    }
    // 206 = 服务器从 offset 起发剩余数据(追加写);200 = 不支持 Range,从头重写
    bool append = (offset > 0 && status == 206);
    int64_t base = append ? offset : 0;
    FILE *f = fopen(dst.c_str(), append ? "ab" : "wb");
    if (!f) { esp_http_client_cleanup(client); return "SD 写入失败(卡满/未挂载?)"; }

    /* 2026-10-06 下载加速:原 1KB 栈缓冲逐片读+写,实测仅 0.11MB/s——
       每次 read 仅 1KB(TCP 吞吐低)+ fwrite 1KB 落盘(SD 4KB 块写放大)。
       改 32KB 堆缓冲(栈缓冲曾让 10KB 栈下载任务贴近溢出极限)+ stdio
       32KB 全缓冲,批量落盘,预期提速 10 倍以上 */
    char *buf = (char*)malloc(32768);
    if (!buf) { fclose(f); esp_http_client_cleanup(client); return "内存不足"; }
    setvbuf(f, NULL, _IOFBF, 32768);
    int64_t done = 0;
    int last_pct = -1;
    while (true) {
        int r = esp_http_client_read(client, buf, 32768);
        if (r <= 0) break;
        fwrite(buf, 1, r, f);
        done += r;
        g_dl_bytes_done += r;   /* 2026-10-07 速度统计真实字节 */
        int64_t full = base + total;   // 206 时 total=剩余部分
        if (full > 0 && step_cb) {
            int pct = (int)((base + done) * 100 / full);
            if (pct != last_pct) {
                last_pct = pct;
                if (!step_cb(pct, nullptr, ud)) {
                    free(buf);
                    fclose(f);
                    esp_http_client_cleanup(client);
                    return "cancelled";   // 保留 .part(断点续传数据)
                }
            }
        }
    }
    free(buf);
    fclose(f);
    esp_http_client_cleanup(client);
    return "";
}

/* 确保目录存在(逐级 mkdir) */
static bool mkdirs(const std::string &path) {
    if (path.empty()) return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;
    size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0) {
        if (!mkdirs(path.substr(0, pos))) return false;
    }
    return mkdir(path.c_str(), 0777) == 0 || stat(path.c_str(), &st) == 0;
}

/* 递归删除目录(含内容);不存在/是文件时 unlink。 */
static void remove_dir_r(const std::string &path) {
    DIR *d = opendir(path.c_str());
    if (!d) { unlink(path.c_str()); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        remove_dir_r(path + "/" + e->d_name);
    }
    closedir(d);
    rmdir(path.c_str());
}

/* FATFS f_rename 不覆盖已存在目标(返回 FR_EXIST):覆盖式改名 = 先删目标再改。
   失败保留 .part(下次续传/重试)。 */
static bool rename_overwrite(const char *from, const char *to) {
    struct stat st;
    if (stat(to, &st) == 0) unlink(to);
    return rename(from, to) == 0;
}

/* ---------- JSON 解析 ---------- */
static void parse_roles(const std::string &body) {
    s_roles.clear();
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return; }
    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n && (int)s_roles.size() < MAX_ROLES; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!item) continue;
        RoleInfo r;
        cJSON *j;
        if ((j = cJSON_GetObjectItem(item, "name")) && cJSON_IsString(j)) r.name = j->valuestring;
        if ((j = cJSON_GetObjectItem(item, "vocation")) && cJSON_IsString(j)) r.vocation = j->valuestring;
        if ((j = cJSON_GetObjectItem(item, "star")) && cJSON_IsString(j)) r.star = j->valuestring;
        if ((j = cJSON_GetObjectItem(item, "incomplete")) && cJSON_IsBool(j)) r.incomplete = cJSON_IsTrue(j);
        if (!r.name.empty()) s_roles.push_back(std::move(r));
    }
    cJSON_Delete(root);
}

static bool parse_manifest(const std::string &body) {
    s_files.clear();
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) return false;
    cJSON *files = cJSON_GetObjectItem(root, "files");
    if (files && cJSON_IsArray(files)) {
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(files, i);
            FileItem f;
            cJSON *j;
            if ((j = cJSON_GetObjectItem(item, "rel")) && cJSON_IsString(j)) f.rel = j->valuestring;
            if ((j = cJSON_GetObjectItem(item, "size"))) f.size = (int64_t)j->valuedouble;
            if (!f.rel.empty()) s_files.push_back(std::move(f));
        }
    }
    cJSON_Delete(root);
    return !s_files.empty();
}

/* ---------- 下载核心(下载页/索引页弹窗共用) ---------- */

/* 2026-10-07 清单总字节统计(局部文件列表用) */
static int64_t sum_files(const std::vector<FileItem> &files) {
    int64_t total = 0;
    for (const FileItem &f : files) total += f.size;
    ESP_LOGI(TAG, "probe OK: %d 个文件, 共 %d KB", (int)files.size(), (int)(total / 1024));
    return total;
}

static int64_t probe_impl(const char *voc, const char *star, const char *name,
                          bool umode, int uid, const std::string &urel,
                          bool pmode, const std::string &prel,
                          std::vector<FileItem> *files_out) {
    std::string murl = manifest_url_for(voc, star, name, umode, uid, urel, pmode, prel);
    ESP_LOGI(TAG, "probe: %s", murl.c_str());
    std::string body, err;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = http_get(murl, body);
        if (err.empty()) {
            /* 解析临界区:全局 s_files 是共享缓冲,拷局部后解锁(并发任务安全) */
            dl_lock();
            bool ok = parse_manifest(body);
            std::vector<FileItem> files = s_files;
            dl_unlock();
            if (ok && files_out) *files_out = files;
            if (ok) return files.empty() ? -2 : sum_files(files);
            err = "parse fail";
        }
        if (err.rfind("HTTP 404", 0) == 0) return -2;   // 云端无此角色包:重试无意义
        ESP_LOGW(TAG, "probe manifest 失败(第%d次): %s", attempt + 1, err.c_str());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (err.rfind("HTTP 404", 0) == 0) return -2;   // 云端资源未就绪(区别于网络失败)
    if (!err.empty()) return -1;
    return -1;
}

int64_t role_download_probe(const char *voc, const char *star, const char *name) {
    return probe_impl(voc, star, name, false, 0, "", false, "", NULL);
}

int role_download_estimate_minutes(int64_t total_bytes) {
    const int64_t KBPS = 500;   // 保守速度估算(kB/s,实测 2.4G WiFi 明文下载 1MB/s+)
    int64_t secs = total_bytes / KBPS / 1024;
    int m = (int)((secs + 59) / 60);
    return m < 1 ? 1 : m;
}

static int check_impl(const char *voc, const char *star, const char *name,
                      bool umode, int uid, const std::string &urel,
                      bool pmode, const std::string &prel, int64_t *total_out) {
    std::vector<FileItem> files;
    int64_t total = probe_impl(voc, star, name, umode, uid, urel, pmode, prel, &files);
    if (total_out) *total_out = (total >= 0 ? total : -1);
    if (total == -2) return -2;   // 云端无此角色(手工角色):按齐全处理,直接展示
    if (total < 0) return -1;     // 网络失败:未知
    for (const FileItem &f : files) {
        /* 2026-09-26:__index__ 缩略图缺失不判"有更新"——缩略图由索引页 thumbs
           同步静默补,不该因缺一张缩略图弹"更新资源"弹窗(资源本体齐全即齐全) */
        if (f.rel.rfind("__index__/", 0) == 0) continue;
        std::string p = file_dst_for(voc, star, name, f.rel, umode, uid, urel, pmode, prel);
        struct stat st;
        if (stat(p.c_str(), &st) != 0 || st.st_size != f.size) {
            ESP_LOGI(TAG, "check %s: 缺/异 %s", name, f.rel.c_str());
            return 0;   // 部分缺失
        }
        /* 2026-09-17:mesh.ppdq 下载中断残留(大小与清单一致但 magic 坏)——
           曾导致设备永远 bad magic、永不重下、动作按钮全无的死循环 */
        if (f.rel.size() >= 9 && f.rel.compare(f.rel.size() - 9, 9, "mesh.ppdq") == 0) {
            FILE *ff = fopen(p.c_str(), "rb");
            char mg[4] = {0, 0, 0, 0};
            bool bad = true;
            if (ff) {
                if (fread(mg, 1, 4, ff) == 4 && memcmp(mg, "PPDQ", 4) == 0) bad = false;
                fclose(ff);
            }
            if (bad) {
                ESP_LOGW(TAG, "check %s: %s magic 坏(残留) → 判缺重下", name, f.rel.c_str());
                return 0;
            }
        }
    }
    return 1;   // 齐全
}

int role_download_check(const char *voc, const char *star, const char *name, int64_t *total_out) {
    return check_impl(voc, star, name, false, 0, "", false, "", total_out);
}

int role_download_list(std::vector<CloudRole> &out) {
    std::string body, err;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = http_get(url_host() + "/api/roles", body, 8000);
        if (err.empty()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!err.empty()) {
        ESP_LOGW(TAG, "role_download_list 失败: %s", err.c_str());
        return 1;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return 1; }
    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!item) continue;
        CloudRole r;
        cJSON *j;
        if ((j = cJSON_GetObjectItem(item, "name")) && cJSON_IsString(j)) r.name = j->valuestring;
        if ((j = cJSON_GetObjectItem(item, "vocation")) && cJSON_IsString(j)) r.voc = j->valuestring;
        if ((j = cJSON_GetObjectItem(item, "star")) && cJSON_IsString(j)) r.star = j->valuestring;
        if ((j = cJSON_GetObjectItem(item, "incomplete")) && cJSON_IsBool(j)) r.incomplete = cJSON_IsTrue(j);
        if (!r.name.empty() && !r.voc.empty() && !r.star.empty()) out.push_back(std::move(r));
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "role_download_list: %d roles", (int)out.size());
    return 0;
}

int role_download_thumbs(std::vector<CloudRole> &out) {
    std::string body, err;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = http_get(url_host() + "/api/roles/thumbs", body, 8000);
        if (err.empty()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!err.empty()) {
        ESP_LOGW(TAG, "thumbs 获取失败: %s", err.c_str());
        return 1;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) return 1;
    cJSON *arr = cJSON_GetObjectItem(root, "thumbs");
    if (arr && cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (!item) continue;
            CloudRole r;
            cJSON *j;
            if ((j = cJSON_GetObjectItem(item, "voc")) && cJSON_IsString(j)) r.voc = j->valuestring;
            if ((j = cJSON_GetObjectItem(item, "star")) && cJSON_IsString(j)) r.star = j->valuestring;
            if ((j = cJSON_GetObjectItem(item, "name")) && cJSON_IsString(j)) r.name = j->valuestring;
            if (!r.name.empty() && !r.voc.empty() && !r.star.empty()) out.push_back(std::move(r));
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "role_download_thumbs: %d", (int)out.size());
    return 0;
}

int role_download_thumb(const char *voc, const char *star, const char *name) {
    /* 已有则跳过(下载器完整下载也会写入同路径) */
    char dst[320];
    snprintf(dst, sizeof(dst), "/sdcard/Arknights/main/operator/INDEX/%.31s_108x228/%.15s/%.63s.jpg",
             voc, star, name);   // 精度限制:职业/星级/角色名均有界,防截断告警
    struct stat st;
    if (stat(dst, &st) == 0 && st.st_size > 0) return 0;
    std::string durl = url_host() + "/api/roles/file?v=" + url_encode(voc) +
                       "&s=" + url_encode(star) + "&n=" + url_encode(name) +
                       "&p=__index__/" + url_encode(name) + ".jpg";
    std::string dir = dst;
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos) mkdirs(dir.substr(0, slash));
    /* SD 卡与 WiFi 协处理器共享 SDMMC 总线:小文件连续写会与 WiFi 下行争抢
       导致瞬时写超时(0x107)——失败重试 2 次,间隔拉长让总线喘息 */
    std::string err;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = http_download_ex(durl, dst, nullptr, nullptr);
        if (err.empty()) return 0;
        if (err.rfind("HTTP 4", 0) == 0) return -1;   // 404/400:服务器无此缩略图
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    return 1;
}

static int fetch_impl(const char *voc, const char *star, const char *name,
                      bool umode, int uid, const std::string &urel,
                      bool pmode, const std::string &prel,
                      bool (*step_cb)(int pct, const char *file, void *ud), void *ud) {
    /* 1. manifest(URL 由显式参数构造,不受并发任务影响) */
    std::string murl = manifest_url_for(voc, star, name, umode, uid, urel, pmode, prel);
    std::string body, err;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = http_get(murl, body);
        if (err.empty()) {
            dl_lock();
            bool ok = parse_manifest(body);
            dl_unlock();
            if (ok) { err = ""; break; }
            err = "parse fail";
        }
        ESP_LOGW(TAG, "manifest 获取失败(第%d次): %s", attempt + 1, err.c_str());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!err.empty()) return 1;
    /* 文件列表局部拷贝:循环内引用绝不悬空(他任务 parse 不再影响本任务) */
    dl_lock();
    std::vector<FileItem> files = s_files;
    dl_unlock();
    if (files.empty()) return 1;

    /* 2. 逐文件增量下载:
     *    - 已存在且大小与清单一致 → 跳过(增量)
     *    - 先下到 <目标>.part(断点续传载体),完成后再改名落位
     *    - .part 已有部分数据 → HTTP Range 续传
     *    旧文件在失败/取消时保持原样(不再整体搬备份区);.done 只在全部就绪时写。 */
    int ok = 0, skip = 0, fail = 0;
    bool cancelled = false;
    size_t total_files = files.size();
    for (size_t i = 0; i < total_files; i++) {
        const FileItem &f = files[i];
        if (step_cb && !step_cb((int)(i * 100 / total_files), f.rel.c_str(), ud)) {
            cancelled = true;
            break;
        }
        std::string dst = file_dst_for(voc, star, name, f.rel, umode, uid, urel, pmode, prel);
        struct stat st;
        if (stat(dst.c_str(), &st) == 0 && st.st_size == f.size) {
            /* 2026-09-17:mesh.ppdq 坏文件(下载中断残留,大小一致 magic 错)→ 不跳过,强制重下 */
            bool bad_ppdq = false;
            if (f.rel.size() >= 9 && f.rel.compare(f.rel.size() - 9, 9, "mesh.ppdq") == 0) {
                FILE *ff = fopen(dst.c_str(), "rb");
                char mg[4] = {0, 0, 0, 0};
                if (!ff || fread(mg, 1, 4, ff) != 4 || memcmp(mg, "PPDQ", 4) != 0) bad_ppdq = true;
                if (ff) fclose(ff);
            }
            if (!bad_ppdq) {
                skip++;   // 已一致:不重复下载
                continue;
            }
            ESP_LOGW(TAG, "fetch: %s magic 坏 → 重下", f.rel.c_str());
        }
        std::string durl = file_url_for(voc, star, name, f.rel, umode, uid, urel, pmode, prel);

        /* 目录不存在时 mkdirs(根目录在第 1 个文件时创建) */
        std::string dir = dst;
        size_t slash = dir.rfind('/');
        if (slash != std::string::npos) {
            dir = dir.substr(0, slash);
            if (!mkdirs(dir)) {
                ESP_LOGW(TAG, "mkdirs fail: %s", dir.c_str());
            }
        }
        std::string part = dst + ".part";
        int64_t offset = 0;
        if (stat(part.c_str(), &st) == 0) {
            if (st.st_size == f.size) {   // 上次已下完未改名:直接落位
                if (rename_overwrite(part.c_str(), dst.c_str())) { ok++; continue; }
                ESP_LOGW(TAG, "%s .part 改名失败,重下", f.rel.c_str());
                unlink(part.c_str());
            } else if (st.st_size > f.size) {
                unlink(part.c_str());     // 异常:比目标还大,作废重下
            } else {
                offset = st.st_size;      // 断点续传
            }
        }

        ESP_LOGI(TAG, "fetch %s: 清单大小=%d, 断点=%d, part存在=%d",
                 f.rel.c_str(), (int)f.size, (int)offset,
                 (int)(stat(part.c_str(), &st) == 0));
        std::string derr;
        for (int attempt = 0; attempt < 3; attempt++) {
            derr = http_download_ex(durl, part, step_cb, ud, offset);
            if (derr.empty()) break;
            if (derr == "cancelled") { cancelled = true; break; }
            ESP_LOGW(TAG, "%s 下载失败(第%d次): %s | URL: %s", f.rel.c_str(), attempt + 1,
                     derr.c_str(), durl.c_str());   /* 2026-10-07 URL 入日志(排查 404/400) */
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (stat(part.c_str(), &st) == 0 && st.st_size > offset)
                offset = st.st_size;   // 重试从新进度续传
        }
        if (cancelled) break;
        if (derr.empty()) {
            if (rename_overwrite(part.c_str(), dst.c_str())) ok++;
            else { ESP_LOGW(TAG, "rename .part 失败: %s", dst.c_str()); fail++; }
        } else {
            fail++;   // .part 保留,下次续传
        }
    }

    /* 3. 收尾:全部就绪(下载+跳过)写 .done;失败/取消保留 .part 与旧 .done(角色按旧数据仍完整) */
    if (cancelled) {
        ESP_LOGI(TAG, "%s 取消: %d ok / %d skip(.part 保留可续传)", name, ok, skip);
        return 2;
    }
    if (fail == 0) {
        FILE *df = fopen((role_local_base(voc, star, name, umode, uid, urel, pmode, prel) + ".done").c_str(), "w");
        if (df) {
            fprintf(df, "ok");
            fclose(df);
        }
        ESP_LOGI(TAG, "Download complete: %s (%d 下载 / %d 跳过)", name, ok, skip);
        return 0;
    }
    ESP_LOGW(TAG, "%s 下载不完整: %d ok / %d skip / %d fail(.part 保留可续传)",
             name, ok, skip, fail);
    return 1;
}

/* 2026-10-07 防重入:双下载任务并发会互相踩 s_files(引用悬空→乱码 URL),
   且"取消一个另一个仍在跑";同一时刻只允许一个下载任务 */
static int fetch_guarded(const char *voc, const char *star, const char *name,
                         bool umode, int uid, const std::string &urel,
                         bool pmode, const std::string &prel,
                         bool (*step_cb)(int pct, const char *file, void *ud), void *ud) {
    dl_lock();
    if (s_fetching) {
        dl_unlock();
        ESP_LOGW(TAG, "已有下载任务进行中,忽略新请求");
        return 3;
    }
    s_fetching = true;
    dl_unlock();
    int r = fetch_impl(voc, star, name, umode, uid, urel, pmode, prel, step_cb, ud);
    dl_lock();
    s_fetching = false;
    dl_unlock();
    return r;
}

int role_download_fetch(const char *voc, const char *star, const char *name,
                        bool (*step_cb)(int pct, const char *file, void *ud), void *ud) {
    return fetch_guarded(voc, star, name, false, 0, "", false, "", step_cb, ud);
}

/* ---------- 用户仓库三件套(复用公共库逻辑,URL 与落位走 _users/<rel>) ---------- */

int64_t role_download_probe_user(int uid, const char *rel) {
    return probe_impl("", "", "", true, uid, rel, false, "", NULL);
}

int role_download_check_user(int uid, const char *rel, int64_t *total_out) {
    return check_impl("", "", "", true, uid, rel, false, "", total_out);
}

int role_download_fetch_user(int uid, const char *rel,
                             bool (*step_cb)(int pct, const char *file, void *ud), void *ud) {
    return fetch_guarded("", "", "", true, uid, rel, false, "", step_cb, ud);
}

/* ---------- 公共共享资源三件套(背景/音乐等,2026-09-10;rel 相对 Arknights/main) ---------- */

int64_t role_download_probe_public(const char *rel) {
    return probe_impl("", "", "", false, 0, "", true, rel, NULL);
}

int role_download_check_public(const char *rel, int64_t *total_out) {
    return check_impl("", "", "", false, 0, "", true, rel, total_out);
}

int role_download_fetch_public(const char *rel,
                               bool (*step_cb)(int pct, const char *file, void *ud), void *ud) {
    /* 2026-10-05 资源服务器未配置(CONFIG_RESOURCE_SERVER 空)时直接失败:
       url 会是 "/api/..." 无 host,再往下走 http_get 曾触发 lwip assert 整机崩溃 */
    if (url_host().empty()) {
        ESP_LOGW(TAG, "fetch_public: 资源服务器地址为空,跳过");
        return -1;
    }
    return fetch_guarded("", "", "", false, 0, "", true, rel, step_cb, ud);
}

/* ---------- 背景音乐清单检查/下载(2026-09-11) ---------- */

int role_download_check_music_missing(void) {
    /* 拉 /api/public/music_manifest,对比本地 /sdcard/Arknights/main/music 目录下的 wav。
       返回:≥0=缺失首数(缺失文件名写入 s_music_missing 供下载) -1=网络失败/清单不可用 */
    std::string body, err;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = http_get(url_host() + "/api/public/music_manifest", body);
        if (err.empty()) break;
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (!err.empty()) return -1;
    /* 清单存本地:播放时按场景名查实际文件名(2026-09-11)。
       首次使用时 music 目录可能还不存在,必须先建目录(2026-09-13 修:
       "音乐清单不存在"根因——目录缺失时 fopen 静默失败,清单从未落盘) */
    {
        mkdirs("/sdcard/Arknights/main/music");
        FILE *mf = fopen("/sdcard/Arknights/main/music/music_manifest.json", "wb");
        if (mf) {
            fwrite(body.c_str(), 1, body.size(), mf);
            fclose(mf);
        } else {
            ESP_LOGW(TAG, "music_manifest.json 落盘失败");
        }
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) return -1;
    int missing = 0;
    dl_lock();
    s_music_missing.clear();
    cJSON *tracks = cJSON_GetObjectItem(root, "tracks");
    if (tracks && cJSON_IsArray(tracks)) {
        int n = cJSON_GetArraySize(tracks);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(tracks, i);
            cJSON *fj = it ? cJSON_GetObjectItem(it, "file") : NULL;
            if (!fj || !cJSON_IsString(fj)) continue;
            FileItem fi;
            fi.rel = fj->valuestring;
            fi.size = 0;
            std::string dst = "/sdcard/Arknights/main/music/" + fi.rel;
            struct stat st;
            if (stat(dst.c_str(), &st) != 0 || st.st_size == 0) {
                s_music_missing.push_back(fi);
                missing++;
            }
        }
    }
    dl_unlock();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "music check: 缺失 %d 首", missing);
    return missing;
}

int role_download_fetch_music_missing(bool (*step_cb)(int pct, const char *file, void *ud), void *ud) {
    /* 下载 s_music_missing 中记录的缺失 wav(本地已存在且 >0 跳过;无断点,单曲小文件) */
    dl_lock();
    std::vector<FileItem> files = s_music_missing;
    dl_unlock();
    size_t total_files = files.size();
    int ok = 0, fail = 0;
    for (size_t i = 0; i < total_files; i++) {
        const FileItem &f = files[i];
        if (step_cb && !step_cb((int)(i * 100 / total_files), f.rel.c_str(), ud)) return 2;
        std::string dst = "/sdcard/Arknights/main/music/" + f.rel;
        struct stat st;
        if (stat(dst.c_str(), &st) == 0 && st.st_size > 0) { ok++; continue; }
        std::string dir = dst;
        size_t slash = dir.rfind('/');
        if (slash != std::string::npos) mkdirs(dir.substr(0, slash));
        std::string durl = url_host() + "/api/public/file?rel=music&p=" + url_encode(f.rel);
        std::string derr = http_download_ex(durl, dst, step_cb, ud, 0);
        if (derr.empty()) ok++;
        else {
            fail++;
            ESP_LOGW(TAG, "music 下载失败: %s (%s)", f.rel.c_str(), derr.c_str());
        }
    }
    ESP_LOGI(TAG, "music fetch: %d ok / %d fail", ok, fail);
    return fail == 0 ? 0 : 1;
}

/* ---------- 一键克隆(公共库→绑定用户仓库,2026-09-10) ---------- */

/* POST JSON(小请求小响应;克隆是低频操作,timeout 拉长到 90s 覆盖 copytree) */
static std::string http_post(const std::string &url, const std::string &json_body,
                             std::string &resp_body, int *out_status) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 90000;
    cfg.buffer_size = 1024;
    cfg.user_agent = "szfz-device/1.0";
    cfg.method = HTTP_METHOD_POST;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "client init fail";
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, (int)json_body.size());
    if (err != ESP_OK) { esp_http_client_cleanup(client); return "open fail"; }
    int w = esp_http_client_write(client, json_body.c_str(), (int)json_body.size());
    if (w < 0) { esp_http_client_cleanup(client); return "write fail"; }
    int64_t total = esp_http_client_fetch_headers(client);
    if (total < 0) { esp_http_client_cleanup(client); return "fetch headers fail"; }
    resp_body.clear();
    char buf[1024];
    while (true) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r <= 0) break;
        resp_body.append(buf, r);
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (out_status) *out_status = status;
    if (status < 200 || status >= 300) {
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP %d", status);
        return msg;
    }
    return "";
}

int role_download_clone(const char *voc, const char *star, const char *name) {
    /* POST /api/user_repo/clone(mac 定位用户;服务器复制公共库角色目录+建 agent 行)。
       返回:0=成功 1=网络/其它失败 2=配额不足(403 storage/quota)
             3=已克隆过(409,直接进下载即可) 4=云端无此角色(404) */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "voc", voc);
    cJSON_AddStringToObject(root, "star", star);
    cJSON_AddStringToObject(root, "name", name);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string body(js ? js : "{}");
    if (js) free(js);

    std::string resp, err;
    int status = 0;
    ESP_LOGI(TAG, "clone: %s/%s/%s", voc, star, name);
    err = http_post(url_host() + "/api/user_repo/clone", body, resp, &status);
    if (!err.empty()) {
        if (status == 403) {
            /* 2026-09-30 未验收角色:服务器拒绝克隆(error=notaccepted) */
            if (resp.find("notaccepted") != std::string::npos ||
                resp.find("验收") != std::string::npos) return 5;
            /* 区分存储配额/克隆个数/未绑定:消息里含"配额"则按配额处理 */
            if (resp.find("storage") != std::string::npos ||
                resp.find("quota") != std::string::npos ||
                resp.find("配额") != std::string::npos) return 2;
            return 1;   // 未绑定等其它 403
        }
        if (status == 409) return 3;
        if (status == 404) return 4;
        ESP_LOGW(TAG, "clone 失败: %s (HTTP %d)", err.c_str(), status);
        return 1;
    }
    ESP_LOGI(TAG, "clone OK: %s", resp.c_str());
    return 0;
}

/* ---------- 目录清空工具(换绑用户时清 operator,2026-09-10) ---------- */

void role_download_remove_dir(const char *path) {
    if (!path || !path[0]) return;
    remove_dir_r(path);
}

int role_download_fetch_public_profile(const char *fname, const char *dst) {
    /* 2026-10-06 公共 Ur_Info 下载(蟑螂派对默认照片/动图):单文件无断点 */
    if (url_host().empty()) return -1;   // 网络未就绪防护(曾触发 lwip assert)
    std::string url = url_host() + "/api/public/user_profile?f=" + fname;
    std::string err = http_download_ex(url, dst, NULL, NULL, 0);
    return err.empty() ? 0 : -1;
}

/* ---------- CP(协处理器)固件自动 OTA(2026-10-07) ----------
 * C6 出厂固件 2.3.2 无 SDIO SW_AGGR → 兼容流模式 → WiFi 吞吐 ~20KB/s
 * (换热点不变、PC 同链路 1MB/s,瓶颈确在板内 SDIO 通道)。
 * 开机网络就绪后检查 CP 版本,低于 host 3.0.9 则从资源服务器下载官方
 * 预编译固件(esphome/esp-hosted-firmware v3.0.9,服务器侧已 SHA256 校验),
 * 经 SDIO OTA 写入后整机重启(开机流程自动复位 CP 跑新固件)。
 * 失败安全:任何一步失败 → 下次开机自动重试;版本已匹配 → 静默跳过。 */
#include "eh_host_cp_ota.h"   // feature include 在组件公开路径

/* transport 头不在公开 include 路径,extern 声明即可(符号随 esp_hosted 链接) */
extern "C" uint32_t eh_host_mcu_transport_get_fw_version(void);

/* host 组件版本 3.0.9(与 esp_hosted 组件 idf_component.yml 一致;
   打包格式 major<<16|minor<<8|patch,与 eh_common_fw_version.h 相同) */
#define CP_OTA_HOST_VER 0x030009u
#define CP_OTA_CHUNK 1536u   /* EH_RPC_OTA_CHUNK_MAX */

static bool cp_ota_download(uint8_t **buf_out, size_t *len_out) {
    if (url_host().empty()) return false;
    std::string url = url_host() + "/api/public/file?rel=main&p=cp_fw.bin";
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 120000;
    cfg.buffer_size = 4096;
    cfg.user_agent = "szfz-device/1.0";
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return false; }
    int64_t total = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || total <= 0 || total > 4 * 1024 * 1024) {
        esp_http_client_cleanup(client);
        return false;
    }
    uint8_t *buf = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(client); return false; }
    int64_t done = 0;
    while (done < total) {
        int r = esp_http_client_read(client, (char*)buf + done, (int)(total - done));
        if (r <= 0) break;
        done += r;
    }
    esp_http_client_cleanup(client);
    if (done != total) { heap_caps_free(buf); return false; }
    *buf_out = buf;
    *len_out = (size_t)total;
    return true;
}

void cp_ota_task(void *arg) {
    /* 等网络就绪(实测 WiFi 40s+ 才连上,OTA 需完整下载) */
    for (int i = 0; i < 90 && !WifiStation::GetInstance().IsConnected(); i++)
        vTaskDelay(pdMS_TO_TICKS(2000));
    if (!WifiStation::GetInstance().IsConnected()) {
        vTaskDelete(NULL);
        return;
    }
    uint32_t cp_ver = eh_host_mcu_transport_get_fw_version();
    if (cp_ver == 0 || cp_ver >= CP_OTA_HOST_VER) {
        ESP_LOGI("CpOta", "cp fw 0x%06x 已匹配 host 0x%06x,跳过", (unsigned)cp_ver, (unsigned)CP_OTA_HOST_VER);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW("CpOta", "cp fw 0x%06x 落后 host 0x%06x → 自动 OTA", (unsigned)cp_ver, (unsigned)CP_OTA_HOST_VER);
    uint8_t *fw = NULL;
    size_t fw_len = 0;
    if (!cp_ota_download(&fw, &fw_len)) {
        ESP_LOGE("CpOta", "固件下载失败,下次开机重试");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI("CpOta", "固件 %u 字节已下载,开始 SDIO OTA", (unsigned)fw_len);
    bool ok = (eh_host_cp_ota_begin() == ESP_OK);
    size_t off = 0;
    while (ok && off < fw_len) {
        size_t n = (fw_len - off > CP_OTA_CHUNK) ? CP_OTA_CHUNK : (fw_len - off);
        if (eh_host_cp_ota_write(fw + off, (uint32_t)n) != ESP_OK) {
            ok = false;
            break;
        }
        off += n;
        vTaskDelay(pdMS_TO_TICKS(5));   // CP 写内部闪存,每块稍作停顿
        if ((off & 0x1FFFF) == 0) ESP_LOGI("CpOta", "OTA %u/%u", (unsigned)off, (unsigned)fw_len);
    }
    heap_caps_free(fw);
    if (!ok || eh_host_cp_ota_end() != ESP_OK || eh_host_cp_ota_activate() != ESP_OK) {
        ESP_LOGE("CpOta", "OTA 传输失败,下次开机重试");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW("CpOta", "OTA 完成 → 1 秒后整机重启应用新固件");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

int role_download_clear_operator(void) {
    /* 清空 /sdcard/Arknights/main/operator 全部角色目录,保留 INDEX(缩略图公共共享)。
       返回 0=完成 1=目录不存在(视为已清) -1=部分失败 */
    const char *base = "/sdcard/Arknights/main/operator";
    DIR *d = opendir(base);
    if (!d) return 1;
    struct dirent *e;
    int removed = 0, failed = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (strcmp(e->d_name, "INDEX") == 0) continue;   // 缩略图保留
        std::string p = std::string(base) + "/" + e->d_name;
        remove_dir_r(p);
        struct stat st;
        if (stat(p.c_str(), &st) == 0) failed++;
        else removed++;
    }
    closedir(d);
    ESP_LOGI(TAG, "clear operator: %d 目录删除, %d 失败", removed, failed);
    return failed > 0 ? -1 : 0;
}

/* ---------- UI(下载页) ---------- */
/* 任务线程更新 LVGL 必须加锁 */
static void ui_status(const char *text) {
    if (s_status_label && lvgl_port_lock(pdMS_TO_TICKS(200))) {
        lv_label_set_text(s_status_label, text);
        lvgl_port_unlock();
    }
}
static void ui_hide_progress() {
    if (s_progress && lvgl_port_lock(pdMS_TO_TICKS(200))) {
        lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

static void page_close() {
    if (s_page) {
        lv_obj_del(s_page);
        s_page = nullptr;
        s_list = nullptr;
        s_progress = nullptr;
        s_status_label = nullptr;
        s_confirm_box = nullptr;
        s_confirm_lbl = nullptr;
    }
    s_confirm_idx = -1;
    s_quit = true;
}

void role_downloader_close(void) {
    /* 供 ImageDisplay 互斥调用(干员索引页打开前关闭本页):LVGL 任务上下文 */
    page_close();
}

/* ---------- 确认框 ---------- */
static void hide_confirm() {
    if (s_confirm_box) lv_obj_add_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);
    s_confirm_idx = -1;
}

static void show_confirm(int idx) {
    if (!s_confirm_box || idx < 0 || idx >= (int)s_roles.size()) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "下载 %s ?", s_roles[idx].name.c_str());
    lv_label_set_text(s_confirm_lbl, msg);
    lv_obj_remove_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_confirm_box);
    s_confirm_idx = idx;
}

static void download_task(void *arg);

static void on_role_click(lv_event_t *e) {
    if (s_busy) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s_roles.size()) return;
    if (s_roles[idx].incomplete) {
        ui_status("该角色资源不完整,暂不可下载");
        ESP_LOGW(TAG, "%s incomplete, refuse download", s_roles[idx].name.c_str());
        return;
    }
    show_confirm(idx);
}

static void start_download(int idx) {
    if (idx < 0 || idx >= (int)s_roles.size()) return;
    s_busy = true;
    s_quit = false;
    lv_obj_t *bar = s_progress;
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status_label, "准备下载…");
    ESP_LOGI(TAG, "Download start: %s (%s/%s)", s_roles[idx].name.c_str(),
             s_roles[idx].vocation.c_str(), s_roles[idx].star.c_str());
    /* 栈 12KB:http_download_ex 内 4KB 缓冲 + esp_http_client 解析栈 */
    xTaskCreate(download_task, "role_dl", 12288, (void *)(intptr_t)idx, 5, nullptr);
}

static void download_task(void *arg) {
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= (int)s_roles.size()) { s_busy = false; vTaskDelete(nullptr); return; }
    RoleInfo role = s_roles[idx];

    int r = role_download_fetch(role.vocation.c_str(), role.star.c_str(), role.name.c_str(),
        [](int pct, const char *file, void *ud) -> bool {
            if (s_quit) return false;
            if (file && s_status_label && lvgl_port_lock(pdMS_TO_TICKS(100))) {
                lv_label_set_text(s_status_label, file);
                lvgl_port_unlock();
            }
            if (pct >= 0 && s_progress && lvgl_port_lock(pdMS_TO_TICKS(100))) {
                lv_bar_set_value(s_progress, pct, LV_ANIM_OFF);
                lvgl_port_unlock();
            }
            return true;
        }, nullptr);

    /* manifest 是否带 INDEX 缩略图(2026-10-07 锁内拷局部:他任务 probe 会改 s_files) */
    dl_lock();
    std::vector<FileItem> files_after = s_files;
    dl_unlock();
    bool has_thumb = false;
    for (const FileItem &f : files_after)
        if (f.rel.rfind("__index__/", 0) == 0) has_thumb = true;

    if (r == 0) {
        ui_status(has_thumb ? "下载完成!点'返回'去罗德岛选角色"
                            : "已下载,但缺角色页缩略图");
    } else if (r == 2) {
        ui_status("已取消");
    } else if (r == 3) {
        ui_status("已有下载任务进行中,请稍候");   // 2026-10-07 防重入
    } else {
        ui_status("下载失败,原角色已恢复,可重试");
    }
    ui_hide_progress();
    s_busy = false;
    vTaskDelete(nullptr);
}

static void on_back(lv_event_t *e) {
    s_quit = true;
    page_close();
}

void role_downloader_show(void) {
    if (s_page) return;
    agent_index_hide_for_app();   // 互斥:关闭干员索引页(两页同层,叠加会串事件)
    if (!lvgl_port_lock(0)) return;
    s_quit = false;

    s_page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_page, 480, 800);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x1a1a24), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);

    /* 标题栏 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "角色下载");
    lv_obj_set_pos(title, 20, 8);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, efont(), 0);

    lv_obj_t *btn = lv_btn_create(s_page);
    lv_obj_set_size(btn, 70, 34);
    lv_obj_set_pos(btn, 390, 8);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_font(bl, efont(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);

    /* 状态 + 进度 */
    s_status_label = lv_label_create(s_page);
    lv_obj_set_pos(s_status_label, 20, 56);
    lv_label_set_text(s_status_label, "加载角色列表…");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9fd3b0), 0);
    lv_obj_set_style_text_font(s_status_label, efont(), 0);

    s_progress = lv_bar_create(s_page);
    lv_obj_set_size(s_progress, 440, 14);
    lv_obj_set_pos(s_progress, 20, 84);
    lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);

    /* 列表(LVGL 裁剪了 list 组件,用滚动容器+按钮列) */
    s_list = lv_obj_create(s_page);
    lv_obj_set_size(s_list, 440, 680);
    lv_obj_set_pos(s_list, 20, 108);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x24242f), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);

    /* 确认框(点角色后弹出:[下载]/[取消]) */
    s_confirm_box = lv_obj_create(s_page);
    lv_obj_set_size(s_confirm_box, 340, 150);
    lv_obj_set_pos(s_confirm_box, 70, 300);
    lv_obj_set_style_bg_color(s_confirm_box, lv_color_hex(0x2a2a35), 0);
    lv_obj_set_style_bg_opa(s_confirm_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_confirm_box, 2, 0);
    lv_obj_set_style_border_color(s_confirm_box, lv_color_hex(0x666688), 0);
    lv_obj_set_style_radius(s_confirm_box, 10, 0);
    lv_obj_set_style_pad_all(s_confirm_box, 0, 0);
    s_confirm_lbl = lv_label_create(s_confirm_box);
    lv_label_set_text(s_confirm_lbl, "下载 ?");
    lv_obj_align(s_confirm_lbl, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_text_color(s_confirm_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_confirm_lbl, efont(), 0);

    lv_obj_t *ok_btn = lv_btn_create(s_confirm_box);
    lv_obj_set_size(ok_btn, 120, 44);
    lv_obj_set_pos(ok_btn, 36, 88);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x00AA55), 0);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "下载");
    lv_obj_set_style_text_font(ok_lbl, efont(), 0);
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok_btn, [](lv_event_t *e) {
        int idx = s_confirm_idx;
        hide_confirm();
        start_download(idx);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *no_btn = lv_btn_create(s_confirm_box);
    lv_obj_set_size(no_btn, 120, 44);
    lv_obj_set_pos(no_btn, 184, 88);
    lv_obj_set_style_bg_color(no_btn, lv_color_hex(0x555555), 0);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "取消");
    lv_obj_set_style_text_font(no_lbl, efont(), 0);
    lv_obj_center(no_lbl);
    lv_obj_add_event_cb(no_btn, [](lv_event_t *e) {
        hide_confirm();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();

    /* 拉取角色列表(小任务,栈 10KB:http_get 1KB 栈缓冲 + esp_http_client) */
    s_busy = true;
    ESP_LOGI(TAG, "Downloader page shown, server=%s", url_host().c_str());
    xTaskCreate([](void *) {
        std::string body, err;
        for (int attempt = 0; attempt < 3; attempt++) {
            err = http_get(url_host() + "/api/roles", body);
            if (err.empty()) break;
            ESP_LOGW(TAG, "roles 获取失败(第%d次): %s url=%s", attempt + 1, err.c_str(),
                     (url_host() + "/api/roles").c_str());
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (!lvgl_port_lock(0)) { s_busy = false; vTaskDelete(nullptr); return; }
        lv_obj_clean(s_list);
        if (err.empty()) {
            parse_roles(body);
            for (size_t i = 0; i < s_roles.size(); i++) {
                std::string label = (s_roles[i].incomplete ? "[缺立绘] " : "") + s_roles[i].name;
                lv_obj_t *btn = lv_btn_create(s_list);
                lv_obj_set_size(btn, 424, 46);
                lv_obj_set_style_pad_all(btn, 0, 0);
                lv_obj_t *lbl = lv_label_create(btn);
                lv_label_set_text(lbl, label.c_str());
                lv_obj_set_style_text_font(lbl, efont(), 0);
                lv_obj_center(lbl);
                lv_obj_add_event_cb(btn, on_role_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            }
            lv_label_set_text(s_status_label, s_roles.empty() ? "没有可下载角色" : "点角色名开始下载");
            ESP_LOGI(TAG, "Roles loaded: %d", (int)s_roles.size());
        } else {
            lv_label_set_text(s_status_label, "连不上服务器,检查网络");
            ESP_LOGW(TAG, "roles 获取失败: %s", err.c_str());
        }
        s_busy = false;
        lvgl_port_unlock();
        vTaskDelete(nullptr);
    }, "role_list", 10240, nullptr, 5, nullptr);
}
