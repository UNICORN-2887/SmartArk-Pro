/* 拼豆拆分工具 — 前端交互（原生 JS，无依赖） */
'use strict';

const $ = (id) => document.getElementById(id);

const NAME_RE = /^[A-Za-z0-9 _-]{1,20}$/;   // 与后端一致（设备键盘只出 ASCII）
const CANVAS_MAX = 460;                     // 裁剪画布最大显示尺寸

// ── 状态 ──
let sourceFile = null;
let sourceImage = null;   // HTMLImageElement
let cropSel = null;       // {x, y, size} 原图像素坐标（null = 未裁剪/1:1 中心）
let curJob = null;

// ── 步骤切换 ──
function showStep(step) {
  for (const s of ['upload', 'crop', 'params', 'result']) {
    $(`step-${s}`).hidden = s !== step;
  }
}

function resetAll() {
  sourceFile = null;
  sourceImage = null;
  cropSel = null;
  curJob = null;
  $('file-input').value = '';
  showStep('upload');
}

// ── 上传 ──
const dropzone = $('dropzone');
dropzone.addEventListener('click', () => $('file-input').click());
dropzone.addEventListener('dragover', (e) => { e.preventDefault(); });
dropzone.addEventListener('drop', (e) => {
  e.preventDefault();
  if (e.dataTransfer.files.length) loadFile(e.dataTransfer.files[0]);
});
$('file-input').addEventListener('change', (e) => {
  if (e.target.files.length) loadFile(e.target.files[0]);
});

function loadFile(file) {
  if (!file.type.startsWith('image/')) { alert('请选择图片文件'); return; }
  sourceFile = file;
  const img = new Image();
  img.onload = () => {
    sourceImage = img;
    if (img.width === img.height) {
      cropSel = null;                       // 1:1 直接中心（即全图）
      enterParams();
    } else {
      cropSel = null;
      enterCrop();
    }
  };
  img.onerror = () => alert('图片载入失败');
  img.src = URL.createObjectURL(file);
}

// ── 裁剪 ──
const cropCanvas = $('crop-canvas');
const cropCtx = cropCanvas.getContext('2d');
const previewCanvas = $('crop-preview');
const previewCtx = previewCanvas.getContext('2d');

let disp = null;      // {w, h, scale, box, dx, dy} 显示布局
let dragOffset = null;

function enterCrop() {
  showStep('crop');
  const w = sourceImage.width, h = sourceImage.height;
  const scale = CANVAS_MAX / Math.max(w, h);
  const dispW = Math.round(w * scale), dispH = Math.round(h * scale);
  const box = Math.round(Math.min(dispW, dispH));
  disp = { w: dispW, h: dispH, scale, box,
           dx: Math.round((dispW - box) / 2), dy: Math.round((dispH - box) / 2) };
  cropCanvas.width = dispW;
  cropCanvas.height = dispH;
  drawCrop();
  updatePreview();
}

function drawCrop() {
  const { w, h, scale, box, dx, dy } = disp;
  cropCtx.clearRect(0, 0, w, h);
  cropCtx.drawImage(sourceImage, 0, 0, w, h);
  // 选框外暗化
  cropCtx.fillStyle = 'rgba(0,0,0,0.45)';
  cropCtx.fillRect(0, 0, w, dy);
  cropCtx.fillRect(0, dy + box, w, h - dy - box);
  cropCtx.fillRect(0, dy, dx, box);
  cropCtx.fillRect(dx + box, dy, w - dx - box, box);
  // 选框描边
  cropCtx.strokeStyle = '#ff3b30';
  cropCtx.lineWidth = 2;
  cropCtx.strokeRect(dx + 1, dy + 1, box - 2, box - 2);
}

function updatePreview() {
  const { scale, box, dx, dy } = disp;
  const px = Math.round(dx / scale), py = Math.round(dy / scale);
  const size = Math.round(box / scale);
  previewCtx.clearRect(0, 0, 200, 200);
  previewCtx.drawImage(sourceImage, px, py, size, size, 0, 0, 200, 200);
}

cropCanvas.addEventListener('pointerdown', (e) => {
  const rect = cropCanvas.getBoundingClientRect();
  const px = e.clientX - rect.left, py = e.clientY - rect.top;
  const { box, dx, dy } = disp;
  if (px >= dx && px <= dx + box && py >= dy && py <= dy + box) {
    dragOffset = { ox: px - dx, oy: py - dy };
    cropCanvas.setPointerCapture(e.pointerId);
  }
});
cropCanvas.addEventListener('pointermove', (e) => {
  if (!dragOffset) return;
  const rect = cropCanvas.getBoundingClientRect();
  const px = e.clientX - rect.left, py = e.clientY - rect.top;
  const { w, h, box } = disp;
  disp.dx = Math.max(0, Math.min(w - box, Math.round(px - dragOffset.ox)));
  disp.dy = Math.max(0, Math.min(h - box, Math.round(py - dragOffset.oy)));
  drawCrop();
  updatePreview();
});
cropCanvas.addEventListener('pointerup', (e) => { dragOffset = null; });
cropCanvas.addEventListener('pointercancel', () => { dragOffset = null; });

$('crop-confirm').addEventListener('click', () => {
  const { scale, box, dx, dy } = disp;
  cropSel = { x: Math.round(dx / scale), y: Math.round(dy / scale),
              size: Math.round(box / scale) };
  enterParams();
});
$('crop-reset').addEventListener('click', resetAll);

// ── 参数 ──
function enterParams() {
  showStep('params');
  const n = Number($('n-select').value);
  const hint = $('param-hint');
  if (sourceImage && sourceImage.width !== sourceImage.height && !cropSel) {
    hint.textContent = '（未裁剪：使用图片中心正方形区域）';
  } else if (cropSel) {
    hint.textContent = `裁剪区域：原图 (${cropSel.x}, ${cropSel.y}) 边长 ${cropSel.size}px`;
  } else {
    hint.textContent = '（1:1 图片，全图转换）';
  }
}

$('params-reset').addEventListener('click', resetAll);

$('convert-btn').addEventListener('click', async () => {
  const name = $('name-input').value.trim();
  if (!NAME_RE.test(name)) {
    alert('作品名仅限 ASCII 字母/数字/空格/_/-，长度 1~20（设备端键盘限制）');
    return;
  }
  const fd = new FormData();
  fd.append('image', sourceFile);
  fd.append('n', $('n-select').value);
  fd.append('name', name);
  if (cropSel) fd.append('crop', JSON.stringify(cropSel));

  const btn = $('convert-btn');
  btn.disabled = true;
  btn.textContent = '处理中...（216 档约数秒）';
  try {
    const res = await fetch('/api/convert', { method: 'POST', body: fd });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      alert('转换失败: ' + err.error);
      return;
    }
    curJob = await res.json();
    renderResult(curJob);
  } catch (e) {
    alert('请求失败: ' + e);
  } finally {
    btn.disabled = false;
    btn.textContent = '确认转换';
  }
});

// ── 结果 ──
function tileCard(job, name, pos216, pos72) {
  const div = document.createElement('div');
  div.className = 'tile';
  const img = document.createElement('img');
  img.loading = 'lazy';
  img.src = `/api/job/${job.job_id}/tiles/${name}`;
  const cap = document.createElement('div');
  cap.className = 'tile-cap';
  const p216 = pos216 ? `216:r${pos216[0]}c${pos216[1]}` : '216:-';
  const p72 = pos72 ? `72:r${pos72[0]}c${pos72[1]}` : '72:-';
  cap.textContent = `${name}  (${p216} ${p72})`;
  div.append(img, cap);
  return div;
}

function renderResult(job) {
  showStep('result');
  $('download-link').href = `/api/job/${job.job_id}/download`;
  $('download-link').textContent =
    `下载 zip 成品包（beads_${job.name}_${job.n}.zip）`;
  $('overview-img').src = `/api/job/${job.job_id}/overview.png`;
  $('palette-img').src = `/api/job/${job.job_id}/palette.png`;

  const grid = $('tiles-grid');
  grid.replaceChildren();
  for (const t of job.tiles) grid.append(tileCard(job, t.name, t.pos216, t.pos72));

  const b2grid = $('b2-grid');
  b2grid.replaceChildren();
  if (job.b2s.length) {
    $('b2-heading').hidden = false;
    for (const b of job.b2s) b2grid.append(tileCard(job, b.name, b.pos216, null));
  } else {
    $('b2-heading').hidden = true;
  }
  window.scrollTo({ top: 0 });
}

$('result-reset').addEventListener('click', resetAll);

// ── 色卡提示（进入页面时从后端拉取，保证与后端一致）──
fetch('/api/palette')
  .then((r) => r.json())
  .catch(() => null);  // 前端本地校验已足够，拉取仅作预热
