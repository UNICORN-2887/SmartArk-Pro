"""Extract .cmo3 (Cubism Editor 5.0 project) files.

.cmo3 uses the CAFF (Cubism Archive File Format) container — a custom binary
archive with XOR obfuscation and deflate compression.

Ported from D2Evil CaffArchive.cs + CaffBinaryReader (UlyssesWu/D2Evil).
Key details:
  - All multi-byte integers are BIG-ENDIAN
  - XOR obfuscation at integer level (not byte level)
  - Strings use variable-length integer for size
  - Compressed entries use ZIP (not raw deflate)

Usage:
  python cmo3_decrypt.py input.cmo3 [output_dir]
"""

import sys
import os
import struct
import zipfile
import io


def create_int64_mask(key):
    """Replicate CaffBinaryPrimitives.CreateInt64Mask."""
    lower = key & 0xFFFFFFFF
    upper = 0xFFFFFFFF if key < 0 else (key & 0xFFFFFFFF)
    return ((upper << 32) | lower) & 0xFFFFFFFFFFFFFFFF


class CaffReader:
    """Port of D2Evil CaffBinaryReader — big-endian, XOR obfuscation."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def skip(self, n):
        self.pos += n

    def read_byte(self, key=0):
        val = self.data[self.pos]
        self.pos += 1
        return (val ^ key) & 0xFF

    def read_bool(self, key=0):
        return self.read_byte(key) != 0

    def read_int16(self, key=0):
        b = self.data[self.pos:self.pos+2]
        self.pos += 2
        val = (b[0] << 8) | b[1]  # big-endian
        result = val ^ (key & 0xFFFF)
        if result >= 0x8000:
            result -= 0x10000
        return result

    def read_int32(self, key=0):
        b = self.data[self.pos:self.pos+4]
        self.pos += 4
        val = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]  # big-endian
        result = (val ^ key) & 0xFFFFFFFF
        if result >= 0x80000000:
            result -= 0x100000000
        return result

    def read_int64(self, key=0):
        b = self.data[self.pos:self.pos+8]
        self.pos += 8
        val = 0
        for byte in b:
            val = (val << 8) | byte
        mask = create_int64_mask(key)
        result = (val ^ mask) & 0xFFFFFFFFFFFFFFFF
        if result >= 0x8000000000000000:
            result -= 0x10000000000000000
        return result

    def read_bytes(self, length, key=0):
        raw = self.data[self.pos:self.pos+length]
        self.pos += length
        if key == 0 or length == 0:
            return raw
        return bytes((b ^ (key & 0xFF)) & 0xFF for b in raw)

    def read_number(self, key=0):
        """Variable-length integer (1-4 bytes, high bit = continuation)."""
        first = self.read_byte(key)
        if (first & 128) == 0:
            return first
        second = self.read_byte(key)
        if (second & 128) == 0:
            return ((first & 127) << 7) | (second & 127)
        third = self.read_byte(key)
        if (third & 128) == 0:
            return ((first & 127) << 14) | ((second & 127) << 7) | third
        fourth = self.read_byte(key)
        if (fourth & 128) == 0:
            return ((first & 127) << 21) | ((second & 127) << 14) | ((third & 127) << 7) | fourth
        raise ValueError("Unsupported variable-length integer")

    def read_string(self, key=0):
        length = self.read_number(key)
        if length <= 0:
            return ""
        raw = self.read_bytes(length, key)
        return raw.decode('utf-8', errors='replace')


def inflate_zip(stored_bytes):
    """Decompress a ZIP-wrapped entry (CAFF uses ZIP, not raw deflate).
    Falls back to manual local header parsing if zipfile can't handle it."""
    import zlib
    import struct as st

    # Method 1: standard zipfile
    try:
        with zipfile.ZipFile(io.BytesIO(stored_bytes)) as zf:
            return zf.read(zf.namelist()[0])
    except Exception:
        pass

    # Method 2: manual ZIP local header parsing (handles streaming ZIPs)
    if len(stored_bytes) >= 30 and stored_bytes[:4] == b'PK\x03\x04':
        fname_len = st.unpack_from('<H', stored_bytes, 26)[0]
        extra_len = st.unpack_from('<H', stored_bytes, 28)[0]
        comp_method = st.unpack_from('<H', stored_bytes, 8)[0]
        comp_size = st.unpack_from('<I', stored_bytes, 18)[0]
        data_offset = 30 + fname_len + extra_len

        if comp_method == 0:  # stored
            return stored_bytes[data_offset:data_offset + comp_size] if comp_size > 0 else stored_bytes[data_offset:]
        elif comp_method == 8:  # deflate
            # If comp_size is 0 (streaming), use everything up to data descriptor
            payload = stored_bytes[data_offset:data_offset + comp_size] if comp_size > 0 else stored_bytes[data_offset:]
            try:
                return zlib.decompress(payload, -15)
            except Exception:
                # Try without size limit, strip trailing data descriptor
                for trim in [0, 12, 16]:
                    try:
                        return zlib.decompress(stored_bytes[data_offset:len(stored_bytes) - trim], -15)
                    except Exception:
                        continue

    # Method 3: raw deflate/zlib
    for wbits in [-15, 15, 15 + 32]:
        try:
            return zlib.decompress(stored_bytes, wbits)
        except Exception:
            continue

    raise ValueError(f"Cannot decompress {len(stored_bytes)} bytes (header: {stored_bytes[:8].hex()})")


def extract_caff(path, output_dir=None):
    """Extract a CAFF (.cmo3) archive."""
    if output_dir is None:
        output_dir = os.path.splitext(path)[0] + "_extracted"

    with open(path, 'rb') as f:
        data = f.read()

    print(f"File: {path} ({len(data):,} bytes)")

    r = CaffReader(data)

    # Header
    magic = ''.join(chr(r.read_byte(0)) for _ in range(4))
    assert magic == "CAFF", f"Not a CAFF archive: {magic!r}"

    archive_ver = [r.read_byte(0) for _ in range(3)]
    format_id = ''.join(chr(r.read_byte(0)) for _ in range(4))
    format_ver = [r.read_byte(0) for _ in range(3)]
    obf_key = r.read_int32(0)
    r.skip(8)

    # Preview
    img_fmt = r.read_byte(0)
    color_type = r.read_byte(0)
    r.skip(2)
    pw = r.read_int16(0) & 0xFFFF
    ph = r.read_int16(0) & 0xFFFF
    preview_start = r.read_int64(0)
    preview_size = r.read_int32(0)
    r.skip(8)

    ver_str = lambda v: f"{v[0]}.{v[1]}.{v[2]}"
    print(f"Archive: {magic} v{ver_str(archive_ver)}")
    print(f"Format:  {format_id} v{ver_str(format_ver)}")
    print(f"Key:     {obf_key} (0x{obf_key & 0xFFFFFFFF:08X})")
    print(f"Preview: {pw}x{ph} fmt={img_fmt}")

    # File table
    file_count = r.read_int32(obf_key)
    print(f"\nFiles: {file_count}")

    entries = []
    for _ in range(file_count):
        fp = r.read_string(obf_key)
        tag = r.read_string(obf_key)
        start = r.read_int64(obf_key)
        size = r.read_int32(obf_key)
        obf = r.read_bool(obf_key)
        compress = r.read_byte(obf_key)
        r.skip(8)
        entries.append(dict(path=fp, tag=tag, start=start, size=size, obf=obf, compress=compress))

    # Extract
    os.makedirs(output_dir, exist_ok=True)

    for e in entries:
        key = obf_key if e['obf'] else 0
        r.pos = e['start']
        stored = r.read_bytes(e['size'], key)

        if e['compress'] == 16:  # RAW
            content = stored
        else:  # FAST=33, SMALL=37
            content = inflate_zip(stored)

        tag = f" [{e['tag']}]" if e['tag'] else ""
        comp = {16: 'raw', 33: 'fast', 37: 'small'}.get(e['compress'], f'?{e["compress"]}')
        obf_s = "+obf" if e['obf'] else ""
        print(f"  {e['path']}{tag} ({len(content):,} bytes, {comp}{obf_s})")

        out_path = os.path.join(output_dir, e['path'].replace('/', os.sep))
        os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
        with open(out_path, 'wb') as f:
            f.write(content)

    # Guard bytes
    if data[-2:] == bytes([98, 99]):
        print(f"\nGuard bytes: OK")
    else:
        print(f"\nWARNING: guard bytes = {list(data[-2:])}")

    print(f"Extracted to: {output_dir}")
    return output_dir


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python cmo3_decrypt.py input.cmo3 [output_dir]")
        sys.exit(1)
    extract_caff(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
