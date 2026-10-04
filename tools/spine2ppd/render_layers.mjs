// 逐层官方渲染导出：每个附件单独用 spine-canvas 渲染 → 像素扫描 bbox → 切出 raw
// 旋转/翻转/mesh 形变全部由官方渲染器处理，零手写几何（初版手算旋转角每层错 90°）
// 用法：node render_layers.mjs <spine_dir> <out_dir> [scene_name] [--landscape]
//
// 方向约定：Spine 世界 y 向上，渲染缓冲行号向下（人物在缓冲中倒立，头部在底行）。
// 设备端场景空间是 y 向下屏幕系（blit 无翻转），所以导出时对整幅画面做 180° 旋转：
// raw 行列反转 + 图层 x/y/bbox/cx/cy 换算（x 和 y 都要翻转）——scene.json 里人物
// 正立，与仿真器导出一致。与 render_ref.mjs 的 ref.png 像素级一致即验证通过。
//
// --landscape（横屏 Q 版互动用）：Spine 内容顺时针旋转 90° 画入 480×800 竖帧
// （角色头朝画布左 = 用户横持设备的视觉上方，与 standee 预旋转同思路，设备端零旋转）。
// 角色世界高度映射画布宽度、宽度映射画布高度（fit 公式交换 CW/CH）。
import { createCanvas, loadImage } from 'canvas';
import fs from 'fs';
import path from 'path';

const dir = process.argv[2];
const outDir = process.argv[3];
const sceneName = process.argv[4] || path.basename(outDir);
const landscape = process.argv[5] === '--landscape';
fs.mkdirSync(outDir, { recursive: true });
const files = fs.readdirSync(dir);
const skelFile = files.find(f => f.endsWith('.skel'));
const atlasFile = files.find(f => f.endsWith('.atlas'));
const pngFiles = files.filter(f => f.endsWith('.png')).sort();

// 注意：spine-canvas.js 自带一套核心类定义（与 spine-core.js 独立）——
// 解析与渲染必须用同一套类，否则 instanceof 跨版本失败（渲染器跳过所有附件 → 空白）
const cv = fs.readFileSync(new URL('./spine-canvas-3.8.99.js', import.meta.url), 'utf8');
new Function(cv + '\nif (typeof spine !== "undefined") globalThis.__spine = spine;')();
const spine = globalThis.__spine;
spine.Texture.prototype.setFilters = function () {};
spine.Texture.prototype.setWraps = function () {};

// 图集声明尺寸 vs png 实际尺寸：部分下载渠道把图集缩了（基建 png 344×344
// vs atlas 声明 516×516——region 坐标按声明尺寸设计，uv 归一化基准错误 →
// 大量区域越界渲染为空）。放大到声明尺寸恢复几何（内容完整仅分辨率低）。
// 多页 atlas：每页独立 png + size 声明——曾只加载第一页，第二页 region 的 UV
// 落在第一页错误区域，mesh 特效层渲染成实心半透明方块（凯尔希基建 C_Mon3tr_Light）
const atlasText = fs.readFileSync(path.join(dir, atlasFile), 'utf8');
const pages = [];
{
    const re = /(?:^|\n)([^\n:]+\.png)\nsize:\s*(\d+)\s*,\s*(\d+)/g;
    let m;
    while ((m = re.exec(atlasText))) pages.push({ name: m[1], w: +m[2], h: +m[3] });
}
if (!pages.length) pages.push({ name: pngFiles[0], w: 0, h: 0 });
if (pages.length > 1) console.log(`多页图集: ${pages.map(p => p.name).join(', ')}`);
const pageTex = new Map();
// png 小于声明尺寸时:把 atlas 坐标按比例缩小(不放大图片)。
// 曾放大 png 到声明尺寸:凯尔希基建 png 416 vs 声明 624,放大后 mesh UV 采样错位
// 30-60px,采到贴图纯白区域 → 光晕层渲染成实心半透明方块(C_Mon3tr_Light)
let atlasFinal = atlasText;
for (const pg of pages) {
    const timg = await loadImage(path.join(dir, pg.name));
    if (pg.w && (timg.width < pg.w || timg.height < pg.h)) {
        const sx = timg.width / pg.w, sy = timg.height / pg.h;
        if (Math.abs(sx - 1) > 0.01 || Math.abs(sy - 1) > 0.01) {
            atlasFinal = atlasFinal.replace(
                /(xy|size|orig|offset):\s*(\d+),\s*(\d+)/g,
                (m, k, a, b) => `${k}: ${Math.max(1, Math.round(a * sx))}, ${Math.max(1, Math.round(b * sy))}`);
            console.log(`图集页 ${pg.name}: ${timg.width}×${timg.height} vs 声明 ${pg.w}×${pg.h} → atlas 坐标缩放 ${sx.toFixed(3)}/${sy.toFixed(3)}`);
        }
    }
    pageTex.set(pg.name, timg);
}
const W = 480, H = 800;
const canvas = createCanvas(W, H);
const ctx = canvas.getContext('2d');

const atlas = new spine.TextureAtlas(atlasFinal, p => {
    const timg = pageTex.get(p) || pageTex.values().next().value;
    return new spine.Texture(timg, timg.width, timg.height);
});
const bin = new spine.SkeletonBinary(new spine.AtlasAttachmentLoader(atlas));
const data = bin.readSkeletonData(new Uint8Array(fs.readFileSync(path.join(dir, skelFile))));
const skel = new spine.Skeleton(data);
skel.setToSetupPose();
// 应用 Default 动画第 0 帧：正确的初始姿势（单臂单表情）——
// setup pose 同时含 5 套手臂骨骼链与表情变体，不应用动画会全部叠加显示
const defaultAnim = data.animations.find(a => a.name === 'Default');
if (defaultAnim) {
    defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
}
skel.updateWorldTransform();   // 必须：骨骼世界矩阵（bbox 顶点计算依赖）

// 内容 bbox：只算当前实际挂载的附件顶点（Default 动画把隐藏姿势链骨骼移出屏幕，
// 若遍历皮肤全部附件会把远点算进 bbox → 模型缩成一小块）
let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
{
    const verts = new Float32Array(2048);
    for (const s of skel.slots) {
        const at = s.getAttachment();
        if (!at || !at.region) continue;
        if (at.constructor.name === 'RegionAttachment') {
            at.computeWorldVertices(skel.bones[s.data.boneData.index], verts, 0, 2);
            for (let i = 0; i < 8; i += 2) {
                minX = Math.min(minX, verts[i]); maxX = Math.max(maxX, verts[i]);
                minY = Math.min(minY, verts[i + 1]); maxY = Math.max(maxY, verts[i + 1]);
            }
        } else {
            const b = skel.bones[s.data.boneData.index];
            for (const [px0, py0] of [[0, 0], [at.region.width, 0], [0, at.region.height], [at.region.width, at.region.height]]) {
                const wx = b.a * px0 + b.b * py0 + b.worldX;
                const wy = b.c * px0 + b.d * py0 + b.worldY;
                minX = Math.min(minX, wx); maxX = Math.max(maxX, wx);
                minY = Math.min(minY, wy); maxY = Math.max(maxY, wy);
            }
        }
    }
}
const CW = maxX - minX, CH = maxY - minY;
// 正 scale 渲染（无 y 翻转，避开 node-canvas 负 scale 裁剪 drawImage 的坑）；人物在缓冲中倒立，导出时统一翻转
let SCALE, OFF_X, OFF_Y;
if (landscape) {
    // 顺时针旋转 90°：画布 X = OFF_X - S*wy（头朝画布左 = 横持视觉上方），Y = OFF_Y + S*wx；
    // fit 交换方向：角色世界高 CH → 画布宽 480，世界宽 CW → 画布高 800
    // 居中：中心 X = OFF_X - S*avgY = W/2 → OFF_X = W/2 + S*avgY；同理 OFF_Y = H/2 - S*avgX
    SCALE = Math.min(W * 0.95 / CH, H * 0.95 / CW);
    OFF_X = W / 2 + (minY + maxY) / 2 * SCALE;
    OFF_Y = H / 2 - (minX + maxX) / 2 * SCALE;
} else {
    SCALE = Math.min(W * 0.95 / CW, H * 0.95 / CH);
    OFF_X = (W - CW * SCALE) / 2 - minX * SCALE;
    OFF_Y = (H - CH * SCALE) / 2 - minY * SCALE;
}
console.log(`bbox ${minX.toFixed(0)}~${maxX.toFixed(0)},${minY.toFixed(0)}~${maxY.toFixed(0)} scale=${SCALE.toFixed(3)}${landscape ? ' landscape' : ''}`);

const renderer = new spine.canvas.SkeletonRenderer(ctx);
renderer.triangleRendering = true;   // 官方三角形渲染：Region + Mesh 都画（drawImages 只画 Region）

// 收集全部槽：Default 挂载的（可见）+ Default 时附件为 null 的隐藏槽
// （Spine 换链设计：动作时隐藏槽激活——如 Attack 时 F_L_Arm 隐藏、F_L_Arm_2 显示。
//   隐藏槽用皮肤第一个附件 + setup 姿势渲染（Default 动画会把隐藏链移到屏幕外），
//   场景里 visible=false，动作播放时由可见性轨道激活）
// 图层名用槽名（唯一）：两个槽可能挂同名附件（raw 文件名冲突）
const skin0 = data.defaultSkin || data.skins[0];
const entries = [];
for (const s of skel.slots) {
    const at = s.getAttachment();
    if (at) {
        entries.push({ slotIndex: s.data.index, name: s.data.name, att: at, hidden: false });
        continue;
    }
    const map = skin0.attachments[s.data.index];
    if (map && Object.keys(map).length > 0) {
        const nm = Object.keys(map)[0];
        if (map[nm] && map[nm].region)
            entries.push({ slotIndex: s.data.index, name: s.data.name, att: map[nm], hidden: true });
    }
}
console.log(`槽 ${entries.length} 个（隐藏 ${entries.filter(e => e.hidden).length}）`);

// 附件变体收集：动画 attachment timeline 引用的非 Default 附件（Spine 换附件：
// Start 的 F_Eye→F_Eye_3 闭眼线等）。PPD 层无纹理切换——变体导出为隐藏层，
// anims 用 vis 轨道在切换区间激活（与换链隐藏槽同机制，设备端零改动）
const defaultAtt = new Map();   // slotIndex -> Default 姿势附件名
for (const e of entries) defaultAtt.set(e.slotIndex, e.att.name || '');
const variants = [];   // { slotIndex, att }
for (const a of data.animations) {
    for (const tl of a.timelines) {
        if (tl.constructor.name !== 'AttachmentTimeline') continue;
        const dn = defaultAtt.get(tl.slotIndex) || '';
        for (const nm of tl.attachmentNames) {
            if (!nm || nm === dn) continue;
            const map = skin0.attachments[tl.slotIndex];
            const at = map && map[nm];
            if (at && at.region && !variants.some(v => v.slotIndex === tl.slotIndex && v.att.name === nm))
                variants.push({ slotIndex: tl.slotIndex, att: at });
        }
    }
}
console.log(`附件变体 ${variants.length} 个（${variants.map(v => v.att.name).join(', ')}）`);

// head 骨骼子树判定
const isHeadBone = bname => {
    for (const bd of data.bones) {
        if (bd.name === bname) {
            let cur = bd;
            while (cur) { if (cur.name === 'F_Head') return true; cur = cur.parent; }
            return false;
        }
    }
    return false;
};

const layers = [];
let headCenter = null, mouthCenter = null;

// 渲染单个附件 → 像素扫描 → 写 raw → 返回 {entry, hidden}（null=空层）
// useDefault：true=Default 姿势（显式层/附件变体），false=setup 姿势（隐藏槽，
// Default 动画会把隐藏链移到屏幕外）。Default 姿势渲染为空时回退 setup 姿势
// （基建场景的部分姿势链被 Default 动画移出屏幕——附件非 null 但渲染空），
// 返回 hidden=true，anims 轨道基准相应取 setup（与隐藏槽一致）
function exportLayer(slotIndex, name, att, useDefault) {
    // Spine additive(加色混合)层:官方渲染器直接画 → 得到的是"半透明实心方块"贴图,
    // PPD 引擎只做普通 alpha 混合 → 发光特效变成突兀方块(凯尔希基建 C_Mon3tr_Light)。
    // 烘焙转换:alpha = 亮度(黑=透明、亮=发光),颜色保留 → 普通混合也呈光晕效果。
    const isAdditive = data.slots[slotIndex].blendMode === 1;   // spine 枚举:1=additive
    const pose = () => {
        skel.setToSetupPose();
        if (useDefault && defaultAnim) defaultAnim.apply(skel, 0, 0, false, null, 1, 0, 0);
        for (const s of skel.slots) s.setAttachment(null);
        const slot = skel.slots[slotIndex];
        slot.setAttachment(att);
        skel.updateWorldTransform();
    };
    const scan = () => {
        // 透明底 + 渲染（正 scale，无 y 翻转；横屏模式先顺时针转 90°）
        ctx.clearRect(0, 0, W, H);
        ctx.save();
        ctx.translate(OFF_X, OFF_Y);
        if (landscape) ctx.rotate(Math.PI / 2);
        ctx.scale(SCALE, SCALE);
        renderer.draw(skel);
        ctx.restore();
        // 像素扫描 bbox（缓冲行号向下；Spine 世界 y 向上 → 人物在此缓冲中倒立）
        const px = ctx.getImageData(0, 0, W, H).data;
        let x0 = W, y0 = H, x1 = -1, y1 = -1;
        let cxSum = 0, cySum = 0, cnt = 0;
        let rSum = 0, gSum = 0, bSum = 0, aSum = 0;
        for (let sy = 0; sy < H; sy++) {   // sy = 缓冲行 = 屏幕 y
            const row = sy * W * 4;
            for (let x = 0; x < W; x++) {
                if (px[row + x * 4 + 3] > 100) {   // alpha 阈值
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (sy < y0) y0 = sy;
                    if (sy > y1) y1 = sy;
                    cxSum += x; cySum += sy; cnt++;
                    rSum += px[row + x * 4]; gSum += px[row + x * 4 + 1];
                    bSum += px[row + x * 4 + 2]; aSum += px[row + x * 4 + 3];
                }
            }
        }
        return { px, x0, y0, x1, y1, cxSum, cySum, cnt, rSum, gSum, bSum, aSum };
    };
    pose();
    let s2 = scan();
    let hidden = !useDefault;
    if (s2.cnt === 0 && useDefault) {
        useDefault = false; hidden = true;
        pose();
        s2 = scan();
    }
    const { px, x0, y0, x1, y1, cxSum, cySum, cnt, rSum, gSum, bSum, aSum } = s2;
    if (cnt === 0) { console.log(`  skip ${name} (empty)`); return null; }
    const w = x1 - x0 + 1, h = y1 - y0 + 1;
    // 发光特效层：Spine 3.8 mesh 顶点色 alpha 渐变在 spine-canvas 渲染时被丢弃
    // （只用统一 mesh.color）→ 白方块贴图渲染成均匀半透明方块（凯尔希基建
    // C_Mon3tr_Light）。无法还原顶点色渐变，直接跳过导出（anims 轨道引用该层时
    // 设备端 find_layer 自然跳过，无副作用）。
    const meanR = rSum / cnt, meanG = gSum / cnt, meanB = bSum / cnt, meanA = aSum / cnt;
    const isGlowBlock = (att && att.constructor.name === 'MeshAttachment') &&
        (cnt / (w * h) > 0.85) && meanR > 200 && meanG > 200 && meanB > 200;
    if (isGlowBlock) { console.log(`  [发光特效层跳过] ${name}`); return null; }
    // 180° 旋转：缓冲中人物倒立（Spine 世界 y 向上 vs 缓冲行向下），
    // 设备场景空间 y 向下、人物正立 —— 整幅绕画布中心转 180°：
    // 图层 (x,y)→(W-1-x1, H-1-y1)，质心 (cx,cy)→(W-1-cx, H-1-cy)，
    // 纹理行列都反转（初版只翻 y 漏翻 x → 内容左右颠倒）
    const fx0 = W - x0 - w;                        // 旋转后图层左上角
    const fy0 = H - y0 - h;
    const fx1 = W - 1 - x0;
    const fy1 = H - 1 - y0;
    const fcx = W - 1 - cxSum / cnt;
    const fcy = H - 1 - cySum / cnt;

    // 裁切保存 raw（u16w,u16h + RGBA8888；行反转+列反转 = 内容 180° 旋转）
    const wbuf = Buffer.alloc(w * h * 4);
    for (let dy = 0; dy < h; dy++) {
        const srcRow = (y1 - dy) * W * 4;
        for (let dx = 0; dx < w; dx++) {
            const srcOff = srcRow + (x1 - dx) * 4;
            const dstOff = (dy * w + dx) * 4;
            wbuf[dstOff] = px[srcOff];
            wbuf[dstOff + 1] = px[srcOff + 1];
            wbuf[dstOff + 2] = px[srcOff + 2];
            wbuf[dstOff + 3] = px[srcOff + 3];
            if (isAdditive) {
                const lum = Math.round(0.299 * wbuf[dstOff] + 0.587 * wbuf[dstOff + 1]
                                     + 0.114 * wbuf[dstOff + 2]);
                wbuf[dstOff + 3] = Math.min(255, lum);
            }
        }
    }
    if (isAdditive) console.log(`  [additive→alpha=亮度] ${name}`);
    const head = Buffer.alloc(4);
    head.writeUInt16LE(w, 0); head.writeUInt16LE(h, 2);
    const rawName = `${name}.raw`;
    fs.writeFileSync(path.join(outDir, rawName), Buffer.concat([head, wbuf]));
    return { entry: { name, x: fx0, y: fy0, w, h,
                      cx: Math.round(fcx), cy: Math.round(fcy),
                      bbox: [fx0, fy0, fx1, fy1] },
             hidden };
}

for (const { slotIndex, name, att, hidden } of entries) {
    const r = exportLayer(slotIndex, name, att, !hidden);
    if (!r) continue;
    const ly = r.entry;
    layers.push({
        ...ly,
        z: slotIndex,
        group: isHeadBone(data.slots[slotIndex].boneData.name) ? 'head' : 'body',
        // Q 版：F_Mouth 嘴层 + F_Eye 单层双眼条标 eye-l（设备端 F_Eye 走眨眼缩放）
        special: name.startsWith('F_Mouth') ? 'mouth' : (name === 'F_Eye' ? 'eye-l' : ''),
        visible: !hidden && !r.hidden,   // 隐藏槽/Default 渲染空回退 setup 的槽默认不渲染（动画时由可见性轨道激活）
    });
    // pivot 同样取 180° 旋转后的质心（与图层同系——设备端直接在同一场景空间用 pivot 锚定；
    // 默认附件名可能带 _2/_3 后缀（F_Mouth_3 等变体））
    if (name === 'F_Head') headCenter = [ly.cx, ly.cy];
    if (name.startsWith('F_Mouth')) mouthCenter = [ly.cx, ly.cy];
    console.log(`  ${name}: ${ly.w}x${ly.h} at (${ly.x},${ly.y}) cx,cy=(${ly.cx},${ly.cy})${r.hidden ? ' [setup]' : ''}`);
}

// 附件变体层（换附件：F_Eye_3 闭眼线等）：Default 姿势渲染、hidden——
// anims 在切换区间用 vis 轨道激活。层名 = 附件名 + '_v'：附件名可能与
// 换链槽名重名（F_Emoticon_2/F_Scarf 既是槽名又是附件名），曾直接附件名
// 导致轨道键冲突（槽层与变体层共用 frames 键，轨道长度翻倍）
for (const v of variants) {
    const vname = v.att.name + '_v';
    if (layers.some(l => l.name === vname)) { console.log(`  skip variant ${vname} (name conflict)`); continue; }
    const r = exportLayer(v.slotIndex, vname, v.att, true);
    if (!r) continue;
    const ly = r.entry;
    layers.push({ ...ly, z: v.slotIndex,
                  group: isHeadBone(data.slots[v.slotIndex].boneData.name) ? 'head' : 'body',
                  special: '', visible: false });
    console.log(`  ${vname} (variant): ${ly.w}x${ly.h} at (${ly.x},${ly.y})`);
}

// Q 版拆眼：F_Eye 是单层双眼条（两只眼睛同一纹理），整层锚中心眨眼会把双眼拉向中央。
// 沿长轴找最大空隙切成 F_Eye_L/F_Eye_R 两段（各自锚自己的质心眨眼，原地闭合）。
// 返回原 F_Eye 条子质心（场景系）→ scene.eye_bar：anims 拆层补偿的旋转修正基准
// （单眼轨道 = 条子轨道 + (R-I)·(单眼bbox中心-条子质心)，否则双眼离条心远、
// 大角度旋转时错位十几像素——设备端"眼睛角度不对"根因）
function splitEyeLayer(layers, outDir) {
    const i = layers.findIndex(l => l.name === 'F_Eye');
    if (i < 0) return null;
    const ly = layers[i];
    const rawPath = path.join(outDir, 'F_Eye.raw');
    const buf = fs.readFileSync(rawPath);
    const w = buf.readUInt16LE(0), h = buf.readUInt16LE(2);
    const px = buf.subarray(4);
    const alongY = h > w;   // 长轴 = y（横屏眼条沿 y 延伸）
    const n = alongY ? h : w;
    const counts = new Array(n).fill(0);
    for (let k = 0; k < n; k++) {
        let c = 0;
        if (alongY) {
            for (let x = 0; x < w; x++) if (px[(k * w + x) * 4 + 3] > 100) c++;
        } else {
            for (let y = 0; y < h; y++) if (px[(y * w + k) * 4 + 3] > 100) c++;
        }
        counts[k] = c;
    }
    // 最大中间空隙（跳过两端零区）
    let bestStart = -1, bestLen = -1;
    let gapStart = -1;
    for (let k = 1; k < n - 1; k++) {
        if (counts[k] === 0) {
            if (gapStart < 0) gapStart = k;
        } else {
            if (gapStart > 1) {   // 起点不是边缘
                const len = k - gapStart;
                if (len > bestLen) { bestLen = len; bestStart = gapStart; }
            }
            gapStart = -1;
        }
    }
    if (bestLen < 0) { console.log('  F_Eye 无中间空隙，保持单层（eye-l）'); return null; }
    const mid = Math.floor(bestStart + bestLen / 2);
    // 切两段 → 各自扫描 bbox/质心 → 写 raw + 替换图层条目
    const parts = alongY
        ? [{ n0: 0, n1: mid, name: 'F_Eye_L' }, { n0: mid + 1, n1: h, name: 'F_Eye_R' }]
        : [{ n0: 0, n1: mid, name: 'F_Eye_L' }, { n0: mid + 1, n1: w, name: 'F_Eye_R' }];
    const newEntries = [];
    for (const p of parts) {
        // 段内扫描
        let x0 = alongY ? w : p.n0, y0 = alongY ? p.n0 : h, x1 = -1, y1 = -1, cnt = 0;
        let cxSum = 0, cySum = 0;
        for (let yy = alongY ? p.n0 : 0; yy < (alongY ? p.n1 : h); yy++) {
            for (let xx = alongY ? 0 : p.n0; xx < (alongY ? w : p.n1); xx++) {
                if (px[(yy * w + xx) * 4 + 3] > 100) {
                    if (xx < x0) x0 = xx; if (xx > x1) x1 = xx;
                    if (yy < y0) y0 = yy; if (yy > y1) y1 = yy;
                    cxSum += xx; cySum += yy; cnt++;
                }
            }
        }
        if (cnt === 0) { console.log(`  ${p.name} 段为空，跳过拆分`); return null; }
        const pw = x1 - x0 + 1, ph = y1 - y0 + 1;
        const wbuf = Buffer.alloc(pw * ph * 4);
        for (let dy = 0; dy < ph; dy++)
            wbuf.set(px.subarray(((y0 + dy) * w + x0) * 4, ((y0 + dy) * w + x0) * 4 + pw * 4), dy * pw * 4);
        const head = Buffer.alloc(4);
        head.writeUInt16LE(pw, 0); head.writeUInt16LE(ph, 2);
        fs.writeFileSync(path.join(outDir, `${p.name}.raw`), Buffer.concat([head, wbuf]));
        const pcx = cxSum / cnt, pcy = cySum / cnt;
        newEntries.push({
            ...ly,
            name: p.name,
            x: ly.x + x0, y: ly.y + y0, w: pw, h: ph,
            cx: ly.x + Math.round(pcx), cy: ly.y + Math.round(pcy),
            bbox: [ly.x + x0, ly.y + y0, ly.x + x1, ly.y + y1],
            special: p.name === 'F_Eye_L' ? 'eye-l' : 'eye-r',
        });
        console.log(`  ${p.name}: ${pw}x${ph} at (${ly.x + x0},${ly.y + y0})`);
    }
    fs.unlinkSync(rawPath);
    layers.splice(i, 1, ...newEntries);
    console.log('  F_Eye 拆分为 L/R 双图层（各自锚中心眨眼）');
    return [ly.cx, ly.cy];   // 原双眼条质心（场景系）→ scene.eye_bar
}
const eyeBar = splitEyeLayer(layers, outDir);

const scene = {
    name: sceneName,
    landscape: !!landscape,   // 横屏布局标记（设备端眨眼/摸头方向适配）
    eye_bar: eyeBar,          // 原双眼条质心（anims 拆层补偿的旋转修正基准；未拆分=null）
    head_pivot: headCenter || [240, 400],
    mouth_pivot: mouthCenter || [240, 400],
    face_tilt: 5.0, no_cavity: true, mouth_compress: true,
    mouth_gain: 2.5, mouth_floor: 0.45, mouth_width_open: 0.3, mouth_open_gain: 0.55, hair_factor: 0.7,
    layers,
};
fs.writeFileSync(path.join(outDir, 'scene.json'), JSON.stringify(scene, null, 1));
console.log(`scene.json: ${layers.length} layers → ${outDir}`);
console.log('head_pivot:', scene.head_pivot, 'mouth_pivot:', scene.mouth_pivot);
