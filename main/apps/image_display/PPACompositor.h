#pragma once
#include <stdint.h>
#include <stdbool.h>

// 初始化PPA和缓冲区
bool ppa_init(void);

// 加载静态背景图片 (JPEG, 480x800)
bool ppa_load_background(const char *path);
// 卸载背景（释放 PSRAM，切换为直通模式）
void ppa_unload_background(void);
bool ppa_has_background(void);
const uint8_t* ppa_get_background_buffer(void); // returns RGB565 480x800 buffer

// 预加载帧到PSRAM（逐文件模式，消除fopen延迟）
void ppa_preload_frames(const char *paths[], int count);

// 获取已预加载的帧数
int ppa_get_cache_count(void);

// 从MJPEG文件预加载（同步，阻塞显示~130ms）
int ppa_preload_mjpeg(const char *path);

// 异步预加载到后备缓冲区（后台任务，不阻塞显示）
void ppa_preload_mjpeg_async(const char *path);

// 交换活跃/后备缓冲区（<1ms，零SD访问）
// 返回新帧数，若后备未就绪返回0
int ppa_swap_emotion(void);

// ─── 三槽缓存：Cover 槽（永久保留，不被 emotion 换出）───
// 预加载 cover 到独立槽位
int ppa_preload_cover(const char *path);
// 异步预加载 cover（后台 task，不阻塞调用者）
void ppa_preload_cover_async(const char *path);
// 等待 cover 异步加载完成
void ppa_wait_cover_preload(void);
// 等待 pending 异步加载完成
void ppa_wait_pending_preload(void);
// 解码单个 JPEG 到 RGB565 buffer（外部管理内存，返回 NULL 失败）
// 调用者负责 free(rgb_buf)
uint8_t* ppa_decode_jpeg_to_rgb565(const char *path, int *out_w, int *out_h);
// 释放 PPA JPEG 引擎（供外部独占使用）
void ppa_release_jpeg_engine(void);
// 恢复 PPA JPEG 引擎（外部用完归还）
void ppa_restore_jpeg_engine(void);
// 交换 active ↔ cover（秒切，<1ms）
int ppa_swap_to_cover(void);
// 把 active 帧整体搬进 cover 槽（槽必须为空），active 清空
// 场景：profile 释放过 cover 槽后切 expression，避免误把 active 里的 cover 帧当表情复用
int ppa_save_active_to_cover(const char *agent_path);
// 清空 cover 槽旧数据（swap 后 slot 被旧 active 污染）
void ppa_free_cover_slot(void);
// cover 是否已缓存
bool ppa_has_cover(void);
// 返回 cover 所属角色路径（用于判断是否匹配当前 agent）
const char* ppa_get_cover_agent(void);
// 释放 cover 缓存（agent 切换时）
void ppa_unload_cover(void);
int ppa_preload_profile(const char *path);
void ppa_use_profile_cache(bool use);
void ppa_free_profile_slot(void);
void ppa_release_playback_caches(void);  // 释放全部播放缓存（PSRAM 紧张时给大内存需求让路）
void ppa_release_expendable_caches(void);  // 释放可牺牲缓存但保留 active（profile 秒切用）

// 打开MJPEG文件（内存高效模式，fseek读取）
bool ppa_open_mjpeg(const char *path, int *out_frame_count);
// 关闭 MJPEG fseek 模式
void ppa_close_mjpeg(void);

// ─── 横屏立牌：独立 PSRAM 预加载槽 ───
// 根因：SD 卡握手仅 20MHz 且与 WiFi SDIO 共享 SDMMC 总线（实测 ~620KB/s），
// 流式逐帧读只能 ~7fps。standee 全量预加载到独立槽（PSRAM 32MB 充裕），播放零 SD 读。
// 预加载 standee mjpeg 到独立槽（与 cover 槽同架构；同路径重复调用秒返回）
int ppa_preload_standee(const char *path);
// 释放 standee 槽（角色切换/内存紧张时）
void ppa_free_standee_slot(void);
// 解码一帧 480×800 竖帧（数据来自 standee 槽，零 SD 读）到内部 comp 双槽
// 成功返回 RGB565 缓冲（480*800*2），失败返回 NULL
uint8_t* ppa_composite_standee_frame(int frame_index);

// 解码前景帧并PPA抠图合成
uint8_t* ppa_composite_frame(int frame_index);

// ─── 长背景（横屏长图竖屏切片，PPD 交互页用）───
// 双槽预分配（每槽 1440×800 RGB565）+ 复用型 tx 缓冲：进入 PPD 互动时 pd_load 之前分配
// （PSRAM 碎片化红线：角色纹理加载后 largest free block 仅几十 KB）。
// 返回可用槽数（0/1/2）；已分配时返回现有槽数。
int ppa_long_bg_alloc(void);
// 解码 SD 卡 JPEG 到指定槽（宽≤1440 高≤800；可见宽=JPEG 真实宽，行 stride=MCU pad 后宽度）。
// 成功返回可见宽（>0），失败返回 ≤0。
int ppa_long_bg_decode(const char *path, int slot);
uint8_t* ppa_long_bg_buffer(int slot);   // 槽指针（无效槽返回 NULL）
bool ppa_background_set_from_rgb565(const uint8_t* buf, int width, int stride, int offset_x);   /* 2026-10-03 选背景持久化:从槽裁剪 480×800 更新 PPA 背景缓冲 */
int ppa_long_bg_width(int slot);         // 可见宽（JPEG 真实宽）
int ppa_long_bg_height(int slot);        // 可见高
int ppa_long_bg_stride(int slot);        // 行 stride（像素）
void ppa_long_bg_free(void);

// 获取上次解码的图片高度（用于 >800px 图片底部对齐裁切）
int ppa_get_last_decoded_height(void);
// 加载进度回调（加载动画用）：stage 阶段名，percent 0-100（-1=不确定）
typedef void (*PPALoadProgressCb)(const char* stage, int percent);
void ppa_set_load_progress_cb(PPALoadProgressCb cb);
// 释放所有资源
void ppa_deinit(void);
