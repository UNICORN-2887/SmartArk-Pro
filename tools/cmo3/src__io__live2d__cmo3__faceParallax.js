/**
 * FaceParallax warp emit helper.
 *
 * Extracts §3d.2 FaceParallax out of `generateCmo3`. The section emits a
 * single 6×6 warp deformer with 9 keyforms on AngleX × AngleY, parented to
 * Face Rotation. All face-tagged rig warps reparent under it.
 *
 * This is the largest block in the cmo3 pipeline. Key sub-pieces:
 *   - Protected regions (eye/brow/nose/mouth/ear): build mesh-bbox regions,
 *     A.3 L/R pair symmetrization, A.6b grid-cell halfU/halfV expansion.
 *   - `computeFpKeyform(ax, ay)`: 3D rotation of a cylindrical dome plus
 *     #3 eye-parallax amp and #5 far-eye squash.
 *   - `symmetrizeKeyform`: enforce horizontal symmetry at ax=0 keyforms.
 *   - Deformer serialization.
 *
 * See docs/live2d-export/SESSION_2[0-6]_FINDINGS.md + AUTO_RIG_PLAN.md.
 *
 * @module io/live2d/cmo3/faceParallax
 */

import { uuid } from '../xmlbuilder.js';
import { emitKfBinding } from './deformerEmit.js';

/**
 * Emit the FaceParallax warp. Returns `pidFpGuid` (so face rig warps can
 * reparent to it), or `null` if `pidParamAngleX` / `pidParamAngleY` aren't
 * defined.
 *
 * @param {XmlBuilder} x
 * @param {Object} ctx
 * @param {string} ctx.pidParamAngleX
 * @param {string} ctx.pidParamAngleY
 * @param {string} ctx.pidFaceRotGuid
 * @param {{minX:number, minY:number, maxX:number, maxY:number, W:number, H:number}} ctx.faceUnionBbox
 * @param {number} ctx.facePivotCx
 * @param {number} ctx.facePivotCy
 * @param {{minX:number, minY:number, maxX:number, maxY:number}|null} ctx.faceMeshBbox
 * @param {Array} ctx.meshes
 * @param {Array} ctx.allDeformerSources
 * @param {Object} ctx.rootPart
 * @param {string} ctx.pidPartGuid
 * @param {string} ctx.pidCoord
 * @param {Object|null} ctx.rigDebugLog
 * @returns {string|null} pidFpGuid
 */
export function emitFaceParallax(x, ctx) {
  const {
    pidParamAngleX, pidParamAngleY,
    pidFaceRotGuid,
    faceUnionBbox, facePivotCx, facePivotCy, faceMeshBbox,
    meshes,
    allDeformerSources, rootPart,
    pidPartGuid, pidCoord,
    rigDebugLog,
  } = ctx;

  // ── FaceParallax warps (7 groups, 6×6 grid, 9kf on AngleX × AngleY) ──
  const fpCol = 5, fpRow = 5; // 6×6 control points (matches Hiyori)
  const fpGW = fpCol + 1, fpGH = fpRow + 1;
  const fpGridPts = fpGW * fpGH;
  // Hiyori keyform order: Y-fast (AngleY inner, AngleX outer).
  // Binding array order: AngleY first, AngleX second.
  const fpAngleKeys = [-30, 0, 30];
  const fpKeyCombos = []; // [angleX, angleY] in storage order
  for (let xi = 0; xi < 3; xi++) {
    for (let yi = 0; yi < 3; yi++) {
      fpKeyCombos.push([fpAngleKeys[xi], fpAngleKeys[yi]]);
    }
  }

  if (!(pidParamAngleX && pidParamAngleY)) return null;

  // SINGLE FaceParallax warp over the whole face union bbox.
  // Follows Session 15 Body X pattern: uniform rest grid in parent's 0..1 space,
  // keyforms apply a parametric BOW deformation that varies with (cf, rf) grid
  // position. All face meshes are children of this one warp via their rig warps.

  // Rest grid in "Face Rotation's local frame" = canvas-pixel OFFSETS from the
  // face rotation pivot.  Evidence (Hiyori 50+ rotation deformers):
  //   - Rotation deformer children see parent's local frame as canvas-pixel
  //     offsets from parent's own pivot, NOT 0..1 of any warp domain.
  //   - CoordType "DeformerLocal" means "parent's local frame" (whatever it is),
  //     not literally 0..1.
  //   - Hiyori FaceParallax grids are pixel-offset values like (-60..292, -435..-45)
  //     relative to Face Rotation's canvas pivot, not 0..1.
  // Using Body X 0..1 values here (Session 19 attempts) collapsed the face to
  // canvas ~(0,0) because Cubism interpreted 0..1 values as pixel offsets of < 1 px.
  const fpRestLocal = new Float64Array(fpGridPts * 2);
  for (let r = 0; r < fpGH; r++) {
    for (let c = 0; c < fpGW; c++) {
      const idx = (r * fpGW + c) * 2;
      fpRestLocal[idx]     = (faceUnionBbox.minX + c * faceUnionBbox.W / fpCol) - facePivotCx;
      fpRestLocal[idx + 1] = (faceUnionBbox.minY + r * faceUnionBbox.H / fpRow) - facePivotCy;
    }
  }
  // Span for scaling bow magnitudes — canvas-pixel width/height of face bbox.
  const fpSpanX_bx = faceUnionBbox.W;
  const fpSpanY_bx = faceUnionBbox.H;

  // ── P8 (Apr 2026): Depth-weighted ellipsoidal face parallax ──
  // Replaces parametric bow/persp/cross-axis with 3D rotation of a virtual
  // hemisphere centered on the face. Each grid point gets a Z proportional
  // to distance from face center (ellipsoidal falloff). At ±30° param, we
  // rotate the (u, v, z) point around the Y/X axes and project back to 2D.
  //
  // Natural behaviors that emerge from the geometry (not hand-tuned):
  //   - Center of face shifts most (high Z), edges shift least (Z≈0)
  //   - Perspective foreshortening (far side slightly less visible)
  //   - Asymmetric shifts on asymmetric rest poses (tilted heads handled correctly)
  //
  // Tunables:
  //   FP_DEPTH_K         — depth magnitude (0 = flat, 1 = full hemisphere)
  //   FP_MAX_ANGLE_X/Y   — virtual head rotation at ParamAngle = ±30
  //
  // Rotation center = face mesh center (anatomical face). Fallback to union.
  //
  // Phase A.1 (Session 25): force-symmetric face bbox around the face X
  // center. Real art is never perfectly symmetric — e.g. shelby's jawline
  // has a few px of L/R difference from perspective shading, and girl's
  // drawn head-tilt gives a ~20 px asymmetry. The parallax math then
  // produces different shifts at mirror grid points, which looks as
  // "left eye ascends while right eye descends" under AngleY. Forcing a
  // symmetric half-width gives us u=±1 at equal canvas distances from
  // the face center, so mirror grid points see identical geometry.
  //
  const faceMeshCxLocal = faceMeshBbox
    ? (faceMeshBbox.minX + faceMeshBbox.maxX) / 2
    : facePivotCx;
  const faceMeshCyLocal = faceMeshBbox
    ? (faceMeshBbox.minY + faceMeshBbox.maxY) / 2
    : (faceUnionBbox.minY + faceUnionBbox.maxY) / 2;
  let fpRadiusX, fpRadiusY;
  if (faceMeshBbox) {
    // Phase A.1: symmetric half-width = max of (centerX - minX, maxX - centerX).
    // Extends the shorter side so left and right match. Y stays as-is
    // (vertical asymmetry is expected — chin below eyes).
    const halfLeft  = faceMeshCxLocal - faceMeshBbox.minX;
    const halfRight = faceMeshBbox.maxX - faceMeshCxLocal;
    fpRadiusX = Math.max(halfLeft, halfRight);
    fpRadiusY = (faceMeshBbox.maxY - faceMeshBbox.minY) / 2;
  } else {
    fpRadiusX = fpSpanX_bx / 2;
    fpRadiusY = fpSpanY_bx / 2;
  }
  const FP_DEPTH_K         = 0.80;  // Z at face center (cylindrical fallback)
  const FP_EDGE_DEPTH_K    = 0.30;  // Z at face edges (cylindrical fallback)
  const FP_MAX_ANGLE_X_DEG = 15;
  // Session 25b: reduced 12° → 8° because user reported "goblin" on
  // shelby at AngleY=±30 — the combination of AMP=3.0 (from A.4) and
  // 12° virtual rotation around the X-axis produced too-aggressive
  // vertical compression (forehead stretched, chin receded). AngleX
  // stays at 15° because horizontal swing needs more amplitude to
  // read as a head turn.
  const FP_MAX_ANGLE_Y_DEG = 8;

  // Cylindrical dome depth model. Depth-PSD driven per-pixel Z was
  // evaluated in Sessions 20–25 but found to add noise without clear
  // wins across character types (see session cleanup). The dome is a
  // simple u-driven hemisphere and is always symmetric — the default
  // and only path now.
  const FP_DEPTH_AMP = 3.0;
  const fpZAt = (_canvasGx, _canvasGy, u) => {
    const uu = u * u;
    const dome = uu < 1 ? Math.sqrt(1 - uu) : 0;
    return FP_EDGE_DEPTH_K + (FP_DEPTH_K - FP_EDGE_DEPTH_K) * dome;
  };

  // Protected regions: tagged meshes that should rigidly translate (not stretch)
  // during parallax. Grid points inside/near these regions get blended toward
  // the rigid shift at the region's center instead of their own position's shift.
  // Value is 0..1: 0 = no protection (full parallax), 1 = fully rigid at center.
  const FP_PROTECTION_STRENGTH = 1.0; // global multiplier on all protection values
  const PROTECTION_PER_TAG = {
    // Fully rigid — features that should translate as a unit, never deform,
    // under head rotation. Bumped from 0.95 to 1.00 so eye sub-meshes don't
    // drift apart at extreme angles (fixes per-feature "stretch-to-goblin"
    // on realistic art when geometric depth drives protection).
    'eyelash':     1.00, 'eyelash-l':  1.00, 'eyelash-r':  1.00,
    'eyewhite':    1.00, 'eyewhite-l': 1.00, 'eyewhite-r': 1.00,
    'irides':      1.00, 'irides-l':   1.00, 'irides-r':   1.00,
    // Ears and eyebrows also translate rigidly. Previously unprotected
    // (ears) or half-protected (eyebrow 0.50), which left them free to
    // bend under the grid's bow → user-reported "too strong" deformation.
    'ears':        0.90, 'ears-l':     0.90, 'ears-r':     0.90,
    'eyebrow':     0.80, 'eyebrow-l':  0.80, 'eyebrow-r':  0.80,
    // Nose and mouth partial — these SHOULD flex slightly with face curvature.
    'mouth':       0.30,
    'nose':        0.30,
  };
  // Extra falloff buffer around each region (in normalized u/v units).
  // Larger = smoother transition to natural parallax.
  const FP_PROTECTION_FALLOFF_BUFFER = 0.12;

  // Super-groups: meshes that should behave as ONE protected region.
  // Eye sub-meshes (eyelash + eyewhite + irides) share a single anchor
  // per side — they're anatomically ONE eye, must rigid-translate as a
  // unit. Previously each sub-mesh was its own region with its own
  // (ru, rv, rz); slight coordinate differences between 3 sub-meshes
  // produced averaging artifacts where left and right eyes received
  // inconsistent rigid shifts (left drifts while right stays).
  const SUPER_GROUPS = {
    'eye-l': ['eyelash-l', 'eyewhite-l', 'irides-l'],
    'eye-r': ['eyelash-r', 'eyewhite-r', 'irides-r'],
  };
  const meshByTag = new Map();
  for (const m of meshes) {
    if (m.tag) meshByTag.set(m.tag, m);
  }
  const meshesInSuperGroups = new Set();
  for (const tags of Object.values(SUPER_GROUPS)) {
    for (const t of tags) {
      if (meshByTag.has(t)) meshesInSuperGroups.add(t);
    }
  }

  // Helper: compute vertex bbox union across multiple meshes.
  const unionVertexBbox = (meshList) => {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    let count = 0;
    for (const mm of meshList) {
      const vv = mm.vertices;
      if (!vv || vv.length < 2) continue;
      for (let i = 0; i < vv.length; i += 2) {
        if (vv[i]     < minX) minX = vv[i];
        if (vv[i]     > maxX) maxX = vv[i];
        if (vv[i + 1] < minY) minY = vv[i + 1];
        if (vv[i + 1] > maxY) maxY = vv[i + 1];
      }
      count++;
    }
    if (count === 0 || maxX <= minX || maxY <= minY) return null;
    return { minX, minY, maxX, maxY };
  };

  // Build protected regions: super-groups become ONE region each,
  // per-mesh entries cover non-grouped tags (brows, ears, mouth, nose).
  // Each region records inner halfU/halfV (mesh bbox) AND outer
  // falloffU/falloffV (bbox + buffer). Grid cells INSIDE the inner bbox
  // get full rigid protection (no blend → no stretch at mesh boundary).
  // Grid cells BETWEEN inner and outer boundary get graduated fade.
  // Grid cells OUTSIDE outer boundary skip this region.
  const protectedRegions = [];
  for (const [groupTag, memberTags] of Object.entries(SUPER_GROUPS)) {
    const memberMeshes = memberTags.map(t => meshByTag.get(t)).filter(Boolean);
    if (!memberMeshes.length) continue;
    const bbox = unionVertexBbox(memberMeshes);
    if (!bbox) continue;
    const rcx = (bbox.minX + bbox.maxX) / 2;
    const rcy = (bbox.minY + bbox.maxY) / 2;
    const ru = fpRadiusX > 0 ? (rcx - faceMeshCxLocal) / fpRadiusX : 0;
    const rv = fpRadiusY > 0 ? (rcy - faceMeshCyLocal) / fpRadiusY : 0;
    const rz = fpZAt(rcx, rcy, ru);
    const halfU = fpRadiusX > 0 ? (bbox.maxX - bbox.minX) / (2 * fpRadiusX) : 0.05;
    const halfV = fpRadiusY > 0 ? (bbox.maxY - bbox.minY) / (2 * fpRadiusY) : 0.05;
    protectedRegions.push({
      tag: groupTag,
      protection: 1.00 * FP_PROTECTION_STRENGTH,
      u: ru, v: rv, z: rz,
      halfU, halfV,
      falloffU: halfU + FP_PROTECTION_FALLOFF_BUFFER,
      falloffV: halfV + FP_PROTECTION_FALLOFF_BUFFER,
    });
  }
  for (const m of meshes) {
    if (meshesInSuperGroups.has(m.tag)) continue; // handled via super-group
    const basePro = PROTECTION_PER_TAG[m.tag];
    if (basePro == null) continue;
    const v = m.vertices;
    if (!v || v.length < 2) continue;
    let rMinX = Infinity, rMinY = Infinity, rMaxX = -Infinity, rMaxY = -Infinity;
    for (let i = 0; i < v.length; i += 2) {
      if (v[i]     < rMinX) rMinX = v[i];
      if (v[i]     > rMaxX) rMaxX = v[i];
      if (v[i + 1] < rMinY) rMinY = v[i + 1];
      if (v[i + 1] > rMaxY) rMaxY = v[i + 1];
    }
    if (rMaxX <= rMinX || rMaxY <= rMinY) continue;
    const rcx = (rMinX + rMaxX) / 2;
    const rcy = (rMinY + rMaxY) / 2;
    const ru = fpRadiusX > 0 ? (rcx - faceMeshCxLocal) / fpRadiusX : 0;
    const rv = fpRadiusY > 0 ? (rcy - faceMeshCyLocal) / fpRadiusY : 0;
    const rz = fpZAt(rcx, rcy, ru);
    const halfU = fpRadiusX > 0 ? (rMaxX - rMinX) / (2 * fpRadiusX) : 0.05;
    const halfV = fpRadiusY > 0 ? (rMaxY - rMinY) / (2 * fpRadiusY) : 0.05;
    protectedRegions.push({
      tag: m.tag,
      protection: basePro * FP_PROTECTION_STRENGTH,
      u: ru, v: rv, z: rz,
      halfU, halfV,
      falloffU: halfU + FP_PROTECTION_FALLOFF_BUFFER,
      falloffV: halfV + FP_PROTECTION_FALLOFF_BUFFER,
    });
  }

  // ── Phase A.3 (Session 25): pair L/R protected regions as exact mirrors ──
  // For paired tags like irides-l/irides-r, the per-mesh bbox centers have
  // sub-pixel L/R asymmetries (from drawn art or PSD alpha anti-aliasing)
  // that propagate into the rigid shift computed per region. Under AngleY
  // that asymmetry manifested as the Session 23 "left eye translates
  // vertically while right eye deforms" bug. Forcing each pair to share
  // an |u|, an averaged v, an averaged z, and averaged half-extents
  // eliminates that asymmetry at the region level.
  //
  {
    const pairKeyFor = (tag) => {
      if (tag.endsWith('-l')) return tag.slice(0, -2);
      if (tag.endsWith('-r')) return tag.slice(0, -2);
      return null;
    };
    const pairs = new Map(); // base tag → { L, R } indices into protectedRegions
    for (let i = 0; i < protectedRegions.length; i++) {
      const r = protectedRegions[i];
      const base = pairKeyFor(r.tag);
      if (!base) continue;
      const slot = r.tag.endsWith('-l') ? 'L' : 'R';
      if (!pairs.has(base)) pairs.set(base, {});
      pairs.get(base)[slot] = i;
    }
    for (const [base, slots] of pairs) {
      if (slots.L == null || slots.R == null) continue;
      const rL = protectedRegions[slots.L];
      const rR = protectedRegions[slots.R];
      // |u| average — preserves which side is left vs right, forces the
      // magnitude to match so |uL| === |uR|.
      const uAbs = (Math.abs(rL.u) + Math.abs(rR.u)) / 2;
      rL.u = rL.u < 0 ? -uAbs : uAbs;
      rR.u = rR.u < 0 ? -uAbs : uAbs;
      // v, z, half-extents, falloff — plain average.
      const vAvg      = (rL.v + rR.v) / 2;
      const zAvg      = (rL.z + rR.z) / 2;
      const halfUAvg  = (rL.halfU + rR.halfU) / 2;
      const halfVAvg  = (rL.halfV + rR.halfV) / 2;
      const falloffUAvg = halfUAvg + FP_PROTECTION_FALLOFF_BUFFER;
      const falloffVAvg = halfVAvg + FP_PROTECTION_FALLOFF_BUFFER;
      rL.v = vAvg; rR.v = vAvg;
      rL.z = zAvg; rR.z = zAvg;
      rL.halfU = halfUAvg; rR.halfU = halfUAvg;
      rL.halfV = halfVAvg; rR.halfV = halfVAvg;
      rL.falloffU = falloffUAvg; rR.falloffU = falloffUAvg;
      rL.falloffV = falloffVAvg; rR.falloffV = falloffVAvg;
    }
  }

  // ── Phase A.6 (Session 25c): grid-sized rigid-zone expansion ──
  // Root cause of the L/R "stroke face" asymmetry that survived A.1–A.3 +
  // rectangular inner zone: the warp grid is SPARSE (6 × 6 on faceUnionBbox).
  // Cell width in u-space ≈ (faceUnionBbox.W / fpCol) / fpRadiusX — typically
  // 0.3–0.5. Eye super-group halfU is only ≈0.1, much smaller than one cell.
  // Rigid protection takes effect on GRID-POINT shifts, but mesh vertices are
  // bilinearly interpolated from 4 surrounding grid corners. When a region's
  // halfU is smaller than the cell, those 4 corners don't all fall inside the
  // rigid zone — some land in the fade zone, contributing position-dependent
  // natural shift. Drawn art has sub-pixel L/R asymmetry in eye-mesh vertex
  // positions, and the natural-shift field varies across u/v/z — so the
  // bilinear blend diverges between L and R vertices even though A.3 forced
  // their REGION parameters to match.
  //
  // Fix: expand every protected region's halfU/halfV by one grid-cell width,
  // so that for any mesh vertex inside the ORIGINAL bbox, all 4 surrounding
  // grid corners land inside the expanded rigid zone. Bilinear interp of four
  // identical rigid shifts is just that rigid shift — no natural-shift leak.
  //
  // Side effect: a small "flat slab" around each feature (eyes, brows, nose,
  // ears, mouth) that rigid-translates as a unit. The FP_PROTECTION_FALLOFF_BUFFER
  // outer fade zone still smooths the transition to surrounding skin. Net
  // result is slightly less parallax detail in the immediate neighborhood of
  // protected features, traded for guaranteed L/R symmetry.
  {
    const cellU = fpRadiusX > 0 ? (faceUnionBbox.W / fpCol) / fpRadiusX : 0;
    const cellV = fpRadiusY > 0 ? (faceUnionBbox.H / fpRow) / fpRadiusY : 0;
    for (const r of protectedRegions) {
      // Preserve the pre-expansion (A.3-averaged mesh) halves, so features
      // like far-eye squash (#5) can scope their effect to the actual
      // mesh bbox instead of the wide A.6b-expanded protection zone.
      r.meshHalfU = r.halfU;
      r.meshHalfV = r.halfV;
      r.halfU += cellU;
      r.halfV += cellV;
      r.falloffU = r.halfU + FP_PROTECTION_FALLOFF_BUFFER;
      r.falloffV = r.halfV + FP_PROTECTION_FALLOFF_BUFFER;
    }
  }

  // Precompute (u, v, z) per grid point.
  //   • With See-Through depth PSD: z sampled from Marigold's face depth
  //     field, giving true per-pixel 3D parallax (nose protrusion, cheek
  //     curvature, chin shape all encoded at the pixel level).
  //   • Without depth PSD: cylindrical dome along V (P10) — Z varies with
  //     u only; preserves legacy behavior for users without See-Through.
  const fpUVZ = new Float64Array(fpGridPts * 3);
  for (let r = 0; r < fpGH; r++) {
    for (let c = 0; c < fpGW; c++) {
      const gi = r * fpGW + c;
      const canvasGx = faceUnionBbox.minX + c * faceUnionBbox.W / fpCol;
      const canvasGy = faceUnionBbox.minY + r * faceUnionBbox.H / fpRow;
      const u = fpRadiusX > 0 ? (canvasGx - faceMeshCxLocal) / fpRadiusX : 0;
      const v = fpRadiusY > 0 ? (canvasGy - faceMeshCyLocal) / fpRadiusY : 0;
      const z = fpZAt(canvasGx, canvasGy, u);
      fpUVZ[gi * 3]     = u;
      fpUVZ[gi * 3 + 1] = v;
      fpUVZ[gi * 3 + 2] = z;
    }
  }

  if (rigDebugLog) {
    // Peak shift: at center (u=0, v=0, z=FP_DEPTH_K), rotation by θ gives u' = z·sin θ.
    const peakThetaX = FP_MAX_ANGLE_X_DEG * Math.PI / 180;
    const peakThetaY = FP_MAX_ANGLE_Y_DEG * Math.PI / 180;
    const peakX = FP_DEPTH_K * Math.sin(peakThetaX) * fpRadiusX;
    const peakY = FP_DEPTH_K * Math.sin(peakThetaY) * fpRadiusY;
    rigDebugLog.faceParallax = {
      algorithm: 'depth-weighted-cylindrical + protected-regions',
      depthAmpScalar: FP_DEPTH_AMP,
      gridCols: fpGW, gridRows: fpGH,
      spanX_canvasPx: fpSpanX_bx, spanY_canvasPx: fpSpanY_bx,
      faceMeshCenter: { cx: faceMeshCxLocal, cy: faceMeshCyLocal },
      fpRadius: { x: fpRadiusX, y: fpRadiusY },
      constants: {
        FP_DEPTH_K,
        FP_EDGE_DEPTH_K,
        FP_MAX_ANGLE_X_DEG,
        FP_MAX_ANGLE_Y_DEG,
        FP_PROTECTION_STRENGTH,
        FP_PROTECTION_FALLOFF_BUFFER,
      },
      peakShifts_canvasPx: {
        angleX_plus30_center: peakX,
        angleY_plus30_center: peakY,
      },
      protectedRegions: protectedRegions.map(r => ({
        tag: r.tag,
        protection: r.protection,
        centerUVZ: { u: r.u, v: r.v, z: r.z },
        falloff: { u: r.falloffU, v: r.falloffV },
      })),
      note: 'Grid point Z from ellipsoidal falloff + per-region protection blend. Protected regions (eyes, brows, mouth, nose) rigidly translate via their center-shift; skin/hair/ears get full depth parallax. FaceParallax grid in canvas-px offsets from facePivot.',
    };
  }
  // Helper: compute a keyform's grid position at (ax, ay) directly via
  // the 3D rotation math. Separated so we can use it for the canonical
  // +ax side and then MIRROR to the -ax side for guaranteed symmetry.
  const computeFpKeyform = (ax, ay) => {
    const thetaX = (ax / 30) * FP_MAX_ANGLE_X_DEG * Math.PI / 180;
    const thetaY = (ay / 30) * FP_MAX_ANGLE_Y_DEG * Math.PI / 180;
    const cosX = Math.cos(thetaX), sinX = Math.sin(thetaX);
    const cosY = Math.cos(thetaY), sinY = Math.sin(thetaY);
    const pos = new Float64Array(fpRestLocal);
    if (ax === 0 && ay === 0) return pos;
    const regionShifts = protectedRegions.map(r => {
      const rUy = r.u * cosX + r.z * sinX;
      const rZy = -r.u * sinX + r.z * cosX;
      const rVp = r.v * cosY - rZy * sinY;
      return { shiftU: rUy - r.u, shiftV: rVp - r.v };
    });
    // ── 3D punch-up: eye parallax amp (Session 25e) ──
    // Eyes sit on the face's convex front surface (high dome-z ≈ 0.78 at
    // u≈±0.25). Under AngleX, the rotation math already gives them a
    // substantial shiftU (~0.19 u ≈ 39px for fpRadiusX=200 at θ=15°).
    // A mild 1.3× amp on that shiftU makes the eyes "pop" slightly more
    // than the surrounding skin, selling the 3D curvature of the face.
    const EYE_PARALLAX_AMP_X = 1.3;
    for (let ri = 0; ri < protectedRegions.length; ri++) {
      const t = protectedRegions[ri].tag;
      if (t === 'eye-l' || t === 'eye-r') {
        regionShifts[ri].shiftU *= EYE_PARALLAX_AMP_X;
      }
    }
    for (let gi = 0; gi < fpGridPts; gi++) {
      const u = fpUVZ[gi * 3];
      const v = fpUVZ[gi * 3 + 1];
      const z = fpUVZ[gi * 3 + 2];
      const uY = u * cosX + z * sinX;
      const zY = -u * sinX + z * cosX;
      const vP = v * cosY - zY * sinY;
      const natShiftU = uY - u;
      const natShiftV = vP - v;
      let totalWeight = 0;
      let rigidShiftU = 0;
      let rigidShiftV = 0;
      for (let ri = 0; ri < protectedRegions.length; ri++) {
        const r = protectedRegions[ri];
        // Inner bbox (halfU/halfV) → full protection (proximity = 1).
        // Outer fade zone (between halfU and falloffU) → graduated.
        // This keeps grid cells INSIDE the mesh bbox from blending
        // rigid+natural shifts, which was producing visible stretch at
        // the mesh boundary (alien-looking eyes under rotation).
        const duInner = (u - r.u) / r.halfU;
        const dvInner = (v - r.v) / r.halfV;
        let proximity;
        // Rectangular bbox test (Chebyshev). Previously we used the
        // INSCRIBED ellipse (duInner² + dvInner² ≤ 1), which covered
        // only ~78% of the bbox and left the 4 CORNERS in the fade
        // zone. Eye-corner mesh vertices (inner/outer canthus, upper
        // eyelash tips) land exactly there — and since drawn art has
        // sub-pixel L/R asymmetry at those tips, they saw different
        // blended shifts and produced the "left eye moves differently
        // than right under AngleY" artifact even AFTER A.1–A.3.
        // Rectangle covers the full bbox so every vertex inside the
        // region's halfU/halfV gets the region-center rigid shift.
        if (Math.abs(duInner) <= 1 && Math.abs(dvInner) <= 1) {
          proximity = 1; // inside rectangular bbox — full rigid protection
        } else {
          const duOuter = (u - r.u) / r.falloffU;
          const dvOuter = (v - r.v) / r.falloffV;
          const distSqOuter = duOuter * duOuter + dvOuter * dvOuter;
          if (distSqOuter >= 1) continue; // outside fade zone
          // Map [1, falloff-boundary] → [1, 0] using (1 - distSqOuter).
          // At bbox edge: distSqOuter = (halfU/falloffU)² < 1 → proximity ≈ 1 - (halfU/falloffU)². This is discontinuous
          // with the inner=1 zone, but in practice `max(inner, outer)`
          // keeps boundary smooth. Use that:
          proximity = Math.max(0, 1 - distSqOuter);
        }
        const w = r.protection * proximity;
        totalWeight += w;
        rigidShiftU += w * regionShifts[ri].shiftU;
        rigidShiftV += w * regionShifts[ri].shiftV;
      }
      const effP = Math.min(1, totalWeight);
      let finalShiftU, finalShiftV;
      if (totalWeight > 0) {
        finalShiftU = natShiftU * (1 - effP) + (rigidShiftU / totalWeight) * effP;
        finalShiftV = natShiftV * (1 - effP) + (rigidShiftV / totalWeight) * effP;
      } else {
        finalShiftU = natShiftU;
        finalShiftV = natShiftV;
      }
      pos[gi * 2]     += finalShiftU * fpRadiusX;
      pos[gi * 2 + 1] += finalShiftV * fpRadiusY;
    }
    // ── 3D punch-up: far-eye squash (Session 25f, effect #5) ──
    // Perspective foreshortening: under head yaw, the FAR eye (side of
    // the face that's rotating away from viewer) appears horizontally
    // narrower. We simulate this by shifting warp grid points on the
    // far eye's OUTER side (u further from face center than r.u) INWARD
    // toward the eye center. Cubism bilinear-interps mesh vertices from
    // grid corners → the outer half of the far eye's mesh compresses,
    // inner half stays. Reads as "the eye is viewed at an angle".
    //
    // Scoped to r.meshHalfU/V (pre-A.6b bbox), not falloffU/V, so the
    // squash doesn't smear into temple / eyebrow area (which would look
    // like a weird dent in the forehead).
    //
    // Condition r.u * sinX < 0: far eye is the one whose u sign differs
    // from thetaX's sign (because +thetaX rotates +z face surface in +u
    // direction, so the -u side recedes into the "back" of the head).
    if (Math.abs(sinX) > 1e-6) {
      const FAR_EYE_SQUASH_AMP = 0.18; // peak inward shift per |sinX|=1
      for (let ri = 0; ri < protectedRegions.length; ri++) {
        const r = protectedRegions[ri];
        if (r.tag !== 'eye-l' && r.tag !== 'eye-r') continue;
        if ((r.u * sinX) >= 0) continue; // near eye — skip
        const squash = Math.abs(sinX) * FAR_EYE_SQUASH_AMP;
        const signU = r.u > 0 ? 1 : -1;
        for (let row = 0; row < fpGH; row++) {
          for (let c = 0; c < fpGW; c++) {
            const gi = row * fpGW + c;
            const u = fpUVZ[gi * 3];
            const v = fpUVZ[gi * 3 + 1];
            const duFromEye = u - r.u;
            const dvFromEye = v - r.v;
            // Outer side only: duFromEye has same sign as r.u
            if (duFromEye * r.u <= 0) continue;
            // Scope to original mesh bbox, not A.6b-expanded zone
            if (Math.abs(duFromEye) > r.meshHalfU) continue;
            if (Math.abs(dvFromEye) > r.meshHalfV) continue;
            // Gradient: full squash at outer edge, fades to 0 at eye center
            const uStr = Math.abs(duFromEye) / r.meshHalfU;
            const vStr = 1 - Math.abs(dvFromEye) / r.meshHalfV;
            pos[gi * 2] += -signU * squash * uStr * vStr * fpRadiusX;
          }
        }
      }
    }
    return pos;
  };

  // Helper: in-keyform horizontal symmetrization. For each (c, r) and
  // its mirror (Cmax-c, r), enforce anti-symmetric X shift and symmetric
  // Y shift. Eliminates asymmetry caused by non-perfectly-symmetric depth
  // field (EDT on real face masks has sub-pixel asymmetries that show as
  // "one eye deforms more than the other" under pure AngleY).
  //
  // Shifts are computed relative to rest positions. Rest positions are
  // geometrically symmetric around fpBboxCenterX (canvas coord =
  // restX + facePivotCx → mirror X around fpBboxCenterX), so a pair's
  // rest-local x coordinates sum to mirrorKx. We symmetrize the SHIFT:
  //   shift(c) + shift(Cmax-c) ≡ pos(c) + pos(Cmax-c) - (rest(c) + rest(Cmax-c))
  //                            = pos(c) + pos(Cmax-c) - mirrorKx
  // For antisymmetric U (mirror shifts cancel when added in a mirror sense):
  //   avgAsymSx = (shift(c) - shift(Cmax-c)) / 2
  // For symmetric V:
  //   avgSymSy = (shift(c).y + shift(Cmax-c).y) / 2
  const symmetrizeKeyform = (pos) => {
    for (let r = 0; r < fpGH; r++) {
      const halfCols = Math.floor(fpGW / 2);
      for (let c = 0; c < halfCols; c++) {
        const mc = fpGW - 1 - c;
        const giL = r * fpGW + c;
        const giR = r * fpGW + mc;
        const restXL = fpRestLocal[giL * 2];
        const restXR = fpRestLocal[giR * 2];
        const restY  = fpRestLocal[giL * 2 + 1]; // same as giR (same row)
        const sxL = pos[giL * 2]     - restXL;
        const syL = pos[giL * 2 + 1] - restY;
        const sxR = pos[giR * 2]     - restXR;
        const syR = pos[giR * 2 + 1] - restY;
        const avgAsymSx = (sxL - sxR) / 2;   // antisymmetric X shift
        const avgSymSy  = (syL + syR) / 2;   // symmetric   Y shift
        pos[giL * 2]     = restXL + avgAsymSx;
        pos[giL * 2 + 1] = restY  + avgSymSy;
        pos[giR * 2]     = restXR - avgAsymSx;
        pos[giR * 2 + 1] = restY  + avgSymSy;
      }
      // Center column (odd fpGW): under symmetric physics, its shift_x
      // at ax=0 should be 0; at ax≠0 it inherits a real nonzero shift
      // from the depth's symmetric z at u=0. Leave as-is (self-mirror).
    }
    return pos;
  };

  // Raw physics for all keyforms, symmetrizing ax=0 keyforms (pure pitch
  // L/R symmetry) to fix "one eye sinks while the other rises" caused by
  // depth-field asymmetry noise.
  const fpGridPositions = [];
  const fpFormGuids = [];
  for (const [ax, ay] of fpKeyCombos) {
    let pos = computeFpKeyform(ax, ay);
    if (ax === 0) pos = symmetrizeKeyform(pos);
    fpGridPositions.push(pos);
    const [, pidForm] = x.shared('CFormGuid', {
      uuid: uuid(), note: `FaceParallax_ax${ax}_ay${ay}`,
    });
    fpFormGuids.push(pidForm);
  }

  // Emit the single FaceParallax deformer (CWarpDeformerSource) targeting Body X.
  const [, pidFpGuid] = x.shared('CDeformerGuid', {
    uuid: uuid(), note: 'FaceParallax',
  });

  // KeyformBindings — AngleY first, AngleX second (Hiyori convention).
  const [fpKfbY, pidFpKfbY] = x.shared('KeyformBindingSource');
  const [fpKfbX, pidFpKfbX] = x.shared('KeyformBindingSource');
  const [fpKfg, pidFpKfg]   = x.shared('KeyformGridSource');
  const fpKfogList = x.sub(fpKfg, 'array_list', {
    'xs.n': 'keyformsOnGrid', count: String(fpKeyCombos.length),
  });
  for (let ki = 0; ki < fpKeyCombos.length; ki++) {
    const ax = fpKeyCombos[ki][0], ay = fpKeyCombos[ki][1];
    const xi = fpAngleKeys.indexOf(ax);
    const yi = fpAngleKeys.indexOf(ay);
    const kog = x.sub(fpKfogList, 'KeyformOnGrid');
    const ak = x.sub(kog, 'KeyformGridAccessKey', { 'xs.n': 'accessKey' });
    const kop = x.sub(ak, 'array_list', { 'xs.n': '_keyOnParameterList', count: '2' });
    const konY = x.sub(kop, 'KeyOnParameter');
    x.subRef(konY, 'KeyformBindingSource', pidFpKfbY, { 'xs.n': 'binding' });
    x.sub(konY, 'i', { 'xs.n': 'keyIndex' }).text = String(yi);
    const konX = x.sub(kop, 'KeyOnParameter');
    x.subRef(konX, 'KeyformBindingSource', pidFpKfbX, { 'xs.n': 'binding' });
    x.sub(konX, 'i', { 'xs.n': 'keyIndex' }).text = String(xi);
    x.subRef(kog, 'CFormGuid', fpFormGuids[ki], { 'xs.n': 'keyformGuid' });
  }
  const fpKfbList = x.sub(fpKfg, 'array_list', { 'xs.n': 'keyformBindings', count: '2' });
  x.subRef(fpKfbList, 'KeyformBindingSource', pidFpKfbY);
  x.subRef(fpKfbList, 'KeyformBindingSource', pidFpKfbX);
  emitKfBinding(x, fpKfbY, pidFpKfg, pidParamAngleY,
    fpAngleKeys.map(k => k + '.0'), 'ParamAngleY');
  emitKfBinding(x, fpKfbX, pidFpKfg, pidParamAngleX,
    fpAngleKeys.map(k => k + '.0'), 'ParamAngleX');

  // Emit the CWarpDeformerSource
  const [fpDf, pidFpDf] = x.shared('CWarpDeformerSource');
  allDeformerSources.push({ pid: pidFpDf, tag: 'CWarpDeformerSource' });
  const fpAcdfs = x.sub(fpDf, 'ACDeformerSource', { 'xs.n': 'super' });
  const fpAcpcs = x.sub(fpAcdfs, 'ACParameterControllableSource', { 'xs.n': 'super' });
  x.sub(fpAcpcs, 's', { 'xs.n': 'localName' }).text = 'FaceParallax';
  x.sub(fpAcpcs, 'b', { 'xs.n': 'isVisible' }).text = 'true';
  x.sub(fpAcpcs, 'b', { 'xs.n': 'isLocked' }).text = 'false';
  x.subRef(fpAcpcs, 'CPartGuid', pidPartGuid, { 'xs.n': 'parentGuid' });
  x.subRef(fpAcpcs, 'KeyformGridSource', pidFpKfg, { 'xs.n': 'keyformGridSource' });
  const fpMft = x.sub(fpAcpcs, 'KeyFormMorphTargetSet', { 'xs.n': 'keyformMorphTargetSet' });
  x.sub(fpMft, 'carray_list', { 'xs.n': '_morphTargets', count: '0' });
  const fpBwc = x.sub(fpMft, 'MorphTargetBlendWeightConstraintSet', { 'xs.n': 'blendWeightConstraintSet' });
  x.sub(fpBwc, 'carray_list', { 'xs.n': '_constraints', count: '0' });
  x.sub(fpAcpcs, 'carray_list', { 'xs.n': '_extensions', count: '0' });
  x.sub(fpAcpcs, 'null', { 'xs.n': 'internalColor_direct_argb' });
  x.sub(fpAcpcs, 'null', { 'xs.n': 'internalColor_indirect_argb' });
  x.subRef(fpAcdfs, 'CDeformerGuid', pidFpGuid, { 'xs.n': 'guid' });
  x.sub(fpAcdfs, 'CDeformerId', { 'xs.n': 'id', idstr: 'FaceParallax' });
  // FaceParallax targets Face Rotation → Body X.  Coord scales:
  //   - Face Rotation pivot:  in Body X 0..1  (its parent is a warp)
  //   - FaceParallax grid:    in canvas-pixel OFFSETS from Face Rotation's pivot
  //                           (its parent is a rotation deformer — see WARP_DEFORMERS.md
  //                           "Rotation Deformer Local Frame" for the evidence).
  // At rest (ParamAngleZ=0) Face Rotation is identity, so the chain is transparent.
  // At ±30 (mapped to ±10° rotation) Face Rotation rotates FaceParallax's grid
  // around the face pivot, producing head tilt for all face rig warp descendants.
  x.subRef(fpAcdfs, 'CDeformerGuid', pidFaceRotGuid, { 'xs.n': 'targetDeformerGuid' });
  x.sub(fpDf, 'i', { 'xs.n': 'col' }).text = String(fpCol);
  x.sub(fpDf, 'i', { 'xs.n': 'row' }).text = String(fpRow);
  x.sub(fpDf, 'b', { 'xs.n': 'isQuadTransform' }).text = 'false';
  const fpKfsList = x.sub(fpDf, 'carray_list', {
    'xs.n': 'keyforms', count: String(fpKeyCombos.length),
  });
  for (let ki = 0; ki < fpKeyCombos.length; ki++) {
    const wdf = x.sub(fpKfsList, 'CWarpDeformerForm');
    const wdfAdf = x.sub(wdf, 'ACDeformerForm', { 'xs.n': 'super' });
    const wdfAcf = x.sub(wdfAdf, 'ACForm', { 'xs.n': 'super' });
    x.subRef(wdfAcf, 'CFormGuid', fpFormGuids[ki], { 'xs.n': 'guid' });
    x.sub(wdfAcf, 'b', { 'xs.n': 'isAnimatedForm' }).text = 'false';
    x.sub(wdfAcf, 'b', { 'xs.n': 'isLocalAnimatedForm' }).text = 'false';
    x.subRef(wdfAcf, 'CWarpDeformerSource', pidFpDf, { 'xs.n': '_source' });
    x.sub(wdfAcf, 'null', { 'xs.n': 'name' });
    x.sub(wdfAcf, 's', { 'xs.n': 'notes' }).text = '';
    x.sub(wdfAdf, 'f', { 'xs.n': 'opacity' }).text = '1.0';
    x.sub(wdfAdf, 'CFloatColor', {
      'xs.n': 'multiplyColor', red: '1.0', green: '1.0', blue: '1.0', alpha: '1.0',
    });
    x.sub(wdfAdf, 'CFloatColor', {
      'xs.n': 'screenColor', red: '0.0', green: '0.0', blue: '0.0', alpha: '1.0',
    });
    x.subRef(wdfAdf, 'CoordType', pidCoord, { 'xs.n': 'coordType' });
    x.sub(wdf, 'float-array', {
      'xs.n': 'positions', count: String(fpGridPts * 2),
    }).text = Array.from(fpGridPositions[ki]).map(v => v.toFixed(6)).join(' ');
  }
  rootPart.childGuidsNode.children.push(x.ref('CDeformerGuid', pidFpGuid));
  rootPart.childGuidsNode.attrs.count = String(rootPart.childGuidsNode.children.length);

  return pidFpGuid;
}
