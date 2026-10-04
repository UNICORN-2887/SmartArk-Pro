"""v14: fixed2 baseline + FULLY lock the neck:
- neck mesh's own ArtMesh keyforms (bound to ParamAngleX!) all set to rest
- Neck Warp deformer keyforms disabled (as before)
FaceParallax untouched (fixed2 original-field+mean-strip, head/hair move together).
"""
import sys, re, importlib.util

dec_path = r'C:\Users\HP\AppData\Local\Temp\ss_src\docs__live2d-export__scripts__cmo3_decrypt.py'
pack_path = r'C:\Users\HP\AppData\Local\Temp\ss_src\docs__live2d-export__scripts__caff_packer.py'
spec = importlib.util.spec_from_file_location('cmo3_decrypt', dec_path)
dec = importlib.util.module_from_spec(spec); spec.loader.exec_module(dec)
spec2 = importlib.util.spec_from_file_location('caff_packer', pack_path)
pk = importlib.util.module_from_spec(spec2); spec2.loader.exec_module(pk)

SRC = r'E:\Passport\source\live2d\Mon3tr\Mon3tr_fixed2.cmo3'
DST = r'E:\Passport\source\live2d\Mon3tr\Mon3tr_fixed15.cmo3'

data = open(SRC, 'rb').read()
r = dec.CaffReader(data)
assert ''.join(chr(r.read_byte(0)) for _ in range(4)) == 'CAFF'
r.skip(3+4+3)
obf_key = r.read_int32(0)
r.skip(8)
r.read_byte(0); r.read_byte(0); r.skip(2)
r.read_int16(0); r.read_int16(0)
r.read_int64(0); r.read_int32(0); r.skip(8)
n = r.read_int32(obf_key)
entries = []
for _ in range(n):
    fp = r.read_string(obf_key); tag = r.read_string(obf_key)
    start = r.read_int64(obf_key); size = r.read_int32(obf_key)
    obf = r.read_bool(obf_key); comp = r.read_byte(obf_key); r.skip(8)
    entries.append(dict(path=fp, tag=tag, start=start, size=size, obf=obf, compress=comp))
files, xml_doc = [], None
for e in entries:
    key = obf_key if e['obf'] else 0
    r.pos = e['start']
    stored = r.read_bytes(e['size'], key)
    content = stored if e['compress'] == 16 else dec.inflate_zip(stored)
    if e['path'] == 'main.xml': xml_doc = content
    else: files.append(dict(path=e['path'], tag=e['tag'], content=content,
                            obfuscated=e['obf'], compress=e['compress']))
xml = xml_doc.decode('utf-8', errors='replace')

POS_PAT = r'(<float-array xs\.n="positions" count="\d+">)([\s\d.\-E]+)(</float-array>)'

def write_grids(blk, grids):
    pat = re.compile(POS_PAT)
    ms = list(pat.finditer(blk))
    out, last, idx = [], 0, 0
    for mo in ms[:len(grids)]:
        out.append(blk[last:mo.start()])
        out.append(mo.group(1) + ' '.join(f'{v:.4f}' for v in grids[idx]) + mo.group(3))
        idx += 1
        last = mo.end()
    out.append(blk[last:])
    return ''.join(out)

# 1. neck mesh keyforms -> all rest
def fix_neck_mesh(mo):
    blk = mo.group(0)
    if not re.search(r'<s [^>]*xs\.n="localName"[^>]*>neck</s>', blk):
        return blk
    grids = [[float(v) for v in m.group(2).split()] for m in re.finditer(POS_PAT, blk)]
    print(f'neck mesh keyform grids: {len(grids)}')
    if len(grids) >= 3:
        rest = grids[1]  # key=0
        print('neck mesh keyforms locked to rest')
        return write_grids(blk, [list(rest)] * len(grids))
    return blk
xml = re.sub(r'<CArtMeshSource(?:\s[^>]*)?>.*?</CArtMeshSource>', fix_neck_mesh, xml, flags=re.S)

# 2. Neck Warp deformer disable (as before)
def fix_neck_warp(mo):
    blk = mo.group(0)
    if not re.search(r'<s [^>]*xs\.n="localName"[^>]*>Neck Warp</s>', blk):
        return blk
    grids = [[float(v) for v in m.group(2).split()] for m in re.finditer(POS_PAT, blk)]
    if len(grids) >= 3:
        print('Neck Warp keyforms disabled')
        return write_grids(blk, [list(grids[1])] * 3)
    return blk
xml = re.sub(r'<CWarpDeformerSource(?:\s[^>]*)?>.*?</CWarpDeformerSource>', fix_neck_warp, xml, flags=re.S)

# 3. FaceParallax realign to canvas (fixes headwear direction)
def fix_fp_align(mo):
    blk = mo.group(0)
    if not re.search(r'<s [^>]*xs\.n="localName"[^>]*>FaceParallax</s>', blk):
        return blk
    grids = [[float(v) for v in m2.group(2).split()] for m2 in re.finditer(POS_PAT, blk)]
    print(f'FaceParallax grids to realign: {len(grids)}')
    new_grids = [[(v + 6.0) if j % 2 == 0 else (v + 936.9) for j, v in enumerate(g)] for g in grids]
    return write_grids(blk, new_grids)
xml = re.sub(r'<CWarpDeformerSource(?:\s[^>]*)?>.*?</CWarpDeformerSource>', fix_fp_align, xml, flags=re.S)

files.insert(0, dict(path='main.xml', tag='main_xml', content=xml.encode('utf-8'),
                      obfuscated=True, compress=33))
out = pk.pack_caff(files, obfuscate_key=obf_key)
open(DST, 'wb').write(out)
print(f'written {DST} ({len(out)} bytes)')
