// 调查：轨迹差构成分析
// 对动画逐帧逐层采样三个位置：
//   B = 槽骨骼世界位置（官方轨迹）
//   P = PPD 回放位置（贴图中心 + 轨道 dx/dy，仿真器公式）
//   O = 官方渲染该槽内容的 bbox 中心（含 deform 形变效应）
// 指标：
//   |P - B| = 算法误差（轨迹转换是否忠实于骨骼）
//   |O - B| = deform 偏移（mesh 形变让内容中心偏离骨骼）
//   |O - P| = 总误差（用户在画面上看到的）
// 用法: node investigate.mjs <ppd目录> <spine形态目录> <动画名> <采样点数>
import fs from 'fs';
import path from 'path';
import { createCanvas, loadImage } from '@napi-rs/canvas';

const W = 480, H = 800;
const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const [ppdDir, setDir, ANIM] = process.argv.slice(2);
const N = parseInt(process.argv[5] || '8');

const scene = JSON.parse(fs.readFileSync(path.join(ppdDir, 'scene.json'), 'utf8'));
const anims = JSON.parse(fs.readFileSync(path.join(ppdDir, 'anims.json'), 'utf8'));
const cam = scene.camera;

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
const ad = skelData.animations.find(a => a.name === ANIM);
const A = anims[ANIM];

function applyFrame(t) {
  const s = new spine.Skeleton(skelData);
  s.setToSetupPose();
  s.x = cam.tx; s.y = cam.ty; s.scaleX = cam.scale; s.scaleY = cam.scale;
  ad.apply(s, 0, t, false, null, 1, spine.MixBlend.replace, 1);
  s.updateWorldTransform();
  return s;
}
function sampleTrack(track, duration, t) {
  const n = track.length;
  if (n < 2 || duration <= 0) { const k0 = track[0]; return { dx: k0[0], dy: k0[1], vis: k0[3] >= 0.5 }; }
  const pos = Math.min(Math.max(t / duration * (n - 1), 0), n - 1);
  const i0 = Math.floor(pos);
  const frac = pos - i0;
  const k0 = track[i0], k1 = track[Math.min(i0 + 1, n - 1)];
  return { dx: k0[0] + (k1[0] - k0[0]) * frac, dy: k0[1] + (k1[1] - k0[1]) * frac, vis: k0[3] + (k1[3] - k0[3]) * frac >= 0.5 };
}

const canvas = createCanvas(W, H);
function officialCenter(skeleton, name) {
  const slot = skeleton.slots.find(sl => sl.data.name === name);
  if (!slot || !slot.getAttachment()) return null;
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
  let n = 0, sx = 0, sy = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (d[(y * W + x) * 4 + 3] > 0) { n++; sx += x; sy += y; }
  }
  if (n < 1) return null;
  return { cx: sx / n, cy: sy / n };
}

// 采样统计
const layerStats = new Map();   // 层 -> {algErr: [], defErr: [], totalErr: [], visible: []}
for (const L of scene.layers) layerStats.set(L.name, { algErr: [], defErr: [], totalErr: [], n: 0 });

for (let k = 0; k < N; k++) {
  const t = A.duration * k / (N - 1);
  const s = applyFrame(t);
  for (const L of scene.layers) {
    const track = A.layers[L.name];
    const ppd = track ? sampleTrack(track, A.duration, t) : { dx: 0, dy: 0, vis: L.visible };
    const slot = s.slots.find(sl => sl.data.name === L.name);
    if (!slot) continue;
    const O = officialCenter(s, L.name);
    if (!O) continue;
    const B = { x: slot.bone.worldX, y: slot.bone.worldY };
    const P = { x: L.x + L.w / 2 + ppd.dx, y: L.y + L.h / 2 + ppd.dy };
    const st = layerStats.get(L.name);
    st.algErr.push(Math.hypot(P.x - B.x, P.y - B.y));      // 算法误差
    st.defErr.push(Math.hypot(O.cx - B.x, O.cy - B.y));    // deform 偏移
    st.totalErr.push(Math.hypot(O.cx - P.x, O.cy - P.y));  // 总误差
    st.n++;
  }
}

const avg = a => a.reduce((x, y) => x + y, 0) / (a.length || 1);
let totalAlg = 0, totalDef = 0, totalErr = 0, cnt = 0;
const rows = [];
for (const [name, st] of layerStats) {
  if (st.n === 0) continue;
  totalAlg += avg(st.algErr); totalDef += avg(st.defErr); totalErr += avg(st.totalErr); cnt++;
  rows.push({ name, alg: avg(st.algErr), def: avg(st.defErr), err: avg(st.totalErr), n: st.n });
}
console.log(`=== ${ANIM} 轨迹差构成（${cnt} 层 × ${N} 采样帧，单位 px）===`);
console.log(`  算法误差 |P-B| 平均 = ${(totalAlg / cnt).toFixed(1)}px（轨迹转换忠实度）`);
console.log(`  deform 偏移 |O-B| 平均 = ${(totalDef / cnt).toFixed(1)}px（mesh 形变让内容偏离骨骼）`);
console.log(`  总误差 |O-P| 平均 = ${(totalErr / cnt).toFixed(1)}px（画面所见）`);
console.log();
rows.sort((a, b) => b.err - a.err);
console.log('总误差最大的 10 层：');
for (const r of rows.slice(0, 10)) {
  console.log(`  ${r.name.padEnd(16)} 总=${r.err.toFixed(1).padStart(6)}  算法=${r.alg.toFixed(1).padStart(6)}  deform=${r.def.toFixed(1).padStart(6)}`);
}
console.log();
console.log('算法误差最大的 10 层（若算法有 bug 看这里）：');
rows.sort((a, b) => b.alg - a.alg);
for (const r of rows.slice(0, 10)) {
  console.log(`  ${r.name.padEnd(16)} 算法=${r.alg.toFixed(1).padStart(6)}  deform=${r.def.toFixed(1).padStart(6)}  总=${r.err.toFixed(1).padStart(6)}`);
}
