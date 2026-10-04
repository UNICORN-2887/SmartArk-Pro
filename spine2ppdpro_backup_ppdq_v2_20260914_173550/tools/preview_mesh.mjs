import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';

const require = createRequire(import.meta.url);
function loadCanvasModule() {
    try {
        return require('canvas');
    } catch (e) {
        return require(path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')),
            '../../tools/spine2ppd/node_modules/canvas'));
    }
}
const { createCanvas, loadImage } = loadCanvasModule();

const ppdDir = process.argv[2];
const outPng = process.argv[3] || 'mesh_preview.png';
const animName = process.argv[4] || '';
const tRatio = Number(process.argv[5] || '0');
const landscape = process.argv.includes('--landscape');

function readRaw(file) {
    const b = fs.readFileSync(file);
    const w = b.readUInt16LE(0), h = b.readUInt16LE(2);
    const c = createCanvas(w, h);
    const ctx = c.getContext('2d');
    const img = ctx.createImageData(w, h);
    img.data.set(b.subarray(4));
    ctx.putImageData(img, 0, 0);
    return c;
}

function sampleTrack(track, duration, t) {
    if (!track) return { dx: 0, dy: 0, drot: 0, vis: true, sx: 1, sy: 1 };
    const pos = Math.min(Math.max(t / Math.max(duration, 0.001) * (track.length - 1), 0), track.length - 1);
    const i0 = Math.floor(pos), i1 = Math.min(i0 + 1, track.length - 1), f = pos - i0;
    const a = track[i0], b = track[i1];
    const val = (i, d) => (a[i] ?? d) + ((b[i] ?? d) - (a[i] ?? d)) * f;
    return { dx: val(0, 0), dy: val(1, 0), drot: val(2, 0), vis: val(3, 1) >= 0.5, sx: val(4, 1), sy: val(5, 1) };
}

function tri(ctx, img, sx0, sy0, sx1, sy1, sx2, sy2, dx0, dy0, dx1, dy1, dx2, dy2) {
    const det = sx0 * (sy1 - sy2) + sx1 * (sy2 - sy0) + sx2 * (sy0 - sy1);
    if (Math.abs(det) < 1e-6) return;
    const a = (dx0 * (sy1 - sy2) + dx1 * (sy2 - sy0) + dx2 * (sy0 - sy1)) / det;
    const c = (dx0 * (sx2 - sx1) + dx1 * (sx0 - sx2) + dx2 * (sx1 - sx0)) / det;
    const e = (dx0 * (sx1 * sy2 - sx2 * sy1) + dx1 * (sx2 * sy0 - sx0 * sy2) + dx2 * (sx0 * sy1 - sx1 * sy0)) / det;
    const b = (dy0 * (sy1 - sy2) + dy1 * (sy2 - sy0) + dy2 * (sy0 - sy1)) / det;
    const d = (dy0 * (sx2 - sx1) + dy1 * (sx0 - sx2) + dy2 * (sx1 - sx0)) / det;
    const f = (dy0 * (sx1 * sy2 - sx2 * sy1) + dy1 * (sx2 * sy0 - sx0 * sy2) + dy2 * (sx0 * sy1 - sx1 * sy0)) / det;
    ctx.save();
    const cx = (dx0 + dx1 + dx2) / 3;
    const cy = (dy0 + dy1 + dy2) / 3;
    const grow = (x, y) => {
        const vx = x - cx, vy = y - cy;
        const len = Math.hypot(vx, vy) || 1;
        return [x + vx / len * 0.75, y + vy / len * 0.75];
    };
    const p0 = grow(dx0, dy0), p1 = grow(dx1, dy1), p2 = grow(dx2, dy2);
    ctx.beginPath();
    ctx.moveTo(p0[0], p0[1]); ctx.lineTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]);
    ctx.closePath();
    ctx.clip();
    ctx.transform(a, b, c, d, e, f);
    ctx.drawImage(img, 0, 0);
    ctx.restore();
}

function drawMeshItem(ctx, mesh, images, item) {
    const specMap = mesh.attachments || mesh.layers || {};
    const spec = specMap[item.key];
    const verts = item.vertices;
    if (!spec || !verts) return false;
    const img = images.get(spec.texture);
    if (!img) return false;
    const prevAlpha = ctx.globalAlpha;
    const prevBlend = ctx.globalCompositeOperation;
    if (spec.alpha != null) ctx.globalAlpha = prevAlpha * spec.alpha;
    if (spec.blend === 'additive') ctx.globalCompositeOperation = 'lighter';
    for (let i = 0; i < spec.triangles.length; i += 3) {
        const a = spec.triangles[i], b = spec.triangles[i + 1], c = spec.triangles[i + 2];
        tri(ctx, img,
            spec.uvs[a * 2], spec.uvs[a * 2 + 1], spec.uvs[b * 2], spec.uvs[b * 2 + 1], spec.uvs[c * 2], spec.uvs[c * 2 + 1],
            verts[a * 2], verts[a * 2 + 1], verts[b * 2], verts[b * 2 + 1], verts[c * 2], verts[c * 2 + 1]);
    }
    ctx.globalAlpha = prevAlpha;
    ctx.globalCompositeOperation = prevBlend;
    return true;
}

function drawMesh(ctx, mesh, images, name, verts) {
    return drawMeshItem(ctx, mesh, images, { key: name, vertices: verts });
}

function drawMeshScene(ctx, mesh, images, frame) {
    for (const item of frame.draw || []) drawMeshItem(ctx, mesh, images, item);
}

const scene = JSON.parse(fs.readFileSync(path.join(ppdDir, 'scene.json'), 'utf8'));
const anims = fs.existsSync(path.join(ppdDir, 'anims.json')) ? JSON.parse(fs.readFileSync(path.join(ppdDir, 'anims.json'), 'utf8')) : {};
const mesh = fs.existsSync(path.join(ppdDir, 'mesh.json')) ? JSON.parse(fs.readFileSync(path.join(ppdDir, 'mesh.json'), 'utf8')) : null;
const anim = animName ? anims[animName] : null;
const meshAnim = mesh && animName && mesh.animations[animName] ? mesh.animations[animName] : null;
const meshIndex = meshAnim ? Math.min(meshAnim.frames.length - 1, Math.max(0, Math.floor(tRatio * meshAnim.frames.length))) : -1;
const meshFrame = meshAnim ? meshAnim.frames[meshIndex] : (mesh ? (mesh.base.layers || mesh.base) : {});
const textures = new Map();
for (const l of scene.layers) {
    const f = path.join(ppdDir, `${l.name}.raw`);
    if (fs.existsSync(f)) textures.set(l.name, readRaw(f));
}
const meshImages = new Map();
if (mesh) for (const t of mesh.textures) meshImages.set(t.id, await loadImage(path.join(ppdDir, t.file)));

const canvas = createCanvas(480, 800);
const ctx = canvas.getContext('2d');
ctx.fillStyle = '#1a202b';
ctx.fillRect(0, 0, 480, 800);
if (mesh && mesh.version >= 2) {
    drawMeshScene(ctx, mesh, meshImages, meshFrame);
} else for (const l of [...scene.layers].sort((a, b) => a.z - b.z)) {
    if (mesh && mesh.layers && mesh.layers[l.name]) {
        drawMesh(ctx, mesh, meshImages, l.name, meshFrame[l.name]);
        continue;
    }
    const tex = textures.get(l.name);
    if (!tex) continue;
    let dx = 0, dy = 0, drot = 0, vis = !!l.visible, sx = 1, sy = 1;
    if (anim && anim.layers[l.name]) {
        const s = sampleTrack(anim.layers[l.name], anim.duration, anim.duration * tRatio);
        dx = s.dx; dy = s.dy; drot = s.drot; vis = s.vis; sx = s.sx; sy = s.sy;
    }
    if (!vis) continue;
    const px = l.bone_px ?? l.w / 2, py = l.bone_py ?? l.h / 2;
    ctx.save();
    ctx.translate(l.x + px + dx, l.y + py + dy);
    ctx.rotate(drot * Math.PI / 180);
    ctx.scale(sx, sy);
    ctx.translate(-px, -py);
    ctx.drawImage(tex, 0, 0, l.w, l.h);
    ctx.restore();
}
let out = canvas;
if (landscape) {
    out = createCanvas(800, 480);
    const r = out.getContext('2d');
    r.translate(400, 240);
    r.rotate(Math.PI / 2);
    r.drawImage(canvas, -240, -400);
}
fs.writeFileSync(outPng, out.toBuffer('image/png'));
console.log(`mesh preview → ${outPng}`);
