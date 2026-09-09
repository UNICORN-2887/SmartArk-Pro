// 端到端自测：vm stub DOM 环境跑 data_tx.js + simulator.js，验证内置德克萨斯自动加载
import fs from 'fs';
import path from 'path';
import vm from 'vm';

const ROOT = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const elCache = new Map();
const mkEl = (id) => {
  if (!elCache.has(id)) {
    elCache.set(id, {
      style: {}, classList: { add() {}, remove() {}, toggle() {} },
      textContent: '', value: '', checked: false, disabled: false,
      innerHTML: '', _opts: [],
      add(o) { this._opts.push(o); },
      appendChild() {}, append() {},
      getContext: () => ctxStub(),
      onchange: null, onclick: null, oninput: null,
      width: 0, height: 0,
    });
  }
  return elCache.get(id);
};
const ctxStub = () => ({
  putImageData() {}, getImageData() { throw new Error('no getImageData in e2e stub'); },
  clearRect() {}, save() {}, restore() {}, translate() {}, rotate() {},
  drawImage() {}, scale() {}, beginPath() {}, moveTo() {}, lineTo() {},
  closePath() {}, fill() {}, clip() {}, transform() {}, stroke() {},
  set globalAlpha(v) {}, get globalAlpha() { return 1; },
});
const canvasStub = () => ({ width: 0, height: 0, getContext: () => ctxStub() });
class ImageDataPoly { constructor(w, h) { this.width = w; this.height = h; this.data = new Uint8ClampedArray(w * h * 4); } }
class OptionPoly { constructor(text, value) { this.text = text; this.value = value; } }

const ctx = {
  document: {
    getElementById: mkEl,
    createElement: (tag) => tag === 'canvas' ? canvasStub() : { append() {}, appendChild() {}, style: {}, classList: { add() {}, remove() {} } },
    querySelectorAll: () => [],
  },
  requestAnimationFrame: () => {}, atob, btoa, fetch: () => Promise.reject(new Error('no fetch')),
  TextDecoder, TextEncoder, URL, console, Math, JSON, Object, ArrayBuffer, DataView,
  Uint8Array, Uint8ClampedArray, ImageData: ImageDataPoly, Map, Set, Promise,
  parseFloat, encodeURIComponent, Error, Array, Option: OptionPoly,
  DecompressionStream, Blob, Response,
};
ctx.window = ctx;
const sandbox = vm.createContext(ctx);

vm.runInContext(fs.readFileSync(path.join(ROOT, 'data_tx.js'), 'utf8'), sandbox, { filename: 'data_tx.js' });
console.log('SPINE_PPD 文件数 =', Object.keys(sandbox.SPINE_PPD.files).length);
vm.runInContext(fs.readFileSync(path.join(ROOT, 'simulator.js'), 'utf8'), sandbox, { filename: 'simulator.js' });

// 等自动加载完成
await new Promise(r => setTimeout(r, 5000));
const status = mkEl('srcStatus').textContent;
const formOpts = mkEl('formSel')._opts.map(o => `${o.text}:${o.value}`).join(' | ');
const animOpts = mkEl('animSel')._opts.map(o => o.text).join(' | ');
console.log('srcStatus =', status);
console.log('formSel =', formOpts);
console.log('animSel =', animOpts);
let ok = status.includes('内置·拉普兰德')
  && formOpts.includes('基建:forms/基建') && formOpts.includes('背面:forms/背面')
  && animOpts.includes('Die') && animOpts.includes('Idle') && animOpts.includes('Start');

// 形态切换：切到基建，验证 Sleep/Sit 动画存在（单贴图方案，无动画子形态）
if (ok) {
  mkEl('formSel').value = 'forms/基建';
  mkEl('formSel').onchange();
  await new Promise(r => setTimeout(r, 5000));
  const dormStatus = mkEl('srcStatus').textContent;
  const dormAnims = mkEl('animSel')._opts.map(o => o.text).join(' | ');
  console.log('切基建后 srcStatus =', dormStatus);
  console.log('切基建后 animSel =', dormAnims);
  ok = dormStatus.includes('基建') && dormAnims.includes('Sleep') && dormAnims.includes('Sit') && dormAnims.includes('Interact');
}
console.log(ok ? '\n✅ 端到端通过：自动加载 + 形态切换（基建含 Sleep/Sit/Interact）' : '\n❌ 端到端失败');
process.exit(ok ? 0 : 1);
