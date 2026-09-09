// 诊断：t=0 帧 官方整身 vs PPD 回放，定位 50% diff 的具体形态
import fs from 'fs';
import path from 'path';
import { createCanvas, loadImage, ImageData } from '@napi-rs/canvas';

const W = 480, H = 800;
const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const ppdDir = process.argv[4] || 'ppd_out/缄默德克萨斯';
const setDir = process.argv[5] || 'spine_raw/缄默德克萨斯/正面';

const scene = JSON.parse(fs.readFileSync(path.join(ppdDir, 'scene.json'), 'utf8'));
const textures = new Map();
for (const L of scene.layers) {
  const buf = fs.readFileSync(path.join(ppdDir, L.name + '.raw'));
  const w = buf.readUInt16LE(0), h = buf.readUInt16LE(2);
  const img = new Uint8ClampedArray(w * h * 4);
  buf.copy(img, 0, 4);
  const c = createCanvas(w, h);
  c.getContext('2d').putImageData(new ImageData(img, w, h), 0, 0);
  textures.set(L.name, c);
}

// spine 官方 t=0（=setup）帧
const stem = fs.readdirSync(path.join(setDir)).find(f => f.endsWith('.skel')).replace(/\.skel$/, '');
const atlasText = fs.readFileSync(path.join(setDir, stem + '.atlas'), 'utf8').replace(/\r\n/g, '\n');
const image = await loadImage(fs.readFileSync(path.join(setDir, stem + '.png')));
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};
// 下载渠道缩放（与 convert.mjs 相同逻辑）
const declaredSize = new Map();
for (const m of atlasText.matchAll(/^([\w.-]+\.png)[\s\S]*?size:\s*(\d+)\s*,\s*(\d+)/gm)) {
  declaredSize.set(m[1], { w: +m[2], h: +m[3] });
}
const atlas = new spine.TextureAtlas(atlasText, p => new spine.canvas.CanvasTexture(image));
for (const page of atlas.pages) {
  const img = image;
  const ds = declaredSize.get(page.name);
  if (img && ds && (ds.w !== img.width || ds.h !== img.height)) {
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
const skelData = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas))
  .readSkeletonData(new Uint8Array(fs.readFileSync(path.join(setDir, stem + '.skel'))));

// 相机：读 scene.json 的 camera 字段（convert 写入，保证一致）
const cam = scene.camera;
if (!cam) throw new Error('scene.json 无 camera 字段，请用新版 convert.mjs 重新转换');

function spineFrame(ad, t) {
  const s = new spine.Skeleton(skelData);
  s.setToSetupPose();
  s.x = cam.tx; s.y = cam.ty; s.scaleX = cam.scale; s.scaleY = cam.scale;
  if (ad) ad.apply(s, 0, t, false, null, 1, spine.MixBlend.replace, 1);
  s.updateWorldTransform();
  const c = createCanvas(W, H);
  const r = new spine.canvas.SkeletonRenderer(c.getContext('2d'));
  r.triangleRendering = true;
  r.draw(s);
  return c;
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
function replayFrame(anim, t) {
  const c = createCanvas(W, H);
  const ctx = c.getContext('2d');
  ctx.clearRect(0, 0, W, H);
  const sorted = [...scene.layers].sort((a, b) => a.z - b.z);
  for (const L of sorted) {
    let dx = 0, dy = 0, drot = 0, vis = !!L.visible;
    if (anim) {
      const track = anim.layers && anim.layers[L.name];
      if (track) { const s = sampleTrack(track, anim.duration, t); dx = s.dx; dy = s.dy; drot = s.drot; vis = s.vis; }
    }
    if (!vis) continue;
    const tex = textures.get(L.name);
    if (!tex) continue;
    ctx.save();
    ctx.translate(L.x + L.w / 2 + dx, L.y + L.h / 2 + dy);
    ctx.rotate(drot * Math.PI / 180);
    ctx.translate(-L.w / 2, -L.h / 2);
    ctx.drawImage(tex, 0, 0, L.w, L.h);
    ctx.restore();
  }
  return c;
}

const ANIM = process.argv[2] || 'Die';
const T = parseFloat(process.argv[3] || '0.5');
const anims = JSON.parse(fs.readFileSync(path.join(ppdDir, 'anims.json'), 'utf8'));
const A = anims[ANIM];
const ad = skelData.animations.find(a => a.name === ANIM);
console.log(`动画 ${ANIM} t=${T} (duration=${A.duration})`);
const ref = spineFrame(ad, T);
const rep = replayFrame(A, T);
fs.writeFileSync('diag_ref.png', await ref.encode('png'));
fs.writeFileSync('diag_rep.png', await rep.encode('png'));
const da = ref.getContext('2d').getImageData(0, 0, W, H).data;
const db = rep.getContext('2d').getImageData(0, 0, W, H).data;
let bad = 0, total = 0;
const samples = [];
for (let i = 0; i < da.length; i += 4) {
  const aa = da[i + 3] > 0, ba = db[i + 3] > 0;
  if (!aa && !ba) continue;
  total++;
  const rgb = Math.abs(da[i] - db[i]) + Math.abs(da[i + 1] - db[i + 1]) + Math.abs(da[i + 2] - db[i + 2]);
  if (aa !== ba || rgb > 30) {
    bad++;
    if (samples.length < 8) {
      const px = (i / 4) % W, py = Math.floor(i / 4 / W);
      samples.push({ px, py, a: [da[i], da[i + 1], da[i + 2], da[i + 3]], b: [db[i], db[i + 1], db[i + 2], db[i + 3]] });
    }
  }
}
console.log(`总内容像素=${total} diff=${bad} (${(bad / total * 100).toFixed(1)}%)`);
console.log('diff 像素样例:');
for (const s of samples) {
  console.log(`  (${s.px},${s.py}) 官方RGBA=[${s.a}] 回放RGBA=[${s.b}]`);
}

// 逐层 diff：每层区域内的像素差异（区域按层当前帧轨道位置外扩）
console.log('\n逐层 diff（全部层，按 diff 降序）:');
const layerDiffs = [];
for (const L of scene.layers) {
  // 计算该层在当前帧的位置
  let dx = 0, dy = 0, drot = 0;
  const track = A.layers && A.layers[L.name];
  if (track) { const s = sampleTrack(track, A.duration, T); dx = s.dx; dy = s.dy; drot = s.drot; }
  const cx = L.x + L.w / 2 + dx, cy = L.y + L.h / 2 + dy;
  const rw = Math.abs(Math.cos(drot * Math.PI / 180)) * L.w + Math.abs(Math.sin(drot * Math.PI / 180)) * L.h;
  const rh = Math.abs(Math.sin(drot * Math.PI / 180)) * L.w + Math.abs(Math.cos(drot * Math.PI / 180)) * L.h;
  let lb = 0, lt = 0;
  for (let y = Math.max(0, Math.floor(cy - rh / 2) - 2); y < Math.min(H, Math.ceil(cy + rh / 2) + 2); y++) {
    for (let x = Math.max(0, Math.floor(cx - rw / 2) - 2); x < Math.min(W, Math.ceil(cx + rw / 2) + 2); x++) {
      const i = (y * W + x) * 4;
      const aa = da[i + 3] > 0, ba = db[i + 3] > 0;
      if (!aa && !ba) continue;
      lt++;
      const rgb = Math.abs(da[i] - db[i]) + Math.abs(da[i + 1] - db[i + 1]) + Math.abs(da[i + 2] - db[i + 2]);
      if (aa !== ba || rgb > 30) lb++;
    }
  }
  if (lt > 0) layerDiffs.push({ name: L.name, ratio: lb / lt, lb, lt, x: L.x, y: L.y, w: L.w, h: L.h, vis: L.visible, z: L.z, dx: r2(dx), dy: r2(dy), drot: r2(drot) });
}
function r2(v) { return Math.round(v * 10) / 10; }
layerDiffs.sort((a, b) => b.ratio - a.ratio);
for (const d of layerDiffs.slice(0, 15)) {
  console.log(`  ${d.name}: ${(d.ratio * 100).toFixed(0)}% (${d.lb}/${d.lt}) x=${d.x} y=${d.y} w=${d.w} h=${d.h} vis=${d.vis} z=${d.z} 帧dx=${d.dx} dy=${d.dy} drot=${d.drot}`);
}
