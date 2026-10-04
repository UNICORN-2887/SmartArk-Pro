// spine → PPD 转换器（本地重写版 v2：骨骼轨迹直接转换）
// 用法: node convert.mjs <spine形态目录> <输出目录>
//   spine形态目录: 含 <stem>.skel|.json + <stem>.atlas + <stem>.png（wiki_fetch 产物）
//   输出: scene.json + <槽名>.raw + anims.json（交接文档第二节格式）
//
// 算法：
//   - 贴图姿势 = Idle t=0（无 Idle 取首个非空动画 t=0）——所有动画共享的待机姿势；
//     逐槽渲染（官方 spine-canvas 渲染器）导出贴图，仅此一次渲染用于贴图
//   - 动画轨道 = 直接读官方骨骼时间轴：每帧 apply 动画后读槽骨骼的世界变换（有向、无歧义），
//     相对贴图姿势的骨骼变换算出 dx/dy/drot（贴图 bbox 左上随骨骼刚体变换）
//   - vis = 该帧槽附件是否存在且附件名与贴图姿势一致（官方 AttachmentTimeline，天然硬切）
//   - 相机吸收进骨骼根变换（skeleton.x/y/scaleX/scaleY），ctx 恒等
//   - 子部件层（手指节/脚趾节等）轨道跟随主部件（组刚体），vis 保留
import fs from 'fs';
import path from 'path';
import { createCanvas, loadImage } from '@napi-rs/canvas';

const W = 480, H = 800;
const BIG_DEFORM = 2048;
const FPS = 20;
const HEAD_RE = /Head|Eye|Brow|Mouth|Nose|Sclera|Hair|Ear|Face/i;
const LOOP_RE = /(Loop|Idle|Default)/i;

const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const r2 = v => Math.round(v * 100) / 100;
const clamp = (v, a, b) => Math.min(Math.max(v, a), b);

// ---------- spine 形态加载 ----------
async function loadSet(setDir) {
  const files = fs.readdirSync(setDir);
  const skelF = files.find(f => f.endsWith('.skel')) || files.find(f => f.endsWith('.json'));
  if (!skelF) throw new Error(`无 .skel/.json: ${setDir}`);
  const stem = skelF.replace(/\.(skel|json)$/, '');
  const atlasText = fs.readFileSync(path.join(setDir, stem + '.atlas'), 'utf8').replace(/\r\n/g, '\n');
  const images = new Map();
  await Promise.all([...new Set([...atlasText.matchAll(/^([\w.-]+\.png)$/gm)].map(m => m[1]))].map(async name => {
    images.set(name, await loadImage(fs.readFileSync(path.join(setDir, name))));
  }));
  spine.Texture.prototype.setFilters = function () {};
  spine.Texture.prototype.setWraps = function () {};
  // 下载渠道缩放处理：atlas 文本声明的 page 尺寸可能与 png 实际尺寸不一致
  //（spine TextureAtlas 会把 page.width 覆盖为实际图尺寸，无法再用它检测——必须解析文本声明）
  const declaredSize = new Map();
  for (const m of atlasText.matchAll(/^([\w.-]+\.png)[\s\S]*?size:\s*(\d+)\s*,\s*(\d+)/gm)) {
    declaredSize.set(m[1], { w: +m[2], h: +m[3] });
  }
  const atlas = new spine.TextureAtlas(atlasText, p => new spine.canvas.CanvasTexture(images.get(p)));
  for (const page of atlas.pages) {
    const img = images.get(page.name);
    const ds = declaredSize.get(page.name);
    if (img && ds && (ds.w !== img.width || ds.h !== img.height)) {
      console.log(`  [atlas缩放] ${page.name}: 声明 ${ds.w}x${ds.h} vs 实际 ${img.width}x${img.height}`);
      const kx = img.width / ds.w, ky = img.height / ds.h;
      for (const r of atlas.regions) {
        if (r.page !== page) continue;
        r.u *= kx; r.v *= ky; r.u2 *= kx; r.v2 *= ky;
        r.x *= kx; r.y *= ky; r.width *= kx; r.height *= ky;
        r.originalWidth *= kx; r.originalHeight *= ky;
        if (r.offsetX) { r.offsetX *= kx; r.offsetY *= ky; }
        if (r.uvs) { for (let i = 0; i < r.uvs.length; i += 2) { r.uvs[i] *= kx; r.uvs[i + 1] *= ky; } }
      }
      page.width = img.width; page.height = img.height;
    }
  }
  const loader = new spine.AtlasAttachmentLoader(atlas);
  const skelData = skelF.endsWith('.skel')
    ? new spine.SkeletonBinary(loader).readSkeletonData(new Uint8Array(fs.readFileSync(path.join(setDir, skelF))))
    : new spine.SkeletonJson(loader).readSkeletonData(fs.readFileSync(path.join(setDir, skelF), 'utf8'));
  return {
    stem, skelData,
    makeSkeleton() {
      const s = new spine.Skeleton(skelData);
      s.setToSetupPose();
      s.updateWorldTransform();
      return s;
    },
  };
}

// ---------- 像素扫描（仅用于贴图 bbox 与相机标定） ----------
function scanRegion(canvas, x0, y0, x1, y1) {
  const ctx = canvas.getContext('2d');
  const w = x1 - x0, h = y1 - y0;
  const d = ctx.getImageData(x0, y0, w, h).data;
  let minX = 1e9, minY = 1e9, maxX = -1, maxY = -1, count = 0;
  for (let y = 0; y < h; y++) {
    const row = y * w;
    for (let x = 0; x < w; x++) {
      if (d[(row + x) * 4 + 3] > 0) {
        const gx = x0 + x, gy = y0 + y;
        if (gx < minX) minX = gx;
        if (gx > maxX) maxX = gx;
        if (gy < minY) minY = gy;
        if (gy > maxY) maxY = gy;
        count++;
      }
    }
  }
  if (count < 1) return null;
  return { minX, minY, maxX, maxY, count, w: maxX - minX + 1, h: maxY - minY + 1 };
}

// 半像素对齐：回放公式 translate(x + w/2 + dx, ...) 要求 x+w/2 为整数，
// 否则整层贴图被 Canvas 重采样。bbox 的 (minX+maxX) 为偶数时中心是 .5 → 右/下补 1 透明像素
function fixAlign(m) {
  const mm = { ...m };
  if ((mm.minX + mm.maxX) % 2 === 0) { mm.maxX += 1; mm.w += 1; }
  if ((mm.minY + mm.maxY) % 2 === 0) { mm.maxY += 1; mm.h += 1; }
  return mm;
}

// 整身 bbox 估算：槽骨骼世界变换 + 附件尺寸的 4 角（粗估，用于相机标定）
function estimateBounds(skeleton) {
  let minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  for (const slot of skeleton.drawOrder) {
    const att = slot.getAttachment();
    if (!att) continue;
    const hw = Math.max(att.width || 0, 20) / 2, hh = Math.max(att.height || 0, 20) / 2;
    const b = slot.bone;
    for (const [lx, ly] of [[-hw, -hh], [hw, -hh], [-hw, hh], [hw, hh]]) {
      const x = b.a * lx + b.b * ly + b.worldX;
      const y = b.c * lx + b.d * ly + b.worldY;
      minX = Math.min(minX, x); maxX = Math.max(maxX, x);
      minY = Math.min(minY, y); maxY = Math.max(maxY, y);
    }
  }
  if (minX === 1e9) throw new Error('无附件可估算 bbox');
  return { minX, minY, maxX, maxY, w: maxX - minX, h: maxY - minY };
}

// ---------- 渲染 ----------
function renderSlots(canvas, skeleton, slots) {
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  const saved = skeleton.drawOrder;
  skeleton.drawOrder = slots;
  try {
    const r = new spine.canvas.SkeletonRenderer(ctx);
    r.triangleRendering = true;
    r.draw(skeleton);
  } finally {
    skeleton.drawOrder = saved;
  }
}

function applyCam(skeleton, cam) {
  skeleton.x = cam.tx; skeleton.y = cam.ty;
  skeleton.scaleX = cam.scale; skeleton.scaleY = cam.scale;
  skeleton.updateWorldTransform();
}

// Deform vis 检测：大画布渲染单槽，扫描区域基于当前帧骨骼世界位置动态定位
//（骨骼已含相机变换=屏幕坐标，+1024 偏移落在 2048 画布内）
function renderCount(bigCanvas, skeleton, slot, rr) {
  const ctx = bigCanvas.getContext('2d');
  ctx.clearRect(0, 0, BIG_DEFORM, BIG_DEFORM);
  const saved = skeleton.drawOrder;
  skeleton.drawOrder = [slot];
  try {
    const r = new spine.canvas.SkeletonRenderer(ctx);
    r.triangleRendering = true;
    r.draw(skeleton);
  } finally {
    skeleton.drawOrder = saved;
  }
  // 骨骼世界 = 屏幕坐标（相机已进骨骼根），直接作为画布坐标，无 translate 偏移
  const cx = slot.bone.worldX, cy = slot.bone.worldY;
  const hw = rr.bbox.w / 2 + 80, hh = rr.bbox.h / 2 + 80;
  const x0 = Math.max(0, Math.floor(cx - hw));
  const y0 = Math.max(0, Math.floor(cy - hh));
  const x1 = Math.min(BIG_DEFORM, Math.ceil(cx + hw));
  const y1 = Math.min(BIG_DEFORM, Math.ceil(cy + hh));
  const w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) return 0;
  const d = ctx.getImageData(x0, y0, w, h).data;
  let n = 0;
  for (let i = 3; i < d.length; i += 4) if (d[i] > 0) n++;
  return n;
}

function mkLayer(name, m, z, head, visible) {
  return {
    name, x: m.minX, y: m.minY, w: m.w, h: m.h,
    cx: Math.round(m.minX + m.w / 2), cy: Math.round(m.minY + m.h / 2),
    bbox: [m.minX, m.minY, m.maxX, m.maxY],
    z, group: head ? 'head' : 'body', special: '', visible,
  };
}
function mkLayerEmpty(name, z, head) {
  return { name, x: 0, y: 0, w: 1, h: 1, cx: 0, cy: 0, bbox: [0, 0, 1, 1], z, group: head ? 'head' : 'body', special: '', visible: false };
}
function cropRaw(canvas, x, y, w, h) {
  const d = canvas.getContext('2d').getImageData(x, y, w, h).data;
  const buf = Buffer.alloc(4 + w * h * 4);
  buf.writeUInt16LE(w, 0);
  buf.writeUInt16LE(h, 2);
  Buffer.from(d).copy(buf, 4);
  return buf;
}

// ---------- 骨骼变换（官方轨迹直接读取） ----------
function boneWorld(slot) {
  const b = slot.bone;
  return { a: b.a, b: b.b, c: b.c, d: b.d, x: b.worldX, y: b.worldY };
}
// 相对变换：cur 相对 ref 的 2×3 仿射（R_rel = R_cur · R_ref⁻¹）
function relTransform(ref, cur) {
  const det = ref.a * ref.d - ref.b * ref.c;
  const ia = ref.d / det, ib = -ref.b / det, ic = -ref.c / det, id = ref.a / det;
  const ra = cur.a * ia + cur.b * ic;
  const rb = cur.a * ib + cur.b * id;
  const rc = cur.c * ia + cur.d * ic;
  const rd = cur.c * ib + cur.d * id;
  const tx = cur.x - (ra * ref.x + rb * ref.y);
  const ty = cur.y - (rc * ref.x + rd * ref.y);
  return { ra, rb, rc, rd, tx, ty, drot: Math.atan2(rb, ra) * 180 / Math.PI };
}
// 贴图中心 c0 在相对变换下的位移——必须用中心：仿真器渲染公式是
// translate(x+w/2+dx, y+h/2+dy) → rotate → translate(-w/2, -h/2)，旋转绕贴图中心。
// d = (R_rel - I)·c0 + t_rel
function cornerDelta(rel, cx0, cy0) {
  return {
    dx: (rel.ra - 1) * cx0 + rel.rb * cy0 + rel.tx,
    dy: rel.rc * cx0 + (rel.rd - 1) * cy0 + rel.ty,
  };
}

// ---------- 主流程 ----------
// 单贴图方案（与服务器版一致）：所有动画共享一套贴图（贴图姿势 = Idle t=0 优先），
// 数据量 = 一套贴图 + 全部动画轨道；不按动画拆形态（那会复制贴图导致数据膨胀，
// 且服务器版其他角色都是单贴图结构）。

async function convert(setDir, outDir) {
  const { stem, skelData, makeSkeleton } = await loadSet(setDir);
  console.log(`[${path.basename(outDir)}] bones=${skelData.bones.length} slots=${skelData.slots.length} anims=${skelData.animations.length}`);
  fs.rmSync(outDir, { recursive: true, force: true });   // 清空旧产物（防多轮转换残留混入打包）
  fs.mkdirSync(outDir, { recursive: true });

  // 全部非空动画一组（单贴图）
  const group = { name: '全部', anims: skelData.animations.filter(a => a.duration > 0.001) };
  await convertGroup(setDir, outDir, group, skelData, makeSkeleton);
}

// 单组转换：贴图姿势 = Idle t=0 优先，否则第一个动画 t=0
async function convertGroup(setDir, outDir, group, skelData, makeSkeleton) {
  const t0 = Date.now();
  fs.mkdirSync(outDir, { recursive: true });

  const texAnim = group.anims.find(a => a.name === 'Idle') || group.anims[0];
  const poseSkeleton = (anim, t) => {
    const s = makeSkeleton();
    if (anim) anim.apply(s, 0, t, false, null, 1, spine.MixBlend.replace, 1);
    s.updateWorldTransform();
    return s;
  };
  const sPose = poseSkeleton(texAnim, 0);
  console.log(`  [${path.basename(outDir)}] 贴图姿势 = ${texAnim.name} t=0`);

  // 1. 相机标定：贴图姿势整身 bbox 用骨骼+附件尺寸估算（纯数学，
  //    不受"动画首帧把角色移到远处"影响——渲染法在大偏移时会扫空画布）
  const eb = estimateBounds(sPose);
  const scale = Math.min(W / eb.w, H / eb.h);
  const cam = {
    scale,
    tx: (W - eb.w * scale) / 2 - eb.minX * scale,
    ty: (H - eb.h * scale) / 2 - eb.minY * scale,
  };
  console.log(`  相机 scale=${scale.toFixed(3)} tx=${cam.tx.toFixed(1)} ty=${cam.ty.toFixed(1)}`);

  // 2. 贴图姿势逐槽导出贴图 + 记录骨骼变换参考
  const canvas = createCanvas(W, H);
  const s0 = poseSkeleton(texAnim, 0);
  applyCam(s0, cam);
  const drawOrderBase = s0.drawOrder.map(slot => slot.data.name);
  console.log('  drawOrder 槽数 =', drawOrderBase.length);

  const layers = [];
  const raws = new Map();          // 层名 -> raw buffer
  const ref = new Map();           // 层名 -> {bbox, bone, attName} 全局参考
  const slotOf0 = new Map();       // 槽名 -> slot 对象（slots 数组不随 DrawOrderTimeline 重排）
  s0.slots.forEach(sl => slotOf0.set(sl.data.name, sl));
  const zOf = new Map();           // 槽名 -> z（drawOrder 顺序）
  drawOrderBase.forEach((name, i) => zOf.set(name, i));
  const refPixels = new Map();       // 槽 -> 贴图姿势渲染像素数（Deform vis 判定用）
  for (let i = 0; i < drawOrderBase.length; i++) {
    const name = drawOrderBase[i];
    const slot = slotOf0.get(name);
    const att = slot.getAttachment();
    renderSlots(canvas, s0, [slot]);
    const m = scanRegion(canvas, 0, 0, W, H);
    const head = HEAD_RE.test(name);
    if (att && m) {
      const m2 = fixAlign(m);
      raws.set(name, cropRaw(canvas, m2.minX, m2.minY, m2.w, m2.h));
      layers.push(mkLayer(name, m2, i, head, true));
      ref.set(name, { bbox: m2, bone: boneWorld(slot), attName: att.name });
      refPixels.set(name, m.count);
    } else {
      // 贴图姿势无附件或无内容：留待动画帧取贴图（visible=false）
    }
  }
  console.log(`  贴图姿势有内容层 = ${layers.length}`);

  // 3. 动画轨道：直接读官方骨骼时间轴（无渲染）
  const bigCanvas = createCanvas(BIG_DEFORM, BIG_DEFORM);   // Deform 内容量检测用大画布（防内容出画布）
  const anims = {};
  for (const ad of group.anims) {
    if (ad.duration <= 0.001) { console.log(`  跳过空动画 ${ad.name} (duration=0)`); continue; }
    const loop = LOOP_RE.test(ad.name);
    const n = clamp(Math.round(ad.duration * FPS), 8, 80);
    const tracks = {};
    // 该动画有 DeformTimeline 的槽：内容量由 mesh 形变驱动（如闭眼线压没），须渲染检测 vis
    const deformSlots = new Set();
    for (const tl of ad.timelines) {
      if (tl instanceof spine.DeformTimeline) deformSlots.add(skelData.slots[tl.slotIndex].name);
    }
    const sA = makeSkeleton();
    applyCam(sA, cam);
    const slotOfA = new Map();
    sA.slots.forEach(sl => slotOfA.set(sl.data.name, sl));
    for (let i = 0; i < n; i++) {
      const t = ad.duration * i / (n - 1);
      sA.setToSetupPose();
      applyCam(sA, cam);
      ad.apply(sA, 0, t, loop, null, 1, spine.MixBlend.replace, 1);
      sA.updateWorldTransform();
      for (const name of drawOrderBase) {
        const slot = slotOfA.get(name);
        const att = slot.getAttachment();
        const rr = ref.get(name);
        if (!att) { pushFrame(tracks, name, 0, 0, 0, 0); continue; }
        // Deform 槽：渲染检测内容量（deform 可把内容压到近乎消失 → 该帧应隐藏）
        if (deformSlots.has(name) && rr) {
          const cnt = renderCount(bigCanvas, sA, slot, rr);
          const base = refPixels.get(name) || 1;
          if (cnt < Math.max(base * 0.1, 4)) { pushFrame(tracks, name, 0, 0, 0, 0); continue; }
        }
        if (!rr) {
          // 贴图姿势无内容的槽：首个有附件帧 = 全局参考 + 贴图来源（渲染该帧该槽）
          renderSlots(canvas, sA, [slot]);
          const m = scanRegion(canvas, 0, 0, W, H);
          if (!m) { pushFrame(tracks, name, 0, 0, 0, 0); continue; }
          const m2 = fixAlign(m);
          ref.set(name, { bbox: m2, bone: boneWorld(slot), attName: att.name });
          if (!raws.has(name)) {
            raws.set(name, cropRaw(canvas, m2.minX, m2.minY, m2.w, m2.h));
            layers.push(mkLayer(name, m2, zOf.get(name), HEAD_RE.test(name), false));
          }
          pushFrame(tracks, name, 0, 0, 0, 1);
          continue;
        }
        if (att.name !== rr.attName) { pushFrame(tracks, name, 0, 0, 0, 0); continue; } // 换附件：单贴图无法表达 → 隐藏
        const rel = relTransform(rr.bone, boneWorld(slot));
        const d = cornerDelta(rel, rr.bbox.minX + rr.bbox.w / 2, rr.bbox.minY + rr.bbox.h / 2);
        pushFrame(tracks, name, r2(d.dx), r2(d.dy), r2(rel.drot), 1);
      }
    }
    anims[ad.name] = { duration: ad.duration, loop, layers: tracks };
    console.log(`  ${ad.name}: duration=${ad.duration} loop=${loop} 帧=${n}`);
  }

  // 4. 全程无附件的槽：1×1 透明占位
  for (const name of drawOrderBase) {
    if (!raws.has(name)) {
      const pad = Buffer.alloc(4 + 4, 0);
      pad.writeUInt16LE(1, 0);
      pad.writeUInt16LE(1, 2);
      raws.set(name, pad);
      layers.push(mkLayerEmpty(name, zOf.get(name), HEAD_RE.test(name)));
    }
  }
  layers.sort((a, b) => a.z - b.z);

  // 5. 组跟随：手指节/脚趾节等子部件层的 dx/dy/drot 跟随主部件（vis 保留）
  applyFollow(anims, layers);

  // 6. 写输出（camera 字段供 verify/diag 复用同一相机，仿真器忽略）
  fs.writeFileSync(path.join(outDir, 'scene.json'), JSON.stringify({
    landscape: true, landscape_rot: 1, camera: cam, layers,
  }));
  for (const L of layers) {
    const raw = raws.get(L.name);
    if (raw) fs.writeFileSync(path.join(outDir, L.name + '.raw'), raw);
  }
  fs.writeFileSync(path.join(outDir, 'anims.json'), JSON.stringify(anims));
  console.log(`  层=${layers.length} 动画=${Object.keys(anims).length} 耗时=${((Date.now() - t0) / 1000).toFixed(1)}s`);
}

function pushFrame(tracks, name, dx, dy, drot, vis) {
  if (!tracks[name]) tracks[name] = [];
  tracks[name].push([dx, dy, drot, vis]);
}

// 子部件槽的候选父槽名（按命名约定剥离后缀；父槽不存在则不跟随）
function followParentName(name) {
  let m = name.match(/^(.+)_II_[A-Z]$/);   // F_R_Hand_II_A → F_R_Hand
  if (m) return m[1];
  m = name.match(/^(.+)_(B|C)$/);          // F_Foot_L_B → F_Foot_L
  if (m) return m[1];
  m = name.match(/^(.+)b$/);               // F_R_Handb → F_R_Hand
  if (m) return m[1];
  return null;
}

// 组跟随：子部件层轨道 dx/dy/drot 用主部件轨道（vis 保留各自的）
function applyFollow(anims, layers) {
  const byName = new Map(layers.map(l => [l.name, l]));
  const overlap = (a, b) => {
    const ix = Math.max(0, Math.min(a.bbox[2], b.bbox[2]) - Math.max(a.bbox[0], b.bbox[0]));
    const iy = Math.max(0, Math.min(a.bbox[3], b.bbox[3]) - Math.max(a.bbox[1], b.bbox[1]));
    return (ix * iy) / (a.w * a.h || 1);
  };
  let n = 0;
  for (const L of layers) {
    const parent = followParentName(L.name);
    if (!parent || !byName.has(parent)) continue;
    const pl = byName.get(parent);
    if (overlap(L, pl) < 0.5) continue;   // 子部件贴图须与主部件重叠过半
    for (const a of Object.values(anims)) {
      const pt = a.layers[parent], ct = a.layers[L.name];
      if (!pt || !ct) continue;
      for (let i = 0; i < ct.length && i < pt.length; i++) {
        ct[i] = [pt[i][0], pt[i][1], pt[i][2], ct[i][3]];
      }
    }
    n++;
  }
  console.log(`  组跟随: ${n} 个子部件层`);
}

// ---------- CLI ----------
const [setDir, outDir] = process.argv.slice(2);
if (!setDir || !outDir) {
  console.log('用法: node convert.mjs <spine形态目录> <输出目录>');
  process.exit(1);
}
convert(setDir, outDir).catch(e => { console.error(e); process.exit(1); });
