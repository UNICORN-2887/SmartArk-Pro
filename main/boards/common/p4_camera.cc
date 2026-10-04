/** ESP32-P4 MIPI-CSI 摄像头(2026-09-30/10-01):esp_video + 本地 esp_cam_sensor
 *  (1.2.1+OV02C10 驱动补丁)+ esp_new_jpeg 编码 → multipart POST 到服务器视觉
 *  分析接口。SCCB 与音频 codec 共享 I2C 总线(板子 GPIO7/8,init_sccb=false)。
 *  2026-10-01 延迟初始化:摄像头组件占内部 RAM,启动时初始化会挤爆 AFE
 *  (唤醒词)内存(sr_rb_create: Memory exhausted),首次拍照时才真正初始化。 */
#include "p4_camera.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/poll.h>  /* 2026-10-04 DQBUF 超时保护(newlib 路径) */
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <linux/videodev2.h>
#include <esp_video_init.h>
#include <esp_video_device.h>
#include <esp_video_isp_ioctl.h>
#include <esp_jpeg_enc.h>

#include "board.h"
#include "system_info.h"

#define TAG "P4Camera"

P4Camera::P4Camera(i2c_master_bus_handle_t i2c_bus) : i2c_bus_(i2c_bus) {
    // 2026-10-01 延迟初始化:见 init_lazy()
}

bool P4Camera::init_lazy() {
    ESP_LOGI(TAG, "init_lazy: start (fd=%d tried=%d)", video_fd_, (int)init_tried_);
    if (video_fd_ >= 0) return true;      // 已初始化
    if (init_tried_) return false;        // 初始化失败过,不再重试(下次重启)
    init_tried_ = true;

    // 2026-10-01 在独立 FreeRTOS 动态栈任务中初始化:工具线程(pthread)栈仅
    // 6KB(静态池),esp_video_init 栈需求大,直接跑会栈溢出崩溃
    init_pending_ = true;
    if (xTaskCreate([](void *arg) {
        ((P4Camera*)arg)->init_inner();
        ((P4Camera*)arg)->init_pending_ = false;
        vTaskDelete(NULL);
    }, "cam_init", 32768, this, 5, NULL) != pdPASS) {
        init_pending_ = false;
        ESP_LOGE(TAG, "cam_init task create failed");
        return false;
    }
    // 等待初始化完成(5 秒超时保护)
    int timeout = 250;
    while (init_pending_ && timeout-- > 0) vTaskDelay(pdMS_TO_TICKS(20));
    if (init_pending_) {
        ESP_LOGE(TAG, "cam_init timeout");
        return false;
    }
    ESP_LOGI(TAG, "init_lazy: done (fd=%d)", video_fd_);
    return video_fd_ >= 0;
}

void P4Camera::init_inner() {
    // 2026-10-01 拍照前释放可牺牲缓存(PD 立绘纹理占满 PSRAM 时,
    // 摄像头 1.87MB 连续缓冲无法分配;释放播放/预取缓存腾出连续空间)
    extern void ppa_release_playback_caches(void);
    extern void ppa_release_expendable_caches(void);
    extern void pd_unload_for_camera(void);
    extern void pd_restore_after_camera(void);
    ppa_release_playback_caches();
    ppa_release_expendable_caches();
    pd_unload_for_camera();   // 立绘纹理十几 MB,必须卸载才能容下摄像头缓冲
    pd_restore_pending_ = true;

    // 手写 esp_video_init:SCCB 共享板子自建 I2C 总线(GPIO 7/8,
    // I2C_NUM_1),传感器驱动由本地 esp_cam_sensor 1.2.1+OV02C10 补丁提供
    esp_video_init_csi_config_t csi_config[] = {{
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 400000,
        },
        .reset_pin = GPIO_NUM_NC,
        .pwdn_pin = GPIO_NUM_NC,
    }};
    esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };
    esp_err_t err = esp_video_init(&cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: 0x%x", err);
        return;
    }

    video_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (video_fd_ < 0) {
        ESP_LOGE(TAG, "Open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return;
    }

    // 设 RGB565 格式(ISP 把 OV02C10 RAW10 转 RGB565)
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd_, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "G_FMT failed");
        close(video_fd_); video_fd_ = -1;
        return;
    }
    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    // 2026-10-01 尝试 ISP 缩放到 640x360:PSRAM 碎片化时 1288x728 的
    // 1.87MB 连续缓冲分配失败(Failed to create buffer)
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 360;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(video_fd_, VIDIOC_S_FMT, &fmt) == 0 &&
        fmt.fmt.pix.width == 640 && fmt.fmt.pix.height == 360) {
        width_ = 640;
        height_ = 360;
    } else {
        ESP_LOGW(TAG, "S_FMT 640x360 不支持,用默认 %ux%u", width_, height_);
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(video_fd_, VIDIOC_G_FMT, &fmt);   // 恢复默认
    }
    ESP_LOGI(TAG, "camera format: %ux%u RGB565", width_, height_);

    // mmap 帧缓冲(2026-10-01 单帧:PSRAM 连续块紧张,2 帧 3.7MB 分配失败)
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(video_fd_, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS failed");
        close(video_fd_); video_fd_ = -1;
        return;
    }
    for (int i = 0; i < 2; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(video_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF %d failed", i);
            close(video_fd_); video_fd_ = -1;
            return;
        }
        bufs_[i] = (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, video_fd_, buf.m.offset);
        if (bufs_[i] == (void *)-1) {   // MAP_FAILED(部分工具链未声明该宏)
            ESP_LOGE(TAG, "mmap %d failed", i);
            close(video_fd_); video_fd_ = -1;
            return;
        }
        buf_size_ = buf.length;
        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF %d failed", i);
            close(video_fd_); video_fd_ = -1;
            return;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd_, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON failed");
        close(video_fd_); video_fd_ = -1;
        return;
    }

    // 2026-10-02 手动强制启用 ISP AWB:白色 LED 灯光源拍出来偏绿。
    // AWB 控制挂在 ISP 设备(/dev/video1),发到 video0(cam)会报 ctrl not
    // supported。参数取自 ov02c10_default.json 的 awb.range。
    {
        int isp_fd = open("/dev/video1", O_RDWR);
        if (isp_fd < 0) {
            ESP_LOGW(TAG, "ISP device open failed, AWB not set");
        } else {
            esp_video_isp_awb_t awb;
            memset(&awb, 0, sizeof(awb));
            awb.enable = true;
            awb.green_max = 190;
            awb.green_min = 81;
            awb.rg_max = 0.9096f;
            awb.rg_min = 0.573f;
            awb.bg_max = 0.9634f;
            awb.bg_min = 0.5368f;
            struct v4l2_ext_controls ctrls;
            struct v4l2_ext_control ctrl;
            memset(&ctrls, 0, sizeof(ctrls));
            memset(&ctrl, 0, sizeof(ctrl));
            ctrls.ctrl_class = V4L2_CID_USER_CLASS;
            ctrls.count = 1;
            ctrls.controls = &ctrl;
            ctrl.id = V4L2_CID_USER_ESP_ISP_AWB;
            ctrl.p_u8 = (uint8_t*)&awb;
            if (ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
                ESP_LOGW(TAG, "manual AWB set failed on ISP");
            } else {
                ESP_LOGI(TAG, "manual AWB enabled (green fix)");
            }
            close(isp_fd);
        }
    }
    ESP_LOGI(TAG, "P4 camera ready (%ux%u, buf %u bytes)", width_, height_,
             (unsigned)buf_size_);
    return;
}

P4Camera::~P4Camera() {
    if (video_fd_ >= 0) {
        if (cur_index_ >= 0) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = cur_index_;
            ioctl(video_fd_, VIDIOC_QBUF, &buf);
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(video_fd_, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < 2; i++) {
            if (bufs_[i]) munmap(bufs_[i], buf_size_);
        }
        close(video_fd_);
        video_fd_ = -1;
    }
}

void P4Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool P4Camera::Capture() {
    ESP_LOGI(TAG, "Capture called");
    if (!init_lazy()) return false;
    // 归还上一帧
    if (cur_index_ >= 0) {
        struct v4l2_buffer q;
        memset(&q, 0, sizeof(q));
        q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        q.memory = V4L2_MEMORY_MMAP;
        q.index = cur_index_;
        ioctl(video_fd_, VIDIOC_QBUF, &q);
        cur_index_ = -1;
    }
    // 2026-10-01 预热 20 帧再取正式帧:ISP 的 AE/AWB 逐帧迭代收敛,只取
    // 1-2 帧白平衡未稳定(白色 LED 灯光源被拍成绿色);20 帧@30fps≈0.7s
    // 2026-10-04 DQBUF 前加 poll 超时:同开机周期第二次拍照时流可能停摆,
    // DQBUF 永久阻塞无声卡死(工具超时、LLM 乱转)。超时则重置状态,
    // 下次拍照重新初始化自愈。
    const int warmup = 20;
    for (int i = 0; i < warmup + 1; i++) {
        struct pollfd pfd;
        pfd.fd = video_fd_;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 3000);
        if (pr <= 0) {
            ESP_LOGE(TAG, "DQBUF poll timeout (stream stalled), reset for next retry");
            close(video_fd_);
            video_fd_ = -1;
            init_tried_ = false;
            cur_index_ = -1;
            return false;
        }
        struct v4l2_buffer dq;
        memset(&dq, 0, sizeof(dq));
        dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dq.memory = V4L2_MEMORY_MMAP;
        if (ioctl(video_fd_, VIDIOC_DQBUF, &dq) != 0) {
            ESP_LOGE(TAG, "DQBUF failed");
            return false;
        }
        if (i < warmup) {
            ioctl(video_fd_, VIDIOC_QBUF, &dq);   // 预热帧丢弃,让 AE/AWB 收敛
        } else {
            cur_index_ = dq.index;
        }
    }
    return true;
}

bool P4Camera::SetHMirror(bool enabled) {
    if (!init_lazy()) return false;
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control ctrl;
    memset(&ctrls, 0, sizeof(ctrls));
    memset(&ctrl, 0, sizeof(ctrl));
    ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrl.id = V4L2_CID_HFLIP;
    ctrl.value = enabled ? 1 : 0;
    return ioctl(video_fd_, VIDIOC_S_EXT_CTRLS, &ctrls) == 0;
}

bool P4Camera::SetVFlip(bool enabled) {
    if (!init_lazy()) return false;
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control ctrl;
    memset(&ctrls, 0, sizeof(ctrls));
    memset(&ctrl, 0, sizeof(ctrl));
    ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrl.id = V4L2_CID_VFLIP;
    ctrl.value = enabled ? 1 : 0;
    return ioctl(video_fd_, VIDIOC_S_EXT_CTRLS, &ctrls) == 0;
}

std::string P4Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return "{\"success\": false, \"message\": \"Image explain URL or token is not set\"}";
    }
    if (!init_lazy() || cur_index_ < 0) {
        return "{\"success\": false, \"message\": \"No camera frame captured\"}";
    }

    // RGB565 → RGB888 转换(esp_new_jpeg 0.6.1 编码器不支持 RGB565_LE)
    size_t px = width_ * height_;
    uint8_t* rgb888 = (uint8_t*)heap_caps_malloc(px * 3, MALLOC_CAP_SPIRAM);
    if (!rgb888) {
        return "{\"success\": false, \"message\": \"RGB888 buffer alloc failed\"}";
    }
    {
        const uint16_t* src = (const uint16_t*)bufs_[cur_index_];
        uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
        for (size_t i = 0; i < px; i++) {
            uint16_t p = src[i];   // RGB565 LE: RRRRRGGG GGGBBBBB
            rgb888[i * 3 + 0] = (uint8_t)(((p >> 11) & 0x1F) << 3);   // R
            rgb888[i * 3 + 1] = (uint8_t)(((p >> 5) & 0x3F) << 2);    // G
            rgb888[i * 3 + 2] = (uint8_t)((p & 0x1F) << 3);           // B
            r_sum += rgb888[i * 3 + 0];
            g_sum += rgb888[i * 3 + 1];
            b_sum += rgb888[i * 3 + 2];
        }
        // 2026-10-02 软件白平衡兜底(灰色世界):ISP AWB 未生效时图像整体偏绿
        // (G 通道均值显著高于 R/B)。统计增益使三通道均值相等,补偿硬件白平衡缺失。
        float r_avg = (float)r_sum / px, g_avg = (float)g_sum / px, b_avg = (float)b_sum / px;
        float kr = (g_avg > 1.0f && r_avg > 1.0f) ? g_avg / r_avg : 1.0f;
        float kb = (g_avg > 1.0f && b_avg > 1.0f) ? g_avg / b_avg : 1.0f;
        if (kr > 1.6f) kr = 1.6f;   // 限幅防过冲(极端偏色不强行拉满)
        if (kb > 1.6f) kb = 1.6f;
        if (kr > 1.02f || kb > 1.02f) {
            static uint8_t lut_r[256], lut_b[256];
            static bool lut_ready = false;
            for (int v = 0; v < 256; v++) {
                int rv = (int)(v * kr);  if (rv > 255) rv = 255;
                int bv = (int)(v * kb);  if (bv > 255) bv = 255;
                lut_r[v] = (uint8_t)rv;
                lut_b[v] = (uint8_t)bv;
            }
            lut_ready = true;
            for (size_t i = 0; i < px; i++) {
                rgb888[i * 3 + 0] = lut_r[rgb888[i * 3 + 0]];
                rgb888[i * 3 + 2] = lut_b[rgb888[i * 3 + 2]];
            }
            ESP_LOGI(TAG, "software WB: kr=%.2f kb=%.2f (avg R=%.0f G=%.0f B=%.0f)",
                     kr, kb, r_avg, g_avg, b_avg);
        }
    }

    // RGB888 → JPEG(esp_new_jpeg,P4 软件编码)
    jpeg_enc_config_t jpeg_cfg = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_cfg.width = width_;
    jpeg_cfg.height = height_;
    jpeg_cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    jpeg_cfg.quality = 80;
    jpeg_enc_handle_t jpeg_enc = NULL;
    if (jpeg_enc_open(&jpeg_cfg, &jpeg_enc) != ESP_OK) {
        heap_caps_free(rgb888);
        ESP_LOGE(TAG, "jpeg_enc_open failed");
        return "{\"success\": false, \"message\": \"JPEG encoder init failed\"}";
    }
    // JPEG 输出缓冲:最坏按 3 字节/像素估算
    size_t out_cap = px * 3;
    uint8_t* jpeg_buf = (uint8_t*)heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
    int jpeg_len = 0;   // jpeg_enc_process 的 out_size 为 int*
    esp_err_t jerr = ESP_FAIL;
    if (jpeg_buf) {
        jerr = jpeg_enc_process(jpeg_enc, rgb888, (int)(px * 3),
                                jpeg_buf, (int)out_cap, &jpeg_len);
    }
    jpeg_enc_close(jpeg_enc);
    heap_caps_free(rgb888);
    if (jerr != ESP_OK || !jpeg_buf || jpeg_len == 0) {
        if (jpeg_buf) heap_caps_free(jpeg_buf);
        ESP_LOGE(TAG, "JPEG encode failed: 0x%x", jerr);
        return "{\"success\": false, \"message\": \"JPEG encode failed\"}";
    }
    ESP_LOGI(TAG, "JPEG: %ux%u -> %d bytes", width_, height_, jpeg_len);

    // 2026-10-01 保存到 SD 卡,便于直接查看拍到的原图(画质核对)
    {
        FILE* f = fopen("/sdcard/camera_last.jpg", "wb");
        if (f) {
            fwrite(jpeg_buf, 1, (size_t)jpeg_len, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved /sdcard/camera_last.jpg (%d bytes)", jpeg_len);
        } else {
            ESP_LOGW(TAG, "SD save failed (no card?)");
        }
    }

    // multipart POST(question + file=camera.jpg)
    // 2026-10-01 HttpClient(78__esp-ml307)不支持流式 POST body:
    // Write() 发的是 chunk 帧,且无 SetContent 时 BuildHttpRequest 会自动
    // 追加 "Content-Length: 0",与手动 Content-Length 头冲突 → 服务器 RST
    // (errno=104)。正确用法 = SetContent 整包发送(Content-Length 由组件
    // 按 body 实际长度自动生成,唯一且正确)。
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    std::string question_field = "--" + boundary + "\r\n";
    question_field += "Content-Disposition: form-data; name=\"question\"\r\n\r\n";
    question_field += question + "\r\n";
    std::string file_header = "--" + boundary + "\r\n";
    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n\r\n";
    std::string footer = "\r\n--" + boundary + "--\r\n";
    std::string body;
    body.reserve(question_field.size() + file_header.size() + (size_t)jpeg_len + footer.size());
    body += question_field;
    body += file_header;
    body.append((const char*)jpeg_buf, (size_t)jpeg_len);
    body += footer;
    heap_caps_free(jpeg_buf);   // body 已拷贝,尽早释放 PSRAM

    // 上传完成(无论成败)后恢复立绘:拍照期间立绘被卸载以腾 PSRAM
    auto restore_pd = [this]() {
        if (pd_restore_pending_) {
            extern void pd_restore_after_camera(void);
            pd_restore_after_camera();
            pd_restore_pending_ = false;
        }
    };

    http->SetTimeout(90000);   // 视觉分析(vllm 调用)可能超过默认 30s
    http->SetContent(std::move(body));
    if (!http->Open("POST", explain_url_)) {
        restore_pd();
        return "{\"success\": false, \"message\": \"Failed to connect to explain URL\"}";
    }
    if (http->GetStatusCode() != 200) {
        http->Close();
        restore_pd();
        return "{\"success\": false, \"message\": \"Failed to upload photo\"}";
    }
    std::string result = http->ReadAll();
    http->Close();
    restore_pd();
    ESP_LOGI(TAG, "Explain result: %s", result.c_str());
    return result;
}
