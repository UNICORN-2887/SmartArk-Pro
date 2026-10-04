# -*- coding: utf-8 -*-
"""绕开 release.py 的 set-target+fullclean 循环:
1. 手动把 config.json 的 sdkconfig_append 应用进 sdkconfig(utf-8 无 BOM)
2. 清掉半成品 build 目录
3. 直接 idf.py build(target 已设 esp32p4,依赖解析已通过)
"""
import io
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
cfg = json.load(io.open(os.path.join(ROOT, 'main', 'boards', 'guition-jc4880p443', 'config.json'),
                        encoding='utf-8'))
items = cfg['builds'][0]['sdkconfig_append']

# 1. 应用 append(同名替换,新项追加;utf-8 无 BOM 写回)
sk_path = os.path.join(ROOT, 'sdkconfig')
if not os.path.isfile(sk_path):
    old = os.path.join(ROOT, 'sdkconfig.old')
    if os.path.isfile(old):
        shutil.copyfile(old, sk_path)
        print('sdkconfig 缺失,已从 sdkconfig.old 恢复')
    else:
        print('错误: sdkconfig 与 sdkconfig.old 都不存在,请先运行一次 idf.py set-target esp32p4')
        sys.exit(2)
lines = io.open(sk_path, encoding='utf-8').read().splitlines()
orig_content = '\n'.join(lines) + '\n'
keys = {}
order = []
for ln in lines:
    k = ln.split('=', 1)[0] if '=' in ln else ln
    keys[k] = ln
    if k not in order:
        order.append(k)
for it in items:
    k = it.split('=', 1)[0]
    if k in keys:
        # 同名替换:保留原位置,值更新
        keys[k] = it
    else:
        keys[k] = it
        order.append(k)
out = [keys[k] for k in order if k in keys]
# 板子 choice 清理:sdkconfig.old 是 Kconfig 默认(旧板子宏可能 =y),
# 只保留 GUITION_JC4880P443(choice 只能一个 y)
out = [ln for ln in out
       if not (ln.startswith('CONFIG_BOARD_TYPE_') and ln.endswith('=y')
               and not ln.startswith('CONFIG_BOARD_TYPE_GUITION_JC4880P443='))]
io.open(sk_path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print('sdkconfig_append 已应用: %d 项' % len(items))
# 2026-10-02 bootloader 不随 sdkconfig 变化重编(此前 SPIRAM_XIP_FROM_PSRAM 改动后
# 烧录旧 bootloader → unpack_load_app rom_index==2 assert)。内容有变则清 bootloader 缓存
if '\n'.join(out) + '\n' != orig_content:
    # 2026-10-02 bootloader 不随 sdkconfig 变化重编 → 配置不一致崩溃
    # (unpack_load_app rom_index==2)。清 bootloader 目录+ExternalProject stamp
    for sub in ('build/bootloader', 'build/bootloader-prefix'):
        p = os.path.join(ROOT, sub)
        if os.path.isdir(p):
            shutil.rmtree(p)
    print('sdkconfig 有变化,已清 bootloader 缓存+stamp(强制重编,防配置不一致崩溃)')

# 2. build 目录仅在异常残留时清理(正常增量编译,避免每次重新拉依赖元数据)
bd = os.path.join(ROOT, 'build')
if os.path.isdir(bd) and not os.path.isfile(os.path.join(bd, 'CMakeCache.txt')):
    shutil.rmtree(bd)
    print('build 目录(无缓存)已清')

# 3. 直接构建(2026-10-02 限制并行防编译器 OOM;机器内存充足可设 IDF_BUILD_JOBS=8)
jobs = os.environ.get('IDF_BUILD_JOBS', '2')
bd = os.path.join(ROOT, 'build')
if os.path.isfile(os.path.join(bd, 'CMakeCache.txt')):
    # 已配置:直接 ninja 增量编译(sdkconfig 变化 ninja 会自动重跑 cmake)
    print('开始 ninja -C build -j %s(增量编译)...' % jobs, flush=True)
    sys.exit(subprocess.call('ninja -C build -j %s' % jobs, cwd=ROOT, shell=True))
print('开始 idf.py build(首次全量,10-20 分钟,请勿中断)...', flush=True)
sys.exit(subprocess.call('idf.py build', cwd=ROOT, shell=True))
