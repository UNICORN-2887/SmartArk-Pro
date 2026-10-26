/* 交友雷达(2026-10-22):BLE 信标第一层 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动雷达:BLE 广播+扫描+周期上报。网络/绑定未就绪时内部等待,不阻塞。 */
void radar_ble_start(void);

/* 当前绑定用户 uid(应用级 + SD 目录兜底;0 = 未绑定)。 */
uint32_t radar_bound_uid(void);

/* ── 附近用户数据(供 UI) ── */
typedef struct {
    uint32_t uid;
    char name[32];      /* 昵称(UTF-8) */
    char dist[8];       /* 距离档:贴脸/近/中/远 */
    int8_t rssi;        /* 最后一次信号强度 */
    int8_t trend;       /* 2026-10-22 接近趋势:+1 靠近 / -1 远离 / 0 稳定 */
    int8_t side;        /* 2026-10-22 双机方位:-1 偏我 / +1 偏队友 / 0 正中 */
    int16_t dir;        /* 2026-10-22 真实方位角 0-359(北=0,顺时针);-1=无定位 */
    uint16_t dist_m;    /* 2026-10-22 远距离估算(米,WiFi 定位);0=无 */
    float rel_x;        /* 2026-10-22 BLE 三边定位相对坐标(米,我=原点,+x=最强邻居方向) */
    float rel_y;        /* +y 为 +x 逆时针 90° */
    uint8_t has_rel;    /* rel_x/rel_y 有效 */
    uint32_t ts;        /* 发现时刻(s) */
} radar_user_t;

/* 取最近一次 nearby 结果(最多 max 个,返回实际数量)。 */
int radar_nearby_list(radar_user_t *out, int max);

/* 当前用户昵称(nearby 响应的 me.name;未知返回空串)。 */
const char *radar_me_name(void);

/* 2026-10-23 CP OTA 等敏感操作期间暂停雷达扫描/上报
   (BLE 扫描事件走 SDIO HCI,与 OTA 传输抢总线导致写失败) */
void radar_ble_pause(void);
void radar_ble_resume(void);

/* 2026-10-25 暂停标志查询:ESNOW 心跳等 SDIO 流量也遵守(下载期间
   SD 卡与 C6 SDIO 共用 P4 SDMMC,心跳 RPC 可能参与跨槽互锁) */
bool radar_ble_paused(void);

#ifdef __cplusplus
}
#endif
