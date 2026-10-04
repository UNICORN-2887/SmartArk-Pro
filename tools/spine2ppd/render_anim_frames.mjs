// 官方 spine-canvas 渲染指定动画的帧序列 → <outdir>/<anim>_f<n>.png
// 输出 480×800 渲染缓冲原样（landscape 变换后、未转正立——与设备端场景同系，
// 供 synth_diff.py 逐像素对照）。渲染参数与 render_layers.mjs 完全一致
// （SCALE/OFF 基于 Default 姿势 bbox），动画按标准流程
// setToSetupPose → Default.apply → anim.apply(0,t)。
// 用法：node render_anim_frames.mjs <spine_dir> <outdir> <anim> [--landscape]
import { createCanvas, loadImage } from 'canvas';
import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const outDir = process.argv[3];
const animName = process.argv[4];
const landscape = process.argv[5] === '--landscape';
fs.mkdirSync(outDir, { recursive: true });
const files = fs.readdirSync(dir);
const skelFile = files.find(f => f.endsWith('.skel'));
const atlasFile = files.find(f => f.endsWith('.atlas'));
const pngFile = files.find(f => f.endsWith('.png'));

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

// 图集声明尺寸 vs png 实际尺寸：部分下载渠道把图集缩了——放大到声明尺寸
// 恢复几何（uv 归一化基准正确）
const atlasText = fs.readFileSync(path.join(dir, atlasFile), 'utf8');
const declM = atlasText.match(/^size:\s*(\d+)\s*,\s*(\d+)/m);
let img = await loadImage(path.join(dir, pngFile));
if (declM && (img.width < +declM[1] || img.height < +declM[2])) {
    const c = createCanvas(+declM[1], +declM[2]);
    const cctx = c.getContext('2d');
    cctx.drawImage(img, 0, 0, +declM[1], +declM[2]);
    img = c;
    console.log(`图集 ${img.width}×${img.height} → ${declM[1]}×${declM[2]} 放大（png 被下载渠道缩放）`);
}
const W = 480, H = 800;
const canvas = createCanvas(W, H);
const ctx = canvas.getContext('2d');

const atlas = new spine.TextureAtlas(atlasText,
    p => new spine.Texture(img, img.width, img.height));
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, skelFile))));
const skel = new spine.Skeleton(data);
const defaultAnim = data.animations.find(a => a.name === 'Default');
const anim = data.animations.find(a => a.name === animName);
if (!anim) { console.error(`动画不存在: ${animName}`); process.exit(1); }

// 内容 bbox（Default 姿势，与 render_layers.mjs 相同）
skel.setToSetupPose();
if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
skel.updateWorldTransform();
let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
{
    const verts = new Float32Array(2048);
    for (const s of skel.slots) {
        const at = s.getAttachment();
        if (!at || !at.region) continue;
        if (at.constructor.name === 'RegionAttachment') {
            at.computeWorldVertices(skel.bones[s.data.boneData.index], verts, 0, 2);
            for (let i = 0; i < 8; i += 2) {
                minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
                minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
            }
        } else {
            const b = skel.bones[s.data.boneData.index];
            for (const [px0, py0] of [[0, 0], [at.region.width, 0], [0, at.region.height], [at.region.width, at.region.height]]) {
                const wx = b.a * px0 + b.b * py0 + b.worldX;
                const wy = b.c * px0 + b.d * py0 + b.worldY;
                minX = Math.min(minX, wx); maxX = Math.max(maxX, wx);
                minY = Math.min(minY, wy); maxY = Math.max(maxY, wy);
            }
        }
    }
}
const CW = maxX - minX, CH = maxY - minY;
let SCALE, OFF_X, OFF_Y;
if (landscape) {
    SCALE = Math.min(W * 0.95 / CH, H * 0.95 / CW);
    OFF_X = W / 2 + (minY + maxY) / 2 * SCALE;
    OFF_Y = H / 2 - (minX + maxX) / 2 * SCALE;
} else {
    SCALE = Math.min(W * 0.95 / CW, H * 0.95 / CH);
    OFF_X = (W - CW * SCALE) / 2 - minX * SCALE;
    OFF_Y = (H - CH * SCALE) / 2 - minY * SCALE;
}

const renderer = new spine.canvas.SkeletonRenderer(ctx);
renderer.triangleRendering = true;
// 帧数 = 导出采样数（synth_diff 同公式，帧时刻完全对齐）
const FPS = 20;
const n = Math.max(8, Math.min(80, Math.round(anim.duration * FPS)));
for (let f = 0; f < n; f++) {
    const t = anim.duration * f / n;
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    anim.apply(skel, 0, t, false, null, 1, 0, 0);
    skel.updateWorldTransform();
    ctx.fillStyle = '#1e2430';
    ctx.fillRect(0, 0, W, H);
    ctx.save();
    ctx.translate(OFF_X, OFF_Y);
    if (landscape) ctx.rotate(Math.PI / 2);
    ctx.scale(SCALE, SCALE);
    renderer.draw(skel);
    ctx.restore();
    fs.writeFileSync(path.join(outDir, `${animName}_f${f}.png`), canvas.toBuffer('image/png'));
}
console.log(`${animName}: ${n} 帧 → ${outDir}`);
