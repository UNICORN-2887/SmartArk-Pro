#pragma once
/* 角色下载:云端角色包 → SD 卡。两处入口共用:
 *   1. 菜单"角色下载"页:清单列表 → 确认框 → 进度条
 *   2. 干员索引页:选中未缓存角色 → 弹窗"是否下载" → 弹窗内进度 → 自动进立绘
 * 下载核心 role_download_fetch(回调式)供两处调用;失败/取消自动恢复原目录。
 * 服务器:http://<RESOURCE_SERVER>/api/roles 等(见 sdkconfig CONFIG_RESOURCE_SERVER)
 */
#include <stdint.h>
#include <string>
#include <vector>
#include <lvgl.h>

/* 云端角色条目(罗德岛页全量列表用) */
struct CloudRole {
    std::string voc, star, name;
    bool incomplete = false;   // 缺竖屏立绘(不可下载)
};

void role_downloader_show(void);
void role_downloader_close(void);   /* 互斥:干员索引页打开前关闭下载页(LVGL 任务上下文) */
void role_downloader_set_font(const lv_font_t *font);   /* 板级中文字体(防豆腐块) */

/* 拉取角色 manifest,返回总字节数(含 INDEX 缩略图)。
 * 失败:-1 = 网络错误,-2 = 云端无此角色包(立绘未就绪,HTTP 404)。
 * 调用后 s_files 缓存 = 该角色清单(与 fetch 共用,单下载并发由页面互斥保证)。 */
int64_t role_download_probe(const char *voc, const char *star, const char *name);

/* 下载一个角色到 SD 卡(阻塞式,任务线程调用)。
 * step_cb: 每文件开始(pct=-1, file=相对路径)与下载进度(pct 0-100)回调;
 *          返回 false = 取消。LVGL 操作须在回调内自行加锁。
 * 返回:0=成功(已写 .done) 1=失败(原目录已恢复) 2=取消(原目录已恢复) */
int role_download_fetch(const char *voc, const char *star, const char *name,
                        bool (*step_cb)(int pct, const char *file, void *ud), void *ud);

/* 下载耗时估算:总字节数 → 分钟(保守速度 500KB/s,至少 1 分钟) */
int role_download_estimate_minutes(int64_t total_bytes);

/* 本地角色包与云端清单逐文件核对(大小比对,__index__ 缩略图也查)。
 * 返回:1=齐全(第一档直接展示) 0=部分缺失(第二档弹窗)
 *      -1=网络失败(未知) -2=云端无此角色(手工角色,按齐全处理)
 * total_out: 清单总字节数(成功时有效,用于耗时估算)。 */
int role_download_check(const char *voc, const char *star, const char *name, int64_t *total_out);

/* 拉取云端角色全量列表(罗德岛页"搜索所有角色"数据源)。
 * 返回 0=成功(含 PRTS/INDEX 等特殊条目,由调用方过滤);1=网络失败 */
int role_download_list(std::vector<CloudRole> &out);

/* 拉取云端 INDEX 缩略图全量清单(全部干员,如 427 条)。
 * 返回 0=成功;1=网络失败 */
int role_download_thumbs(std::vector<CloudRole> &out);

/* 下载角色 INDEX 缩略图到 SD 卡(已有则跳过)。
 * 返回:0=就绪 1=网络失败 -1=服务器无此缩略图(incomplete 角色) */
int role_download_thumb(const char *voc, const char *star, const char *name);

/* ── 用户仓库下载(设备已绑定用户,uid 从 resource_path 解析) ──
 * 与公共库同款增量/断点续传;服务器接口 /api/user_repo(按设备 mac 定位 uid),
 * rel = 用户根下相对路径(如 Arknights/main/operator/MEDIC/6STAR/Kaltsit 或 other/我的OC),
 * 落位 /sdcard/_users/u<uid>/<rel>/。
 * 语义同公共库三件套:probe=-1 网络失败/-2 云端无此目录;check=1 齐全/0 缺失;fetch=0/1/2。 */
int64_t role_download_probe_user(int uid, const char *rel);
int role_download_check_user(int uid, const char *rel, int64_t *total_out);
int role_download_fetch_user(int uid, const char *rel,
                             bool (*step_cb)(int pct, const char *file, void *ud), void *ud);

/* ── 公共共享资源三件套(背景/音乐,rel 相对 Arknights/main;2026-09-10) ──
 * 语义同公共库三件套:probe -1 网络/-2 云端无;check 1 齐全/0 缺失;fetch 0/1/2。
 * 落位 /sdcard/Arknights/main/<rel>/。 */
int64_t role_download_probe_public(const char *rel);
int role_download_check_public(const char *rel, int64_t *total_out);
int role_download_fetch_public(const char *rel,
                               bool (*step_cb)(int pct, const char *file, void *ud), void *ud);

/* ── 公共 Ur_Info 下载(2026-10-06 蟑螂派对默认照片/动图):0 成功 -1 失败 ── */
int role_download_fetch_public_profile(const char *fname, const char *dst);

/* ── 一键克隆(公共库角色→绑定用户仓库;服务器复制目录+建 agent 行) ──
 * 返回:0=成功 1=网络/其它失败 2=配额不足 3=已克隆过(直接进下载) 4=云端无此角色 */
int role_download_clone(const char *voc, const char *star, const char *name);

/* ── 背景音乐清单检查/下载(2026-09-11) ──
 * check:拉 /api/public/music_manifest 对比本地 music 目录下的 wav,
 *       返回 ≥0=缺失首数(缺失名写入内部清单) -1=网络失败;
 * fetch:下载内部清单中缺失的 wav(存在且>0 跳过)。 */
int role_download_check_music_missing(void);
int role_download_fetch_music_missing(bool (*step_cb)(int pct, const char *file, void *ud), void *ud);

/* ── 目录清空(换绑用户清理;2026-09-10) ── */
void role_download_remove_dir(const char *path);   // 递归删除(包装静态 remove_dir_r)
int role_download_clear_operator(void);            // 清 operator 保留 INDEX;0=完成 1=目录不存在 -1=部分失败

/* ── CP(协处理器)固件自动 OTA(2026-10-07) ──
 * C6 出厂 2.3.2 无 SW_AGGR → 流模式吞吐 ~20KB/s;开机网络就绪后自动
 * 下载官方 3.0.9 预编译固件 → SDIO OTA → 整机重启。版本匹配则静默跳过。 */
void cp_ota_task(void *arg);
