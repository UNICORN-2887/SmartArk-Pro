'use strict';
/* ============================================================
 * Spine 仿真器：直接用官方 spine-canvas 3.8.99 运行时渲染
 * 原始 .skel/.atlas/.png 资源，不经 PPD 转换。
 * 画布 800×800（比 480×800 宽，动作不易出画布）；切换动画时
 * 自动按该动画 t=0 姿势 fit 相机。
 * ============================================================ */

const CW = 800, CH = 800;

const el = {};
for (const id of ['canvas', 'stage', 'view', 'charSel', 'formSel', 'animSel', 'btnPlay', 'btnStop',
  'seek', 'timeLabel', 'loopBadge', 'zoomSel', 'fitSel', 'bgSel', 'btnExport', 'stageHint']) {
  el[id] = document.getElementById(id);
}
const canvas = el.canvas;
const ctx = canvas.getContext('2d');

const st = {
  data: null,          // window.SPINE_DATA
  char: '',
  form: '',
  skelData: null,
  skeleton: null,
  state: null,         // AnimationState
  animName: '',
  playing: false,
  cam: { scale: 1, tx: 0, ty: 0 },
  fitExtra: 1.0,       // 用户角色缩放
  zoom: 0.7,
  lastTs: null,
  time: 0,
};

/* ---------------- 工具 ---------------- */
function b64ToBytes(b64) {
  const bin = atob(b64);
  const buf = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
  return buf;
}
function loadPng(b64) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = 'data:image/png;base64,' + b64;
  });
}

/* ---------------- 加载角色形态 ---------------- */
async function loadForm(char, form) {
  const files = st.data[char][form];
  if (!files) throw new Error(`无 ${char}/${form}`);
  const atlasText = new TextDecoder('utf-8').decode(b64ToBytes(files.atlas)).replace(/\r\n/g, '\n');
  const images = new Map();
  for (const [name, b64] of Object.entries(files.pngs || {})) {
    images.set(name, await loadPng(b64));
  }
  spine.Texture.prototype.setFilters = function () {};
  spine.Texture.prototype.setWraps = function () {};
  // atlas 下载缩放处理（与 convert.mjs 相同）
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
  const skelBytes = b64ToBytes(files.skel);
  const skelData = new spine.SkeletonBinary(loader).readSkeletonData(skelBytes);
  return { skelData, atlas, images };
}

function makeSkeleton(skelData) {
  const s = new spine.Skeleton(skelData);
  s.setToSetupPose();
  s.updateWorldTransform();
  return s;
}

/* ---------------- 相机：按姿势 fit 到画布 ---------------- */
function fitCamera(skeleton, extra) {
  // 在临时大画布渲染整身 → bbox → 适配 CW×CH（渲染不翻转，显示时翻转——
  // 翻转为绕画布中心的对称，bbox 中心不变，相机居中语义不受影响）
  const BIG = 2048;
  const c = document.createElement('canvas');
  c.width = BIG; c.height = BIG;
  const g = c.getContext('2d');
  g.translate(BIG / 2, BIG / 2);
  const r = new spine.canvas.SkeletonRenderer(g);
  r.triangleRendering = true;
  r.draw(skeleton);
  const d = g.getImageData(0, 0, BIG, BIG).data;
  let minX = 1e9, minY = 1e9, maxX = -1, maxY = -1;
  for (let y = 0; y < BIG; y++) for (let x = 0; x < BIG; x++) {
    if (d[(y * BIG + x) * 4 + 3] > 0) {
      minX = Math.min(minX, x); maxX = Math.max(maxX, x);
      minY = Math.min(minY, y); maxY = Math.max(maxY, y);
    }
  }
  if (minX === 1e9) { st.cam = { scale: 1, tx: 0, ty: 0 }; return; }
  const bw = maxX - minX + 1, bh = maxY - minY + 1;
  const scale = Math.min(CW / bw, CH / bh) * extra;
  st.cam = {
    scale,
    tx: (CW - bw * scale) / 2 - (minX - BIG / 2) * scale,
    ty: (CH - bh * scale) / 2 - (minY - BIG / 2) * scale,
  };
}

function applyCamToSkeleton(s) {
  s.x = st.cam.tx; s.y = st.cam.ty;
  s.scaleX = st.cam.scale; s.scaleY = st.cam.scale;
  s.updateWorldTransform();
}

/* ---------------- 渲染 ---------------- */
function drawSkeleton() {
  ctx.clearRect(0, 0, CW, CH);
  ctx.save();
  // spine 世界坐标 y 向上、canvas y 向下 → 绕画布中心镜像 y（与官方 player 一致）
  ctx.translate(0, CH);
  ctx.scale(1, -1);
  const r = new spine.canvas.SkeletonRenderer(ctx);
  r.triangleRendering = true;
  r.draw(st.skeleton);
  ctx.restore();
}

function updateTimeUI() {
  const dur = st.animName ? st.skeleton.data.findAnimation(st.animName).duration : 0;
  if (st.animName) {
    el.seek.disabled = false;
    el.seek.value = Math.round(st.time / dur * 1000);
    el.timeLabel.textContent = `${st.time.toFixed(2)} / ${dur.toFixed(2)}s`;
  } else {
    el.seek.disabled = true;
    el.seek.value = 0;
    el.timeLabel.textContent = '0.00 / -';
  }
}

/* ---------------- 主循环 ---------------- */
function frame(ts) {
  if (st.playing && st.state && st.animName) {
    const dt = st.lastTs != null ? Math.min((ts - st.lastTs) / 1000, 0.1) : 0;
    st.state.update(dt);
    st.state.apply(st.skeleton);
    applyCamToSkeleton(st.skeleton);   // 动画可能覆盖 root 骨骼（相机变换），每帧重套
    st.time = st.state.getCurrent(0).getAnimationTime();   // 3.8.75 字段：trackTime/getAnimationTime（3.8.99 才叫 time）
    updateTimeUI();
  }
  st.lastTs = ts;
  if (st.skeleton) drawSkeleton();
  requestAnimationFrame(frame);
}

/* ---------------- UI ---------------- */
function rebuildAnims() {
  el.animSel.innerHTML = '';
  el.animSel.add(new Option('（默认姿势）', ''));
  for (const a of st.skelData.animations) el.animSel.add(new Option(a.name, a.name));
}

function rebuildForms() {
  el.formSel.innerHTML = '';
  for (const f of Object.keys(st.data[st.char])) el.formSel.add(new Option(f, f));
}

async function applyLoaded(char, form) {
  const { skelData } = await loadForm(char, form);
  st.char = char; st.form = form;
  st.skelData = skelData;
  st.skeleton = makeSkeleton(skelData);
  st.state = new spine.AnimationState(new spine.AnimationStateData(skelData));
  st.animName = '';
  st.playing = false;
  st.lastTs = null;
  st.time = 0;
  rebuildAnims();
  // 默认姿势 fit
  fitCamera(st.skeleton, st.fitExtra);
  applyCamToSkeleton(st.skeleton);
  drawSkeleton();
  updateTimeUI();
  el.stageHint.style.display = 'none';
  el.loopBadge.textContent = '';
}

function playAnim(name) {
  if (!st.skeleton) return;
  st.animName = name;
  st.state.clearTracks();
  if (name) {
    const loop = /(Loop|Idle|Default)/i.test(name);
    st.state.setAnimation(0, name, loop);
    st.time = 0;
    // 切换动画：按该动画 t=0 姿势重 fit
    const s = makeSkeleton(st.skelData);
    st.skelData.findAnimation(name).apply(s, 0, 0, false, null, 1, spine.MixBlend.replace, 1);
    s.updateWorldTransform();
    fitCamera(s, st.fitExtra);
    st.state.update(0);
    st.state.apply(st.skeleton);
    applyCamToSkeleton(st.skeleton);
    el.loopBadge.textContent = loop ? '循环' : '单次';
  } else {
    st.skeleton = makeSkeleton(st.skelData);
    fitCamera(st.skeleton, st.fitExtra);
    applyCamToSkeleton(st.skeleton);
    el.loopBadge.textContent = '';
  }
  updateTimeUI();
  drawSkeleton();
}

/* ---------------- 事件 ---------------- */
el.charSel.onchange = async () => {
  rebuildForms();
  try { await applyLoaded(el.charSel.value, el.formSel.value); } catch (e) { showError(e); }
};
el.formSel.onchange = async () => {
  try { await applyLoaded(el.charSel.value, el.formSel.value); } catch (e) { showError(e); }
};
el.animSel.onchange = () => {
  st.playing = false;
  playAnim(el.animSel.value);
  el.btnPlay.textContent = '▶ 播放';
};
el.btnPlay.onclick = () => {
  if (!st.animName) { el.stageHint.textContent = '请先选择动画'; return; }
  st.playing = !st.playing;
  el.btnPlay.textContent = st.playing ? '⏸ 暂停' : '▶ 播放';
  st.lastTs = null;
};
el.btnStop.onclick = () => {
  st.playing = false;
  el.btnPlay.textContent = '▶ 播放';
  st.state.clearTracks();
  st.skeleton = makeSkeleton(st.skelData);
  applyCamToSkeleton(st.skeleton);
  st.time = 0;
  updateTimeUI();
  drawSkeleton();
};
el.seek.oninput = () => {
  if (!st.animName) return;
  const dur = st.skelData.findAnimation(st.animName).duration;
  st.time = el.seek.value / 1000 * dur;
  st.state.clearTracks();
  const loop = /(Loop|Idle|Default)/i.test(st.animName);
  st.state.setAnimation(0, st.animName, loop);
  st.state.update(0);
  st.state.getCurrent(0).trackTime = st.time;   // 3.8.75 字段：trackTime
  st.state.apply(st.skeleton);
  applyCamToSkeleton(st.skeleton);
  drawSkeleton();
};
el.zoomSel.onchange = () => { st.zoom = parseFloat(el.zoomSel.value); applyView(); };
el.fitSel.onchange = () => {
  st.fitExtra = parseFloat(el.fitSel.value);
  // 重 fit 当前姿势
  const s = makeSkeleton(st.skelData);
  if (st.animName) {
    st.skelData.findAnimation(st.animName).apply(s, 0, st.time, false, null, 1, spine.MixBlend.replace, 1);
  }
  s.updateWorldTransform();
  fitCamera(s, st.fitExtra);
  applyCamToSkeleton(st.skeleton);
  drawSkeleton();
};
el.bgSel.onchange = () => {
  canvas.classList.remove('bg-checker', 'bg-black');
  if (el.bgSel.value !== 'white') canvas.classList.add('bg-' + el.bgSel.value);
};
el.btnExport.onclick = () => {
  const c = document.createElement('canvas');
  c.width = CW; c.height = CH;
  const g = c.getContext('2d');
  g.fillStyle = '#ffffff';
  g.fillRect(0, 0, CW, CH);
  g.drawImage(canvas, 0, 0);
  c.toBlob(b => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(b);
    a.download = 'spine_frame.png';
    a.click();
    URL.revokeObjectURL(a.href);
  });
};

function applyView() {
  el.stage.style.transform = 'scale(' + st.zoom + ')';
}

/* ---------------- 启动 ---------------- */
window.onerror = (msg, src, line) => {
  el.stageHint.style.display = 'block';
  el.stageHint.textContent = `JS 错误: ${msg} @${line}`;
};
applyView();
requestAnimationFrame(frame);

function showError(e) {
  el.stageHint.style.display = 'block';
  el.stageHint.textContent = '加载失败: ' + (e && e.message ? e.message : e);
}

if (window.SPINE_DATA) {
  st.data = window.SPINE_DATA;
  for (const c of Object.keys(st.data)) el.charSel.add(new Option(c, c));
  st.char = el.charSel.value;              // 必须先设 char，rebuildForms 读 st.data[st.char]
  rebuildForms();
  applyLoaded(el.charSel.value, el.formSel.value).catch(showError);
} else {
  el.stageHint.textContent = '无内置数据（data_spine.js 未加载）';
}
