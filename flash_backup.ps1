# 烧录 9/26 备份全套(bootloader+app+分区表+otadata+模型)——设备最后正常运行的组合
# 用法: .\flash_backup.ps1    (默认 COM4; 换口: .\flash_backup.ps1 COM7)
param([string]$Port = "COM4")

$BK = "E:\Passport\espp4\sparepart\JC4880P443C_I_W\1-Demo\idf_examples\ESP-IDF\xiaozhi-esp32sp1 开机修复以及ppdq优化\build"
$ESP = "C:\Users\HP\.espressif\python_env\idf5.5_py3.10_env\Scripts\python.exe"

& $ESP -m esptool --chip esp32p4 -p $Port --before=default_reset --after=hard_reset write_flash --verify --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x2000   "$BK\bootloader\bootloader.bin" `
  0x300000 "$BK\xiaozhi.bin" `
  0x8000   "$BK\partition_table\partition-table.bin" `
  0xd000   "$BK\ota_data_initial.bin" `
  0x10000  "$BK\srmodels\srmodels.bin"
