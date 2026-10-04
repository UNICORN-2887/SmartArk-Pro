# 只烧新 app(0x300000),bootloader 保持刚烧的新版——二分测试
# 用法: .\flash_new_app.ps1
param([string]$Port = "COM4")
$ESP = "C:\Users\HP\.espressif\python_env\idf5.5_py3.10_env\Scripts\python.exe"
& $ESP -m esptool --chip esp32p4 -p $Port --before=default_reset --after=hard_reset write_flash --verify --flash_mode dio --flash_freq 80m --flash_size 16MB 0x300000 build\xiaozhi.bin
