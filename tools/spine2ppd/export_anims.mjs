// 动作导出：Spine 每个动画逐帧渲染（横屏变换与 render_layers 一致）→ MJPEG 序列
// 设备端动作测试页播放（复用 cover 播放管线，PC 预旋转零设备端旋转）
// 用法：node export_anims.mjs <spine_dir> <out_dir> [fps] [--bg <竖帧背景PNG>]
// 输出：<out_dir>/anim/<动画名>.mjpeg + anim.json
//
// 关键：所有动画所有帧共享一个 fit 变换（全部帧可见附件 bbox 的并集），
// 保证播放过程中画面不跳变（若逐帧各自 fit，动作范围变化会导致画面抖动）。
// 导出帧与场景同方向：渲染后 180° 翻转（与 render_layers 导出一致，否则动画倒立）；
// 可选背景图（--bg）：与互动背景一致的竖帧 PNG，动作在"原有画面"上执行而非纯色背景。
import { createCanvas, loadImage } from 'canvas';
import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const outDir = process.argv[3];
const FPS = parseInt(process.argv[4] || '20', 10);
const bgPath = process.argv[5] === '--bg' ? process.argv[6] : null;
const animDir = path.join(outDir, 'anim');
fs.mkdirSync(animDir, { recursive: true });

const files = fs.readdirSync(dir);
const skelFile = files.find(f => f.endsWith('.skel'));
const atlasFile = files.find(f => f.endsWith('.atlas'));
const pngFile = files.find(f => f.endsWith('.png'));

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const img = await loadImage(path.join(dir, pngFile));
const W = 480, H = 800;
const canvas = createCanvas(W, H);
const ctx = canvas.getContext('2d');

const atlas = new spine.TextureAtlas(fs.readFileSync(path.join(dir, atlasFile), 'utf8'),
    p => new spine.Texture(img, img.width, img.height));
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, skelFile))));
const skel = new spine.Skeleton(data);

// 动作白名单：跳过 Begin/End（0s 过渡帧）
const SKIP = new Set(['Begin', 'End']);
const anims = data.animations.filter(a => !SKIP.has(a.name) && a.duration > 0);

// 采样帧的可见附件 bbox 并集
function visibleBbox() {
    const verts = new Float32Array(2048);
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
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
    return [minX, minY, maxX, maxY];
}

// 每动画独立 fit：动画内所有帧 bbox 并集 → 该动画的 SCALE/OFF（帧已烘焙，
// 跨动画尺度不同无妨——关键保证单动画内帧间画面不跳变；Start 等入场动画
// 首帧角色在远处，全动画 union 会被污染）
const renderer = new spine.canvas.SkeletonRenderer(ctx);
renderer.triangleRendering = true;

function animFit(a, n) {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (let f = 0; f < n; f++) {
        skel.setToSetupPose();
        a.apply(skel, a.duration * f / n, 0, false, null, 1, 0, 0);
        skel.updateWorldTransform();
        const [ax0, ay0, ax1, ay1] = visibleBbox();
        if (!isFinite(ax0)) continue;
        x0 = Math.min(x0, ax0); x1 = Math.max(x1, ax1);
        y0 = Math.min(y0, ay0); y1 = Math.max(y1, ay1);
    }
    const CW = x1 - x0, CH = y1 - y0;
    // 横屏 fit（同 render_layers --landscape）：角色高 → 画布宽，宽 → 画布高，顺时针 90°
    const scale = Math.min(W * 0.95 / CH, H * 0.95 / CW);
    return { scale, ox: W / 2 + (y0 + y1) / 2 * scale, oy: H / 2 - (x0 + x1) / 2 * scale };
}

// 渲染一帧 → JPEG buffer（180° 翻转导出：与 render_layers 场景方向一致，否则设备上倒立）
function renderFrame(a, t, scale, ox, oy) {
    skel.setToSetupPose();
    a.apply(skel, t, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
    if (bgImg) {
        ctx.drawImage(bgImg, 0, 0, W, H);   // 互动背景（与场景 bg.raw 同源，动作在原画面执行）
    } else {
        ctx.fillStyle = '#1e2430';
        ctx.fillRect(0, 0, W, H);
    }
    ctx.save();
    ctx.translate(ox, oy);
    ctx.rotate(Math.PI / 2);
    ctx.scale(scale, scale);
    renderer.draw(skel);
    ctx.restore();
    // 180° 旋转成正立导出帧（与场景 180° 翻转向一致）
    const flipped = createCanvas(W, H);
    const fctx = flipped.getContext('2d');
    fctx.translate(W / 2, H / 2);
    fctx.rotate(Math.PI);
    fctx.drawImage(canvas, -W / 2, -H / 2);
    return flipped.toBuffer('image/jpeg', { quality: 88 });
}

const bgImg = bgPath ? await loadImage(bgPath) : null;
if (bgImg) console.log(`背景图: ${bgPath} (${bgImg.width}x${bgImg.height})`);

const manifest = {};
for (const a of anims) {
    const n = Math.max(8, Math.min(80, Math.round(a.duration * FPS)));
    const { scale, ox, oy } = animFit(a, n);
    console.log(`  ${a.name}: ${n}f ${a.duration.toFixed(2)}s scale=${scale.toFixed(3)}`);
    const bufs = [];
    for (let f = 0; f < n; f++) bufs.push(renderFrame(a, a.duration * f / n, scale, ox, oy));
    // MJPEG 封装：u32 帧数 + n×u32 偏移表 + 连续帧数据（设备端 load_mjpeg_into 约定）
    const header = Buffer.alloc(4 + 4 * n);
    header.writeUInt32LE(n, 0);
    let off = 4 + 4 * n;
    for (let f = 0; f < n; f++) {
        header.writeUInt32LE(off, 4 + 4 * f);
        off += bufs[f].length;
    }
    const mj = path.join(animDir, `${a.name}.mjpeg`);
    fs.writeFileSync(mj, Buffer.concat([header, ...bufs]));
    manifest[a.name] = { file: `anim/${a.name}.mjpeg`, frames: n, duration: a.duration };
    console.log(`    → ${a.name}.mjpeg ${(off / 1024).toFixed(0)} KB`);
}
fs.writeFileSync(path.join(outDir, 'anim.json'), JSON.stringify(manifest, null, 1));
console.log(`anim.json: ${anims.length} 动画 → ${animDir}`);
