import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const animName = process.argv[3] || 'Default';
const tArg = process.argv[4] || '0';

const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

const files = fs.readdirSync(dir);
const atlasText = fs.readFileSync(path.join(dir, files.find(f => f.endsWith('.atlas'))), 'utf8');
const atlas = new spine.TextureAtlas(atlasText, () => new spine.Texture({ width: 1024, height: 1024 }, 1024, 1024));
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, files.find(f => f.endsWith('.skel'))))));

console.log(data.animations.map(a => `${a.name}:${a.duration.toFixed(3)}`).join('\n'));
const skel = new spine.Skeleton(data);
const anim = data.animations.find(a => a.name === animName);
const t = tArg === 'end' && anim ? anim.duration : Number(tArg);
skel.setToSetupPose();
if (anim) anim.apply(skel, 0, t, false, null, 1, 0, 0);
skel.updateWorldTransform();

const visible = [];
for (const s of skel.slots) {
    const at = s.getAttachment();
    if (!at || !at.region) continue;
    const slotAlpha = s.color ? s.color.a : 1;
    const attAlpha = at.color ? at.color.a : 1;
    const b = skel.bones[s.data.boneData.index];
    const det = b.a * b.d - b.b * b.c;
    if (slotAlpha * attAlpha > 0.01)
        visible.push(`${s.data.index}\t${s.data.name}\t${at.name}\ta=${(slotAlpha * attAlpha).toFixed(3)}\tdet=${det.toFixed(3)}\tb=${b.data.name}`);
}
console.log(`VISIBLE ${visible.length} @ ${animName} ${t.toFixed(3)}`);
for (const line of visible) console.log(line);
