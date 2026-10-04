// Spine attachments -> PPD-Mesh extension.
// Output: <out_dir>/mesh.json plus atlas PNG pages copied next to scene.json.
// This is an additive extension: old scene.json/anims.json/*.raw remain valid.
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
const { loadImage } = loadCanvasModule();

const dir = process.argv[2];
const outDir = process.argv[3];
const FPS = parseInt(process.argv[4] || '20', 10);
if (!dir || !outDir) throw new Error('usage: node export_mesh_ppd.mjs <spine_dir> <out_dir> [fps]');

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const files = fs.readdirSync(dir);
const skelFile = files.find(f => f.endsWith('.skel'));
const atlasFile = files.find(f => f.endsWith('.atlas'));
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
const pageSizes = new Map();
for (const pg of pages) {
    const src = path.join(dir, pg.name);
    const img = await loadImage(src);
    pageSizes.set(pg.name, { w: img.width, h: img.height });
    pageTex.set(pg.name, img);
    fs.copyFileSync(src, path.join(outDir, pg.name));
    if (pg.w && (img.width < pg.w || img.height < pg.h)) {
        const sx = img.width / pg.w, sy = img.height / pg.h;
        if (Math.abs(sx - 1) > 0.01 || Math.abs(sy - 1) > 0.01) {
            atlasText = atlasText.replace(
                /(xy|size|orig|offset):\s*(\d+),\s*(\d+)/g,
                (m, k, a, b) => `${k}: ${Math.max(1, Math.round(a * sx))}, ${Math.max(1, Math.round(b * sy))}`);
        }
    }
}

const atlas = new spine.TextureAtlas(atlasText, p => {
    const img = pageTex.get(p) || pageTex.values().next().value;
    return new spine.Texture(img, img.width, img.height);
});
const data = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas))
    .readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, skelFile))));
const skel = new spine.Skeleton(data);
const skin0 = data.defaultSkin || data.skins[0];
const defaultAnim = data.animations.find(a => a.name === 'Default');

function applyPose(anim, time = 0) {
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    if (anim && anim !== defaultAnim) anim.apply(skel, 0, time, false, null, 1, 0, 0);
    skel.updateWorldTransform();
}

function slotVisible(slot, at) {
    if (!at || !at.region) return false;
    const sa = slot.color ? slot.color.a : 1;
    const aa = at.color ? at.color.a : 1;
    return sa * aa > 0.01;
}

// Same fit as render_layers.mjs: only currently visible attachments define the scene frame.
applyPose(defaultAnim, 0);
let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
{
    const verts = new Float32Array(4096);
    for (const s of skel.slots) {
        const at = s.getAttachment();
        if (!slotVisible(s, at)) continue;
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
}

const W = 480, H = 800;
const CW = maxX - minX, CH = maxY - minY;
const SCALE = Math.min(W * 0.95 / CH, H * 0.95 / CW);
const OFF_X = W / 2 + (minY + maxY) / 2 * SCALE;
const OFF_Y = H / 2 - (minX + maxX) / 2 * SCALE;

function worldToScene(wx, wy) {
    const bx = OFF_X - wy * SCALE;
    const by = OFF_Y + wx * SCALE;
    return [Math.round((W - 1 - bx) * 10) / 10, Math.round((H - 1 - by) * 10) / 10];
}

function attachmentKey(slot, at) {
    return `${slot.data.name}__${at.name || slot.data.name}`;
}

function isDrawableAttachment(at) {
    return at && at.region && (at.constructor.name === 'MeshAttachment' || at.constructor.name === 'RegionAttachment');
}

function attachmentSpec(slot, at) {
    const pg = at.region && at.region.page && at.region.page.name ? at.region.page.name : pages[0].name;
    const sz = pageSizes.get(pg) || pageSizes.values().next().value || { w: 1, h: 1 };
    const isRegion = at.constructor.name === 'RegionAttachment';
    return {
        slot: slot.data.name,
        attachment: at.name || slot.data.name,
        type: isRegion ? 'region' : 'mesh',
        texture: pg,
        blend: slot.data.blendMode === 1 ? 'additive' : 'normal',
        alpha: Math.round(((slot.color ? slot.color.a : 1) * (at.color ? at.color.a : 1)) * 1000) / 1000,
        triangles: isRegion ? [0, 1, 2, 2, 3, 0] : Array.from(at.triangles),
        uvs: Array.from(at.uvs).map((v, i) => Math.round(v * (i % 2 ? sz.h : sz.w) * 10) / 10),
    };
}

function collectFrame(anim, time) {
    applyPose(anim, time);
    const verts = new Float32Array(4096);
    const draw = [];
    for (const slot of skel.drawOrder) {
        const at = slot.getAttachment();
        if (!slotVisible(slot, at) || !isDrawableAttachment(at)) continue;
        const key = attachmentKey(slot, at);
        const worldLen = at.constructor.name === 'RegionAttachment' ? 8 : at.worldVerticesLength;
        if (at.constructor.name === 'RegionAttachment') {
            at.computeWorldVertices(skel.bones[slot.data.boneData.index], verts, 0, 2);
        } else {
            at.computeWorldVertices(slot, 0, worldLen, verts, 0, 2);
        }
        const out = [];
        for (let i = 0; i < worldLen; i += 2) {
            const [x, y] = worldToScene(verts[i], verts[i + 1]);
            out.push(x, y);
        }
        draw.push({ key, vertices: out });
        if (!mesh.attachments[key]) mesh.attachments[key] = attachmentSpec(slot, at);
    }
    return { draw };
}

const mesh = {
    version: 2,
    canvas: { w: W, h: H, landscape: true },
    textures: pages.map(p => ({ id: p.name, file: p.name, ...(pageSizes.get(p.name) || {}) })),
    attachments: {},
    base: { draw: [] },
    animations: {},
};

mesh.base = collectFrame(defaultAnim, 0);
for (const anim of data.animations) {
    if (anim.name === 'Default') continue;
    const n = Math.max(8, Math.min(80, Math.round(anim.duration * FPS)));
    const frames = [];
    for (let f = 0; f < n; f++)
        frames.push(collectFrame(anim, anim.duration * f / n));
    mesh.animations[anim.name] = {
        duration: Math.round(anim.duration * 1000) / 1000,
        loop: /Idle|Wait|Default/.test(anim.name),
        frames,
    };
}

fs.writeFileSync(path.join(outDir, 'mesh.json'), JSON.stringify(mesh));
console.log(`mesh.json v2: ${Object.keys(mesh.attachments).length} attachments, ${Object.keys(mesh.animations).length} animations → ${outDir}`);
