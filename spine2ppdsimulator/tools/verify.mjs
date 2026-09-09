// 验收脚本：PPD 回放 vs spine 官方整身渲染，逐采样点像素 diff（交接文档标准：<5%）
// 用法: node verify.mjs <ppd目录> <spine形态目录> [动画名]
// 回放严格按仿真器公式（层中心平移+旋转、线性插值、vis 硬切、z 升序、无分组变换）
import fs from 'fs';
import path from 'path';
import { createCanvas, loadImage, ImageData } from '@napi-rs/canvas';

const W = 480, H = 800;
const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const ppdDir = process.argv[2];
const setDir = process.argv[3];
const onlyAnim = process.argv[4];

// ---------- PPD 加载 ----------
const scene = JSON.parse(fs.readFileSync(path.join(ppdDir, 'scene.json'), 'utf8'));
const anims = JSON.parse(fs.readFileSync(path.join(ppdDir, 'anims.json'), 'utf8'));
const textures = new Map();
for (const L of scene.layers) {
  const buf = fs.readFileSync(path.join(ppdDir, L.name + '.raw'));
  const w = buf.readUInt16LE(0), h = buf.readUInt16LE(2);
  const img = new Uint8ClampedArray(w * h * 4);
  buf.copy(img, 0, 4);
  const c = createCanvas(w, h);
  const g = c.getContext('2d');
  g.putImageData(new ImageData(img, w, h), 0, 0);
  textures.set(L.name, c);
}

// ---------- spine 加载（与 convert 相同） ----------
async function loadSpine() {
  const files = fs.readdirSync(setDir);
  const skelF = files.find(f => f.endsWith('.skel')) || files.find(f => f.endsWith('.json'));
  const stem = skelF.replace(/\.(skel|json)$/, '');
  const atlasText = fs.readFileSync(path.join(setDir, stem + '.atlas'), 'utf8').replace(/\r\n/g, '\n');
  const images = new Map();
  await Promise.all([...new Set([...atlasText.matchAll(/^([\w.-]+\.png)$/gm)].map(m => m[1]))].map(async name => {
    images.set(name, await loadImage(fs.readFileSync(path.join(setDir, name))));
  }));
  spine.Texture.prototype.setFilters = function () {};
  spine.Texture.prototype.setWraps = function () {};
  // 下载渠道缩放：atlas 声明尺寸 vs png 实际（与 convert.mjs 相同逻辑）
  const declaredSize = new Map();
  for (const m of atlasText.matchAll(/^([\w.-]+\.png)[\s\S]*?size:\s*(\d+)\s*,\s*(\d+)/gm)) {
    declaredSize.set(m[1], { w: +m[2], h: +m[3] });
  }
  const atlas = new spine.TextureAtlas(atlasText, p => new spine.canvas.CanvasTexture(images.get(p)));
  for (const page of atlas.pages) {
    const img = images.get(page.name);
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
  const loader = new spine.AtlasAttachmentLoader(atlas);
  const skelData = skelF.endsWith('.skel')
    ? new spine.SkeletonBinary(loader).readSkeletonData(new Uint8Array(fs.readFileSync(path.join(setDir, skelF))))
    : new spine.SkeletonJson(loader).readSkeletonData(fs.readFileSync(path.join(setDir, skelF), 'utf8'));
  // 相机：直接读 scene.json 的 camera 字段（convert 写入，保证与转换端一致）
  if (!scene.camera) throw new Error('scene.json 无 camera 字段，请用新版 convert.mjs 重新转换');
  const cam = scene.camera;
  return { skelData, cam };
}
const { skelData, cam } = await loadSpine();

function spineFrame(ad, t, loop) {
  const s = new spine.Skeleton(skelData);
  s.setToSetupPose();
  s.x = cam.tx; s.y = cam.ty; s.scaleX = cam.scale; s.scaleY = cam.scale;
  ad.apply(s, 0, t, loop, null, 1, spine.MixBlend.replace, 1);
  s.updateWorldTransform();
  const c = createCanvas(W, H);
  const ctx = c.getContext('2d');
  const r = new spine.canvas.SkeletonRenderer(ctx);
  r.triangleRendering = true;
  r.draw(s);
  return c;
}

// ---------- PPD 回放（仿真器公式逐字复刻） ----------
function sampleTrack(track, duration, t) {
  const n = track.length;
  if (n < 2 || duration <= 0) { const k0 = track[0]; return { dx: k0[0], dy: k0[1], drot: k0[2], vis: k0[3] >= 0.5 }; }
  const pos = Math.min(Math.max(t / duration * (n - 1), 0), n - 1);
  const i0 = Math.floor(pos);
  const frac = pos - i0;
  const k0 = track[i0], k1 = track[Math.min(i0 + 1, n - 1)];
  return {
    dx: k0[0] + (k1[0] - k0[0]) * frac,
    dy: k0[1] + (k1[1] - k0[1]) * frac,
    drot: k0[2] + (k1[2] - k0[2]) * frac,
    vis: k0[3] + (k1[3] - k0[3]) * frac >= 0.5,
  };
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

function diff(a, b) {
  const da = a.getContext('2d').getImageData(0, 0, W, H).data;
  const db = b.getContext('2d').getImageData(0, 0, W, H).data;
  let union = 0, bad = 0;
  for (let i = 0; i < da.length; i += 4) {
    const aa = da[i + 3] > 0, ba = db[i + 3] > 0;
    if (!aa && !ba) continue;
    union++;
    if (aa !== ba) { bad++; continue; }
    if (Math.abs(da[i] - db[i]) + Math.abs(da[i + 1] - db[i + 1]) + Math.abs(da[i + 2] - db[i + 2]) > 30) bad++;
  }
  return { union, bad, ratio: bad / (union || 1) };
}

// ---------- 逐动画验收 ----------
const SAMPLE_N = 6;
const names = onlyAnim ? [onlyAnim] : Object.keys(anims);
for (const name of names) {
  const ad = skelData.animations.find(a => a.name === name);
  const A = anims[name];
  if (!ad) { console.log(`${name}: spine 中不存在（跳过）`); continue; }
  let worst = 0;
  const ratios = [];
  for (let k = 0; k < SAMPLE_N; k++) {
    const t = A.duration * k / (SAMPLE_N - 1);
    const ref = spineFrame(ad, t, A.loop);
    const rep = replayFrame(A, t);
    const d = diff(ref, rep);
    ratios.push(d.ratio);
    worst = Math.max(worst, d.ratio);
  }
  const avg = ratios.reduce((a, b) => a + b, 0) / ratios.length;
  const ok = worst < 0.05;
  console.log(`${ok ? '✅' : '❌'} ${name}: 最差=${(worst * 100).toFixed(1)}% 平均=${(avg * 100).toFixed(1)}% (采样点 ${ratios.map(r => (r * 100).toFixed(1) + '%').join(', ')})`);
}
