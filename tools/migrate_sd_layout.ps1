# 虚拟 SD 卡文件结构规范迁移（一次性脚本，跑完即作废）
# 目标：main/operator/<职业>/<星级>/<干员>/{PPD,LIVE2D}
# 来源：paperdoll/<角色>（PPD）、根目录 <角色>（LIVE2D）、Amiya.moc3 散文件
$ErrorActionPreference = "Stop"
$sd = "E:\虚拟SD卡"
$op = "$sd\main\operator"

# ── PPD：源目录小写角色名 → operator 相对路径（目标子目录 PPD）──
$ppd = @{
    "amiya"      = "CASTER\5STAR\Amiya"
    "angelina"   = "SUPPORTER\6STAR\Angelina"
    "eyjafjalla" = "CASTER\6STAR\Eyjafjalla"
    "lappland"   = "GUARD\5STAR\Lappland"
    "mon3tr"     = "MEDIC\6STAR\Mon3tr"
    "muelsyse"   = "SPECIALIST\6STAR\Muelsyse"
    "texas"      = "VANGUARD\5STAR\Texas"
    "theresia"   = "SUPPORTER\6STAR\Civilight_Eterna"
}
foreach ($k in $ppd.Keys) {
    $src = "$sd\paperdoll\$k"
    if (-not (Test-Path $src)) { Write-Output "PPD MISSING: $src"; continue }
    $dst = "$op\$($ppd[$k])\PPD"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Move-Item "$src\*" $dst -Force
    Remove-Item $src -Force
    Write-Output "PPD ok: $k -> $dst"
}

# ── LIVE2D：源目录名 → operator 相对路径（目标子目录 LIVE2D）──
$lv = @{
    "Amiya"    = "CASTER\5STAR\Amiya"
    "Theresia" = "SUPPORTER\6STAR\Civilight_Eterna"
}
foreach ($k in $lv.Keys) {
    $src = "$sd\$k"
    if (-not (Test-Path $src)) { Write-Output "LIVE2D MISSING: $src"; continue }
    $dst = "$op\$($lv[$k])\LIVE2D"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Move-Item "$src\*" $dst -Force
    Remove-Item $src -Force
    Write-Output "LIVE2D ok: $k -> $dst"
}

# ── Amiya.moc3 散文件 → Amiya/LIVE2D ──
if (Test-Path "$sd\Amiya.moc3") {
    New-Item -ItemType Directory -Force "$op\CASTER\5STAR\Amiya\LIVE2D" | Out-Null
    Move-Item "$sd\Amiya.moc3" "$op\CASTER\5STAR\Amiya\LIVE2D" -Force
    Write-Output "moc3 ok -> CASTER\5STAR\Amiya\LIVE2D"
}

# ── 非干员角色 → 根目录 tempLIVE2D 临时存放 ──
foreach ($k in @("Furina", "zll0")) {
    $src = "$sd\$k"
    if (-not (Test-Path $src)) { Write-Output "temp MISSING: $src"; continue }
    $dst = "$sd\tempLIVE2D\$k"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Move-Item "$src\*" $dst -Force
    Remove-Item $src -Force
    Write-Output "temp ok: $k -> $dst"
}

# ── 清理空的 paperdoll 根目录 ──
if ((Test-Path "$sd\paperdoll") -and -not (Get-ChildItem "$sd\paperdoll" -Force)) {
    Remove-Item "$sd\paperdoll" -Force
    Write-Output "paperdoll root removed (empty)"
}
Write-Output "=== DONE ==="
