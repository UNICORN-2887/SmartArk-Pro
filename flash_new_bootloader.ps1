# 只烧新 bootloader(0x2000),其余保持备份版——二分测试
# 用法: .\flash_new_bootloader.ps1
param([string]$Port = "COM4")
$ESP = "C:\Users\HP\.espressif\python_env\idf5.5_py3.10_env\Scripts\python.exe"
& $ESP -m esptool --chip esp32p4 -p $Port --before=default_reset --after=hard_reset write_flash --verify --flash_mode dio --flash_freq 80m --flash_size 16MB 0x2000 build\bootloader\bootloader.bin
