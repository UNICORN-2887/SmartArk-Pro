#ifndef P4_CAMERA_H
#define P4_CAMERA_H

#include "camera.h"
#include <string>

#include <driver/i2c_master.h>

class P4Camera : public Camera {
public:
    // 2026-09-30 ESP32-P4 MIPI-CSI 摄像头(OV02C10,esp_video 栈):
    // SCCB 与音频 codec 共享同一 I2C 总线(JC4880P443 GPIO7/8)
    explicit P4Camera(i2c_master_bus_handle_t i2c_bus);
    ~P4Camera();

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::string Explain(const std::string& question) override;

private:
    bool init_lazy();       // 首次拍照时初始化(独立大栈任务执行)
    void init_inner();      // 初始化本体(在 cam_init 任务中跑)
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    bool init_tried_ = false;
    volatile bool init_pending_ = false;
    volatile bool pd_restore_pending_ = false;   // 拍照完成后需恢复立绘
    int video_fd_ = -1;
    uint8_t *bufs_[2] = {nullptr, nullptr};   // mmap 帧缓冲
    size_t buf_size_ = 0;
    uint32_t width_ = 0, height_ = 0;
    int cur_index_ = -1;                       // 当前 DQBUF 到的帧
    std::string explain_url_, explain_token_;
};

#endif // P4_CAMERA_H
