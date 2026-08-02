@echo off
chcp 65001 >nul
title 静图处理器 — 图片 → 480x800 JPG

echo ============================================
echo   静图处理器 — 图片 → 480x800 JPG
echo ============================================
echo.

:: 检查 Python
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [错误] 未找到 Python，请先安装: https://www.python.org/downloads/
    pause
    exit /b 1
)
echo [✓] Python 已就绪

:: 检查并安装 Pillow
python -c "from PIL import Image" 2>nul
if %errorlevel% neq 0 (
    echo [安装] 正在安装 Pillow...
    pip install Pillow -q
    python -c "from PIL import Image" 2>nul
    if %errorlevel% neq 0 (
        echo [错误] Pillow 安装失败，请检查网络或手动执行: pip install Pillow
        pause
        exit /b 1
    )
)
echo [✓] Pillow 已就绪
echo.

:: 查找输入文件
set "SRC="
for %%f in ("%~dp0*.png" "%~dp0*.bmp" "%~dp0*.webp" "%~dp0*.gif" "%~dp0*.tiff" "%~dp0*.tif" "%~dp0*.jpeg" "%~dp0*.jpg") do (
    if /i not "%%~nxf"=="Profile.jpg" (
        if exist "%%f" (
            set "SRC=%%~nxf"
            goto :found
        )
    )
)
:check_jpg
if not defined SRC (
    :: 也检查 JPG 但不是 Profile.jpg
    for %%f in ("%~dp0*.jpg" "%~dp0*.jpeg") do (
        if /i not "%%~nxf"=="Profile.jpg" (
            if exist "%%f" (
                set "SRC=%%~nxf"
                goto :found
            )
        )
    )
)
if not defined SRC (
    echo [提示] 未找到需要转换的图片文件
    echo 支持格式: PNG / BMP / WEBP / GIF / TIFF / JPEG
    echo 请将任意图片放入本文件夹，然后重新运行本脚本
    echo.
    echo 当前文件夹内容:
    dir "%~dp0" /b
    pause
    exit /b 1
)

:found
echo [✓] 找到: %SRC%
echo.

echo 转换: %SRC% → Profile.jpg (480x800)
python -c "from PIL import Image;img=Image.open(r'%~dp0%SRC%').convert('RGBA');bg=Image.new('RGB',img.size,(255,255,255));bg.paste(img,mask=img.split()[3]);bg=bg.resize((480,800),Image.LANCZOS);bg.save(r'%~dp0Profile.jpg','JPEG',quality=92);print('完成: Profile.jpg (480x800)')"
if %errorlevel% neq 0 (
    echo [错误] 图片转换失败
    pause
    exit /b 1
)
echo.
echo ============================================
echo   ✓ 完成！Profile.jpg 已生成
echo   可拔出 SD 卡插入设备，点击"蟑螂派对"查看
echo ============================================
pause
