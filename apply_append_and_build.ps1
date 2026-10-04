# 绕开 release.py 的 set-target+fullclean 循环:
# 1. 手动把 config.json 的 sdkconfig_append 应用进 sdkconfig
# 2. 清掉半成品 build 目录
# 3. 直接 idf.py build(target 已设 esp32p4,依赖解析已通)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# 1. 应用 sdkconfig_append
$cfg = Get-Content "$root\main\boards\guition-jc4880p443\config.json" -Raw | ConvertFrom-Json
$lines = Get-Content "$root\sdkconfig"
$out = New-Object System.Collections.Generic.List[string]
$applied = New-Object System.Collections.Generic.HashSet[string]
foreach ($line in $lines) {
    $key = ($line -split '=')[0]
    if ($key -match '^CONFIG_') { [void]$applied.Add($key) }
}
# 同名项替换、新项追加
$newLines = foreach ($line in $lines) {
    $key = ($line -split '=')[0]
    $replacement = $cfg.builds[0].sdkconfig_append | Where-Object { ($_ -split '=')[0] -eq $key } | Select-Object -First 1
    if ($replacement) { $replacement } else { $line }
}
$final = New-Object System.Collections.Generic.List[string]
$final.AddRange([string[]]$newLines)
foreach ($item in $cfg.builds[0].sdkconfig_append) {
    $key = ($item -split '=')[0]
    if (-not $applied.Contains($key)) { $final.Add($item) }
}
Set-Content -Path "$root\sdkconfig" -Value $final -Encoding ascii
Write-Host "sdkconfig_append 已应用(共 $($cfg.builds[0].sdkconfig_append.Count) 项)"

# 2. 清半成品 build
if (Test-Path "$root\build") { Remove-Item -Recurse -Force "$root\build" }
Write-Host "build 目录已清"

# 3. 直接构建
Write-Host "开始 idf.py build(全量编译,请耐心等待 10-20 分钟,勿中断)..."
& idf.py build
