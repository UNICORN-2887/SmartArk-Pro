#!/usr/bin/env python3
"""
SD卡文件结构部署脚本
用法: python deploy_sd.py <SD卡盘符>  例: python deploy_sd.py E:
"""
import os, sys, shutil, struct

CONVERTER = os.path.dirname(os.path.abspath(__file__))

TASKS = [
    # (源, SD卡目标相对路径)
    # ── Cover MJPEG ──
    ("cover_output/amiya_cover.mjpeg",  "main/operator/CASTER/5STAR/Amiya/cover/cover.mjpeg"),
    ("cover_output/kaltsit_cover.mjpeg", "main/operator/MEDIC/6STAR/Kaltsit/cover/cover.mjpeg"),
]

def copy_emoji(src_folder, dst_folder):
    """复制表情 MJPEG 文件夹"""
    if not os.path.isdir(src_folder):
        print(f"  ⚠ 源不存在: {src_folder}")
        return 0
    count = 0
    for f in os.listdir(src_folder):
        if f.endswith('.mjpeg'):
            shutil.copy2(os.path.join(src_folder, f), os.path.join(dst_folder, f))
            count += 1
    return count

def main():
    if len(sys.argv) < 2:
        print("用法: python deploy_sd.py <SD卡盘符>")
        print("示例: python deploy_sd.py E:")
        sys.exit(1)

    sd_root = sys.argv[1].rstrip('\\/')
    if not os.path.exists(sd_root):
        print(f"错误: SD卡路径不存在: {sd_root}")
        sys.exit(1)

    print(f"部署到: {sd_root}\n")

    # ── Cover MJPEG ──
    print("[Cover MJPEG]")
    for src_rel, dst_rel in TASKS:
        src = os.path.join(CONVERTER, src_rel)
        dst = os.path.join(sd_root, dst_rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(src):
            shutil.copy2(src, dst)
            kb = os.path.getsize(dst) / 1024
            print(f"  ✓ {dst_rel} ({kb:.0f} KB)")
        else:
            print(f"  ✗ 源文件不存在: {src}")

    # ── 阿米娅 Emoji ──
    print("\n[阿米娅 Emoji]")
    amiya_dst = os.path.join(sd_root, "main/operator/CASTER/5STAR/Amiya/emoji")
    os.makedirs(amiya_dst, exist_ok=True)
    n = copy_emoji(os.path.join(CONVERTER, "阿米娅"), amiya_dst)
    print(f"  → {n} files")

    # ── 凯尔希 Emoji ──
    print("\n[凯尔希 Emoji]")
    kal_dst = os.path.join(sd_root, "main/operator/MEDIC/6STAR/Kaltsit/emoji")
    os.makedirs(kal_dst, exist_ok=True)
    n = copy_emoji(os.path.join(CONVERTER, "凯尔希"), kal_dst)
    print(f"  → {n} files")

    # ── Voice 目录（占位）──
    for agent in ["CASTER/5STAR/Amiya", "MEDIC/6STAR/Kaltsit"]:
        os.makedirs(os.path.join(sd_root, "main/operator", agent, "voice"), exist_ok=True)

    print(f"\n✓ 部署完成! SD卡根目录: {sd_root}")
    print("  请把 background.jpg 放到 main/background/ 下")

if __name__ == '__main__':
    main()
