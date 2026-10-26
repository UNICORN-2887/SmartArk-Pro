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

/* 下载进度附带信息(2026-10-25):UI 把 &RoleDlPageInfo 当 ud 传给
   role_download_fetch*,下载核心在 bundle 模式逐页填充块号/总块数,
   UI 据此显示"第N/M块"(pages>1 才显示)。逐文件/公共库路径不填充,
   保持 1/1 即不显示。 */
struct RoleDlPageInfo {
    int page = 1;    /* 当前块号(1 起) */
    int pages = 1;   /* 总块数 = ceil(总字节/8MB) */
};

/* 下载进行中标志(2026-10-24):WifiBoard 省电保护、UI 防叠下载共用 */
bool role_download_busy(void);

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

/* ── 懒下载(2026-10-25,漫展快速展示) ──
 * 角色分模块按需下载:首次只下默认集(cover 全部+standee+manifest.json),
 * PPD 后台预取、PPD_Q 基座进 Q 模式时下、voice 点语音记录时下。
 * prefixes = rel 前缀集(如 {"cover/","standee/","manifest.json"}),
 * tags = 成功后写的模块标记名(如 {"cover","standee"},写 .done_<tag>)。
 * check_user_set:只核对前缀集内文件,返回语义同 check_user,total_out=子集字节。
 * module_ready:模块标记是否存在(离线可用,不拉 manifest)。 */
int role_download_check_user_set(int uid, const char *rel,
                                 const std::vector<std::string> &prefixes,
                                 int64_t *total_out);
int role_download_fetch_user_set(int uid, const char *rel,
                                 const std::vector<std::string> &prefixes,
                                 const std::vector<std::string> &tags,
                                 bool (*step_cb)(int pct, const char *file, void *ud), void *ud);
/* Q 模式基座:PPD_Q 顶层文件 + forms/基建(前缀集内部按 manifest 计算) */
int role_download_fetch_user_qbase(int uid, const char *rel,
                                   bool (*step_cb)(int pct, const char *file, void *ud), void *ud);
bool role_download_module_ready(int uid, const char *rel, const char *tag);

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

/* 2026-10-26 音乐下载块级进度上下文:fetch 内部每曲开始前更新 cur(1 起),
   step_cb 可按 cur/total 显示"第 X/N 首"(复用角色下载块号 UI) */
struct MusicDlCtx {
    int cur;    // 当前曲号(1 起)
    int total;  // 总曲数
};
int role_download_fetch_music_missing(bool (*step_cb)(int pct, const char *file, void *ud), void *ud);

/* ── 开机曲自动下载(2026-10-21) ──
 * 只下 Rhodes_Island.wav 到 music 目录:.part 半成品断点续传,完整后
 * rename 原子完成(播放扫描不会遇到半截文件)。正式文件存在且 >0 即跳过。 */
int role_download_fetch_music_boot(void);

/* ── 键盘词组库自动下载(2026-10-26) ──
 * 下载 ime_dict.txt(公开库根,~450KB)到 /sdcard/Arknights/main/。
 * 存在且 >100KB 跳过;失败返回 -1(下次开机重试)。 */
int role_download_fetch_ime_dict(void);

/* ── 服务器可达性探测(2026-10-21) ──
 * 拿到 IP 后确认服务器真实可达:GET manifest,响应含 '{'(JSON)才算通。
 * 认证门户网络(校园 guest)有 IP 无外网,请求被门户劫持返回 HTML → 判不通。
 * 返回:0=可达 -1=超时/门户/失败。仅在 netif 已有 IP 时调用(防 lwip assert)。 */
int role_download_network_ok(int timeout_ms);

/* ── 目录清空(换绑用户清理;2026-09-10) ── */
void role_download_remove_dir(const char *path);   // 递归删除(包装静态 remove_dir_r)
int role_download_clear_operator(void);            // 清 operator 保留 INDEX;0=完成 1=目录不存在 -1=部分失败

/* ── CP(协处理器)固件自动 OTA(2026-10-07) ──
 * C6 出厂 2.3.2 无 SW_AGGR → 流模式吞吐 ~20KB/s;开机网络就绪后自动
 * 下载官方 3.0.9 预编译固件 → SDIO OTA → 整机重启。版本匹配则静默跳过。 */
void cp_ota_task(void *arg);

/* ── 开机无竞争测速(2026-10-25) ──
 * 网络就绪后下载一页 bundle(4MB)计时打日志:无雷达/UI/音频竞争的
 * 下行天花板,用于区分"WiFi 受限"与"运行期竞争"。 */
void role_download_speedtest(void);

/* ── 兜底拿绑定 uid(2026-10-25) ──
 * 词表接口独立同步拉一次解析 uid,不依赖开机流程时序(开机仪式等
 * 点击导致词表延迟时,索引页点卡片 probe 前兜底取 uid);失败返回 0。 */
int role_download_bound_uid_now(void);

/* ── 等待下载任务结束(2026-10-25) ──
 * 取消传播需要时间(乐观关闭弹窗后读循环未到检查点);启动新下载
 * 前等待旧任务真正结束,避免"已有下载任务进行中"忽略新请求。 */
void role_download_wait_idle(int timeout_ms);
