// 纯 Spine 仿真器端到端自测：stub 浏览器环境跑 spine_sim.js 全路径
// （数据解码 → Image dataURL → atlas 缩放 → skel 解析 → AnimationState 播放）
import fs from 'fs';
import path from 'path';
import vm from 'vm';
import { loadImage } from '@napi-rs/canvas';

const ROOT = path.join(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');

// ---- DOM stubs ----
const elCache = new Map();
const mkEl = (id) => {
  if (!elCache.has(id)) {
    elCache.set(id, {
      style: {}, classList: { add() {}, remove() {}, toggle() {} },
      textContent: '', value: '', checked: false, disabled: false,
      innerHTML: '', _opts: [],
      add(o) { this._opts.push(o); if (!this.value) this.value = o.value; },   // 模拟真实 select 默认取第一个
      appendChild() {}, append() {},
      getContext: () => ctxStub(),
      onchange: null, onclick: null, oninput: null,
      width: 0, height: 0,
    });
  }
  return elCache.get(id);
};
const ctxStub = () => ({
  putImageData() {}, getImageData(x, y, w, h) { return { data: new Uint8ClampedArray(w * h * 4) }; },
  clearRect() {}, save() {}, restore() {}, translate() {}, rotate() {}, scale() {},
  drawImage() {}, beginPath() {}, moveTo() {}, lineTo() {}, closePath() {}, fill() {},
  clip() {}, transform() {}, stroke() {}, fillRect() {},
  get globalAlpha() { return 1; }, set globalAlpha(v) {},
});
const canvasStub = () => ({ width: 0, height: 0, getContext: () => ctxStub(), toBlob(cb) { cb && cb(Buffer.alloc(1)); } });
class OptionPoly { constructor(text, value) { this.text = text; this.value = value; } }
// Image stub：dataURL → 真实解码 PNG（@napi-rs）取宽高
class ImageStub {
  set src(v) {
    const b64 = v.split(',')[1];
    loadImage(Buffer.from(b64, 'base64')).then(img => {
      this.width = img.width; this.height = img.height;
      this._img = img;
      if (this.onload) this.onload();
    }).catch(e => { if (this.onerror) this.onerror(e); });
  }
}

let rafCb = null;
const ctx = {
  window: null,
  document: {
    getElementById: mkEl,
    createElement: (tag) => tag === 'canvas' ? canvasStub() : { append() {}, appendChild() {}, style: {}, classList: { add() {}, remove() {} } },
    querySelectorAll: () => [],
  },
  requestAnimationFrame: (cb) => { rafCb = cb; },
  atob, btoa,
  Image: ImageStub,
  TextDecoder, TextEncoder, URL, console, Math, JSON, Object, ArrayBuffer, DataView,
  Uint8Array, Uint8ClampedArray, Map, Set, Promise, Error, Array, Option: OptionPoly,
  parseFloat, isNaN, alert: (m) => console.log('[alert]', m),
};
ctx.window = ctx;
const sandbox = vm.createContext(ctx);
for (const f of ['spine-core.js', 'spine-canvas.js', 'data_spine.js', 'spine_sim.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, f), 'utf8'), sandbox, { filename: f });
}

await new Promise(r => setTimeout(r, 4000));
const formSel = mkEl('formSel');
const animSel = mkEl('animSel');
const hint = mkEl('stageHint');
console.log('stageHint =', hint.textContent);
console.log('formSel =', formSel._opts.map(o => `${o.text}:${o.value}`).join(' | '));
console.log('animSel =', animSel._opts.map(o => o.text).join(' | '));
let ok = formSel._opts.length === 3 && animSel._opts.length > 5 && !hint.textContent.includes('失败');

// 切基建 → 选 Sleep → 播放
if (ok) {
  mkEl('formSel').value = '基建';
  mkEl('formSel').onchange();
  await new Promise(r => setTimeout(r, 4000));
  const dormAnims = mkEl('animSel')._opts.map(o => o.text).join(' | ');
  console.log('切基建后 animSel =', dormAnims, 'hint =', hint.textContent);
  ok = dormAnims.includes('Sleep') && !hint.textContent.includes('失败');
  if (ok) {
    mkEl('animSel').value = 'Sleep';
    mkEl('animSel').onchange();
    await new Promise(r => setTimeout(r, 500));
    mkEl('btnPlay').onclick();
    // 手动推进帧循环（模拟浏览器 rAF）：0ms → 100ms → 300ms → 600ms
    const t0 = mkEl('timeLabel').textContent;
    let ts = 0;
    for (const step of [100, 200, 300]) {
      ts += step;
      rafCb && rafCb(ts);
    }
    const t1 = mkEl('timeLabel').textContent;
    console.log(`播放推进: ${t0} → ${t1}（期望时间前进）`);
    ok = !hint.textContent.includes('错误') && !hint.textContent.includes('失败') && t1 !== t0 && !t1.startsWith('0.00 /');
    if (!ok) {
      // 若时间没推进，看 seek 拖动是否改变帧（隔离 frame 循环 vs 动画应用问题）
      mkEl('seek').value = 500;
      mkEl('seek').oninput();
      const t2 = mkEl('timeLabel').textContent;
      console.log('seek 拖动后 timeLabel =', t2);
    }
    mkEl('btnStop').onclick();
    console.log('播放/停止后 hint =', hint.textContent);
  }
}
console.log(ok ? '\n✅ Spine 仿真器端到端通过：加载/形态/动画/播放全链路' : '\n❌ 失败');
process.exit(ok ? 0 : 1);
