# Warp Deformer & Parameter Export Audit

## Summary
The native warp deformer implementation (CanvasViewport.jsx) supports **41 parameters** with **13 warp deformers**, but the Live2D export (cmo3writer.js) only exports **20 parameters** with limited warp deformer support. This is a **51% coverage gap** on parameters.

---

## Parameters Comparison

### ✅ EXPORTED to Live2D (20 of 41)
**In cmo3writer.js `standardParams`:**

| Group | Exported Parameters |
|-------|-------------------|
| **Face Rotation** (3/3) | ParamAngleX, ParamAngleY, ParamAngleZ |
| **Eyes** (2/11) | ParamEyeLOpen, ParamEyeROpen |
| **Eyeball** (2/4) | ParamEyeBallX, ParamEyeBallY |
| **Brows** (2/8) | ParamBrowLY, ParamBrowRY |
| **Mouth** (2/2) | ParamMouthForm, ParamMouthOpenY |
| **Body Rotation** (3/3) | ParamBodyAngleX, ParamBodyAngleY, ParamBodyAngleZ |
| **Breath** (1/1) | ParamBreath |
| **Hair** (3/3) | ParamHairFront, ParamHairSide, ParamHairBack |
| **Physics Clothing** (4/4) | ParamSkirt, ParamShirt, ParamPants, ParamBust |
| **Total** | **20/41** ✅ (49%) |

### ❌ NOT EXPORTED (21 of 41)

| Group | Missing Parameters | Count |
|-------|-------------------|-------|
| **Eyes** | ParamEyeLSmile, ParamEyeRSmile, ParamEyeBallForm, ParamTear | 4 |
| **Brows** | ParamBrowLX, ParamBrowRX, ParamBrowLAngle, ParamBrowRAngle, ParamBrowLForm, ParamBrowRForm | 6 |
| **Arms** | ParamArmLA, ParamArmRA, ParamArmLB, ParamArmRB, ParamHandL, ParamHandR | 6 |
| **Shoulders** | ParamShoulderY | 1 |
| **Bust** | ParamBustX, ParamBustY | 2 |
| **Global** | ParamCheek, ParamHairFluffy, ParamBaseX, ParamBaseY | 4 |
| **Total Missing** | | **21/41** ❌ (51%) |

---

## Warp Deformers Comparison

### ✅ EXPORTED to Live2D (6-7 of 13)
**Implemented in cmo3writer.js:**

| Warp Deformer | Parameter | Type | Status |
|--------------|-----------|------|--------|
| `FaceWarp` | ParamAngleX | face_angle_x | ✅ Full |
| `BodyWarp` | ParamBodyAngleX | body_angle_x | ✅ Full |
| `NeckWarp` | ParamAngleZ | neck_follow | ✅ Full (bodyRig.js) |
| `EyeLWarp` | ParamEyeLOpen | eye_open | ✅ Partial (rig mesh warps) |
| `EyeRWarp` | ParamEyeROpen | eye_open | ✅ Partial (rig mesh warps) |
| `MouthWarp` | ParamMouthOpenY | mouth_open | ✅ Partial (rig mesh warps) |
| `EyebrowLWarp` | ParamBrowLY | brow_y | ✅ Partial (rig mesh warps) |
| `EyebrowRWarp` | ParamBrowRY | brow_y | ✅ Partial (rig mesh warps) |

### ❌ NOT EXPORTED (6 of 13)
**Defined in CanvasViewport.jsx WARP_SPECS but missing from cmo3writer:**

| Warp Deformer | Parameter | Type | Status |
|--------------|-----------|------|--------|
| `HairFrontWarp` | ParamHairFront | hair_sway | ❌ No mesh warp, param only |
| `HairBackWarp` | ParamHairBack | hair_sway | ❌ No mesh warp, param only |
| `TopWearWarp` | ParamBodyAngleX | body_angle_x | ❌ No dedicated warp |
| `BottomWearWarp` | ParamBodyAngleX | body_angle_x | ❌ No dedicated warp |
| *(ParamEyeLSmile warp)* | ParamEyeLSmile | eye_smile | ❌ Not even defined |
| *(ParamEyeRSmile warp)* | ParamEyeRSmile | eye_smile | ❌ Not even defined |
| ... and 6+ more warp types | ... | ... | ❌ Missing |

---

## What's Actually in cmo3writer.js

### Warp Deformer Emission Code Paths
1. **Body X Warp** (ParamBodyAngleX) — 5×5 grid, targets Breath parameter
2. **Body Y Warp** (ParamBodyAngleY) — full body Y-axis rotation deformer
3. **Body Z Warp** (ParamBodyAngleZ) — controls breathing/chest expansion
4. **Face Parallax** (ParamAngleX/Y/Z) — multi-parameter face warp with 3D perspective
5. **NeckWarp** (ParamAngleZ) — imported from bodyRig.js
6. **Face Rotation** (rotation deformer, not warp) — imported from bodyRig.js
7. **Per-part rig warps** — eye-open, mouth-open, brow-y driven by individual parameters

### Warp Math Types in cmo3writer vs CanvasViewport

| Warp Type | cmo3writer | CanvasViewport | Coverage |
|-----------|-----------|---|---|
| `face_angle_x` | ✅ Full | ✅ Full | 100% |
| `body_angle_x` | ✅ Full | ✅ Full | 100% |
| `neck_follow` | ✅ Full (bodyRig.js) | ✅ Full | 100% |
| `eye_open` | ✅ Partial (rig warps) | ✅ Full | ~60% |
| `mouth_open` | ✅ Partial (rig warps) | ✅ Full | ~60% |
| `brow_y` | ✅ Partial (rig warps) | ✅ Full | ~60% |
| `hair_sway` | ❌ MISSING | ✅ Full | 0% |
| `body_angle_y` | ✅ Full | ✅ Full | 100% |
| `body_angle_z` | ✅ Full | ✅ Full | 100% |
| `face_angle_y` | ❌ MISSING | ✅ Full | 0% |
| `eye_smile` | ❌ MISSING | ✅ Full | 0% |
| `eye_gaze` | ❌ MISSING | ✅ Full | 0% |
| `bust_wobble` | ❌ MISSING | ✅ Full | 0% |

---

## Issues with Current Documentation (docs/warp_deform_implementation.md)

1. **Section 3.4 "Standard Live2D Coverage"** (line 303–379) claims 13 warp deformers are complete
   - Actually only ~8 are exported to Live2D
   - HairFront/Back warps export parameter but NOT warp mesh deformations
   - TopWear/BottomWear warps not separated in cmo3 (reuse ParamBodyAngleX)

2. **"Known Gaps" section** (line 360) lists 32 missing parameters
   - This is CORRECT for the native implementation
   - But the export has gaps that aren't documented:
     - TopWear/BottomWear warps don't get independent mesh bindings
     - Hair warp keyforms aren't baked into the export (param exists but no mesh warp)

3. **Warp specs vs export mismatch**
   - WARP_SPECS in CanvasViewport defines 10 warp deformer specs
   - cmo3writer only emits ~6-7 of them as actual deformers

---

## Root Cause

**cmo3writer.js emits warp deformers through:**
1. Hardcoded rig warps for specific tags (eye_*, mouth_*, brow_*)
2. Structural warps (Body X/Y/Z, NeckWarp)
3. Face Parallax multi-parameter warp

**But it does NOT:**
- Iterate through WARP_SPECS from the native rig definition
- Generate independent TopWear/BottomWear mesh warps
- Emit hair_sway warp deformations (param exists, deformation missing)
- Automatically create warps for any parameter > 9 missing ones

---

## Next Steps to Close the Gap

1. **Import WARP_SPECS or equivalent** into cmo3writer.js
2. **Emit warp deformers for all WARP_SPECS**, not just hardcoded ones
3. **Separate TopWear and BottomWear** into independent warp deformers (not shared ParamBodyAngleX)
4. **Add hair_sway warp emission** for HairFront/Back
5. **Document the 21 parameters still missing** and define warp specs for them if needed
6. **Test against a Hiyori-like standard rig** to ensure feature parity

