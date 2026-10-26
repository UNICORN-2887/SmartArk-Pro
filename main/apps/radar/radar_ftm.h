/* 交友雷达第三层:WiFi FTM(802.11mc)精确测距(2026-10-23)
 * 每台设备开 SoftAP(FTM responder),对邻居 SoftAP 发起 FTM session 测距,
 * 距离为信号飞行时间估算(厘米级),远优于 RSSI 的路径损耗估算。 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 SoftAP(FTM responder)+ 周期测距任务。返回 false=不支持/失败(不致命)。 */
bool radar_ftm_start(void);

/* 最近一次对 peer_uid 的 FTM 测距结果(厘米);0=暂无结果。 */
uint32_t radar_ftm_dist_cm(uint32_t peer_uid);

#ifdef __cplusplus
}
#endif
