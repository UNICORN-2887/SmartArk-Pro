# 彻底重建:清 ccache + 删 build + 全量编译 + 验证 bootloader 与 9/7 备份一致
# 用法: .\rebuild_clean.ps1
$BK = "E:\Passport\espp4\sparepart\JC4880P443C_I_W\1-Demo\idf_examples\ESP-IDF\xiaozhi-esp32sp1 开机修复以及ppdq优化\build"

ccache -C
if ($LASTEXITCODE -ne 0) { Write-Host "警告: ccache -C 返回 $LASTEXITCODE,继续..." }
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
idf.py build
if ($LASTEXITCODE -ne 0) { Write-Host "构建失败,停止"; exit 1 }

$h1 = (Get-FileHash build\bootloader\bootloader.bin -Algorithm MD5).Hash
$h2 = (Get-FileHash "$BK\bootloader\bootloader.bin" -Algorithm MD5).Hash
Write-Host ""
Write-Host "新 bootloader:  $h1"
Write-Host "备份 bootloader: $h2"
if ($h1 -eq $h2) {
    Write-Host "[OK] 与 9/7 备份完全一致——ccache 污染已清除。可以烧录: idf.py -p COM4 flash monitor" -ForegroundColor Green
} else {
    Write-Host "[不同] 产物仍与备份不一致,先烧备份恢复设备,再继续排查" -ForegroundColor Yellow
}
