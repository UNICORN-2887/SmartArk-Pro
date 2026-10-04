// spine → PPD 转换器（本地重写版）
// 用法: node convert.mjs [--prefix F_|B_] [--pose setup|default] <spine形态目录> <输出目录>
//   spine形态目录: 含 <stem>.skel|.json + <stem>.atlas + <stem>.png（wiki_fetch 产物）
//   输出: scene.json + <槽名>.raw + anims.json（交接文档第二节格式）
//
// 算法（与服务器版语义一致）:
//   - 官方 spine-canvas 渲染器（triangleRendering=true）整身渲染 setup 姿势 → 像素扫描定相机
//   - 相机吸收进骨骼根变换（skeleton.x/y/scaleX/scaleY），ctx 恒等（绕开 @napi-rs/canvas 变换+clip 的坑）
//   - 每层贴图: setup 姿势逐槽渲染 → alpha 像素 bbox 裁剪；setup 无内容的槽从动画帧取贴图（visible=false）
//   - 动画轨道: 每帧逐槽渲染 → 像素扫描（bbox 左上对齐 + PCA 主轴方向），帧数 n=clamp(round(dur*20),8,80)
//   - 每槽参考帧 = 该动画内第一个有内容的帧（首帧增量 0）；近圆形层（λ2/λ1≥0.5）方向退化 → drot=0
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';

const require = createRequire(import.meta.url);
function loadCanvasModule() {
  try {
    return require('@napi-rs/canvas');
  } catch (e) {
    const dir = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
    return require(path.resolve(dir, '../../spine2ppdsimulator/tools/node_modules/@napi-rs/canvas'));
  }
}
const { createCanvas, loadImage } = loadCanvasModule();

const W = 480, H = 800;
const FPS = 20;
const MIN_PIX = 1;          // alpha 像素 ≥ 此数才算该槽可见
const PAD = 100;            // 每槽扫描区域 = 参考 bbox 外扩（防动画位移出界）
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
  const atlasSizes = parseAtlasPageSizes(atlasText);
  const images = new Map();
  await Promise.all([...new Set([...atlasText.matchAll(/^([\w.-]+\.png)$/gm)].map(m => m[1]))].map(async name => {
    images.set(name, await loadImage(fs.readFileSync(path.join(setDir, name))));
  }));
  spine.Texture.prototype.setFilters = function () {};
  spine.Texture.prototype.setWraps = function () {};
  const atlas = new spine.TextureAtlas(atlasText, p => new spine.canvas.CanvasTexture(images.get(p)));
  // atlas 声明尺寸与 png 实际尺寸不一致时按比例缩放 region 坐标（下载渠道缩放问题）
  for (const page of atlas.pages) {
    const img = images.get(page.name);
    const declared = atlasSizes.get(page.name);
    const iw = img && (img.width || img.naturalWidth);
    const ih = img && (img.height || img.naturalHeight);
    if (img && declared && (declared.w !== iw || declared.h !== ih)) {
      console.log(`  [atlas缩放] ${page.name}: 声明 ${declared.w}x${declared.h} vs 实际 ${iw}x${ih}`);
      const kx = iw / declared.w, ky = ih / declared.h;
      for (const r of atlas.regions) {
        if (r.page !== page) continue;
        r.u *= kx; r.v *= ky; r.u2 *= kx; r.v2 *= ky;
        r.x *= kx; r.y *= ky; r.width *= kx; r.height *= ky;
        r.originalWidth *= kx; r.originalHeight *= ky;
        if (r.offsetX) { r.offsetX *= kx; r.offsetY *= ky; }
        if (r.uvs) { for (let i = 0; i < r.uvs.length; i += 2) { r.uvs[i] *= kx; r.uvs[i + 1] *= ky; } }
      }
      page.width = iw; page.height = ih;
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

function parseAtlasPageSizes(text) {
  const out = new Map();
  const lines = text.split('\n');
  let page = null;
  for (const raw of lines) {
    const line = raw.trim();
    if (!line) {
      page = null;
      continue;
    }
    if (!raw.startsWith(' ') && !line.includes(':')) {
      page = line;
      continue;
    }
    if (page && line.startsWith('size:')) {
      const m = line.match(/size:\s*(\d+)\s*,\s*(\d+)/);
      if (m) out.set(page, { w: Number(m[1]), h: Number(m[2]) });
    }
  }
  return out;
}

// ---------- 像素扫描：bbox + 矩统计（一次遍历，供 PCA） ----------
function scanRegion(canvas, x0, y0, x1, y1) {
  const ctx = canvas.getContext('2d');
  const w = x1 - x0, h = y1 - y0;
  const d = ctx.getImageData(x0, y0, w, h).data;
  let minX = 1e9, minY = 1e9, maxX = -1, maxY = -1, count = 0;
  let sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
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
        sx += gx; sy += gy; sxx += gx * gx; syy += gy * gy; sxy += gx * gy;
      }
    }
  }
  if (count < MIN_PIX) return null;
  return { minX, minY, maxX, maxY, count, w: maxX - minX + 1, h: maxY - minY + 1, sx, sy, sxx, syy, sxy };
}

// 主轴角（度）：无向 PCA 主轴；退化（λ2/λ1≥0.5 或圆）返回 null
function pcaAngle(m) {
  const n = m.count;
  const mx = m.sx / n, my = m.sy / n;
  const cxx = m.sxx / n - mx * mx;
  const cyy = m.syy / n - my * my;
  const cxy = m.sxy / n - mx * my;
  const tr = cxx + cyy;
  const disc = Math.sqrt(Math.max(tr * tr / 4 - (cxx * cyy - cxy * cxy), 0));
  const l1 = tr / 2 + disc, l2 = tr / 2 - disc;
  if (l1 < 1e-9 || l2 / l1 >= 0.5) return null;
  return Math.atan2(2 * cxy, cxx - cyy) / 2 * 180 / Math.PI;
}

// 无向主轴角度差归一化到 (-90, 90]
function angDiff(a, b) {
  let d = ((a - b) % 180 + 180 + 90) % 180 - 90;
  if (d > 90) d -= 180;
  if (d <= -90) d += 180;
  return d;
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

// 半像素对齐：回放公式 translate(x + w/2 + dx, ...) 要求 x+w/2 为整数，
// 否则整层贴图被 Canvas 重采样（与官方渲染逐像素不一致）。
// bbox 的 (minX+maxX) 为偶数时中心是 .5 → 右侧/下侧补 1 像素透明列（左上不变，轨道 dx/dy 语义不变）
function fixAlign(m) {
  const mm = { ...m };
  if ((mm.minX + mm.maxX) % 2 === 0) { mm.maxX += 1; mm.w += 1; }
  if ((mm.minY + mm.maxY) % 2 === 0) { mm.maxY += 1; mm.h += 1; }
  return mm;
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

// ---------- 主流程 ----------
async function convert(setDir, outDir, opts = {}) {
  const t0 = Date.now();
  const { stem, skelData, makeSkeleton } = await loadSet(setDir);
  console.log(`[${path.basename(outDir)}] bones=${skelData.bones.length} slots=${skelData.slots.length} anims=${skelData.animations.length}`);
  fs.mkdirSync(outDir, { recursive: true });

  // 贴图姿势：所有动画共享的 t=0 待机姿势（优先 Idle t=0，与官方预览一致）；
  // setup 姿势与它四肢相对位置不同（实测差 82.6%），贴图必须取待机姿势
  const baseAnim = opts.pose === 'default'
    ? skelData.animations.find(a => a.name === 'Default')
    : null;
  const texAnim = opts.pose === 'setup'
    ? null
    : (baseAnim || skelData.animations.find(a => a.name === 'Idle' && a.duration > 0.001)
      || skelData.animations.find(a => a.duration > 0.001));
  const poseSkeleton = (anim, t) => {
    const s = makeSkeleton();
    if (anim) anim.apply(s, 0, t, false, null, 1, spine.MixBlend.replace, 1);
    s.updateWorldTransform();
    return s;
  };
  const sPose = poseSkeleton(texAnim, 0);
  console.log(`  贴图姿势 = ${texAnim ? texAnim.name + ' t=0' : 'setup'}`);

  const slotAllowed = name => !opts.prefix || name.startsWith(opts.prefix);
  if (opts.prefix) console.log(`  槽过滤 prefix=${opts.prefix}`);

  // 1. 相机标定：大画布渲染贴图姿势整身 → 像素扫描（世界坐标）
  const BIG = 2048;
  const big = createCanvas(BIG, BIG);
  {
    const ctx = big.getContext('2d');
    ctx.translate(BIG / 2, BIG / 2);
    let saved = null;
    if (opts.prefix) {
      saved = sPose.drawOrder;
      sPose.drawOrder = sPose.drawOrder.filter(slot => slotAllowed(slot.data.name));
      if (!sPose.drawOrder.length) throw new Error(`没有匹配 ${opts.prefix} 的槽`);
    }
    const r = new spine.canvas.SkeletonRenderer(ctx);
    r.triangleRendering = true;
    try {
      r.draw(sPose);
    } finally {
      if (saved) sPose.drawOrder = saved;
    }
  }
  const wb = scanRegion(big, 0, 0, BIG, BIG);
  if (!wb) throw new Error('贴图姿势无渲染内容');
  const scale = Math.min(W / wb.w, H / wb.h);
  const cam = {
    scale,
    tx: (W - wb.w * scale) / 2 - (wb.minX - BIG / 2) * scale,
    ty: (H - wb.h * scale) / 2 - (wb.minY - BIG / 2) * scale,
  };
  console.log(`  相机 scale=${scale.toFixed(3)} tx=${cam.tx.toFixed(1)} ty=${cam.ty.toFixed(1)}`);

  // 2. 贴图姿势逐槽导出贴图
  const canvas = createCanvas(W, H);
  const s0 = poseSkeleton(texAnim, 0);
  applyCam(s0, cam);
  const drawOrderBase = s0.drawOrder.map(slot => slot.data.name).filter(slotAllowed);
  console.log('  drawOrder 槽数 =', drawOrderBase.length);

  const layers = [];
  const raws = new Map();          // 层名 -> raw buffer
  const ref = new Map();           // 层名 -> {bbox, ang} 全局参考：
                                   //   贴图姿势有内容的层 = 贴图姿势（贴图同源）；
                                   //   贴图姿势无内容的层 = 全局首个有内容帧（贴图也从该帧裁剪）
  const slotOf0 = new Map();       // 槽名 -> slot 对象（slots 数组不随 DrawOrderTimeline 重排）
  s0.slots.forEach(sl => slotOf0.set(sl.data.name, sl));
  const zOf = new Map();           // 槽名 -> z（drawOrder 顺序）
  drawOrderBase.forEach((name, i) => zOf.set(name, i));
  for (let i = 0; i < drawOrderBase.length; i++) {
    const name = drawOrderBase[i];
    renderSlots(canvas, s0, [slotOf0.get(name)]);
    const m = scanRegion(canvas, 0, 0, W, H);
    const head = HEAD_RE.test(name);
    if (m) {
      const m2 = fixAlign(m);
      raws.set(name, cropRaw(canvas, m2.minX, m2.minY, m2.w, m2.h));
      layers.push(mkLayer(name, m2, i, head, true));
      ref.set(name, { bbox: m2, ang: pcaAngle(m2) });
    } else {
      // 贴图姿势无内容：留待动画帧取贴图（visible=false）
    }
  }
  console.log(`  贴图姿势有内容层 = ${layers.length}`);

  // 3. 动画轨道导出（顺带解决 setup 无内容槽的贴图与全局参考）
  const anims = {};
  for (const ad of skelData.animations) {
    if (ad.duration <= 0.001) { console.log(`  跳过空动画 ${ad.name} (duration=0)`); continue; }
    const loop = LOOP_RE.test(ad.name);
    const n = clamp(Math.round(ad.duration * FPS), 8, 80);
    const tracks = {};
    const prevDrot = new Map();     // 帧间连续性引导（每动画每层）
    const sA = makeSkeleton();
    applyCam(sA, cam);
    const slotOfA = new Map();      // 槽名 -> slot 对象（DrawOrderTimeline 会重排 drawOrder，slots 数组不变）
    sA.slots.forEach(sl => slotOfA.set(sl.data.name, sl));
    for (let i = 0; i < n; i++) {
      const t = ad.duration * i / (n - 1);
      sA.setToSetupPose();
      applyCam(sA, cam);
      if (baseAnim && baseAnim !== ad) {
        baseAnim.apply(sA, 0, 0, false, null, 1, spine.MixBlend.replace, 1);
      }
      // blend 必须传 MixBlend.replace：spine 3.8.99 的 Rotate/Scale/ShearTimeline 对 blend=undefined
      // 无 switch 匹配分支，中间采样全部不生效（表现为动画"只有位移在动"）
      ad.apply(sA, 0, t, loop, null, 1, spine.MixBlend.replace, 1);
      sA.updateWorldTransform();
      for (const name of drawOrderBase) {
        const slot = slotOfA.get(name);
        renderSlots(canvas, sA, [slot]);
        // 扫描区域：参考 bbox 外扩；无参考（或触界）用全画布兜底
        let m = null;
        const r = ref.get(name);
        if (r) {
          const x0 = Math.max(0, r.bbox.minX - PAD), y0 = Math.max(0, r.bbox.minY - PAD);
          const x1 = Math.min(W, r.bbox.maxX + PAD), y1 = Math.min(H, r.bbox.maxY + PAD);
          m = scanRegion(canvas, x0, y0, x1, y1);
          if (m && (m.minX <= x0 || m.maxX >= x1 - 1 || m.minY <= y0 || m.maxY >= y1 - 1)) {
            m = scanRegion(canvas, 0, 0, W, H);
          }
        } else {
          m = scanRegion(canvas, 0, 0, W, H);
        }
        if (!m) { pushFrame(tracks, name, 0, 0, 0, 0); continue; }
        if (!ref.has(name)) {
          // setup 无内容的槽：首个有内容帧 = 全局参考 + 贴图来源
          const m2 = fixAlign(m);
          ref.set(name, { bbox: m2, ang: pcaAngle(m2) });
          if (!raws.has(name)) {
            raws.set(name, cropRaw(canvas, m2.minX, m2.minY, m2.w, m2.h));
            layers.push(mkLayer(name, m2, zOf.get(name), HEAD_RE.test(name), false));
          }
        }
        const rr = ref.get(name);
        const ang = pcaAngle(m);
        // 帧间连续性约束：无向主轴 ±180 等价，用上一帧 drot 引导选择，避免 ±90 边界跳变
        let drot = (ang != null && rr.ang != null) ? angDiff(ang, rr.ang) : (prevDrot.get(name) || 0);
        const pd = prevDrot.get(name) || 0;
        while (drot - pd > 90) drot -= 180;
        while (drot - pd < -90) drot += 180;
        prevDrot.set(name, drot);
        pushFrame(tracks, name, r2(m.minX - rr.bbox.minX), r2(m.minY - rr.bbox.minY), r2(drot), 1);
      }
    }
    anims[ad.name] = { duration: ad.duration, loop, layers: tracks };
    console.log(`  ${ad.name}: duration=${ad.duration} loop=${loop} 帧=${n}`);
  }

  // 4. 全程无内容的槽：1×1 透明占位（raw 头 w=1,h=1）
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
  // z 可能重复（drawOrder 序号唯一，不会）。但层 sort 后 scene 里 z 保持原序号。

  // 5. 写输出
  cleanOutputFiles(outDir);
  fs.writeFileSync(path.join(outDir, 'scene.json'), JSON.stringify({ landscape: true, landscape_rot: 1, layers }));
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

function cleanOutputFiles(outDir) {
  if (!fs.existsSync(outDir)) return;
  for (const name of fs.readdirSync(outDir)) {
    const p = path.join(outDir, name);
    if (!fs.statSync(p).isFile()) continue;
    if (name === 'scene.json' || name === 'anims.json' || name.endsWith('.raw')) {
      fs.unlinkSync(p);
    }
  }
}

// ---------- CLI ----------
const args = process.argv.slice(2);
const opts = {};
const pos = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--prefix') {
    opts.prefix = args[++i];
  } else if (args[i] === '--pose') {
    opts.pose = args[++i];
  } else {
    pos.push(args[i]);
  }
}
const [setDir, outDir] = pos;
if (!setDir || !outDir) {
  console.log('用法: node convert.mjs [--prefix F_|B_] [--pose setup|default] <spine形态目录> <输出目录>');
  process.exit(1);
}
convert(setDir, outDir, opts).catch(e => { console.error(e); process.exit(1); });
