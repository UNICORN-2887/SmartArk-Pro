// Sleep mesh patch splitter for PPD.
// Usage: node tools/sleep_patch_split.mjs <spine_dir> <ppd_form_dir> [max_layers]
//
// Reads official Spine vertices and current PPD layers. Layers whose Sleep pose cannot
// be represented well by one affine transform are split into a few small PPD sprites.
// It only edits Sleep: normal actions keep the original conversion.
import fs from 'fs';
import path from 'path';

const spineDir = process.argv[2];
const ppdDir = process.argv[3];
const maxLayers = parseInt(process.argv[4] || '10', 10);
if (!spineDir || !ppdDir) {
    console.error('Usage: node tools/sleep_patch_split.mjs <spine_dir> <ppd_form_dir> [max_layers]');
    process.exit(2);
}

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const files = fs.readdirSync(spineDir);
const atlasFile = files.find(f => f.endsWith('.atlas'));
const skelFile = files.find(f => f.endsWith('.skel'));
const pngFiles = files.filter(f => f.endsWith('.png')).sort();
const atlasText = fs.readFileSync(path.join(spineDir, atlasFile), 'utf8');

function pngSize(file) {
    const b = fs.readFileSync(file);
    if (b.length >= 24 && b.toString('ascii', 1, 4) === 'PNG')
        return { w: b.readUInt32BE(16), h: b.readUInt32BE(20) };
    return { w: 1024, h: 1024 };
}

let atlasFinal = atlasText;
const pages = [];
{
    const re = /(?:^|\n)([^\n:]+\.png)\nsize:\s*(\d+)\s*,\s*(\d+)/g;
    let m;
    while ((m = re.exec(atlasText))) pages.push({ name: m[1], w: +m[2], h: +m[3] });
}
if (!pages.length) pages.push({ name: pngFiles[0], w: 0, h: 0 });
const pageSizes = new Map();
for (const pg of pages) {
    const sz = pngSize(path.join(spineDir, pg.name));
    pageSizes.set(pg.name, sz);
    if (pg.w && (sz.w < pg.w || sz.h < pg.h)) {
        const sx = sz.w / pg.w, sy = sz.h / pg.h;
        if (Math.abs(sx - 1) > 0.01 || Math.abs(sy - 1) > 0.01) {
            atlasFinal = atlasFinal.replace(
                /(xy|size|orig|offset):\s*(\d+),\s*(\d+)/g,
                (m, k, a, b) => `${k}: ${Math.max(1, Math.round(a * sx))}, ${Math.max(1, Math.round(b * sy))}`);
        }
    }
}
const firstPage = pageSizes.values().next().value || { w: 1024, h: 1024 };
const atlas = new spine.TextureAtlas(atlasFinal, p => {
    const sz = pageSizes.get(p) || firstPage;
    return new spine.Texture({ width: sz.w, height: sz.h }, sz.w, sz.h);
});
const data = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas))
    .readSkeletonData(new Uint8Array(fs.readFileSync(path.join(spineDir, skelFile))));
const skel = new spine.Skeleton(data);
const skin0 = data.defaultSkin || data.skins[0];
const defaultAnim = data.animations.find(a => a.name === 'Default') || null;
const sleepAnim = data.animations.find(a => a.name === 'Sleep') || null;
if (!sleepAnim) {
    console.error('No Sleep animation in skeleton');
    process.exit(1);
}

function pickVisibilityPoseAnim() {
    if (defaultAnim && defaultAnim.duration > 0) return defaultAnim;
    for (const nm of ['Start', 'Idle', 'Wait']) {
        const a = data.animations.find(x => x.name === nm);
        if (a) return a;
    }
    return defaultAnim;
}
const visibilityPoseAnim = pickVisibilityPoseAnim();

function applyBasePose() {
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
}

function applySleepPose(t) {
    skel.setToSetupPose();
    if (visibilityPoseAnim) visibilityPoseAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    sleepAnim.apply(skel, 0, t, false, null, 1, 0, 0);
    skel.updateWorldTransform();
}

function isSlotVisible(slot, at) {
    if (!at || !at.region) return false;
    const slotAlpha = slot.color ? slot.color.a : 1;
    const attAlpha = at.color ? at.color.a : 1;
    return slotAlpha * attAlpha > 0.01;
}

function worldPoints(at, slot) {
    const out = [];
    const verts = new Float32Array(Math.max(2048, at.worldVerticesLength || 8));
    if (at.constructor.name === 'RegionAttachment') {
        at.computeWorldVertices(skel.bones[slot.data.boneData.index], verts, 0, 2);
        for (let i = 0; i < 8; i += 2) out.push([verts[i], verts[i + 1]]);
    } else {
        at.computeWorldVertices(slot, 0, at.worldVerticesLength, verts, 0, 2);
        for (let i = 0; i < at.worldVerticesLength; i += 2) out.push([verts[i], verts[i + 1]]);
    }
    return out;
}

const visibleDefaultAttachments = new Map();
skel.setToSetupPose();
if (visibilityPoseAnim) visibilityPoseAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
skel.updateWorldTransform();
for (const s of skel.slots) {
    const at = s.getAttachment();
    if (isSlotVisible(s, at)) visibleDefaultAttachments.set(s.data.index, at.name || '');
}

function sceneFit() {
    applyBasePose();
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const s of skel.slots) {
        const nm = visibleDefaultAttachments.get(s.data.index);
        if (nm === undefined) continue;
        const map = skin0.attachments[s.data.index];
        let at = s.getAttachment();
        if (!at || at.name !== nm) at = map && map[nm];
        if (!at || !at.region) continue;
        s.setAttachment(at);
        for (const [x, y] of worldPoints(at, s)) {
            minX = Math.min(minX, x); maxX = Math.max(maxX, x);
            minY = Math.min(minY, y); maxY = Math.max(maxY, y);
        }
    }
    return {
        scale: Math.min(480 * 0.95 / (maxY - minY), 800 * 0.95 / (maxX - minX)),
        avgX: (minX + maxX) / 2,
        avgY: (minY + maxY) / 2,
    };
}
const fit = sceneFit();
function worldToScene(wx, wy) {
    return { x: 240 + (wy - fit.avgY) * fit.scale, y: 400 - (wx - fit.avgX) * fit.scale };
}

function fitAffineScene(bp, cp) {
    if (!bp || !cp || bp.length !== cp.length || bp.length < 3) return null;
    let bx = 0, by = 0, cx = 0, cy = 0;
    for (let i = 0; i < bp.length; i++) {
        bx += bp[i].x; by += bp[i].y; cx += cp[i].x; cy += cp[i].y;
    }
    bx /= bp.length; by /= bp.length; cx /= cp.length; cy /= cp.length;
    let q00 = 0, q01 = 0, q11 = 0, p00 = 0, p01 = 0, p10 = 0, p11 = 0;
    for (let i = 0; i < bp.length; i++) {
        const x = bp[i].x - bx, y = bp[i].y - by;
        const u = cp[i].x - cx, v = cp[i].y - cy;
        q00 += x * x; q01 += x * y; q11 += y * y;
        p00 += u * x; p01 += u * y; p10 += v * x; p11 += v * y;
    }
    const det = q00 * q11 - q01 * q01;
    if (Math.abs(det) < 1e-6) return null;
    const i00 = q11 / det, i01 = -q01 / det, i11 = q00 / det;
    return {
        bx, by, cx, cy,
        a: p00 * i00 + p01 * i01,
        c: p00 * i01 + p01 * i11,
        b: p10 * i00 + p11 * i01,
        d: p10 * i01 + p11 * i11,
    };
}

function applyAffine(m, p) {
    return {
        x: m.cx + m.a * (p.x - m.bx) + m.c * (p.y - m.by),
        y: m.cy + m.b * (p.x - m.bx) + m.d * (p.y - m.by),
    };
}

function matrixSane(m) {
    if (!m) return false;
    const sx = Math.hypot(m.a, m.b);
    const sy = Math.hypot(m.c, m.d);
    const det = Math.abs(m.a * m.d - m.b * m.c);
    const dot = Math.abs(m.a * m.c + m.b * m.d);
    return sx >= 0.25 && sx <= 1.8 && sy >= 0.25 && sy <= 1.8 && det >= 0.08 && det <= 2.8 && dot <= sx * sy * 0.85;
}

function rmsResidual(m, bp, cp) {
    if (!m) return Infinity;
    let sum = 0;
    for (let i = 0; i < bp.length; i++) {
        const q = applyAffine(m, bp[i]);
        const dx = q.x - cp[i].x, dy = q.y - cp[i].y;
        sum += dx * dx + dy * dy;
    }
    return Math.sqrt(sum / bp.length);
}

function readRaw(file) {
    const b = fs.readFileSync(file);
    const w = b.readUInt16LE(0), h = b.readUInt16LE(2);
    if (w * h * 4 + 4 !== b.length) throw new Error(`bad raw ${file}`);
    return { w, h, data: b.subarray(4) };
}

function writeRaw(file, w, h, rgba) {
    const b = Buffer.alloc(4 + w * h * 4);
    b.writeUInt16LE(w, 0);
    b.writeUInt16LE(h, 2);
    rgba.copy(b, 4);
    fs.writeFileSync(file, b);
}

function cropRaw(raw, x0, y0, w, h) {
    let minX = w, minY = h, maxX = -1, maxY = -1;
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const a = raw.data[((y0 + y) * raw.w + (x0 + x)) * 4 + 3];
            if (a) {
                minX = Math.min(minX, x); minY = Math.min(minY, y);
                maxX = Math.max(maxX, x); maxY = Math.max(maxY, y);
            }
        }
    }
    if (maxX < minX) return null;
    const cw = maxX - minX + 1, ch = maxY - minY + 1;
    const out = Buffer.alloc(cw * ch * 4);
    for (let y = 0; y < ch; y++) {
        const src = ((y0 + minY + y) * raw.w + (x0 + minX)) * 4;
        raw.data.copy(out, y * cw * 4, src, src + cw * 4);
    }
    return { x: x0 + minX, y: y0 + minY, w: cw, h: ch, rgba: out };
}

function matrixKeyForPatch(m, layer, crop) {
    const pivot = { x: layer.x + crop.x + crop.w / 2, y: layer.y + crop.y + crop.h / 2 };
    const target = applyAffine(m, pivot);
    return [
        Math.round(target.x - pivot.x),
        Math.round(target.y - pivot.y),
        0,
        1,
        1,
        1,
        Math.round(m.a * 1000) / 1000,
        Math.round(m.b * 1000) / 1000,
        Math.round(m.c * 1000) / 1000,
        Math.round(m.d * 1000) / 1000,
    ];
}

function zeroLike(k) {
    if (k && k.length >= 10) return [k[0], k[1], k[2], 0, k[4], k[5], k[6], k[7], k[8], k[9]];
    if (k && k.length >= 6) return [k[0], k[1], k[2], 0, k[4], k[5]];
    return k ? [k[0], k[1], k[2], 0] : [0, 0, 0, 0];
}

function normDeg(v) {
    v %= 360;
    if (v > 180) v -= 360;
    if (v < -180) v += 360;
    return v;
}

function sampledKey(track, i, fallbackVis) {
    const k = track && track[i] ? track[i] : [0, 0, 0, fallbackVis ? 1 : 0];
    const out = {
        dx: k[0] || 0,
        dy: k[1] || 0,
        rot: k[2] || 0,
        vis: k[3] ?? (fallbackVis ? 1 : 0),
        sx: k.length >= 6 ? (k[4] ?? 1) : 1,
        sy: k.length >= 6 ? (k[5] ?? 1) : 1,
        affine: k.length >= 10,
    };
    if (out.affine) {
        out.a = k[6]; out.b = k[7]; out.c = k[8]; out.d = k[9];
    }
    return out;
}

function keyMatrix(k) {
    if (k.affine) return { a: k.a, b: k.b, c: k.c, d: k.d };
    const th = k.rot * Math.PI / 180;
    const cos = Math.cos(th), sin = Math.sin(th);
    return { a: cos * k.sx, b: sin * k.sx, c: -sin * k.sy, d: cos * k.sy };
}

function layerPivot(layer) {
    return { x: layer.x + (layer.bone_px ?? layer.w / 2), y: layer.y + (layer.bone_py ?? layer.h / 2) };
}

function firstVisibleRot(track, fallbackVis) {
    if (!track) return 0;
    for (const k of track) {
        const s = sampledKey([k], 0, fallbackVis);
        if (s.vis >= 0.5) return s.rot;
    }
    return 0;
}

function syncTailEndLayer(sleep, scene, parentName, childName, frameCount, keepChildVisibility, opt = {}) {
    const parent = scene.layers.find(l => l.name === parentName);
    const child = scene.layers.find(l => l.name === childName);
    if (!parent || !child) return false;
    const parentTrack = sleep.layers[parentName];
    const childTrack = sleep.layers[childName];
    if (!parentTrack) return false;
    const parentPivot = layerPivot(parent);
    const childPivot = layerPivot(child);
    const relRot = normDeg(firstVisibleRot(childTrack, child.visible) - firstVisibleRot(parentTrack, parent.visible));
    const next = [];
    for (let i = 0; i < frameCount; i++) {
        const pk = sampledKey(parentTrack, Math.min(i, parentTrack.length - 1), parent.visible);
        const ck = sampledKey(childTrack, Math.min(i, (childTrack || []).length - 1), child.visible);
        const m = keyMatrix(pk);
        const rx = childPivot.x - parentPivot.x;
        const ry = childPivot.y - parentPivot.y;
        const targetX = parentPivot.x + pk.dx + m.a * rx + m.c * ry;
        const targetY = parentPivot.y + pk.dy + m.b * rx + m.d * ry;
        const vis = keepChildVisibility ? ck.vis : pk.vis;
        next.push([
            Math.round(targetX - childPivot.x + (opt.dx || 0)),
            Math.round(targetY - childPivot.y + (opt.dy || 0)),
            normDeg(pk.rot + relRot + (opt.rot || 0)),
            vis >= 0.5 ? 1 : 0,
            pk.sx,
            pk.sy,
        ]);
    }
    sleep.layers[childName] = next;
    return true;
}

const scenePath = path.join(ppdDir, 'scene.json');
const animsPath = path.join(ppdDir, 'anims.json');
const scene = JSON.parse(fs.readFileSync(scenePath, 'utf8'));
const anims = JSON.parse(fs.readFileSync(animsPath, 'utf8'));
const sleep = anims.Sleep;
if (!sleep || !sleep.layers) {
    console.error('No PPD Sleep animation');
    process.exit(1);
}
scene.layers = scene.layers.filter(l => !l.name.includes('__sleep_patch_'));
for (const a of Object.values(anims)) {
    if (!a || !a.layers) continue;
    for (const name of Object.keys(a.layers)) {
        if (name.includes('__sleep_patch_')) delete a.layers[name];
    }
}

const frameCount = Math.max(8, Math.min(80, Math.round(sleep.duration * 20)));
const slotByName = new Map(data.slots.map((s, i) => [s.name, i]));
const candidates = [];

applyBasePose();
for (const layer of scene.layers) {
    if (layer.name.includes('__') || !slotByName.has(layer.name)) continue;
    if (/Tail|Weapon|Ring/i.test(layer.name)) continue;
    const rawFile = path.join(ppdDir, `${layer.name}.raw`);
    if (!fs.existsSync(rawFile) || layer.w < 12 || layer.h < 12) continue;
    const si = slotByName.get(layer.name);
    const slot = skel.slots[si];
    const map = skin0.attachments[si];
    const visibleName = visibleDefaultAttachments.get(si);
    let at = visibleName !== undefined ? map && map[visibleName] : slot.getAttachment();
    if (!at || !at.region) {
        const first = map && Object.keys(map).find(k => map[k] && map[k].region);
        at = first ? map[first] : null;
    }
    if (!at || !at.region || at.constructor.name === 'RegionAttachment') continue;
    slot.setAttachment(at);
    skel.updateWorldTransform();
    const bpWorld = worldPoints(at, slot);
    const bp = bpWorld.map(p => worldToScene(p[0], p[1]));
    if (bp.length < 8) continue;

    applySleepPose(sleepAnim.duration * 0.5);
    const curAt = skel.slots[si].getAttachment();
    if (!isSlotVisible(skel.slots[si], curAt) || !curAt || !curAt.region) continue;
    const cp = worldPoints(curAt, skel.slots[si]).map(p => worldToScene(p[0], p[1]));
    if (cp.length !== bp.length) continue;
    const m = fitAffineScene(bp, cp);
    const err = rmsResidual(m, bp, cp);
    const area = layer.w * layer.h;
    if (err >= 7 && area >= 700)
        candidates.push({ layer, si, atName: at.name || '', bp, err, area });
    applyBasePose();
}

candidates.sort((a, b) => (b.err * Math.sqrt(b.area)) - (a.err * Math.sqrt(a.area)));
const selected = candidates.slice(0, maxLayers);
let patchCount = 0;
let rawBytes = 0;

for (const item of selected) {
    const { layer, si, bp } = item;
    const raw = readRaw(path.join(ppdDir, `${layer.name}.raw`));
    const horizontal = layer.w >= layer.h * 1.45;
    const vertical = layer.h >= layer.w * 1.45;
    const cols = horizontal ? 4 : (vertical ? 1 : 2);
    const rows = vertical ? 4 : (horizontal ? 1 : 2);
    const patchInfos = [];
    for (let gy = 0; gy < rows; gy++) {
        for (let gx = 0; gx < cols; gx++) {
            const x0 = Math.floor(gx * raw.w / cols);
            const x1 = Math.floor((gx + 1) * raw.w / cols);
            const y0 = Math.floor(gy * raw.h / rows);
            const y1 = Math.floor((gy + 1) * raw.h / rows);
            const crop = cropRaw(raw, x0, y0, x1 - x0, y1 - y0);
            if (!crop || crop.w < 2 || crop.h < 2) continue;
            const cx = layer.x + crop.x + crop.w / 2;
            const cy = layer.y + crop.y + crop.h / 2;
            const local = bp
                .map((p, idx) => ({ p, idx, d: (p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy) }))
                .sort((a, b) => a.d - b.d)
                .slice(0, Math.min(12, Math.max(4, Math.ceil(bp.length / (cols * rows)))));
            patchInfos.push({ crop, indices: local.map(x => x.idx) });
        }
    }
    if (!patchInfos.length) continue;

    const oldTrack = sleep.layers[layer.name] || Array.from({ length: frameCount }, () => [0, 0, 0, layer.visible ? 1 : 0]);
    sleep.layers[layer.name] = oldTrack.map(zeroLike);
    for (let pi = 0; pi < patchInfos.length; pi++) {
        const { crop, indices } = patchInfos[pi];
        const name = `${layer.name}__sleep_patch_${pi}`;
        writeRaw(path.join(ppdDir, `${name}.raw`), crop.w, crop.h, crop.rgba);
        rawBytes += 4 + crop.w * crop.h * 4;
        const patchLayer = {
            ...layer,
            name,
            x: layer.x + crop.x,
            y: layer.y + crop.y,
            w: crop.w,
            h: crop.h,
            cx: Math.round(layer.x + crop.x + crop.w / 2),
            cy: Math.round(layer.y + crop.y + crop.h / 2),
            bone_px: crop.w / 2,
            bone_py: crop.h / 2,
            bbox: [layer.x + crop.x, layer.y + crop.y, layer.x + crop.x + crop.w - 1, layer.y + crop.y + crop.h - 1],
            visible: false,
            z: layer.z + 0.001 * (pi + 1),
        };
        scene.layers.push(patchLayer);
        const track = [];
        for (let f = 0; f < frameCount; f++) {
            applySleepPose(sleepAnim.duration * f / frameCount);
            const slot = skel.slots[si];
            const curAt = slot.getAttachment();
            if (!isSlotVisible(slot, curAt) || !curAt || !curAt.region) {
                track.push([0, 0, 0, 0]);
                continue;
            }
            const cp = worldPoints(curAt, slot).map(p => worldToScene(p[0], p[1]));
            if (cp.length !== bp.length) {
                track.push([0, 0, 0, 0]);
                continue;
            }
            const fullM = fitAffineScene(bp, cp);
            const subBp = indices.map(i => bp[i]);
            const subCp = indices.map(i => cp[i]);
            const localM = fitAffineScene(subBp, subCp);
            const m = matrixSane(localM) ? localM : fullM;
            track.push(m ? matrixKeyForPatch(m, layer, crop) : [0, 0, 0, 0]);
        }
        sleep.layers[name] = track;
        patchCount++;
    }
}

const sortedNames = [...scene.layers].sort((a, b) => a.z - b.z).map(l => l.name);
const backTails = sortedNames.filter(name => /^C_Tail/i.test(name));
const syncedTailEnds = [];
if (sleep.layers.C_Tail_a && scene.layers.some(l => l.name === 'C_Tail_c')) {
    if (syncTailEndLayer(sleep, scene, 'C_Tail_a', 'C_Tail_c', frameCount, false, { dx: 12, dy: 63, rot: 90 }))
        syncedTailEnds.push('C_Tail_c<-C_Tail_a');
}
if (sleep.layers.C_Tail_01 && scene.layers.some(l => l.name === 'C_Tail_02')) {
    if (syncTailEndLayer(sleep, scene, 'C_Tail_01', 'C_Tail_02', frameCount, false))
        syncedTailEnds.push('C_Tail_02<-C_Tail_01');
}
for (const layer of scene.layers) {
    if (!/Tail/i.test(layer.name) || !/Fire/i.test(layer.name)) continue;
    const baseName = layer.name.includes('__') ? layer.name.split('__')[0] : '';
    const parentName = scene.layers.some(l => l.name === baseName) ? baseName : sortedNames.find(n => /Tail/i.test(n) && !/Fire/i.test(n));
    if (parentName && syncTailEndLayer(sleep, scene, parentName, layer.name, frameCount, true))
        syncedTailEnds.push(`${layer.name}<-${parentName}`);
}
if (backTails.length) {
    const orderedBackTails = [];
    for (const pair of [['C_Tail_a', 'C_Tail_c'], ['C_Tail_01', 'C_Tail_02']]) {
        for (const name of pair) {
            if (backTails.includes(name) && !orderedBackTails.includes(name))
                orderedBackTails.push(name);
        }
    }
    for (const name of backTails) {
        if (!orderedBackTails.includes(name)) orderedBackTails.push(name);
    }
    const rest = sortedNames.filter(name => !orderedBackTails.includes(name));
    sleep.order = Array.from({ length: frameCount }, () => [...orderedBackTails, ...rest]);
}

fs.writeFileSync(scenePath, JSON.stringify(scene, null, 1));
fs.writeFileSync(animsPath, JSON.stringify(anims));
console.log(`Sleep patch split: selected ${selected.length}/${candidates.length} layers, patches ${patchCount}, extra raw ${(rawBytes / 1024).toFixed(1)}KB`);
if (syncedTailEnds.length) console.log(`  tail-end sync: ${syncedTailEnds.join(', ')}`);
for (const x of selected)
    console.log(`  ${x.layer.name}: residual=${x.err.toFixed(1)}px size=${x.layer.w}x${x.layer.h}`);
