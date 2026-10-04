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

const dir = process.argv[2];
const animName = process.argv[3] || 'Sleep';
const outPng = process.argv[4] || 'official_frame.png';
const tRatio = Number(process.argv[5] || '0.5');
const noRotate = process.argv.includes('--scene');
const onlyArg = process.argv[6] || '';
const only = onlyArg.startsWith('--') ? '' : onlyArg;

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const files = fs.readdirSync(dir);
const atlasFile = files.find(f => f.endsWith('.atlas'));
const skelFile = files.find(f => f.endsWith('.skel'));
const pngFiles = files.filter(f => f.endsWith('.png')).sort();
let atlasText = fs.readFileSync(path.join(dir, atlasFile), 'utf8');
const pages = [];
{
    const re = /(?:^|\n)([^\n:]+\.png)\nsize:\s*(\d+)\s*,\s*(\d+)/g;
    let m;
    while ((m = re.exec(atlasText))) pages.push({ name: m[1], w: +m[2], h: +m[3] });
}
if (!pages.length) pages.push({ name: pngFiles[0], w: 0, h: 0 });

const pageTex = new Map();
for (const pg of pages) {
    const img = await loadImage(path.join(dir, pg.name));
    if (pg.w && (img.width < pg.w || img.height < pg.h)) {
        const sx = img.width / pg.w, sy = img.height / pg.h;
        if (Math.abs(sx - 1) > 0.01 || Math.abs(sy - 1) > 0.01) {
            atlasText = atlasText.replace(
                /(xy|size|orig|offset):\s*(\d+),\s*(\d+)/g,
                (m, k, a, b) => `${k}: ${Math.max(1, Math.round(a * sx))}, ${Math.max(1, Math.round(b * sy))}`);
        }
    }
    pageTex.set(pg.name, img);
}

const atlas = new spine.TextureAtlas(atlasText, p => {
    const img = pageTex.get(p) || pageTex.values().next().value;
    return new spine.Texture(img, img.width, img.height);
});
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, skelFile))));
const skel = new spine.Skeleton(data);
const defaultAnim = data.animations.find(a => a.name === 'Default');
const anim = data.animations.find(a => a.name === animName);
if (!anim) throw new Error(`animation not found: ${animName}`);

function applyAt(time) {
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    anim.apply(skel, 0, time, false, null, 1, 0, 0);
    skel.updateWorldTransform();
}

skel.setToSetupPose();
if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
skel.updateWorldTransform();
const verts = new Float32Array(4096);
let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
for (const s of skel.slots) {
    const at = s.getAttachment();
    if (!at || !at.region || (s.color && s.color.a <= 0.01)) continue;
    if (at.constructor.name === 'RegionAttachment') {
        at.computeWorldVertices(skel.bones[s.data.boneData.index], verts, 0, 2);
        for (let i = 0; i < 8; i += 2) {
            minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
            minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
        }
    } else {
        at.computeWorldVertices(s, 0, at.worldVerticesLength, verts, 0, 2);
        for (let i = 0; i < at.worldVerticesLength; i += 2) {
            minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
            minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
        }
    }
}

const W = 480, H = 800;
const scale = Math.min(W * 0.95 / (maxY - minY), H * 0.95 / (maxX - minX));
const offX = W / 2 + (minY + maxY) / 2 * scale;
const offY = H / 2 - (minX + maxX) / 2 * scale;

applyAt(anim.duration * tRatio);
if (only) {
    for (const slot of skel.slots) {
        const at = slot.getAttachment();
        const attName = at && at.name ? at.name : '';
        if (!slot.data.name.includes(only) && !attName.includes(only))
            slot.setAttachment(null);
    }
    skel.updateWorldTransform();
}
const canvas = createCanvas(W, H);
const ctx = canvas.getContext('2d');
ctx.fillStyle = '#1a202b';
ctx.fillRect(0, 0, W, H);
ctx.save();
ctx.translate(offX, offY);
ctx.rotate(Math.PI / 2);
ctx.scale(scale, scale);
const renderer = new spine.canvas.SkeletonRenderer(ctx);
renderer.triangleRendering = true;
renderer.draw(skel);
ctx.restore();

if (noRotate) {
    fs.writeFileSync(outPng, canvas.toBuffer('image/png'));
} else {
    const rotated = createCanvas(H, W);
    const rctx = rotated.getContext('2d');
    rctx.translate(H / 2, W / 2);
    rctx.rotate(Math.PI / 2);
    rctx.drawImage(canvas, -W / 2, -H / 2);
    fs.writeFileSync(outPng, rotated.toBuffer('image/png'));
}
console.log(`${outPng} duration=${anim.duration.toFixed(3)} t=${(anim.duration * tRatio).toFixed(3)}`);
