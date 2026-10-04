// Check local PPD eye/face visibility tracks.
// Usage: node tools/check_eye_tracks.mjs <ppd_out_root>
import fs from 'fs';
import path from 'path';

const root = process.argv[2] || 'spine2ppdpro/data/ppd_out';

function eyeSide(name) {
    if (!/Eye/i.test(name) || /Brow/i.test(name)) return null;
    if (/(^|_)R($|_)|Eye_R|R_Eye/i.test(name)) return 'R';
    if (/(^|_)L($|_)|Eye_L|L_Eye/i.test(name)) return 'L';
    return null;
}

function closeEye(name) {
    return /Eye/i.test(name) && /Close/i.test(name);
}

function faceInfo(name) {
    const m = name.match(/^(.+_Face)_([A-Za-z]+)(?:_|$)/);
    if (!m) return null;
    return { root: m[1], group: m[2] };
}

function visAt(track, frame, fallback) {
    const k = track && track[frame] ? track[frame] : null;
    return k ? (k[3] >= 0.5) : fallback;
}

function checkDir(dir, label) {
    const scenePath = path.join(dir, 'scene.json');
    const animPath = path.join(dir, 'anims.json');
    if (!fs.existsSync(scenePath) || !fs.existsSync(animPath)) return [];
    const scene = JSON.parse(fs.readFileSync(scenePath, 'utf8'));
    const anims = JSON.parse(fs.readFileSync(animPath, 'utf8'));
    const byName = new Map(scene.layers.map(l => [l.name, l]));
    const eyeLayers = scene.layers.filter(l => eyeSide(l.name));
    const faceLayers = scene.layers.filter(l => faceInfo(l.name));
    const issues = [];
    for (const [animName, anim] of Object.entries(anims)) {
        if (!anim || !anim.layers) continue;
        const n = Math.max(1, ...Object.values(anim.layers).map(t => Array.isArray(t) ? t.length : 0));
        for (let f = 0; f < n; f++) {
            for (const side of ['L', 'R']) {
                const sideEyes = eyeLayers.filter(l => eyeSide(l.name) === side);
                if (!sideEyes.length) continue;
                const closes = sideEyes.filter(l => closeEye(l.name) && visAt(anim.layers[l.name], f, !!l.visible));
                const opens = sideEyes.filter(l => !closeEye(l.name) && visAt(anim.layers[l.name], f, !!l.visible));
                if (closes.length && opens.length) {
                    issues.push(`${label} ${animName} f${f} ${side}: close+open ${closes.map(l => l.name).join('|')} / ${opens.map(l => l.name).join('|')}`);
                } else if (!closes.length && opens.length) {
                    const maxArea = Math.max(...sideEyes.filter(l => !closeEye(l.name)).map(l => Math.max(1, l.w * l.h)));
                    const visibleMax = Math.max(...opens.map(l => Math.max(1, l.w * l.h)));
                    if (visibleMax < maxArea * 0.18)
                        issues.push(`${label} ${animName} f${f} ${side}: likely eye base missing, visible=${opens.map(l => l.name).join('|')}`);
                }
            }
            const roots = new Map();
            for (const l of faceLayers) {
                if (!visAt(anim.layers[l.name], f, !!l.visible)) continue;
                const info = faceInfo(l.name);
                const groups = roots.get(info.root) || new Set();
                groups.add(info.group);
                roots.set(info.root, groups);
            }
            for (const [r, groups] of roots) {
                if (groups.size > 1) issues.push(`${label} ${animName} f${f}: face groups overlap ${r} ${[...groups].join('|')}`);
            }
        }
    }
    return issues;
}

const all = [];
for (const c of fs.readdirSync(root).filter(n => n.startsWith('char_') && !n.endsWith('_build'))) {
    const base = path.join(root, c);
    all.push(...checkDir(base, `${c}/正面`));
    const forms = path.join(base, 'forms');
    if (fs.existsSync(forms)) {
        for (const f of fs.readdirSync(forms))
            all.push(...checkDir(path.join(forms, f), `${c}/${f}`));
    }
}

if (!all.length) {
    console.log('eye-check ok');
} else {
    console.log(`eye-check issues ${all.length}`);
    for (const line of all.slice(0, 200)) console.log(line);
    if (all.length > 200) console.log(`... ${all.length - 200} more`);
    process.exitCode = 1;
}
