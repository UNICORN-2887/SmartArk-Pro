@echo off
chcp 65001 >nul
title 动图处理器 — MP4 → MJPEG (480x800)

echo ============================================
echo   动图处理器 — MP4 → 480x800 MJPEG
echo ============================================
echo.

:: 检查 ffmpeg
where ffmpeg >nul 2>&1
if %errorlevel% neq 0 (
    echo [错误] 未找到 ffmpeg
    echo 请先安装: https://ffmpeg.org/download.html
    echo 选择 Windows builds → 下载 → 解压 → 将 bin 目录加入系统 PATH
    pause
    exit /b 1
)
echo [✓] ffmpeg 已就绪
echo.

:: 检查输入文件
if not exist "%~dp0Profile.mp4" (
    echo [错误] 请将视频命名为 Profile.mp4 放入本文件夹
    echo 当前文件夹: %~dp0
    dir "%~dp0" /b
    pause
    exit /b 1
)
echo [✓] 找到 Profile.mp4
echo.

:: 创建临时帧目录
set "FRAME_DIR=%~dp0_temp_frames"
if exist "%FRAME_DIR%" rmdir /s /q "%FRAME_DIR%"
mkdir "%FRAME_DIR%"

echo [1/3] 提取帧（480x800, 30fps）...
ffmpeg -i "%~dp0Profile.mp4" -vf "scale=480:800:force_original_aspect_ratio=decrease,pad=480:800:(ow-iw)/2:(oh-ih)/2:color=black" -q:v 3 -r 30 "%FRAME_DIR%\%%04d.jpg" -hide_banner -loglevel error
if %errorlevel% neq 0 (
    echo [错误] ffmpeg 提取帧失败
    rmdir /s /q "%FRAME_DIR%"
    pause
    exit /b 1
)

:: 统计帧数
set count=0
for %%f in ("%FRAME_DIR%\*.jpg") do set /a count+=1
echo    提取完成: %count% 帧
echo.

echo [2/3] 构建 MJPEG 文件...
python -c "import os,struct;d=r'%FRAME_DIR%';files=sorted([f for f in os.listdir(d) if f.endswith('.jpg')]);f=open(r'%~dp0Profile.mjpeg','wb');f.write(struct.pack('<I',len(files)));off=4+len(files)*4;offs=[];[(offs.append(off),off:=off+len(open(os.path.join(d,x),'rb').read())) for x in files];[f.write(struct.pack('<I',o)) for o in offs];[f.write(open(os.path.join(d,x),'rb').read()) for x in files];f.close();print(f'完成: {len(files)} 帧 → Profile.mjpeg ({off/1024:.0f} KB)')"
if %errorlevel% neq 0 (
    echo [错误] MJPEG 构建失败，请检查 Python 环境
    rmdir /s /q "%FRAME_DIR%"
    pause
    exit /b 1
)
echo.

echo [3/3] 清理临时文件...
rmdir /s /q "%FRAME_DIR%"
echo.
echo ============================================
echo   ✓ 完成！Profile.mjpeg 已生成
echo   可拔出 SD 卡插入设备，点击"蟑螂派对"查看
echo ============================================
pause
