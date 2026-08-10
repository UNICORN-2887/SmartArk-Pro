r"""Live2D .moc3 → PNG frames batch renderer (Python+DLL software rasterizer).

Usage:
    python live2d_render.py "E:\pet\阿米娅" --out "E:\out\amiya_emoji" --size 480x800
"""

import ctypes, os, sys, json, glob, math, numpy as np
from PIL import Image

DLL_PATH = r"E:\Passport\pet\CubismSdkForNative\CubismSdkForNative-5-r.5\Core\dll\windows\x86_64\Live2DCubismCore.dll"
CORE = ctypes.cdll.LoadLibrary(DLL_PATH)

csmMoc = ctypes.c_void_p
csmModel = ctypes.c_void_p

# ── parameter APIs ──
CORE.csmGetParameterCount.argtypes = [csmModel]; CORE.csmGetParameterCount.restype = ctypes.c_int32
CORE.csmGetParameterIds.argtypes = [csmModel]; CORE.csmGetParameterIds.restype = ctypes.POINTER(ctypes.c_char_p)
CORE.csmGetParameterValues.argtypes = [csmModel]; CORE.csmGetParameterValues.restype = ctypes.POINTER(ctypes.c_float)
CORE.csmGetParameterDefaultValues.argtypes = [csmModel]; CORE.csmGetParameterDefaultValues.restype = ctypes.POINTER(ctypes.c_float)
CORE.csmGetParameterMaximumValues.argtypes = [csmModel]; CORE.csmGetParameterMaximumValues.restype = ctypes.POINTER(ctypes.c_float)
CORE.csmGetParameterMinimumValues.argtypes = [csmModel]; CORE.csmGetParameterMinimumValues.restype = ctypes.POINTER(ctypes.c_float)

# ── model APIs ──
CORE.csmReviveMocInPlace.argtypes = [ctypes.c_void_p, ctypes.c_uint32]; CORE.csmReviveMocInPlace.restype = csmMoc
CORE.csmInitializeModelInPlace.argtypes = [csmMoc, ctypes.c_uint32, ctypes.c_uint32]; CORE.csmInitializeModelInPlace.restype = csmModel
CORE.csmUpdateModel.argtypes = [csmModel]; CORE.csmUpdateModel.restype = None
CORE.csmResetDrawableDynamicFlags.argtypes = [csmModel]; CORE.csmResetDrawableDynamicFlags.restype = None

# ── drawable APIs ──
CORE.csmGetDrawableCount.argtypes = [csmModel]; CORE.csmGetDrawableCount.restype = ctypes.c_int32
CORE.csmGetDrawableVertexCounts.argtypes = [csmModel]; CORE.csmGetDrawableVertexCounts.restype = ctypes.POINTER(ctypes.c_int32)
CORE.csmGetDrawableVertexPositions.argtypes = [csmModel]; CORE.csmGetDrawableVertexPositions.restype = ctypes.POINTER(ctypes.c_void_p)
CORE.csmGetDrawableVertexUvs.argtypes = [csmModel]; CORE.csmGetDrawableVertexUvs.restype = ctypes.POINTER(ctypes.c_void_p)
CORE.csmGetDrawableIndices.argtypes = [csmModel]; CORE.csmGetDrawableIndices.restype = ctypes.POINTER(ctypes.c_void_p)
CORE.csmGetDrawableIndexCounts.argtypes = [csmModel]; CORE.csmGetDrawableIndexCounts.restype = ctypes.POINTER(ctypes.c_int32)
CORE.csmGetDrawableOpacities.argtypes = [csmModel]; CORE.csmGetDrawableOpacities.restype = ctypes.POINTER(ctypes.c_float)
CORE.csmGetDrawableDrawOrders.argtypes = [csmModel]; CORE.csmGetDrawableDrawOrders.restype = ctypes.POINTER(ctypes.c_int32)
CORE.csmGetDrawableTextureIndices.argtypes = [csmModel]; CORE.csmGetDrawableTextureIndices.restype = ctypes.POINTER(ctypes.c_int32)


def load_model(model_dir):
    jsons = glob.glob(os.path.join(model_dir, "*.model3.json"))
    if not jsons: raise FileNotFoundError(f"No .model3.json in {model_dir}")
    cfg = json.load(open(jsons[0], "r", encoding="utf-8"))

    moc_path = os.path.join(model_dir, cfg["FileReferences"]["Moc"])
    moc_data = open(moc_path, "rb").read()
    # Use numpy for aligned allocation (required by Cubism Core)
    moc_size = len(moc_data)
    buf_size = moc_size * 8
    buf = np.zeros(buf_size + 64, dtype=np.uint8)
    # Align to 64 bytes
    offset = (-buf.ctypes.data) & 63
    buf_ptr = buf.ctypes.data + offset
    np.copyto(buf[offset:offset+moc_size], np.frombuffer(moc_data, dtype=np.uint8))

    moc = CORE.csmReviveMocInPlace(ctypes.c_void_p(buf_ptr), moc_size)
    if not moc: raise RuntimeError("Failed to revive moc")
    model = CORE.csmInitializeModelInPlace(moc, buf_size, 0xFFFFFFFF)
    if not model: raise RuntimeError("Failed to init model")

    textures = []
    for tex_rel in cfg["FileReferences"].get("Textures", []):
        tex_path = os.path.join(model_dir, tex_rel)
        textures.append(Image.open(tex_path).convert("RGBA"))

    return model, textures


def render_frame(model, textures, canvas_w, canvas_h):
    CORE.csmUpdateModel(model)
    CORE.csmResetDrawableDynamicFlags(model)

    draw_count = CORE.csmGetDrawableCount(model)
    if draw_count <= 0:
        return Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))

    vert_counts = CORE.csmGetDrawableVertexCounts(model)
    pos_ptrs = CORE.csmGetDrawableVertexPositions(model)
    uv_ptrs = CORE.csmGetDrawableVertexUvs(model)
    idx_ptrs = CORE.csmGetDrawableIndices(model)
    idx_counts = CORE.csmGetDrawableIndexCounts(model)
    opacities = CORE.csmGetDrawableOpacities(model)
    orders = CORE.csmGetDrawableDrawOrders(model)
    tex_indices = CORE.csmGetDrawableTextureIndices(model)

    draws = []
    for i in range(draw_count):
        vcnt = vert_counts[i]
        if opacities[i] <= 0.01 or vcnt == 0:
            continue
        draws.append({"i": i, "order": orders[i], "vcnt": vcnt,
                       "icnt": idx_counts[i], "alpha": opacities[i],
                       "tex": tex_indices[i]})
    draws.sort(key=lambda d: d["order"])

    canvas = np.zeros((canvas_h, canvas_w, 4), dtype=np.float32)

    for d in draws:
        di = d["i"]
        vcnt = d["vcnt"]
        icnt = d["icnt"]
        alpha = d["alpha"]
        tex_idx = d["tex"]

        pos_ptr = ctypes.cast(pos_ptrs[di], ctypes.POINTER(ctypes.c_float))
        uv_ptr = ctypes.cast(uv_ptrs[di], ctypes.POINTER(ctypes.c_float))
        idx_ptr = ctypes.cast(idx_ptrs[di], ctypes.POINTER(ctypes.c_uint16))

        pos = np.ctypeslib.as_array(pos_ptr, (vcnt * 2,))
        uv = np.ctypeslib.as_array(uv_ptr, (vcnt * 2,))
        idx = np.ctypeslib.as_array(idx_ptr, (icnt,))

        if tex_idx < 0 or tex_idx >= len(textures):
            continue
        tex = np.array(textures[tex_idx], dtype=np.float32)
        tw, th = textures[tex_idx].size

        for t in range(icnt // 3):
            i0, i1, i2 = idx[t*3], idx[t*3+1], idx[t*3+2]
            if i0 >= vcnt or i1 >= vcnt or i2 >= vcnt:
                continue

            x = [pos[i0*2], pos[i1*2], pos[i2*2]]
            y = [pos[i0*2+1], pos[i1*2+1], pos[i2*2+1]]
            u = [uv[i0*2], uv[i1*2], uv[i2*2]]
            v = [uv[i0*2+1], uv[i1*2+1], uv[i2*2+1]]

            min_x = max(0, int(min(x)))
            max_x = min(canvas_w - 1, int(math.ceil(max(x))))
            min_y = max(0, int(min(y)))
            max_y = min(canvas_h - 1, int(math.ceil(max(y))))
            if min_x >= max_x or min_y >= max_y:
                continue

            area = (x[1]-x[0])*(y[2]-y[0]) - (x[2]-x[0])*(y[1]-y[0])
            if abs(area) < 1e-6: continue
            inv_area = 1.0 / area

            for py in range(min_y, max_y + 1):
                for px in range(min_x, max_x + 1):
                    w0 = ((x[1]-px)*(y[2]-py) - (x[2]-px)*(y[1]-py)) * inv_area
                    w1 = ((x[2]-px)*(y[0]-py) - (x[0]-px)*(y[2]-py)) * inv_area
                    w2 = 1.0 - w0 - w1
                    if w0 < 0 or w1 < 0 or w2 < 0: continue

                    tu = u[0]*w0 + u[1]*w1 + u[2]*w2
                    tv = v[0]*w0 + v[1]*w1 + v[2]*w2
                    tx = int(tu * tw) % tw
                    ty = int(tv * th) % th

                    tc = tex[ty, tx]
                    a = tc[3] * alpha / 255.0
                    canvas[py, px] = canvas[py, px] * (1 - a) + tc * a

    return Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8), "RGBA")


# ── 22 emotions with Live2D parameter tweaks ──
EXPRESSIONS = {
    "neutral": {},
    "happy": {"ParamMouthOpenY": 0.6, "ParamEyeLOpen": 1.0, "ParamBrowLY": 0.3},
    "sad": {"ParamMouthOpenY": 0.2, "ParamEyeLOpen": 0.4, "ParamBrowLY": -0.3},
    "angry": {"ParamBrowLY": -0.5, "ParamMouthOpenY": 0.1, "ParamEyeLOpen": 0.9},
    "surprised": {"ParamMouthOpenY": 1.0, "ParamEyeLOpen": 1.0, "ParamBrowLY": 0.5},
    "confused": {"ParamBrowLY": -0.1, "ParamBrowLAngle": 0.3, "ParamEyeLOpen": 0.7},
    "sleepy": {"ParamEyeLOpen": 0.1, "ParamMouthOpenY": 0.0},
    "thinking": {"ParamBrowLY": 0.1, "ParamEyeLOpen": 0.6, "ParamMouthOpenY": 0.1},
    "laughing": {"ParamMouthOpenY": 1.0, "ParamEyeLOpen": 1.0, "ParamBrowLY": 0.4},
    "crying": {"ParamEyeLOpen": 0.5, "ParamMouthOpenY": 0.5, "ParamBrowLY": -0.4},
    "embarrassed": {"ParamBrowLY": -0.1, "ParamEyeLOpen": 0.6, "ParamMouthOpenY": 0.2},
    "shocked": {"ParamMouthOpenY": 1.0, "ParamEyeLOpen": 1.0, "ParamBrowLY": 0.5},
    "silly": {"ParamMouthOpenY": 0.5, "ParamEyeLOpen": 0.8, "ParamBrowLY": 0.5},
    "winking": {"ParamEyeLOpen": 0.2, "ParamBrowLY": 0.3, "ParamMouthOpenY": 0.3},
    "kissy": {"ParamMouthOpenY": 0.3, "ParamEyeLOpen": 0.5},
    "cool": {"ParamEyeLOpen": 0.7, "ParamMouthOpenY": 0.1, "ParamBrowLY": 0.1},
    "delicious": {"ParamMouthOpenY": 0.3, "ParamEyeLOpen": 0.9},
    "confident": {"ParamEyeLOpen": 0.9, "ParamBrowLY": 0.2, "ParamMouthOpenY": 0.4},
    "funny": {"ParamBrowLY": 0.5, "ParamMouthOpenY": 0.8, "ParamEyeLOpen": 0.9},
    "loving": {"ParamEyeLOpen": 0.7, "ParamMouthOpenY": 0.2, "ParamBrowLY": 0.2},
    "relaxed": {"ParamEyeLOpen": 0.5, "ParamMouthOpenY": 0.1},
    "awake": {"ParamEyeLOpen": 1.0, "ParamMouthOpenY": 0.0, "ParamBrowLY": 0.0},
}


def render_emotion(model, textures, name, params, out_dir, w, h, fps=30, dur=4):
    os.makedirs(out_dir, exist_ok=True)
    total = fps * dur
    defaults = CORE.csmGetParameterDefaultValues(model)
    values = CORE.csmGetParameterValues(model)
    pids = CORE.csmGetParameterIds(model)
    pcount = CORE.csmGetParameterCount(model)
    param_map = {}
    for i in range(pcount):
        try: pid = pids[i].decode('utf-8')
        except: pid = f"p{i}"
        param_map[pid] = i

    for fi in range(total):
        for i in range(pcount): values[i] = defaults[i]
        for pid, val in params.items():
            if pid in param_map: values[param_map[pid]] = val
        phase = fi / total * math.pi * 2
        if "ParamBreath" in param_map:
            values[param_map["ParamBreath"]] = 0.5 + 0.5 * math.sin(phase)

        img = render_frame(model, textures, w, h)
        img.save(os.path.join(out_dir, f"{name}{fi:04d}.png"), "PNG")
        if fi % 30 == 0: print(f"  {name}: {fi}/{total}")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("model_dir"); ap.add_argument("--out", required=True)
    ap.add_argument("--size", default="480x800"); ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--duration", type=int, default=4)
    args = ap.parse_args()
    w, h = map(int, args.size.split("x"))
    model, textures = load_model(args.model_dir)
    print(f"Model loaded: {CORE.csmGetDrawableCount(model)} drawables, {len(textures)} textures")
    for name, params in EXPRESSIONS.items():
        render_emotion(model, textures, name, params, os.path.join(args.out, name), w, h, args.fps, args.duration)
    print(f"\nDone! Output: {args.out}")
