'use strict';

/* ============================================================
 * Spine→PPD 仿真器（Q 版重写）
 *
 * 严格按《Spine 模型获取 + 仿真器需求（交接文档）》第三部分实现：
 *  - 渲染公式：唯一正确语义（层中心平移 + 旋转，见 drawFrame）
 *  - 动画插值：线性，无任何过渡包络
 *  - vis 硬切换（插值后 >= 0.5 才渲染），不做淡入淡出
 *  - 无 head/body 分组变换
 *  - 层 z 升序绘制；无轨道的层用 scene.json 默认值，不参与动画
 * ============================================================ */

const CANVAS_W = 480;
const CANVAS_H = 800;
const decoder = new TextDecoder('utf-8');
const encoder = new TextEncoder();

/* ---------------- 虚拟文件系统（HTTP / 本地文件夹 / 内存统一抽象） ---------------- */

class HTTPVFS {
  constructor(base) {
    this.base = base.replace(/\/+$/, '') + '/';
  }
  async get(rel) {
    const url = this.base + rel.split('/').map(encodeURIComponent).join('/');
    const resp = await fetch(url);
    if (!resp.ok) throw new Error(`HTTP ${resp.status}: ${rel}`);
    return resp.arrayBuffer();
  }
}

class FileListVFS {
  // input[webkitdirectory] 选择的文件：webkitRelativePath 首段为所选目录名，去掉后即角色目录内相对路径
  constructor(files) {
    this.map = new Map();
    for (const f of files) {
      const parts = f.webkitRelativePath.split('/');
      if (parts.length < 2) continue;
      this.map.set(parts.slice(1).join('/'), f);
    }
  }
  async get(rel) {
    const f = this.map.get(rel);
    if (!f) throw new Error(`未找到文件: ${rel}`);
    return f.arrayBuffer();
  }
}

class MapVFS {
  constructor(map) { this.map = map; }
  async get(rel) {
    const b = this.map.get(rel);
    if (!b) throw new Error(`未找到文件: ${rel}`);
    return b;
  }
}

// 内置打包数据（data_tx.js 由 tools/pack.py 生成：{name, files:{相对路径: base64(deflate)}}）
function b64ToBytes(b64) {
  const bin = atob(b64);
  const buf = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
  return buf;
}

async function inflateBuf(bytes) {
  const ds = new DecompressionStream('deflate');
  const stream = new Blob([bytes]).stream().pipeThrough(ds);
  return new Response(stream).arrayBuffer();
}

class B64VFS {
  constructor(pkg) {
    this.compressed = !!pkg.compressed;
    this.map = new Map(Object.entries(pkg.files).map(([k, v]) => [k, b64ToBytes(v)]));
  }
  async get(rel) {
    const b = this.map.get(rel);
    if (!b) throw new Error(`未找到文件: ${rel}`);
    if (this.compressed) return inflateBuf(b);
    return b.buffer;
  }
}

/* ---------------- .raw 贴图解码 ----------------
 * 格式：u16 w + u16 h + RGBA8888（每像素 4 字节，byte0=R，byte2=B）
 * 字节序自动检测：LE 尺寸不合文件长度时回退 BE。 */

function decodeRaw(buf) {
  const dv = new DataView(buf);
  const len = buf.byteLength;
  const sizeOk = (w, h) => w > 0 && h > 0 && w * h <= (len - 4) / 4;
  let w = dv.getUint16(0, true), h = dv.getUint16(2, true);
  if (!sizeOk(w, h)) {
    const wb = dv.getUint16(0, false), hb = dv.getUint16(2, false);
    if (sizeOk(wb, hb)) { w = wb; h = hb; }
    else throw new Error(`raw 头尺寸非法: LE(${w},${h}) / BE(${wb},${hb}), 文件长度 ${len}`);
  }
  const img = new ImageData(w, h);
  let o = 4;
  for (let i = 0; i < w * h; i++, o += 4) {
    img.data[i * 4]     = dv.getUint8(o);     // R
    img.data[i * 4 + 1] = dv.getUint8(o + 1); // G
    img.data[i * 4 + 2] = dv.getUint8(o + 2); // B
    img.data[i * 4 + 3] = dv.getUint8(o + 3); // A
  }
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  c.getContext('2d').putImageData(img, 0, 0);
  return c;
}

/* ---------------- 角色加载 ----------------
 * 角色目录结构（交接文档第二节）：
 *   scene.json / <槽名>.raw / anims.json（可选）/ forms.json（可选）
 *   forms/<形态>/ 下同构。 */

async function loadCharacter(vfs, subPath) {
  const base = subPath ? subPath.replace(/\/+$/, '') + '/' : '';
  const getText = async (p) => decoder.decode(await vfs.get(base + p));

  const scene = JSON.parse(await getText('scene.json'));

  let anims = {};
  try { anims = JSON.parse(await getText('anims.json')); } catch (e) { anims = {}; }
  let forms = null;
  try { forms = JSON.parse(await getText('forms.json')); } catch (e) { forms = null; }

  const textures = new Map();
  for (const L of scene.layers) {
    try {
      textures.set(L.name, decodeRaw(await vfs.get(base + L.name + '.raw')));
    } catch (e) {
      throw new Error(`加载贴图 ${base}${L.name}.raw 失败: ${e.message}`);
    }
  }
  return { base, scene, anims, textures, forms };
}

/* ---------------- 动画轨道采样（线性插值，vis 硬切） ---------------- */

function sampleTrack(track, duration, t) {
  const n = track.length;
  if (n < 2 || duration <= 0) {
    const k0 = track[0];
    return { dx: k0[0], dy: k0[1], drot: k0[2], vis: k0[3] >= 0.5 };
  }
  const pos = Math.min(Math.max(t / duration * (n - 1), 0), n - 1);
  const i0 = Math.floor(pos);
  const frac = pos - i0;
  const k0 = track[i0];
  const k1 = track[Math.min(i0 + 1, n - 1)];
  return {
    dx:   k0[0] + (k1[0] - k0[0]) * frac,
    dy:   k0[1] + (k1[1] - k0[1]) * frac,
    drot: k0[2] + (k1[2] - k0[2]) * frac,
    vis:  k0[3] + (k1[3] - k0[3]) * frac >= 0.5,
  };
}

/* ---------------- 渲染 ----------------
 * 交接文档唯一正确语义：
 *   ctx.translate(L.x + L.w/2 + dx, L.y + L.h/2 + dy);
 *   ctx.rotate(drot * PI / 180);
 *   ctx.translate(-L.w/2, -L.h/2);
 *   ctx.drawImage(tex, 0, 0, L.w, L.h);
 * 层按 z 升序；无轨道的层 dx/dy/drot=0、vis=scene.visible。 */

function drawFrame() {
  const cur = st.current;
  if (!cur) return;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, CANVAS_W, CANVAS_H);
  const sorted = [...cur.scene.layers].sort((a, b) => a.z - b.z);
  for (const L of sorted) {
    if (st.hidden.has(L.name)) continue;
    let dx = 0, dy = 0, drot = 0, vis = !!L.visible;
    const anim = st.anim;
    if (anim) {
      const track = anim.layers && anim.layers[L.name];
      if (track) {
        const s = sampleTrack(track, anim.duration, st.t);
        dx = s.dx; dy = s.dy; drot = s.drot; vis = s.vis;
      }
    }
    if (!vis) continue;
    const tex = cur.textures.get(L.name);
    if (!tex) continue;
    ctx.save();
    ctx.translate(L.x + L.w / 2 + dx, L.y + L.h / 2 + dy);
    ctx.rotate(drot * Math.PI / 180);
    ctx.translate(-L.w / 2, -L.h / 2);
    ctx.drawImage(tex, 0, 0, L.w, L.h);
    ctx.restore();
  }
}

/* ---------------- UI 元素与全局状态 ---------------- */

const el = {};
for (const id of ['canvas', 'stage', 'view', 'urlInput', 'srcStatus', 'formSel', 'animSel',
  'btnPlay', 'btnStop', 'seek', 'timeLabel', 'loopBadge', 'bgSel', 'zoomSel',
  'btnExport', 'layerList', 'btnLoadUrl', 'btnPickDir', 'btnDemo', 'stageHint']) {
  el[id] = document.getElementById(id);
}
const canvas = el.canvas;

const st = {
  vfs: null,
  sourceName: '',     // 数据源名（URL / 文件夹名 / 演示数据）
  current: null,      // loadCharacter 结果（当前形态）
  anim: null,         // 当前选中动画对象；null = 默认姿势
  t: 0,               // 当前动画时间（秒）
  playing: false,
  rot: 0,             // 画布旋转 0/90/180/270
  zoom: 0.8,
  hidden: new Set(),  // 调试用：手动隐藏的层
};

/* ---------------- 画布视图（旋转 + 缩放） ---------------- */

function applyView() {
  let sw = CANVAS_W, sh = CANVAS_H;
  switch (st.rot) {
    case 90:  el.view.style.transform = 'rotate(90deg) translateY(-100%)';    sw = CANVAS_H; sh = CANVAS_W; break;
    case 180: el.view.style.transform = 'rotate(180deg) translate(-100%, -100%)'; break;
    case 270: el.view.style.transform = 'rotate(-90deg) translateX(-100%)';   sw = CANVAS_H; sh = CANVAS_W; break;
    default:  el.view.style.transform = ''; st.rot = 0; break;
  }
  el.stage.style.width = (sw * st.zoom) + 'px';
  el.stage.style.height = (sh * st.zoom) + 'px';
  el.stage.style.transform = 'scale(' + st.zoom + ')';
}

/* ---------------- 面板重建 ---------------- */

function applyLoaded(cur, label) {
  st.current = cur;
  st.anim = null;
  st.t = 0;
  st.playing = false;
  st.hidden.clear();
  el.stageHint.style.display = 'none';

  // 形态下拉（子形态 dir 相对当前形态目录，拼上 cur.base 成全局路径）
  el.formSel.innerHTML = '';
  el.formSel.add(new Option('主场景', ''));
  if (cur.forms) for (const f of cur.forms.forms) el.formSel.add(new Option(f.name, cur.base + f.dir));

  // 动画下拉
  el.animSel.innerHTML = '';
  el.animSel.add(new Option('（默认姿势）', ''));
  for (const name of Object.keys(cur.anims)) el.animSel.add(new Option(name, name));

  rebuildLayerList();
  updatePlayBtn();
  updateTimeUI();
  drawFrame();
  el.srcStatus.textContent = label;
  el.srcStatus.classList.remove('error');
}

function rebuildLayerList() {
  el.layerList.innerHTML = '';
  if (!st.current) return;
  const sorted = [...st.current.scene.layers].sort((a, b) => a.z - b.z);
  for (const L of sorted) {
    const li = document.createElement('li');
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = !st.hidden.has(L.name);
    cb.onchange = () => {
      if (cb.checked) st.hidden.delete(L.name); else st.hidden.add(L.name);
      drawFrame();
    };
    const span = document.createElement('span');
    span.textContent = L.name;
    const z = document.createElement('span');
    z.className = 'z';
    z.textContent = `z=${L.z} · ${L.group}`;
    li.append(cb, span, z);
    el.layerList.appendChild(li);
  }
}

function updatePlayBtn() {
  el.btnPlay.textContent = st.playing ? '⏸ 暂停' : '▶ 播放';
  el.loopBadge.textContent = st.anim ? (st.anim.loop ? '循环' : '单次') : '';
}

function updateTimeUI() {
  if (st.anim) {
    el.seek.disabled = false;
    el.seek.value = Math.round(st.t / st.anim.duration * 1000);
    el.timeLabel.textContent = `${st.t.toFixed(2)} / ${st.anim.duration.toFixed(2)}s`;
  } else {
    el.seek.disabled = true;
    el.seek.value = 0;
    el.timeLabel.textContent = '0.00 / -';
  }
}

/* ---------------- 数据源入口 ---------------- */

async function loadFrom(vfs, subPath, sourceName) {
  try {
    el.srcStatus.textContent = '加载中…';
    el.srcStatus.classList.remove('error');
    const cur = await loadCharacter(vfs, subPath);
    st.vfs = vfs;
    st.sourceName = sourceName;
    applyLoaded(cur, `${sourceName} · ${subPath || '主场景'}`);
  } catch (e) {
    el.srcStatus.textContent = '加载失败: ' + e.message;
    el.srcStatus.classList.add('error');
  }
}

el.btnLoadUrl.onclick = () => {
  const u = el.urlInput.value.trim();
  if (!u) { el.srcStatus.textContent = '请输入角色目录 URL'; return; }
  loadFrom(new HTTPVFS(u), '', u);
};

el.btnPickDir.onclick = () => {
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.webkitdirectory = true;
  inp.onchange = () => {
    if (!inp.files || !inp.files.length) return;
    const root = inp.files[0].webkitRelativePath.split('/')[0];
    loadFrom(new FileListVFS(inp.files), '', `本地文件夹（${root}）`);
  };
  inp.click();
};

el.btnDemo.onclick = () => loadFrom(new MapVFS(makeDemoFiles()), '', '内置演示数据（合成，经真实 .raw 解码路径）');

el.formSel.onchange = () => {
  if (!st.vfs) return;
  loadFrom(st.vfs, el.formSel.value, st.sourceName);
};

el.animSel.onchange = () => {
  const name = el.animSel.value;
  st.anim = name ? st.current.anims[name] : null;
  st.t = 0;
  st.playing = false;
  updatePlayBtn();
  updateTimeUI();
  drawFrame();
};

el.btnPlay.onclick = () => {
  if (!st.current || !st.anim) { el.srcStatus.textContent = '请先选择动画'; return; }
  if (st.playing) {
    st.playing = false;
  } else {
    if (st.t >= st.anim.duration && !st.anim.loop) st.t = 0; // 单次动画播完后再点播放：从头
    st.playing = true;
  }
  updatePlayBtn();
};

el.btnStop.onclick = () => {
  st.t = 0;
  st.playing = false;
  updatePlayBtn();
  updateTimeUI();
  drawFrame();
};

el.seek.oninput = () => {
  if (!st.anim) return;
  st.t = el.seek.value / 1000 * st.anim.duration;
  drawFrame();
};

document.querySelectorAll('.rot').forEach(b => {
  b.onclick = () => {
    st.rot = +b.dataset.rot;
    document.querySelectorAll('.rot').forEach(x => x.classList.toggle('active', x === b));
    applyView();
  };
});

el.bgSel.onchange = () => {
  canvas.classList.remove('bg-grey', 'bg-black', 'bg-checker');
  if (el.bgSel.value !== 'white') canvas.classList.add('bg-' + el.bgSel.value);
};

el.zoomSel.onchange = () => {
  st.zoom = parseFloat(el.zoomSel.value);
  applyView();
};

el.btnExport.onclick = () => {
  if (!st.current) return;
  const c = document.createElement('canvas');
  c.width = CANVAS_W; c.height = CANVAS_H;
  const g = c.getContext('2d');
  g.fillStyle = '#ffffff';
  g.fillRect(0, 0, CANVAS_W, CANVAS_H);
  g.drawImage(canvas, 0, 0);
  c.toBlob(b => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(b);
    a.download = 'frame.png';
    a.click();
    URL.revokeObjectURL(a.href);
  });
};

/* ---------------- 主循环 ---------------- */

let lastTs = null;
function frame(ts) {
  if (st.playing && st.anim && st.current) {
    if (lastTs != null) st.t += (ts - lastTs) / 1000;
    if (st.t >= st.anim.duration) {
      if (st.anim.loop) st.t %= st.anim.duration;
      else { st.t = st.anim.duration; st.playing = false; updatePlayBtn(); }
    }
  }
  lastTs = ts;
  if (st.current) drawFrame();
  updateTimeUI();
  requestAnimationFrame(frame);
}

window.onerror = (msg, src, line) => {
  el.srcStatus.textContent = `JS 错误: ${msg} @${line}`;
  el.srcStatus.classList.add('error');
};

applyView();
requestAnimationFrame(frame);

// 内置数据（data_tx.js）：打开页面即自动加载
if (window.SPINE_PPD) {
  loadFrom(new B64VFS(window.SPINE_PPD), '', `内置·${window.SPINE_PPD.name}`);
}

/* ============================================================
 * 内置演示数据：合成一个 Q 版小人（Body/Head/EyeL/EyeR/Hair），
 * 以真实 PPD 文件格式（scene.json / .raw / anims.json / forms.json）
 * 编码进内存 VFS，走与真实数据完全相同的加载与解码路径。
 * ============================================================ */

function makeDemoFiles() {
  const files = new Map();
  const enc = (s) => encoder.encode(s).buffer;

  const mkLayer = (name, x, y, w, h, z, group = 'body') => ({
    name, x, y, w, h,
    cx: Math.round(x + w / 2), cy: Math.round(y + h / 2),
    bbox: [x, y, x + w, y + h], z, group, special: '', visible: true,
  });

  const rootLayers = [
    mkLayer('Body', 180, 430, 120, 220, 0),
    mkLayer('Head', 190, 330, 100, 100, 1),
    mkLayer('EyeL', 212, 370, 16, 10, 2),
    mkLayer('EyeR', 252, 370, 16, 10, 3),
    mkLayer('Hair', 185, 320, 110, 60, 4),
  ];

  // 把 canvas 编码为 .raw（u16 LE w + u16 LE h + RGBA8888），演示数据也覆盖真实解码路径
  const tex = (path, w, h, fn) => {
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    fn(c.getContext('2d'), w, h);
    const id = c.getContext('2d').getImageData(0, 0, w, h);
    const buf = new ArrayBuffer(4 + w * h * 4);
    const dv = new DataView(buf);
    dv.setUint16(0, w, true);
    dv.setUint16(2, h, true);
    new Uint8Array(buf).set(id.data, 4);
    files.set(path, buf);
  };

  const bodyDraw = (color) => (g) => {
    g.fillStyle = color; g.fillRect(0, 0, 120, 220);
    g.fillStyle = '#e8e8e8'; g.fillRect(42, 0, 36, 10); // 领口
  };
  const headDraw = (g) => {
    g.fillStyle = '#f5d3b3'; g.beginPath(); g.arc(50, 50, 48, 0, Math.PI * 2); g.fill();
  };
  const eyeDraw = (g) => {
    g.fillStyle = '#1c1c1c'; g.beginPath(); g.ellipse(8, 5, 6.7, 4.2, 0, 0, Math.PI * 2); g.fill();
  };
  const hairDraw = (g) => {
    g.fillStyle = '#6b4423';
    g.fillRect(0, 0, 110, 30);
    g.beginPath(); g.arc(55, 30, 55, Math.PI, 0); g.fill();
  };
  const toolDraw = (g) => {
    g.strokeStyle = '#9aa0ac'; g.lineWidth = 8;
    g.beginPath(); g.moveTo(4, 8); g.lineTo(76, 4); g.stroke();
  };

  tex('Body.raw', 120, 220, bodyDraw('#3a5a9f'));
  tex('Head.raw', 100, 100, headDraw);
  tex('EyeL.raw', 16, 10, eyeDraw);
  tex('EyeR.raw', 16, 10, eyeDraw);
  tex('Hair.raw', 110, 60, hairDraw);

  // ---- 动画 ----
  // Idle：头部轻浮。轨道值为正弦采样（合法：插值本身仍是线性的，无包络）
  const nIdle = 40;
  const idleLayers = { Head: [], Hair: [], EyeL: [], EyeR: [] };
  for (let i = 0; i < nIdle; i++) {
    const dy = Math.round(4 * Math.sin(2 * Math.PI * i / (nIdle - 1)) * 100) / 100;
    for (const name of Object.keys(idleLayers)) idleLayers[name].push([0, dy, 0, 1]);
  }

  // Blink：眼睛 vis 硬切（帧 7..11 闭眼），验证 vis 阈值语义
  const nBlink = 30;
  const blink = { duration: 1.5, loop: true, layers: { EyeL: [], EyeR: [] } };
  for (let i = 0; i < nBlink; i++) {
    const vis = (i >= 7 && i <= 11) ? 0 : 1;
    blink.layers.EyeL.push([0, 0, 0, vis]);
    blink.layers.EyeR.push([0, 0, 0, vis]);
  }

  // Sleep / Die：整体绕 pivot 旋转——每层绕各自中心转同一角度 + 相应平移差 = 整体躺平/倒下
  const rotateAnim = (layers, thetaMax, px, py, n, duration) => {
    const out = {};
    for (const L of layers) {
      const cx = L.x + L.w / 2, cy = L.y + L.h / 2;
      const arr = [];
      for (let i = 0; i < n; i++) {
        const deg = thetaMax * i / (n - 1);
        const th = deg * Math.PI / 180;
        const dx = (cx - px) * (Math.cos(th) - 1) - (cy - py) * Math.sin(th);
        const dy = (cx - px) * Math.sin(th) + (cy - py) * (Math.cos(th) - 1);
        const r2 = (v) => Math.round(v * 100) / 100;
        arr.push([r2(dx), r2(dy), r2(deg), 1]);
      }
      out[L.name] = arr;
    }
    return { duration, loop: false, layers: out };
  };

  files.set('scene.json', enc(JSON.stringify({ landscape: true, landscape_rot: 1, layers: rootLayers })));
  files.set('anims.json', enc(JSON.stringify({
    Idle: { duration: 2.0, loop: true, layers: idleLayers },
    Blink: blink,
    Sleep: rotateAnim(rootLayers, 90, 240, 540, 40, 2.0),
    Die: rotateAnim(rootLayers, -90, 240, 650, 50, 2.5),
  })));
  files.set('forms.json', enc(JSON.stringify({ forms: [{ name: '基建', dir: 'forms/基建' }] })));

  // ---- 基建形态：换装 + 多一个道具层 ----
  const dormLayers = [
    mkLayer('Body', 180, 430, 120, 220, 0),
    mkLayer('Head', 190, 330, 100, 100, 1),
    mkLayer('EyeL', 212, 370, 16, 10, 2),
    mkLayer('EyeR', 252, 370, 16, 10, 3),
    mkLayer('Hair', 185, 320, 110, 60, 4),
    mkLayer('Tool', 300, 500, 80, 12, 5),
  ];
  tex('forms/基建/Body.raw', 120, 220, bodyDraw('#3f7d54'));
  tex('forms/基建/Head.raw', 100, 100, headDraw);
  tex('forms/基建/EyeL.raw', 16, 10, eyeDraw);
  tex('forms/基建/EyeR.raw', 16, 10, eyeDraw);
  tex('forms/基建/Hair.raw', 110, 60, hairDraw);
  tex('forms/基建/Tool.raw', 80, 12, toolDraw);
  files.set('forms/基建/scene.json', enc(JSON.stringify({ landscape: true, landscape_rot: 1, layers: dormLayers })));
  files.set('forms/基建/anims.json', enc(JSON.stringify({
    Sleep: rotateAnim(dormLayers, 90, 240, 540, 40, 2.0),
  })));

  return files;
}
