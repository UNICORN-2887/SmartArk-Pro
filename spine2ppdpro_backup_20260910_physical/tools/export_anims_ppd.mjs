// Spine 动画 → PPD 原生动画（anims.json）：每个动画逐帧采样，导出每个图层的
// 旋转/位移增量 + 可见性时间轴，设备端纸偶渲染器逐层插值应用——
// 角色图层自然切换动作（含换链：隐藏槽激活/显式槽隐藏），非 MJPEG 录像
// 用法：node export_anims_ppd.mjs <spine_dir> <out_dir> [fps]
// 输出：<out_dir>/anims.json
//
// 格式：{ "<动画名>": {"duration": s, "loop": bool,
//                      "layers": {"<槽名>": [[dx,dy,drot,vis,sx,sy], ...每帧...]}}}
// 旋转表达：scene.json 写入官方槽骨骼点 bone_px/bone_py，
// 设备端绕该点旋转/缩放；delta = 槽骨骼点位移。
// 基准：显式层 Default 姿势 / 隐藏层 setup 姿势（与场景导出基准一致）；
// 世界→场景 R270°·S（与 render_layers --landscape 一致）；vis=1/0 附件可见性。
import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const outDir = process.argv[3];
const FPS = parseInt(process.argv[4] || '20', 10);
const SRC_KEY = `${dir}/${outDir}`.replace(/\\/g, '/');

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const files = fs.readdirSync(dir);
const atlasFile = files.find(f => f.endsWith('.atlas'));
const atlasText = fs.readFileSync(path.join(dir, atlasFile), 'utf8');
const pngFiles = files.filter(f => f.endsWith('.png')).sort();
const pages = [];
{
    const re = /(?:^|\n)([^\n:]+\.png)\nsize:\s*(\d+)\s*,\s*(\d+)/g;
    let m;
    while ((m = re.exec(atlasText))) pages.push({ name: m[1], w: +m[2], h: +m[3] });
}
if (!pages.length) pages.push({ name: pngFiles[0], w: 0, h: 0 });

function pngSize(file) {
    const b = fs.readFileSync(file);
    if (b.length >= 24 && b.toString('ascii', 1, 4) === 'PNG')
        return { w: b.readUInt32BE(16), h: b.readUInt32BE(20) };
    return { w: 1024, h: 1024 };
}

let atlasFinal = atlasText;
const pageSizes = new Map();
for (const pg of pages) {
    const sz = pngSize(path.join(dir, pg.name));
    pageSizes.set(pg.name, sz);
    if (pg.w && (sz.w < pg.w || sz.h < pg.h)) {
        const sx = sz.w / pg.w, sy = sz.h / pg.h;
        if (Math.abs(sx - 1) > 0.01 || Math.abs(sy - 1) > 0.01) {
            atlasFinal = atlasFinal.replace(
                /(xy|size|orig|offset):\s*(\d+),\s*(\d+)/g,
                (m, k, a, b) => `${k}: ${Math.max(1, Math.round(a * sx))}, ${Math.max(1, Math.round(b * sy))}`);
            console.log(`图集页 ${pg.name}: ${sz.w}×${sz.h} vs 声明 ${pg.w}×${pg.h} → atlas 坐标缩放 ${sx.toFixed(3)}/${sy.toFixed(3)}`);
        }
    }
}
const firstPage = pageSizes.values().next().value || { w: 1024, h: 1024 };
const atlas = new spine.TextureAtlas(atlasFinal, p => {
    const sz = pageSizes.get(p) || firstPage;
    return new spine.Texture({ width: sz.w, height: sz.h }, sz.w, sz.h);
});
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, files.find(f => f.endsWith('.skel'))))));
const skel = new spine.Skeleton(data);
const skin0 = data.defaultSkin || data.skins[0];
const defaultAnim = data.animations.find(a => a.name === 'Default');
function pickVisibilityPoseAnim() {
    if (defaultAnim && defaultAnim.duration > 0)
        return defaultAnim;
    for (const nm of ['Start', 'Idle', 'Wait']) {
        const a = data.animations.find(x => x.name === nm);
        if (a) return a;
    }
    return defaultAnim || null;
}
const geometryPoseAnim = defaultAnim || null;
const visibilityPoseAnim = pickVisibilityPoseAnim();
function applyPose(anim) {
    skel.setToSetupPose();
    if (anim) anim.apply(skel, 0, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
}
function applyGeometryPose() {
    applyPose(geometryPoseAnim);
}
function applyVisibilityPose() {
    applyPose(visibilityPoseAnim);
}
console.log(`base pose: geometry=${geometryPoseAnim ? geometryPoseAnim.name : 'setup'} visibility=${visibilityPoseAnim ? visibilityPoseAnim.name : 'setup'}`);

const visibleDefaultAttachments = new Map();
applyVisibilityPose();
for (const s of skel.slots) {
    const at = s.getAttachment();
    if (isSlotVisible(s, at))
        visibleDefaultAttachments.set(s.data.index, at.name || '');
}

// 场景层几何/可见性（scene.json——设备端层 bbox 中心/质心/visible）。
// 提前读取：槽的 hidden 判定要用场景导出时的实际可见性
// （Default 姿势渲染为空、回退 setup 导出的槽在场景里 visible=false）
const sceneJson = JSON.parse(fs.readFileSync(path.join(outDir, 'scene.json'), 'utf8'));
const layerGeom = {};
for (const l of sceneJson.layers)
    layerGeom[l.name] = { boxcx: (l.bbox[0] + l.bbox[2]) / 2, boxcy: (l.bbox[1] + l.bbox[3]) / 2,
                          cx: l.cx, cy: l.cy, w: l.w, h: l.h, visible: l.visible };

// 全部槽（Default 姿势下附件 null 或场景层不可见的为隐藏槽——换链/被 Default
// 动画移出屏幕的姿势链，基准用 setup 姿势，动作时由可见性轨道激活）
const allSlots = [];
const defaultAttName = new Map();   // slotIndex -> Default 姿势附件名（换附件检测基准）
{
    applyGeometryPose();
    for (const s of skel.slots) {
        const visibleName = visibleDefaultAttachments.get(s.data.index);
        let at = s.getAttachment();
        if (visibleName !== undefined && (!at || at.name !== visibleName)) {
            const map = skin0.attachments[s.data.index];
            at = map && map[visibleName];
        }
        const sg = layerGeom[s.data.name];
        const sceneHidden = sg ? sg.visible === false : false;
        if (visibleName !== undefined && at && at.region && !sceneHidden) {
            allSlots.push({ si: s.data.index, name: s.data.name, hidden: false, att: at });
            defaultAttName.set(s.data.index, at.name || '');
            continue;
        }
        const map = skin0.attachments[s.data.index];
        if (map && Object.keys(map).length > 0) {
            const nm = Object.keys(map).find(k => map[k] && map[k].region);
            if (nm) allSlots.push({ si: s.data.index, name: s.data.name, hidden: true, att: map[nm] });
        }
    }
}
console.log(`槽 ${allSlots.length} 个（隐藏 ${allSlots.filter(s => s.hidden).length}）`);

// 附件变体集合（与 render_layers.mjs 同逻辑）：动画 attachment timeline 引用的
// 非 Default 附件（Start 的 F_Eye_3 闭眼线等）→ 场景里是 hidden 变体层，
// 此处生成 vis 轨道（切换区间=1）+ 几何轨道（相对 Default 姿势下该附件的基准）
function variantLayerName(slotIndex, attName) {
    return `${data.slots[slotIndex].name}__${attName}_v`;
}
const variants = new Map();   // layerName -> {slotIndex, attName, att}
for (const a of data.animations) {
    for (const tl of a.timelines) {
        if (tl.constructor.name !== 'AttachmentTimeline') continue;
        const dn = defaultAttName.get(tl.slotIndex) || '';
        for (const nm of tl.attachmentNames) {
            if (!nm || nm === dn) continue;
            const map = skin0.attachments[tl.slotIndex];
            const layerName = variantLayerName(tl.slotIndex, nm);
            if (map && map[nm] && map[nm].region && !variants.has(layerName))
                variants.set(layerName, { slotIndex: tl.slotIndex, attName: nm, att: map[nm] });
        }
    }
}
// 变体附件基准几何（Default 姿势挂上变体附件；与 render_layers 变体层渲染姿势一致）
const variantBase = {};
for (const [layerName, v] of variants) {
    skel.setToSetupPose();
    if (geometryPoseAnim) geometryPoseAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    for (const s of skel.slots) s.setAttachment(null);
    const slot = skel.slots[v.slotIndex];
    slot.setAttachment(v.att);
    skel.updateWorldTransform();
    const at = slot.getAttachment();
    const b = skel.bones[slot.data.boneData.index];
    const verts = new Float32Array(2048);
    if (at.constructor.name === 'RegionAttachment') {
        at.computeWorldVertices(b, verts, 0, 2);
        const minX = Math.min(verts[0], verts[2], verts[4], verts[6]);
        const maxX = Math.max(verts[0], verts[2], verts[4], verts[6]);
        const minY = Math.min(verts[1], verts[3], verts[5], verts[7]);
        const maxY = Math.max(verts[1], verts[3], verts[5], verts[7]);
        variantBase[layerName] = {
            cx: (verts[0] + verts[2] + verts[4] + verts[6]) / 4,
            cy: (verts[1] + verts[3] + verts[5] + verts[7]) / 4,
            px: b.worldX, py: b.worldY,
            rot: Math.atan2(verts[3] - verts[1], verts[2] - verts[0]), usePca: false,
            boxw: maxX - minX, boxh: maxY - minY, detSign: boneDetSign(b),
        };
    } else {
        const g = meshPose(at, slot);
            variantBase[layerName] = { cx: g.cx, cy: g.cy, px: b.worldX, py: b.worldY,
                               rot: g.usePca ? g.th : -Math.atan2(b.b, b.a), usePca: g.usePca,
                               pcaRot: g.th, boneRot: -Math.atan2(b.b, b.a),
                               boxw: g.boxw, boxh: g.boxh, detSign: boneDetSign(b) };
    }
}
if (variants.size) console.log(`附件变体 ${variants.size} 个（${[...variants.keys()].join(', ')}）`);

const sceneZOrder = [...sceneJson.layers].sort((a, b) => a.z - b.z).map(l => l.name);
const sceneLayerNames = new Set(sceneZOrder);
const slotNameSet = new Set(data.slots.map(s => s.name));
const slotToSceneLayers = new Map();
for (const l of sceneJson.layers) {
    let slotName = slotNameSet.has(l.name) ? l.name : null;
    if (!slotName && (l.name === 'F_Eye_L' || l.name === 'F_Eye_R') && slotNameSet.has('F_Eye'))
        slotName = 'F_Eye';
    if (!slotName) continue;
    const arr = slotToSceneLayers.get(slotName) || [];
    arr.push(l.name);
    slotToSceneLayers.set(slotName, arr);
}
for (const [layerName, v] of variants) {
    if (!sceneLayerNames.has(layerName)) continue;
    const slotName = data.slots[v.slotIndex].name;
    const arr = slotToSceneLayers.get(slotName) || [];
    arr.push(layerName);
    slotToSceneLayers.set(slotName, arr);
}
for (const arr of slotToSceneLayers.values())
    arr.sort((a, b) => sceneZOrder.indexOf(a) - sceneZOrder.indexOf(b));

function currentDrawOrderNames() {
    const out = [], added = new Set();
    for (const s of skel.drawOrder) {
        const arr = slotToSceneLayers.get(s.data.name);
        if (!arr) continue;
        for (const name of arr) {
            if (added.has(name)) continue;
            out.push(name);
            added.add(name);
        }
    }
    for (const name of sceneZOrder) {
        if (!added.has(name)) out.push(name);
    }
    return out;
}

function sameOrder(a, b) {
    return a.length === b.length && a.every((v, i) => v === b[i]);
}

function preferBoneRotation(name) {
    if (/(^|_)B_Hair$/.test(name)) return false;
    return /Hair|Ear|Head|Eye|Brow|Face|Tail|Weapon|Shield|Dun|Dragon|Arm|Forearm|Hand|Leg|Calf|Foot/.test(name);
}

function forcePcaRotation(name) {
    return /(^|_)B_Hair$/.test(name);
}

function useMeshShapePosition(name) {
    return /Hair|Ear|Tail|Fire/.test(name);
}

function rotationBiasDeg(name) {
    return 0;
}

function positionBiasPx(name) {
    return [0, 0];
}

function boneDetSign(b) {
    return (b.a * b.d - b.b * b.c) < 0 ? -1 : 1;
}

function allowMirrorScale(name) {
    return /Weapon|Shield|Dun|Dragon/.test(name);
}

function forceMirrorScale(name) {
    return false;
}

function key4(dx, dy, rot, vis, sx = 1, sy = 1) {
    return (sx === 1 && sy === 1) ? [dx, dy, rot, vis] : [dx, dy, rot, vis, sx, sy];
}

function isSlotVisible(slot, at) {
    if (!at || !at.region) return false;
    const slotAlpha = slot.color ? slot.color.a : 1;
    const attAlpha = at.color ? at.color.a : 1;
    return slotAlpha * attAlpha > 0.01;
}

function isGeometryVisible(at, slot, verts) {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    if (at.constructor.name === 'RegionAttachment') {
        const b = skel.bones[slot.data.boneData.index];
        at.computeWorldVertices(b, verts, 0, 2);
        for (let i = 0; i < 8; i += 2) {
            minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
            minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
        }
    } else {
        at.computeWorldVertices(slot, 0, at.worldVerticesLength, verts, 0, 2);
        for (let i = 0; i < at.worldVerticesLength; i += 2) {
            minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
            minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
        }
    }
    return (maxX - minX) * SCL >= 1 && (maxY - minY) * SCL >= 1;
}

function worldPoints(at, slot, verts) {
    const points = [];
    if (at.constructor.name === 'RegionAttachment') {
        at.computeWorldVertices(skel.bones[slot.data.boneData.index], verts, 0, 2);
        for (let i = 0; i < 8; i += 2)
            points.push([verts[i], verts[i + 1]]);
    } else {
        at.computeWorldVertices(slot, 0, at.worldVerticesLength, verts, 0, 2);
        for (let i = 0; i < at.worldVerticesLength; i += 2)
            points.push([verts[i], verts[i + 1]]);
    }
    return points;
}

function pointInPolygon(poly, x, y) {
    let inside = false;
    for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
        const xi = poly[i][0], yi = poly[i][1];
        const xj = poly[j][0], yj = poly[j][1];
        if (((yi > y) !== (yj > y)) && x < (xj - xi) * (y - yi) / ((yj - yi) || 1e-9) + xi)
            inside = !inside;
    }
    return inside;
}

function bboxOf(points) {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const [x, y] of points) {
        minX = Math.min(minX, x); maxX = Math.max(maxX, x);
        minY = Math.min(minY, y); maxY = Math.max(maxY, y);
    }
    return { minX, minY, maxX, maxY };
}

function pointStats(points) {
    let cx = 0, cy = 0;
    for (const [x, y] of points) { cx += x; cy += y; }
    cx /= Math.max(1, points.length);
    cy /= Math.max(1, points.length);
    const bb = bboxOf(points);
    return { cx, cy, boxcx: (bb.minX + bb.maxX) / 2, boxcy: (bb.minY + bb.maxY) / 2,
             boxw: bb.maxX - bb.minX, boxh: bb.maxY - bb.minY };
}

function fitSimilarity(basePoints, curPoints) {
    if (!basePoints || !curPoints || basePoints.length !== curPoints.length || basePoints.length < 2)
        return null;
    const b = pointStats(basePoints);
    const c = pointStats(curPoints);
    let dot = 0, cross = 0, b2 = 0, c2 = 0;
    for (let i = 0; i < basePoints.length; i++) {
        const bx = basePoints[i][0] - b.cx, by = basePoints[i][1] - b.cy;
        const cx = curPoints[i][0] - c.cx, cy = curPoints[i][1] - c.cy;
        dot += bx * cx + by * cy;
        cross += bx * cy - by * cx;
        b2 += bx * bx + by * by;
        c2 += cx * cx + cy * cy;
    }
    if (b2 <= 1e-6 || c2 <= 1e-6) return null;
    return { rot: Math.atan2(cross, dot), scale: Math.sqrt(c2 / b2), stats: c };
}

function clipIntersects(points, poly) {
    let inside = 0;
    for (const [x, y] of points)
        if (pointInPolygon(poly, x, y)) inside++;
    return inside / Math.max(1, points.length) >= 0.6;
}

function clipWorldPolygon(clip, slot, verts) {
    clip.computeWorldVertices(slot, 0, clip.worldVerticesLength, verts, 0, 2);
    const poly = [];
    for (let i = 0; i < clip.worldVerticesLength; i += 2)
        poly.push([verts[i], verts[i + 1]]);
    return poly;
}

function clippedOutSlotIndexes(verts) {
    const out = new Set();
    let active = null;
    for (const slot of skel.drawOrder) {
        const at = slot.getAttachment();
        if (active && at && at.region && isSlotVisible(slot, at)) {
            const points = worldPoints(at, slot, verts);
            if (!clipIntersects(points, active.poly))
                out.add(slot.data.index);
        }
        if (at && at.constructor.name === 'ClippingAttachment')
            active = { poly: clipWorldPolygon(at, slot, verts), endSlot: at.endSlot };
        if (active && active.endSlot === slot.data)
            active = null;
    }
    return out;
}

// Mesh 层姿势几何：真实顶点（computeWorldVertices，含骨骼权重形变）→
// 质心 = 顶点均值；主轴角 θ = PCA 协方差主轴（无向，(-π/2, π/2]）；
// 形状接近圆形（λ2/λ1 ≥ 0.5）主轴不可信 → usePca=false 退化用骨骼角。
// 曾用 region 四角近似质心 + 骨骼矩阵角：头部上下运动（头骨旋转带动
// 刘海 mesh 顶点权重形变）时内容实际运动与骨骼刚性假设偏差大 → 头发分离。
function meshPose(at, slot) {
    const wv = new Float32Array(at.worldVerticesLength);
    at.computeWorldVertices(slot, 0, at.worldVerticesLength, wv, 0, 2);
    const n = wv.length / 2;
    let cx = 0, cy = 0;
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (let i = 0; i < n; i++) { cx += wv[i * 2]; cy += wv[i * 2 + 1]; }
    for (let i = 0; i < n; i++) {
        const x = wv[i * 2], y = wv[i * 2 + 1];
        minX = Math.min(minX, x); maxX = Math.max(maxX, x);
        minY = Math.min(minY, y); maxY = Math.max(maxY, y);
    }
    cx /= n; cy /= n;
    let sxx = 0, syy = 0, sxy = 0;
    for (let i = 0; i < n; i++) {
        const dx = wv[i * 2] - cx, dy = wv[i * 2 + 1] - cy;
        sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
    const d = Math.sqrt(Math.max(0, (sxx - syy) * (sxx - syy) / 4 + sxy * sxy));
    const l1 = (sxx + syy) / 2 + d, l2 = (sxx + syy) / 2 - d;
    return { cx, cy, boxcx: (minX + maxX) / 2, boxcy: (minY + maxY) / 2,
             boxw: maxX - minX, boxh: maxY - minY,
             th: 0.5 * Math.atan2(2 * sxy, sxx - syy),
             usePca: l1 > 1e-9 && l2 / l1 < 0.5 };
}

function scaleKey(baseGeom, boxw, boxh, name, detSign, fitScale = 0) {
    if (/Head|Face|Eye|Mouth|Brow/.test(name)) return [1, 1];
    if (!baseGeom || !baseGeom.boxw || !baseGeom.boxh || boxw <= 0 || boxh <= 0) return [1, 1];
    let s = fitScale > 0 ? fitScale : Math.sqrt(Math.max(0.0001, (boxw * boxh) / (baseGeom.boxw * baseGeom.boxh)));
    if (Math.abs(s - 1) < 0.03) s = 1;
    s = Math.max(0.05, Math.min(3.0, s));
    let sx = s, sy = s;
    if (allowMirrorScale(name) && baseGeom.detSign && detSign && baseGeom.detSign !== detSign)
        sx = -sx;
    if (forceMirrorScale(name))
        sx = -Math.abs(sx);
    return [Math.round(sx * 1000) / 1000, Math.round(sy * 1000) / 1000];
}

// 某姿势下每个槽：质心 cx,cy、骨骼点 px,py、旋转角 rot（附件 null → null）
function poseLayers(useDefault) {
    skel.setToSetupPose();
    if (useDefault && geometryPoseAnim) geometryPoseAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
    const out = {};
    const verts = new Float32Array(2048);
    for (const s of skel.slots) {
        const at = s.getAttachment();
        const b = skel.bones[s.data.boneData.index];
        if (!at || !at.region) { out[s.data.name] = null; continue; }
        if (at.constructor.name === 'RegionAttachment') {
            at.computeWorldVertices(b, verts, 0, 2);
            const points = [[verts[0], verts[1]], [verts[2], verts[3]], [verts[4], verts[5]], [verts[6], verts[7]]];
            const cx = (verts[0] + verts[2] + verts[4] + verts[6]) / 4;
            const cy = (verts[1] + verts[3] + verts[5] + verts[7]) / 4;
            const rot = Math.atan2(verts[3] - verts[1], verts[2] - verts[0]);
            let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
            for (let i = 0; i < 8; i += 2) {
                minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
                minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
            }
            out[s.data.name] = { cx, cy, px: b.worldX, py: b.worldY, rot,
                                 boxw: maxX - minX, boxh: maxY - minY,
                                 detSign: boneDetSign(b), points };
        } else {
            /* Mesh 层：真实顶点几何（形变被捕获）——质心 = 顶点均值；
               rot = PCA 主轴角（内容实际方向；接近圆形退化用骨骼角取负——
               曾 rot=0 导致"原地旋转"动作完全丢失） */
            const g = meshPose(at, s);
            out[s.data.name] = { cx: g.cx, cy: g.cy, px: b.worldX, py: b.worldY,
                                 boxcx: g.boxcx, boxcy: g.boxcy,
                                 boxw: g.boxw, boxh: g.boxh,
                                 rot: g.usePca ? g.th : -Math.atan2(b.b, b.a),
                                 usePca: g.usePca,
                                 pcaRot: g.th,
                                 boneRot: -Math.atan2(b.b, b.a),
                                 detSign: boneDetSign(b),
                                 points: worldPoints(at, s, verts) };   // 视觉骨骼角（clamp 基准）
        }
    }
    return out;
}

function poseAttachment(slotIndex, att, useDefault) {
    skel.setToSetupPose();
    if (useDefault && geometryPoseAnim) geometryPoseAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    for (const s of skel.slots) s.setAttachment(null);
    const slot = skel.slots[slotIndex];
    slot.setAttachment(att);
    skel.updateWorldTransform();
    const b = skel.bones[slot.data.boneData.index];
    const verts = new Float32Array(2048);
    if (att.constructor.name === 'RegionAttachment') {
        att.computeWorldVertices(b, verts, 0, 2);
        const points = [[verts[0], verts[1]], [verts[2], verts[3]], [verts[4], verts[5]], [verts[6], verts[7]]];
        return {
            cx: (verts[0] + verts[2] + verts[4] + verts[6]) / 4,
            cy: (verts[1] + verts[3] + verts[5] + verts[7]) / 4,
            px: b.worldX, py: b.worldY,
            rot: Math.atan2(verts[3] - verts[1], verts[2] - verts[0]),
            boxw: Math.max(verts[0], verts[2], verts[4], verts[6]) - Math.min(verts[0], verts[2], verts[4], verts[6]),
            boxh: Math.max(verts[1], verts[3], verts[5], verts[7]) - Math.min(verts[1], verts[3], verts[5], verts[7]),
            detSign: boneDetSign(b),
            points,
        };
    }
    const g = meshPose(att, slot);
    return { cx: g.cx, cy: g.cy, px: b.worldX, py: b.worldY,
             boxcx: g.boxcx, boxcy: g.boxcy,
             boxw: g.boxw, boxh: g.boxh,
             rot: g.usePca ? g.th : -Math.atan2(b.b, b.a), usePca: g.usePca,
             pcaRot: g.th, boneRot: -Math.atan2(b.b, b.a), detSign: boneDetSign(b),
             points: worldPoints(att, slot, verts) };
}

// 基准：显式层 Default 姿势、隐藏层 setup 姿势（与场景导出基准一致）
const baseDefault = poseLayers(true);
const baseSetup = poseLayers(false);
const base = {};
for (const { si, name, hidden, att } of allSlots)
    base[name] = hidden ? (baseSetup[name] || poseAttachment(si, att, false)) : baseDefault[name];

// 场景 fit 缩放与中心（与 render_layers --landscape 相同公式）：世界→场景 R270°·S
function sceneFit() {
    applyGeometryPose();
    const verts = new Float32Array(2048);
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const s of skel.slots) {
        const visibleName = visibleDefaultAttachments.get(s.data.index);
        if (visibleName === undefined) continue;
        let at = s.getAttachment();
        if (!at || at.name !== visibleName) {
            const map = skin0.attachments[s.data.index];
            at = map && map[visibleName];
        }
        if (!isSlotVisible(s, at)) continue;
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
    return {
        scale: Math.min(480 * 0.95 / (maxY - minY), 800 * 0.95 / (maxX - minX)),
        avgX: (minX + maxX) / 2,
        avgY: (minY + maxY) / 2,
    };
}
const FIT = sceneFit();
const SCL = FIT.scale;
console.log(`场景 scale=${SCL.toFixed(3)}（世界→场景 R270°·S）`);

function worldToScene(wx, wy) {
    return {
        x: 240 + (wy - FIT.avgY) * SCL,
        y: 400 - (wx - FIT.avgX) * SCL,
    };
}

function deltaAroundBonePivot(baseGeom, curCx, curCy, dth, sx, sy) {
    const pivot = worldToScene(baseGeom.px, baseGeom.py);
    const baseCenter = worldToScene(baseGeom.cx, baseGeom.cy);
    const curCenter = worldToScene(curCx, curCy);
    const vx = baseCenter.x - pivot.x;
    const vy = baseCenter.y - pivot.y;
    const cos = Math.cos(dth), sin = Math.sin(dth);
    const tx = cos * sx * vx - sin * sy * vy;
    const ty = sin * sx * vx + cos * sy * vy;
    return [Math.round(curCenter.x - pivot.x - tx), Math.round(curCenter.y - pivot.y - ty)];
}

// 设备端旋转点 = scene.json 里的官方槽骨骼点 bone_px/bone_py。
// 平移 delta 用官方当前帧顶点质心反解，长尾巴/武器/长发不会再绕图层中心甩开。
// （scene.json / layerGeom 已在文件前部读取——含 visible 字段供 hidden 判定）

const SKIP = new Set(['Begin', 'End']);
const anims = data.animations.filter(a => !SKIP.has(a.name) && a.duration > 0);

/* 从附件+骨骼+基准几何算 [dx, dy, drot]（Region 刚性公式 / mesh 顶点几何）
   baseGeom: {cx, cy, px, py, rot, usePca}；name 用于 layerGeom 与 PCA 展开键 */
function calcDelta(at, s, baseGeom, name, prevTh, verts) {
    const b = skel.bones[s.data.boneData.index];
    /* 场景变换 R270°·S 保向（无反射）→ 世界角度差即场景角度差；
       设备端绕官方槽骨骼点旋转，平移 delta 用当前顶点质心反解 */
    let dth, dx, dy, sx = 1, sy = 1;
    if (at.constructor.name === 'RegionAttachment') {
        /* 刚性层：旋转/缩放交给设备端按骨骼点做，平移用当前四角质心反解 */
        at.computeWorldVertices(b, verts, 0, 2);
        const rot = Math.atan2(verts[3] - verts[1], verts[2] - verts[0]);
        const cx = (verts[0] + verts[2] + verts[4] + verts[6]) / 4;
        const cy = (verts[1] + verts[3] + verts[5] + verts[7]) / 4;
        const boxw = Math.max(verts[0], verts[2], verts[4], verts[6]) - Math.min(verts[0], verts[2], verts[4], verts[6]);
        const boxh = Math.max(verts[1], verts[3], verts[5], verts[7]) - Math.min(verts[1], verts[3], verts[5], verts[7]);
        [sx, sy] = scaleKey(baseGeom, boxw, boxh, name, boneDetSign(b));
        dth = rot - baseGeom.rot;
        [dx, dy] = deltaAroundBonePivot(baseGeom, cx, cy, dth, sx, sy);
    } else {
        /* Mesh 形变层：真实顶点几何——质心位移直接取顶点均值差
           （头部上下运动时刘海随权重形变的实际位移被捕获，不再依赖
           骨骼刚性假设）；旋转用 PCA 主轴角（内容实际方向），
           按基准模式统一；无向主轴沿时间轴连续展开（防 180° 翻转，
           且大幅甩动 >90° 不被错 wrap） */
        const g = meshPose(at, s);
        const points = worldPoints(at, s, verts);
        const fit = fitSimilarity(baseGeom.points, points);
        [sx, sy] = scaleKey(baseGeom, g.boxw, g.boxh, name, boneDetSign(b), fit ? fit.scale : 0);
        const boneTh = -Math.atan2(b.b, b.a);
        const useBoneRot = preferBoneRotation(name) && baseGeom.boneRot !== undefined;
        const usePcaRot = forcePcaRotation(name) && baseGeom.pcaRot !== undefined;
        let th = useBoneRot ? boneTh : ((usePcaRot || baseGeom.usePca) ? g.th : boneTh);
        const baseRot = useBoneRot ? baseGeom.boneRot : (usePcaRot ? baseGeom.pcaRot : baseGeom.rot);
        if (!useBoneRot && (usePcaRot || baseGeom.usePca)) {
            const prev = prevTh.get(name);
            if (prev !== undefined) {
                let d = th - prev;
                if (d > Math.PI / 2) d -= Math.PI;
                if (d < -Math.PI / 2) d += Math.PI;
                th = prev + d;
            } else {
                let d = th - baseGeom.rot;
                if (d > Math.PI / 2) d -= Math.PI;
                if (d < -Math.PI / 2) d += Math.PI;
                th = baseGeom.rot + d;
            }
            prevTh.set(name, th);
        }
        const shapePosition = useMeshShapePosition(name);
        dth = shapePosition && fit ? fit.rot : th - baseRot;
        [dx, dy] = deltaAroundBonePivot(baseGeom, g.cx, g.cy, dth, sx, sy);
    }
    let drot = (dth * 180 / Math.PI) % 360;
    drot += rotationBiasDeg(name);
    if (drot > 180) drot -= 360;
    if (drot < -180) drot += 360;
    const [bdx, bdy] = positionBiasPx(name);
    dx += bdx;
    dy += bdy;
    return [dx, dy, Math.round(drot * 10) / 10, sx, sy];
}

function isVisibleKey(k) {
    return k && k[3] >= 0.5;
}

function hideFrameKey(frames, name, f) {
    const ks = frames[name];
    if (!ks || !ks[f]) return;
    const k = ks[f];
    ks[f] = k.length >= 6 ? [k[0], k[1], k[2], 0, k[4], k[5]] : [k[0], k[1], k[2], 0];
}

function eyeSide(name) {
    if (!/Eye/.test(name) || /Brow/.test(name)) return null;
    if (/(^|_)R($|_)|Eye_R|R_Eye/.test(name)) return 'R';
    if (/(^|_)L($|_)|Eye_L|L_Eye/.test(name)) return 'L';
    return null;
}

function isCloseEyeLayer(name) {
    return /Eye.*Close|Close.*Eye/.test(name);
}

function faceExpressionInfo(name) {
    const m = name.match(/^(.+_Face)_([A-Za-z]+)(?:_|$)/);
    if (!m) return null;
    return { root: m[1], group: m[2] };
}

function limbVariantInfo(name) {
    const m = name.match(/^([FB])_([LR])_(Arm|Forearm|Foream|Hand)(?:_([A-Z])|([A-Z])(?=\d|$|_))?/);
    if (!m) return null;
    return { root: `${m[1]}_${m[2]}_${m[3]}`, group: m[4] || m[5] || 'Base' };
}

function pickLargestVariant(counts, preferNonBase) {
    let best = null, bestCount = -1, bestBias = -1;
    for (const [group, count] of counts) {
        const bias = preferNonBase && group !== 'Base' ? 1 : 0;
        if (count > bestCount || (count === bestCount && bias > bestBias)) {
            best = group;
            bestCount = count;
            bestBias = bias;
        }
    }
    return best;
}

function applyFrameMutualExclusion(frames, frameCount) {
    const names = Object.keys(frames);
    for (let f = 0; f < frameCount; f++) {
        for (const side of ['L', 'R']) {
            const hasClose = names.some(name => eyeSide(name) === side && isCloseEyeLayer(name) && isVisibleKey(frames[name][f]));
            if (!hasClose) continue;
            let keepBase = null, keepArea = -1;
            for (const name of names) {
                if (eyeSide(name) !== side || isCloseEyeLayer(name) || !isVisibleKey(frames[name][f])) continue;
                const g = layerGeom[name];
                const area = g ? Math.max(1, g.w * g.h) : 1;
                if (area > keepArea) {
                    keepArea = area;
                    keepBase = name;
                }
            }
            for (const name of names) {
                if (eyeSide(name) === side && !isCloseEyeLayer(name) && name !== keepBase)
                    hideFrameKey(frames, name, f);
            }
        }

        const faceRoots = new Map();
        for (const name of names) {
            if (!isVisibleKey(frames[name][f])) continue;
            const info = faceExpressionInfo(name);
            if (!info) continue;
            const counts = faceRoots.get(info.root) || new Map();
            counts.set(info.group, (counts.get(info.group) || 0) + 1);
            faceRoots.set(info.root, counts);
        }
        for (const [root, counts] of faceRoots) {
            const nonDefault = new Map([...counts].filter(([group]) => group !== 'Default'));
            if (!nonDefault.size) continue;
            const keep = pickLargestVariant(nonDefault, true);
            for (const name of names) {
                const info = faceExpressionInfo(name);
                if (info && info.root === root && info.group !== keep)
                    hideFrameKey(frames, name, f);
            }
        }

        const limbRoots = new Map();
        for (const name of names) {
            if (!isVisibleKey(frames[name][f])) continue;
            const info = limbVariantInfo(name);
            if (!info) continue;
            const counts = limbRoots.get(info.root) || new Map();
            counts.set(info.group, (counts.get(info.group) || 0) + 1);
            limbRoots.set(info.root, counts);
        }
        for (const [root, counts] of limbRoots) {
            if (counts.size <= 1) continue;
            const keep = pickLargestVariant(counts, true);
            for (const name of names) {
                const info = limbVariantInfo(name);
                if (info && info.root === root && info.group !== keep)
                    hideFrameKey(frames, name, f);
            }
        }
    }
}

const manifest = {};
for (const a of anims) {
    const n = Math.max(8, Math.min(80, Math.round(a.duration * FPS)));
    const frames = {};
    const orderFrames = [];
    /* PCA 无向主轴连续展开状态：每层沿时间轴与上一帧取最近方向（增量 ≤90°），
       首帧对齐基准。曾每帧独立对齐基准：大幅甩动（>90°，如 Attack 马尾）
       跨过 ±90° 边界时被错 wrap 成回摆 165°（drot 89→-77 跳变） */
    const prevTh = new Map();
    for (let f = 0; f < n; f++) {
        skel.setToSetupPose();
        if (visibilityPoseAnim) visibilityPoseAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
        /* apply 签名：apply(skeleton, lastTime, time, ...)——曾参数反写 (t, 0)
           = 从 t 回退到 0 的反向应用，永远取起始姿势（所有"挂载链不动"的根源） */
        a.apply(skel, 0, a.duration * f / n, false, null, 1, 0, 0);
        skel.updateWorldTransform();
        orderFrames.push(currentDrawOrderNames());
        const verts = new Float32Array(2048);
        const clippedOut = clippedOutSlotIndexes(verts);
        // 变体层默认帧（非激活 vis=0；激活帧在槽循环内覆盖）。
        // 变体层名 = 附件名 + '_v'（与换链槽名重名的附件如 F_Emoticon_2
        // 不冲突——曾同名导致轨道键冲突、长度翻倍）
        for (const layerName of variants.keys())
            (frames[layerName] = frames[layerName] || []).push([0, 0, 0, 0]);
        for (const { si, name } of allSlots) {
            const s = skel.slots[si];
            const at = s.getAttachment();
            const bn = base[name];
            const visibleNow = !clippedOut.has(si) && isSlotVisible(s, at) && isGeometryVisible(at, s, verts);
            /* 附件切换（换附件，非换链）：变体帧 → 主层 vis=0（快照填充）、
               变体层 vis=1 + 几何（基准 = Default 姿势下该附件）。
               仅当场景里导出了该变体层（layerGeom）才切换——换链槽的多附件
               （F_L_Arm_2 等）未逐附件导层，回退单层纹理近似 */
            const dn = defaultAttName.get(si) || name;
            const variantName = at && at.name ? variantLayerName(si, at.name) : '';
            if (visibleNow && at.name && at.name !== dn && variants.has(variantName) && layerGeom[variantName]) {
                (frames[name] = frames[name] || []).push([0, 0, 0, 0]);
                const vb = variantBase[variantName];
                if (vb) {
                    const k = calcDelta(at, s, vb, variantName, prevTh, verts);
                    frames[variantName][f] = key4(k[0], k[1], k[2], 1, k[3], k[4]);
                }
                continue;
            }
            if (!bn || !visibleNow) {
                (frames[name] = frames[name] || []).push([0, 0, 0, 0]);
                continue;
            }
            const k = calcDelta(at, s, bn, name, prevTh, verts);
            (frames[name] = frames[name] || []).push(key4(k[0], k[1], k[2], 1, k[3], k[4]));
        }
    }
    applyFrameMutualExclusion(frames, n);
    // 静帧压缩：全 0 且可见性无变化的层不写
    const layers = {};
    for (const bn of Object.keys(frames)) {
        const ks = frames[bn];
        const defaultVis = layerGeom[bn] ? (layerGeom[bn].visible ? 1 : 0) : (base[bn] ? 1 : 0);
        const allStatic = ks.every(k => k[0] === 0 && k[1] === 0 && k[2] === 0 &&
                                      k[3] === defaultVis && (k.length < 6 || (k[4] === 1 && k[5] === 1)));
        if (allStatic) continue;
        layers[bn] = ks;
    }
    const hasOrder = orderFrames.some(o => !sameOrder(o, sceneZOrder));
    if (Object.keys(layers).length === 0 && !hasOrder) {
        console.log(`  ${a.name}: 无有效层动画，跳过`);
        continue;
    }
    const loop = /Idle|Wait|Default/.test(a.name);
    manifest[a.name] = { duration: a.duration, loop, layers };
    if (hasOrder) manifest[a.name].order = orderFrames;
    const nk = Object.values(layers).reduce((s, v) => s + v.length, 0);
    console.log(`  ${a.name}: ${n}f ${a.duration.toFixed(2)}s ${loop ? '循环' : '单次'} ${Object.keys(layers).length} 层 (${nk} 键)`);
}

// 隐藏→显示瞬间的姿势快照：隐藏帧（vis=0）的 dx/dy/rot 填最近可见帧的值——
// 曾填 0，换链激活瞬间 drot 从 0 插值到真实姿势角（如 Attack 前臂链 101.5°），
// 产生"从零快速甩到姿势"的伪影。设备端 vis<0.5 不渲染，隐藏期间值无视觉影响；
// 填可见帧值后激活瞬间以正确姿势淡入（vis 0→1 插值时位置/旋转已就位）
for (const a of Object.values(manifest)) {
    for (const ks of Object.values(a.layers)) {
        for (let i = 0; i < ks.length; i++) {
            if (ks[i][3] >= 0.5) continue;
            let best = -1, bestD = 1e9;
            for (let k = 0; k < ks.length; k++) {
                if (ks[k][3] < 0.5) continue;
                const d = Math.abs(k - i);
                if (d < bestD) { bestD = d; best = k; }
            }
            if (best >= 0) {
                ks[i][0] = ks[best][0];
                ks[i][1] = ks[best][1];
                ks[i][2] = ks[best][2];
                if (ks[best].length >= 6) {
                    ks[i][4] = ks[best][4];
                    ks[i][5] = ks[best][5];
                }
            }
        }
    }
}

// 拆层补偿：F_Eye 双眼条轨道 → 各单眼轨道（几何换算）。
// 条子轨道 = 条子质心位移 + 条子角度旋转；单眼渲染绕单眼 bbox 中心——
// 单眼目标中心 = 单眼基准中心 + 条子质心位移 + (R-I)·(单眼中心-条子质心)，
// 设备端渲染中心 = (adx',ady') + boxc + R·(c-boxc) → 令两者相等：
// adx' = 条子位移 + (R-I)·(单眼bbox中心 - 条子质心)。
// 曾直接复制条子轨道：双眼离条心 ~30px，大角度旋转错位十几像素——
// 设备端"start入场动画眼睛角度不对"的根因（Stun 恒定旋转时同样错位但不易察觉）
const eyeBar = sceneJson.eye_bar;
for (const a of Object.values(manifest)) {
    if (!a.layers['F_Eye']) continue;
    const bar = a.layers['F_Eye'];
    delete a.layers['F_Eye'];
    for (const side of ['F_Eye_L', 'F_Eye_R']) {
        const g = layerGeom[side];
        if (!g || !eyeBar) { a.layers[side] = bar; continue; }
        const qx = g.boxcx - eyeBar[0], qy = g.boxcy - eyeBar[1];
        a.layers[side] = bar.map(([dx, dy, drot, vis, sx = 1, sy = 1]) => {
            const th = drot * Math.PI / 180;
            const cos = Math.cos(th), sin = Math.sin(th);
            const rx = (cos - 1) * qx - sin * qy;
            const ry = sin * qx + (cos - 1) * qy;
            return [Math.round(dx + rx), Math.round(dy + ry), drot, vis, sx, sy];
        });
    }
}

// 紧凑输出(无缩进):设备端 cJSON 解析 1.3MB 基建 anims.json 时解析树内存超内部 RAM,
// 紧凑后约省 40-60% 体积(缩进空格是大头)。duration 同时舍入 3 位小数。
const compact = {};
for (const [nm, a] of Object.entries(manifest)) {
    compact[nm] = { duration: Math.round(a.duration * 1000) / 1000, loop: a.loop, layers: a.layers };
}
fs.writeFileSync(path.join(outDir, 'anims.json'), JSON.stringify(compact));
const kb = (fs.statSync(path.join(outDir, 'anims.json')).size / 1024).toFixed(0);
console.log(`anims.json: ${Object.keys(manifest).length} 动画, ${kb} KB → ${outDir}`);
