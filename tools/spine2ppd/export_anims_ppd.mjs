// Spine 动画 → PPD 原生动画（anims.json）：每个动画逐帧采样，导出每个图层的
// 旋转/位移增量 + 可见性时间轴，设备端纸偶渲染器逐层插值应用——
// 角色图层自然切换动作（含换链：隐藏槽激活/显式槽隐藏），非 MJPEG 录像
// 用法：node export_anims_ppd.mjs <spine_dir> <out_dir> [fps]
// 输出：<out_dir>/anims.json
//
// 格式：{ "<动画名>": {"duration": s, "loop": bool,
//                      "layers": {"<槽名>": [[dx,dy,drot,vis], ...每帧...]}}}
// 旋转精确表达：设备端绕层质心旋转 + 平移 delta。真实 Spine 旋转绕骨骼点——
// delta = 骨骼点位移 + (R(θ)-I)·(质心-骨骼点)（修正项），大角度（Stun 腿 75°）
// 不再错位（曾用纯质心位移，误差 (R-I)·(C-P) 达几十像素）。
// 基准：显式层 Default 姿势 / 隐藏层 setup 姿势（与场景导出基准一致）；
// 世界→场景 R270°·S（与 render_layers --landscape 一致）；vis=1/0 附件可见性。
import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const outDir = process.argv[3];
const FPS = parseInt(process.argv[4] || '20', 10);

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const files = fs.readdirSync(dir);
const atlas = new spine.TextureAtlas(fs.readFileSync(path.join(dir, files.find(f => f.endsWith('.atlas'))), 'utf8'),
    p => new spine.Texture({ width: 1024, height: 1024 }, 1024, 1024));
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, files.find(f => f.endsWith('.skel'))))));
const skel = new spine.Skeleton(data);
const skin0 = data.defaultSkin || data.skins[0];
const defaultAnim = data.animations.find(a => a.name === 'Default');

// 场景层几何/可见性（scene.json——设备端层 bbox 中心/质心/visible）。
// 提前读取：槽的 hidden 判定要用场景导出时的实际可见性
// （Default 姿势渲染为空、回退 setup 导出的槽在场景里 visible=false）
const sceneJson = JSON.parse(fs.readFileSync(path.join(outDir, 'scene.json'), 'utf8'));
const layerGeom = {};
for (const l of sceneJson.layers)
    layerGeom[l.name] = { boxcx: (l.bbox[0] + l.bbox[2]) / 2, boxcy: (l.bbox[1] + l.bbox[3]) / 2,
                          cx: l.cx, cy: l.cy, visible: l.visible };

// 全部槽（Default 姿势下附件 null 或场景层不可见的为隐藏槽——换链/被 Default
// 动画移出屏幕的姿势链，基准用 setup 姿势，动作时由可见性轨道激活）
const allSlots = [];
const defaultAttName = new Map();   // slotIndex -> Default 姿势附件名（换附件检测基准）
{
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
    for (const s of skel.slots) {
        const at = s.getAttachment();
        const sg = layerGeom[s.data.name];
        const sceneHidden = sg ? sg.visible === false : false;
        if (at && !sceneHidden) {
            allSlots.push({ si: s.data.index, name: s.data.name, hidden: false });
            defaultAttName.set(s.data.index, at.name || '');
            continue;
        }
        const map = skin0.attachments[s.data.index];
        if (map && Object.keys(map).length > 0)
            allSlots.push({ si: s.data.index, name: s.data.name, hidden: true });
    }
}
console.log(`槽 ${allSlots.length} 个（隐藏 ${allSlots.filter(s => s.hidden).length}）`);

// 附件变体集合（与 render_layers.mjs 同逻辑）：动画 attachment timeline 引用的
// 非 Default 附件（Start 的 F_Eye_3 闭眼线等）→ 场景里是 hidden 变体层，
// 此处生成 vis 轨道（切换区间=1）+ 几何轨道（相对 Default 姿势下该附件的基准）
const variants = new Map();   // 附件名 -> slotIndex
for (const a of data.animations) {
    for (const tl of a.timelines) {
        if (tl.constructor.name !== 'AttachmentTimeline') continue;
        const dn = defaultAttName.get(tl.slotIndex) || '';
        for (const nm of tl.attachmentNames) {
            if (!nm || nm === dn) continue;
            const map = skin0.attachments[tl.slotIndex];
            if (map && map[nm] && map[nm].region && !variants.has(nm)) variants.set(nm, tl.slotIndex);
        }
    }
}
// 变体附件基准几何（Default 姿势挂上变体附件；与 render_layers 变体层渲染姿势一致）
const variantBase = {};
for (const [attNm, slotIndex] of variants) {
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    for (const s of skel.slots) s.setAttachment(null);
    const slot = skel.slots[slotIndex];
    slot.setAttachment(skin0.attachments[slotIndex][attNm]);
    skel.updateWorldTransform();
    const at = slot.getAttachment();
    const b = skel.bones[slot.data.boneData.index];
    const verts = new Float32Array(2048);
    if (at.constructor.name === 'RegionAttachment') {
        at.computeWorldVertices(b, verts, 0, 2);
        variantBase[attNm] = {
            cx: (verts[0] + verts[2] + verts[4] + verts[6]) / 4,
            cy: (verts[1] + verts[3] + verts[5] + verts[7]) / 4,
            px: b.worldX, py: b.worldY,
            rot: Math.atan2(verts[3] - verts[1], verts[2] - verts[0]), usePca: false,
        };
    } else {
        const g = meshPose(at, slot);
        variantBase[attNm] = { cx: g.cx, cy: g.cy, px: b.worldX, py: b.worldY,
                               rot: g.usePca ? g.th : -Math.atan2(b.b, b.a), usePca: g.usePca,
                               boneRot: -Math.atan2(b.b, b.a) };
    }
}
if (variants.size) console.log(`附件变体 ${variants.size} 个（${[...variants.keys()].join(', ')}）`);

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
    for (let i = 0; i < n; i++) { cx += wv[i * 2]; cy += wv[i * 2 + 1]; }
    cx /= n; cy /= n;
    let sxx = 0, syy = 0, sxy = 0;
    for (let i = 0; i < n; i++) {
        const dx = wv[i * 2] - cx, dy = wv[i * 2 + 1] - cy;
        sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
    const d = Math.sqrt(Math.max(0, (sxx - syy) * (sxx - syy) / 4 + sxy * sxy));
    const l1 = (sxx + syy) / 2 + d, l2 = (sxx + syy) / 2 - d;
    return { cx, cy, th: 0.5 * Math.atan2(2 * sxy, sxx - syy),
             usePca: l1 > 1e-9 && l2 / l1 < 0.5 };
}

// 某姿势下每个槽：质心 cx,cy、骨骼点 px,py、旋转角 rot（附件 null → null）
function poseLayers(useDefault) {
    skel.setToSetupPose();
    if (useDefault && defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
    const out = {};
    const verts = new Float32Array(2048);
    for (const s of skel.slots) {
        const at = s.getAttachment();
        const b = skel.bones[s.data.boneData.index];
        if (!at || !at.region) { out[s.data.name] = null; continue; }
        if (at.constructor.name === 'RegionAttachment') {
            at.computeWorldVertices(b, verts, 0, 2);
            const cx = (verts[0] + verts[2] + verts[4] + verts[6]) / 4;
            const cy = (verts[1] + verts[3] + verts[5] + verts[7]) / 4;
            const rot = Math.atan2(verts[3] - verts[1], verts[2] - verts[0]);
            out[s.data.name] = { cx, cy, px: b.worldX, py: b.worldY, rot };
        } else {
            /* Mesh 层：真实顶点几何（形变被捕获）——质心 = 顶点均值；
               rot = PCA 主轴角（内容实际方向；接近圆形退化用骨骼角取负——
               曾 rot=0 导致"原地旋转"动作完全丢失） */
            const g = meshPose(at, s);
            out[s.data.name] = { cx: g.cx, cy: g.cy, px: b.worldX, py: b.worldY,
                                 rot: g.usePca ? g.th : -Math.atan2(b.b, b.a),
                                 usePca: g.usePca,
                                 boneRot: -Math.atan2(b.b, b.a) };   // 视觉骨骼角（clamp 基准）
        }
    }
    return out;
}

// 基准：显式层 Default 姿势、隐藏层 setup 姿势（与场景导出基准一致）
const baseDefault = poseLayers(true);
const baseSetup = poseLayers(false);
const base = {};
for (const { name, hidden } of allSlots) base[name] = hidden ? baseSetup[name] : baseDefault[name];

// 场景 fit 缩放（与 render_layers --landscape 相同公式）：世界→场景 R270°·S
function sceneScale() {
    skel.setToSetupPose();
    if (defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
    skel.updateWorldTransform();
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
    return Math.min(480 * 0.95 / (maxY - minY), 800 * 0.95 / (maxX - minX));
}
const SCL = sceneScale();
console.log(`场景 scale=${SCL.toFixed(3)}（世界→场景 R270°·S）`);

// 设备端旋转点 = 层内容 bbox 中心（L->w/2, L->h/2），不是质心——
// 头发等细长不对称层两者差几十 px，大角度旋转时按质心算修正项会分离错位。
// （scene.json / layerGeom 已在文件前部读取——含 visible 字段供 hidden 判定）

const SKIP = new Set(['Begin', 'End']);
const anims = data.animations.filter(a => !SKIP.has(a.name) && a.duration > 0);

/* 从附件+骨骼+基准几何算 [dx, dy, drot]（Region 刚性公式 / mesh 顶点几何）
   baseGeom: {cx, cy, px, py, rot, usePca}；name 用于 layerGeom 与 PCA 展开键 */
function calcDelta(at, s, baseGeom, name, prevTh, verts) {
    const b = skel.bones[s.data.boneData.index];
    /* 场景变换 R270°·S 保向（无反射）→ 世界角度差即场景角度差；
       设备端绕层 bbox 中心（w/2,h/2）旋转 + 平移 delta */
    let dth, dx, dy;
    if (at.constructor.name === 'RegionAttachment') {
        /* 刚性层：质心随骨骼刚性运动——骨骼点位移 + (R-I)·(质心-骨骼点)
           修正项精确成立（无顶点形变，无近似） */
        at.computeWorldVertices(b, verts, 0, 2);
        const rot = Math.atan2(verts[3] - verts[1], verts[2] - verts[0]);
        dth = rot - baseGeom.rot;
        const pxs = (b.worldY - baseGeom.py) * SCL;        // 骨骼点位移
        const pys = -(b.worldX - baseGeom.px) * SCL;
        const g = layerGeom[name];
        const vx = g ? (g.boxcx - g.cx) + (baseGeom.cy - baseGeom.py) * SCL : (baseGeom.cy - baseGeom.py) * SCL;
        const vy = g ? (g.boxcy - g.cy) - (baseGeom.cx - baseGeom.px) * SCL : -(baseGeom.cx - baseGeom.px) * SCL;
        const cos = Math.cos(dth), sin = Math.sin(dth);
        dx = Math.round(pxs + (cos - 1) * vx - sin * vy);
        dy = Math.round(pys + sin * vx + (cos - 1) * vy);
    } else {
        /* Mesh 形变层：真实顶点几何——质心位移直接取顶点均值差
           （头部上下运动时刘海随权重形变的实际位移被捕获，不再依赖
           骨骼刚性假设）；旋转用 PCA 主轴角（内容实际方向），
           按基准模式统一；无向主轴沿时间轴连续展开（防 180° 翻转，
           且大幅甩动 >90° 不被错 wrap） */
        const g = meshPose(at, s);
        let th = baseGeom.usePca ? g.th : -Math.atan2(b.b, b.a);
        if (baseGeom.usePca) {
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
        dth = th - baseGeom.rot;
        const mx = (g.cy - baseGeom.cy) * SCL;        // 质心位移（世界差→场景）
        const my = -(g.cx - baseGeom.cx) * SCL;
        const gg = layerGeom[name];
        const ux = gg ? gg.cx - gg.boxcx : 0;   // 质心-bbox中心（场景系）
        const uy = gg ? gg.cy - gg.boxcy : 0;
        const cos = Math.cos(dth), sin = Math.sin(dth);
        // 设备端渲染：内容中心 = (adx,ady) + boxc + R·(质心-bbox中心)
        // 目标 = 基准质心 + (mx,my) → adx = mx - (R-I)·(质心-bbox中心)
        dx = Math.round(mx - ((cos - 1) * ux - sin * uy));
        dy = Math.round(my - (sin * ux + (cos - 1) * uy));
    }
    let drot = (dth * 180 / Math.PI) % 360;
    if (drot > 180) drot -= 360;
    if (drot < -180) drot += 360;
    return [dx, dy, Math.round(drot * 10) / 10];
}

const manifest = {};
for (const a of anims) {
    const n = Math.max(8, Math.min(80, Math.round(a.duration * FPS)));
    const frames = {};
    /* PCA 无向主轴连续展开状态：每层沿时间轴与上一帧取最近方向（增量 ≤90°），
       首帧对齐基准。曾每帧独立对齐基准：大幅甩动（>90°，如 Attack 马尾）
       跨过 ±90° 边界时被错 wrap 成回摆 165°（drot 89→-77 跳变） */
    const prevTh = new Map();
    for (let f = 0; f < n; f++) {
        skel.setToSetupPose();
        /* apply 签名：apply(skeleton, lastTime, time, ...)——曾参数反写 (t, 0)
           = 从 t 回退到 0 的反向应用，永远取起始姿势（所有"挂载链不动"的根源） */
        a.apply(skel, 0, a.duration * f / n, false, null, 1, 0, 0);
        skel.updateWorldTransform();
        const verts = new Float32Array(2048);
        // 变体层默认帧（非激活 vis=0；激活帧在槽循环内覆盖）。
        // 变体层名 = 附件名 + '_v'（与换链槽名重名的附件如 F_Emoticon_2
        // 不冲突——曾同名导致轨道键冲突、长度翻倍）
        for (const attNm of variants.keys())
            (frames[attNm + '_v'] = frames[attNm + '_v'] || []).push([0, 0, 0, 0]);
        for (const { si, name } of allSlots) {
            const s = skel.slots[si];
            const at = s.getAttachment();
            const bn = base[name];
            /* 附件切换（换附件，非换链）：变体帧 → 主层 vis=0（快照填充）、
               变体层 vis=1 + 几何（基准 = Default 姿势下该附件）。
               仅当场景里导出了该变体层（layerGeom）才切换——换链槽的多附件
               （F_L_Arm_2 等）未逐附件导层，回退单层纹理近似 */
            const dn = defaultAttName.get(si) || name;
            if (at && at.name && at.name !== dn && variants.has(at.name) && layerGeom[at.name + '_v']) {
                (frames[name] = frames[name] || []).push([0, 0, 0, 0]);
                const vb = variantBase[at.name];
                if (vb) {
                    const k = calcDelta(at, s, vb, at.name + '_v', prevTh, verts);
                    frames[at.name + '_v'][f] = [k[0], k[1], k[2], 1];
                }
                continue;
            }
            if (!bn || !at || !at.region) {
                (frames[name] = frames[name] || []).push([0, 0, 0, 0]);
                continue;
            }
            const k = calcDelta(at, s, bn, name, prevTh, verts);
            (frames[name] = frames[name] || []).push([k[0], k[1], k[2], 1]);
        }
    }
    // 静帧压缩：全 0 且可见性无变化的层不写
    const layers = {};
    for (const bn of Object.keys(frames)) {
        const ks = frames[bn];
        const allStatic = ks.every(k => k[0] === 0 && k[1] === 0 && k[2] === 0 &&
                                      k[3] === (base[bn] ? 1 : 0));
        if (allStatic) continue;
        layers[bn] = ks;
    }
    if (Object.keys(layers).length === 0) {
        console.log(`  ${a.name}: 无有效层动画，跳过`);
        continue;
    }
    const loop = /Idle|Wait|Default/.test(a.name);
    manifest[a.name] = { duration: a.duration, loop, layers };
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
        a.layers[side] = bar.map(([dx, dy, drot, vis]) => {
            const th = drot * Math.PI / 180;
            const cos = Math.cos(th), sin = Math.sin(th);
            const rx = (cos - 1) * qx - sin * qy;
            const ry = sin * qx + (cos - 1) * qy;
            return [Math.round(dx + rx), Math.round(dy + ry), drot, vis];
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
