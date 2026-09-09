// 对比表：官方各姿态初始（t=0）各零件位置/角度 vs PPD 输出位置/角度
// 用法: node compare_table.mjs <ppd目录> <spine形态目录> <输出csv>
// 每层一行：官方渲染 bbox 中心 + PCA 视觉角 vs PPD 贴图中心 + drot，算位置差/角度差
import fs from 'fs';
import path from 'path';
import { createCanvas, loadImage } from '@napi-rs/canvas';

const W = 480, H = 800;
const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const [ppdDir, setDir, outCsv] = process.argv.slice(2);

// ---- PPD 加载 ----
const scene = JSON.parse(fs.readFileSync(path.join(ppdDir, 'scene.json'), 'utf8'));
const anims = JSON.parse(fs.readFileSync(path.join(ppdDir, 'anims.json'), 'utf8'));
const layerOf = new Map(scene.layers.map(l => [l.name, l]));

// ---- spine 加载（含 atlas 缩放） ----
const stem = fs.readdirSync(path.join(setDir)).find(f => f.endsWith('.skel')).replace(/\.skel$/, '');
const atlasText = fs.readFileSync(path.join(setDir, stem + '.atlas'), 'utf8').replace(/\r\n/g, '\n');
const image = await loadImage(fs.readFileSync(path.join(setDir, stem + '.png')));
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};
const declaredSize = new Map();
for (const m of atlasText.matchAll(/^([\w.-]+\.png)[\s\S]*?size:\s*(\d+)\s*,\s*(\d+)/gm)) {
  declaredSize.set(m[1], { w: +m[2], h: +m[3] });
}
const atlas = new spine.TextureAtlas(atlasText, p => new spine.canvas.CanvasTexture(image));
for (const page of atlas.pages) {
  const ds = declaredSize.get(page.name);
  if (image && ds && (ds.w !== image.width || ds.h !== image.height)) {
    const kx = image.width / ds.w, ky = image.height / ds.h;
    for (const r of atlas.regions) {
      if (r.page !== page) continue;
      r.u *= kx; r.v *= ky; r.u2 *= kx; r.v2 *= ky;
      r.x *= kx; r.y *= ky; r.width *= kx; r.height *= ky;
      r.originalWidth *= kx; r.originalHeight *= ky;
      if (r.offsetX) { r.offsetX *= kx; r.offsetY *= ky; }
      if (r.uvs) { for (let i = 0; i < r.uvs.length; i += 2) { r.uvs[i] *= kx; r.uvs[i + 1] *= ky; } }
    }
    page.width = image.width; page.height = image.height;
  }
}
const skelData = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas))
  .readSkeletonData(new Uint8Array(fs.readFileSync(path.join(setDir, stem + '.skel'))));
const cam = scene.camera;

function makeSkeleton() {
  const s = new spine.Skeleton(skelData);
  s.setToSetupPose();
  s.updateWorldTransform();
  return s;
}
function applyFrame(ad, t) {
  const s = makeSkeleton();
  s.x = cam.tx; s.y = cam.ty; s.scaleX = cam.scale; s.scaleY = cam.scale;
  if (ad) ad.apply(s, 0, t, false, null, 1, spine.MixBlend.replace, 1);
  s.updateWorldTransform();
  return s;
}

// 渲染单槽 → bbox 中心 + PCA 视觉角
const canvas = createCanvas(W, H);
function renderSlotInfo(skeleton, slotName) {
  const slot = skeleton.slots.find(sl => sl.data.name === slotName);
  if (!slot) return null;
  const att = slot.getAttachment();
  if (!att) return null;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, W, H);
  const saved = skeleton.drawOrder;
  skeleton.drawOrder = [slot];
  try {
    const r = new spine.canvas.SkeletonRenderer(ctx);
    r.triangleRendering = true;
    r.draw(skeleton);
  } finally {
    skeleton.drawOrder = saved;
  }
  const d = ctx.getImageData(0, 0, W, H).data;
  let n = 0, minX = 1e9, minY = 1e9, maxX = -1, maxY = -1, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (d[(y * W + x) * 4 + 3] > 0) {
      n++; sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
      minX = Math.min(minX, x); maxX = Math.max(maxX, x);
      minY = Math.min(minY, y); maxY = Math.max(maxY, y);
    }
  }
  if (n < 1) return null;
  const mx = sx / n, my = sy / n;
  const cxx = sxx / n - mx * mx, cyy = syy / n - my * my, cxy = sxy / n - mx * my;
  const tr = cxx + cyy, disc = Math.sqrt(Math.max(tr * tr / 4 - (cxx * cyy - cxy * cxy), 0));
  const l1 = tr / 2 + disc, l2 = tr / 2 - disc;
  let ang = null;
  if (l1 > 1e-9 && l2 / l1 < 0.5) ang = Math.atan2(2 * cxy, cxx - cyy) / 2 * 180 / Math.PI;
  return { cx: mx, cy: my, ang, count: n, w: maxX - minX + 1, h: maxY - minY + 1 };
}

function sampleTrack(track, duration, t) {
  const n = track.length;
  if (n < 2 || duration <= 0) { const k0 = track[0]; return { dx: k0[0], dy: k0[1], drot: k0[2], vis: k0[3] >= 0.5 }; }
  const pos = Math.min(Math.max(t / duration * (n - 1), 0), n - 1);
  const i0 = Math.floor(pos);
  const frac = pos - i0;
  const k0 = track[i0], k1 = track[Math.min(i0 + 1, n - 1)];
  return { dx: k0[0] + (k1[0] - k0[0]) * frac, dy: k0[1] + (k1[1] - k0[1]) * frac, drot: k0[2] + (k1[2] - k0[2]) * frac, vis: k0[3] + (k1[3] - k0[3]) * frac >= 0.5 };
}
const angDiff = (a, b) => { let d = ((a - b) % 180 + 180 + 90) % 180 - 90; if (d > 90) d -= 180; if (d <= -90) d += 180; return d; };

// ---- 对比 ----
const rows = [];
for (const name of Object.keys(anims)) {
  const A = anims[name];
  const ad = skelData.animations.find(a => a.name === name);
  const s = applyFrame(ad, 0);
  for (const L of scene.layers) {
    const official = renderSlotInfo(s, L.name);
    const track = A.layers[L.name];
    const ppd = track ? sampleTrack(track, A.duration, 0) : { dx: 0, dy: 0, drot: 0, vis: L.visible };
    const ocx = official ? official.cx : NaN, ocy = official ? official.cy : NaN;
    const pcx = L.x + L.w / 2 + ppd.dx, pcy = L.y + L.h / 2 + ppd.dy;
    const posDiff = official && ppd.vis ? Math.hypot(ocx - pcx, ocy - pcy) : NaN;
    // PPD 角度 = drot（相对贴图姿势）；官方视觉角与贴图姿势视觉角差对比
    const baseTrack = A.layers[L.name];
    const baseAng = null; // 贴图姿势视觉角在该动画 t=0 与 drot 的对照见下
    // 官方视觉角差 = 官方 t=0 视觉角（无向对齐 drot 方向）
    let angDiffVal = NaN;
    if (official && official.ang != null && ppd.vis) {
      let d = angDiff(official.ang, 0);  // 相对贴图姿势的近似（贴图姿势无向角记为 0 基准不成立）
      // 无法直接对比：官方视觉角 vs PPD drot 需要贴图姿势视觉角。简化：对比"帧视觉角 - 贴图姿势视觉角"
      const base = renderSlotInfo(applyFrame(null, 0), L.name);
      if (base && base.ang != null) angDiffVal = angDiff(official.ang, base.ang);
    }
    rows.push({
      anim: name, layer: L.name,
      ox: +ocx.toFixed(1), oy: +ocy.toFixed(1), oAng: official && official.ang != null ? +official.ang.toFixed(1) : '',
      px: +pcx.toFixed(1), py: +pcy.toFixed(1), pAng: +ppd.drot.toFixed(1), vis: ppd.vis ? 1 : 0,
      posDiff: isNaN(posDiff) ? '' : +posDiff.toFixed(1),
      angDiff: angDiffVal === NaN || angDiffVal === null ? '' : +angDiffVal.toFixed(1),
    });
  }
}

const head = '动画,层,官方位置x,官方位置y,官方视觉角,PPD位置x,PPD位置y,PPD角度,PPD可见,位置差px,官方视觉角差';
const lines = [head];
for (const r of rows) {
  lines.push([r.anim, r.layer, r.ox, r.oy, r.oAng, r.px, r.py, r.pAng, r.vis, r.posDiff, r.angDiff].join(','));
}
fs.writeFileSync(path.join(DIR, outCsv), '﻿' + lines.join('\n'), 'utf8');
console.log(`写入 ${outCsv}: ${rows.length} 行`);

// 摘要：位置差最大的层（官方有内容且 PPD 可见）
const worst = rows.filter(r => r.posDiff !== '').sort((a, b) => b.posDiff - a.posDiff).slice(0, 15);
console.log('\n位置差最大的 15 行:');
for (const r of worst) console.log(`  ${r.anim}/${r.layer}: ${r.posDiff}px (官方 ${r.ox},${r.oy} vs PPD ${r.px},${r.py}, drot ${r.pAng})`);
