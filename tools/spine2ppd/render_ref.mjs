// 官方 spine-canvas 渲染整身参考图 → ref.png（与 preview.png 对照：
// 若复合预览 ≠ 整身参考，说明逐层隔离有问题）
// 渲染参数与 render_layers.mjs 完全一致（Default 动画第 0 帧 + 可见附件 bbox），
// 输出时 180° 旋转成正立图方便肉眼对照；--landscape 与 render_layers 同变换
// 用法：node render_ref.mjs <spine_dir> [ref.png] [--landscape]
import { createCanvas, loadImage } from 'canvas';
import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const out = process.argv[3] || 'ref.png';
const landscape = process.argv[4] === '--landscape';
const files = fs.readdirSync(dir);
const skelFile = files.find(f => f.endsWith('.skel'));
const atlasFile = files.find(f => f.endsWith('.atlas'));
const pngFile = files.find(f => f.endsWith('.png'));

// 只用 canvas 版本（自带核心类集；与 core 版本混用会 instanceof 失败 → 空白渲染）
const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

// 图集声明尺寸 vs png 实际尺寸：部分下载渠道把图集缩了（基建 png 344×344
// vs atlas 声明 516×516）——放大到声明尺寸恢复几何（uv 归一化基准正确）
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
skel.setToSetupPose();
// Default 动画第 0 帧 = 正确初始姿势（setup pose 同时含 5 套姿势链会叠加）
const defaultAnim = data.animations.find(a => a.name === 'Default');
if (defaultAnim) {
    defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
}
skel.updateWorldTransform();

// 内容 bbox：只算当前挂载的可见附件（与 render_layers.mjs 相同逻辑）
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

// 正 scale 渲染（与 render_layers 同参数；缓冲中人物倒立，横屏模式再转 90°）
ctx.fillStyle = '#1e2430';
ctx.fillRect(0, 0, W, H);
ctx.save();
ctx.translate(OFF_X, OFF_Y);
if (landscape) ctx.rotate(Math.PI / 2);
ctx.scale(SCALE, SCALE);
const renderer = new spine.canvas.SkeletonRenderer(ctx);
renderer.triangleRendering = true;
renderer.draw(skel);
ctx.restore();

// 输出：竖屏 180° 旋转成正立图；横屏逆时针 90° 转成 800×480 正立图（同 preview.py）
if (landscape) {
    const upright = createCanvas(H, W);
    const uctx = upright.getContext('2d');
    uctx.translate(H / 2, W / 2);
    uctx.rotate(-Math.PI / 2);
    uctx.drawImage(canvas, -W / 2, -H / 2);
    fs.writeFileSync(out, upright.toBuffer('image/png'));
} else {
    const upright = createCanvas(W, H);
    const uctx = upright.getContext('2d');
    uctx.translate(W / 2, H / 2);
    uctx.rotate(Math.PI);
    uctx.drawImage(canvas, -W / 2, -H / 2);
    fs.writeFileSync(out, upright.toBuffer('image/png'));
}
console.log(`ref rendered → ${out}${landscape ? ' (landscape)' : ''}`);
