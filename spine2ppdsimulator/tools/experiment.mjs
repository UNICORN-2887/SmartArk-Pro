// 实验：对比两种 PPD 位置估计方法在 mesh 形变层上的误差
//  A. 骨骼轨迹（贴图中心随骨骼刚体变换）——当前 convert.mjs 方法
//  B. 像素扫描（贴图左上随扫描 bbox 左上）——跟踪形变后的内容位置
// 基准：官方渲染该槽的 bbox 中心
// 用法: node experiment.mjs <ppd目录> <spine形态目录> <动画名> [采样点数]
import fs from 'fs';
import path from 'path';
import { createCanvas, loadImage } from '@napi-rs/canvas';

const W = 480, H = 800;
const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const [ppdDir, setDir, ANIM] = process.argv.slice(2);
const N = parseInt(process.argv[5] || '5');

const scene = JSON.parse(fs.readFileSync(path.join(ppdDir, 'scene.json'), 'utf8'));
const layerOf = new Map(scene.layers.map(l => [l.name, l]));

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
const ad = skelData.animations.find(a => a.name === ANIM);

function makeSkeleton() {
  const s = new spine.Skeleton(skelData);
  s.setToSetupPose();
  s.updateWorldTransform();
  return s;
}
function applyFrameT(t, animName) {
  const s = makeSkeleton();
  s.x = cam.tx; s.y = cam.ty; s.scaleX = cam.scale; s.scaleY = cam.scale;
  const anim = animName ? skelData.findAnimation(animName) : ad;
  anim.apply(s, 0, t, false, null, 1, spine.MixBlend.replace, 1);
  s.updateWorldTransform();
  return s;
}
function applyFrame(t) { return applyFrameT(t, null); }
function boneWorld(slot) {
  const b = slot.bone;
  return { a: b.a, b: b.b, c: b.c, d: b.d, x: b.worldX, y: b.worldY };
}
// 贴图姿势参考（贴图 = 贴图姿势渲染；参考骨骼变换需重算——用 applyFrame(0) 当参考姿势
// 因为贴图姿势 = Idle t=0，而实验动画 t=0 姿势可能与贴图姿势不同。
// 简化：用 applyFrame(0) 的骨骼与扫描作为参考）
const canvas = createCanvas(W, H);
function renderSlot(skeleton, name) {
  const slot = skeleton.slots.find(sl => sl.data.name === name);
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
  } finally { skeleton.drawOrder = saved; }
  const d = ctx.getImageData(0, 0, W, H).data;
  let n = 0, minX = 1e9, minY = 1e9, maxX = -1, maxY = -1;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (d[(y * W + x) * 4 + 3] > 0) { n++; minX = Math.min(minX, x); maxX = Math.max(maxX, x); minY = Math.min(minY, y); maxY = Math.max(maxY, y); }
  }
  if (n < 1) return null;
  return { cx: (minX + maxX) / 2, cy: (minY + maxY) / 2, minX, minY, n };
}

// 参考帧：贴图姿势（与 convert 一致——基建取 Interact t=0）
const TEXANIM = 'Interact';
const sRef = applyFrameT(0, TEXANIM);
const refScan = new Map();
const refBone = new Map();
for (const L of scene.layers) {
  const m = renderSlot(sRef, L.name);
  if (m) { refScan.set(L.name, m); refBone.set(L.name, boneWorld(sRef.slots.find(sl => sl.data.name === L.name))); }
}

// 采样对比
const stats = { bone: [], scan: [] };
for (let k = 1; k < N; k++) {
  const t = ad.duration * k / (N - 1);
  const s = applyFrame(t);
  for (const L of scene.layers) {
    const official = renderSlot(s, L.name);
    const rS = refScan.get(L.name), rB = refBone.get(L.name);
    if (!official || !rS || !rB) continue;
    // 法 A：骨骼刚体
    const det = rB.a * rB.d - rB.b * rB.c;
    const ia = rB.d / det, ib = -rB.b / det, ic = -rB.c / det, id = rB.a / det;
    const cur = boneWorld(s.slots.find(sl => sl.data.name === L.name));
    const ra = cur.a * ia + cur.b * ic, rb = cur.a * ib + cur.b * id;
    const rc = cur.c * ia + cur.d * ic, rd = cur.c * ib + cur.d * id;
    const tx = cur.x - (ra * rB.x + rb * rB.y);
    const ty = cur.y - (rc * rB.x + rd * rB.y);
    const bx = (ra - 1) * rS.cx + rb * rS.cy + tx + rS.cx;
    const by = rc * rS.cx + (rd - 1) * rS.cy + ty + rS.cy;
    // 法 B：扫描 bbox 左上平移
    const sx = rS.cx + (official.minX - rS.minX);
    const sy = rS.cy + (official.minY - rS.minY);
    stats.bone.push(Math.hypot(official.cx - bx, official.cy - by));
    stats.scan.push(Math.hypot(official.cx - sx, official.cy - sy));
  }
}
const avg = a => a.reduce((x, y) => x + y, 0) / a.length;
const p95 = a => [...a].sort((x, y) => x - y)[Math.floor(a.length * 0.95)];
console.log(`${ANIM}: ${stats.bone.length} 层×帧样本`);
console.log(`  法A 骨骼刚体: 平均 ${avg(stats.bone).toFixed(1)}px, P95 ${p95(stats.bone).toFixed(1)}px`);
console.log(`  法B 像素扫描: 平均 ${avg(stats.scan).toFixed(1)}px, P95 ${p95(stats.scan).toFixed(1)}px`);
console.log(`  结论: ${avg(stats.scan) < avg(stats.bone) ? '扫描法更准' : '骨骼法更准'}`);
