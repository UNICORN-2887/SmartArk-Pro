// 检查动画的 AttachmentTimeline：哪些槽在动画中换附件
import fs from 'fs';
import path from 'path';
import { loadImage } from '@napi-rs/canvas';

const DIR = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const SET = process.argv[2] || 'spine_raw/缄默德克萨斯/正面';
const ANIM = process.argv[3] || 'Die';
const spineCode = fs.readFileSync(path.join(DIR, 'spine-canvas-3.8.99.js'), 'utf8');
const THREE = new Proxy({}, { get: () => class {} });
const spine = new Function('THREE', spineCode + '\n;return spine;')(THREE);

const stem = fs.readdirSync(path.join(DIR, SET)).find(f => f.endsWith('.skel')).replace(/\.skel$/, '');
const atlasText = fs.readFileSync(path.join(DIR, SET, stem + '.atlas'), 'utf8').replace(/\r\n/g, '\n');
const image = await loadImage(fs.readFileSync(path.join(DIR, SET, stem + '.png')));
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};
const atlas = new spine.TextureAtlas(atlasText, p => new spine.canvas.CanvasTexture(image));
const skelData = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas))
  .readSkeletonData(new Uint8Array(fs.readFileSync(path.join(DIR, SET, stem + '.skel'))));

const slotIndex = new Map();
skelData.slots.forEach((s, i) => slotIndex.set(i, s.name));
for (const a of skelData.animations) {
  if (ANIM && a.name !== ANIM) continue;
  const types = {};
  const attSlots = [];
  for (const tl of a.timelines) {
    types[tl.constructor.name] = (types[tl.constructor.name] || 0) + 1;
    if (tl instanceof spine.AttachmentTimeline) {
      const atts = [...new Set(tl.attachmentNames)].filter(x => x);
      const setupAtt = skelData.slots[tl.slotIndex].attachmentName;
      attSlots.push({ slot: skelData.slots[tl.slotIndex].name, setup: setupAtt, frames: atts.join(', ') });
    }
  }
  console.log(`\n${a.name} (duration=${a.duration}): timeline 类型 =`, JSON.stringify(types));
  if (attSlots.length) {
    console.log('  换附件槽:');
    for (const x of attSlots) console.log(`    ${x.slot}: setup=${x.setup} → 帧附件=[${x.frames}]`);
  }
}
// setup 各槽默认附件为空（潜在"帧中出现"槽）
const emptySetup = skelData.slots.filter(s => !s.attachmentName).map(s => s.name);
console.log(`\nsetup 无附件的槽 (${emptySetup.length}):`, emptySetup.join(', '));
