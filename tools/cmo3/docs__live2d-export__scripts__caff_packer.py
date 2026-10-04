"""CAFF (Cubism Archive File Format) packer.

Creates .cmo3 files from main.xml + PNG files.
Binary layout from D2Evil RE + our Java decompile of Cubism Editor 5.0.

All multi-byte integers are BIG-ENDIAN.
XOR obfuscation at integer level (not byte level).
"""

import struct
import io
import zipfile


def _create_int64_mask(key):
    """Expand int32 key to int64 mask (from CaffBinaryPrimitives)."""
    lower = key & 0xFFFFFFFF
    upper = 0xFFFFFFFF if key < 0 else (key & 0xFFFFFFFF)
    return ((upper << 32) | lower) & 0xFFFFFFFFFFFFFFFF


class CaffWriter:
    """Binary writer with big-endian XOR obfuscation."""

    def __init__(self):
        self._buf = io.BytesIO()

    @property
    def position(self):
        return self._buf.tell()

    @position.setter
    def position(self, val):
        self._buf.seek(val)

    def write_byte(self, value, key=0):
        self._buf.write(bytes([(value ^ key) & 0xFF]))

    def write_int16(self, value, key=0):
        encoded = (value ^ key) & 0xFFFF
        self._buf.write(struct.pack('>H', encoded))

    def write_int32(self, value, key=0):
        encoded = (value ^ key) & 0xFFFFFFFF
        self._buf.write(struct.pack('>I', encoded))

    def write_int64(self, value, key=0):
        mask = _create_int64_mask(key)
        val_unsigned = value & 0xFFFFFFFFFFFFFFFF
        encoded = (val_unsigned ^ mask) & 0xFFFFFFFFFFFFFFFF
        self._buf.write(struct.pack('>Q', encoded))

    def write_bool(self, value, key=0):
        self.write_byte(1 if value else 0, key)

    def write_bytes(self, data, key=0):
        if key == 0:
            self._buf.write(data)
        else:
            k = key & 0xFF
            self._buf.write(bytes((b ^ k) & 0xFF for b in data))

    def write_number(self, value, key=0):
        """Variable-length integer (1-4 bytes)."""
        if value < 128:
            self.write_byte(value, key)
        elif value < 16384:
            self.write_byte(((value >> 7) & 127) | 128, key)
            self.write_byte(value & 127, key)
        elif value < 2097152:
            self.write_byte(((value >> 14) & 127) | 128, key)
            self.write_byte(((value >> 7) & 127) | 128, key)
            self.write_byte(value & 127, key)
        else:
            self.write_byte(((value >> 21) & 127) | 128, key)
            self.write_byte(((value >> 14) & 127) | 128, key)
            self.write_byte(((value >> 7) & 127) | 128, key)
            self.write_byte(value & 127, key)

    def write_string(self, value, key=0):
        encoded = value.encode('utf-8')
        self.write_number(len(encoded), key)
        self.write_bytes(encoded, key)

    def skip(self, count):
        self._buf.write(b'\x00' * count)

    def get_bytes(self):
        return self._buf.getvalue()


# Compression options
COMPRESS_RAW = 16
COMPRESS_FAST = 33
COMPRESS_SMALL = 37

# Preview types
NO_PREVIEW = 127


def _compress_zip(content):
    """Compress content as ZIP archive with single entry 'contents'."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('contents', content)
    return buf.getvalue()


def pack_caff(files, obfuscate_key=0):
    """
    Pack files into a CAFF archive.

    Args:
        files: list of dicts with keys:
            - path: str (file path in archive)
            - content: bytes (file content)
            - tag: str (optional, e.g. "main_xml", "preview")
            - obfuscated: bool (default True)
            - compress: int (COMPRESS_RAW, COMPRESS_FAST, COMPRESS_SMALL)
        obfuscate_key: int32 XOR key (0 = no obfuscation)

    Returns:
        bytes: complete CAFF archive
    """
    w = CaffWriter()
    key = obfuscate_key

    # === Header ===
    # Archive identifier: "CAFF"
    for c in "CAFF":
        w.write_byte(ord(c), 0)
    # Archive version: [0, 0, 0]
    w.write_byte(0, 0)
    w.write_byte(0, 0)
    w.write_byte(0, 0)
    # Format identifier: "----"
    for c in "----":
        w.write_byte(ord(c), 0)
    # Format version: [0, 0, 0]
    w.write_byte(0, 0)
    w.write_byte(0, 0)
    w.write_byte(0, 0)
    # Obfuscate key
    w.write_int32(key, 0)
    # Reserved
    w.skip(8)

    # === Preview image (none) ===
    w.write_byte(NO_PREVIEW, 0)      # ImageFormat
    w.write_byte(NO_PREVIEW, 0)      # ColorType
    w.skip(2)                         # padding
    w.write_int16(0, 0)              # Width
    w.write_int16(0, 0)              # Height
    preview_start_addr = w.position
    w.write_int64(0, 0)              # StartPosition (no preview)
    w.write_int32(0, 0)              # FileSize
    w.skip(8)                         # reserved

    # === File table ===
    w.write_int32(len(files), key)

    # Prepare entries
    entries = []
    for f in files:
        content = f['content']
        compress = f.get('compress', COMPRESS_RAW)
        obfuscated = f.get('obfuscated', True)

        if compress == COMPRESS_RAW:
            stored = content
        else:
            stored = _compress_zip(content)

        entry = {
            'path': f['path'],
            'tag': f.get('tag', ''),
            'stored': stored,
            'obfuscated': obfuscated,
            'compress': compress,
            'start_pos_addr': 0,  # will be filled
            'start_pos': 0,       # will be filled
        }
        entries.append(entry)

    # Write file table entries (with placeholder start positions)
    PLACEHOLDER = 0x1234567812345678
    for entry in entries:
        w.write_string(entry['path'], key)
        w.write_string(entry['tag'], key)
        entry['start_pos_addr'] = w.position
        w.write_int64(PLACEHOLDER, key)
        w.write_int32(len(entry['stored']), key)
        w.write_bool(entry['obfuscated'], key)
        w.write_byte(entry['compress'], key)
        w.skip(8)

    # === File data ===
    for entry in entries:
        entry['start_pos'] = w.position
        ekey = key if entry['obfuscated'] else 0
        w.write_bytes(entry['stored'], ekey)

    # === Guard bytes ===
    w.write_byte(98, 0)
    w.write_byte(99, 0)

    # === Patch start positions ===
    for entry in entries:
        w.position = entry['start_pos_addr']
        w.write_int64(entry['start_pos'], key)

    return w.get_bytes()


if __name__ == '__main__':
    # Quick test: pack a simple archive
    data = pack_caff([
        {'path': 'test.txt', 'content': b'Hello CAFF!', 'tag': '', 'obfuscated': False, 'compress': COMPRESS_RAW},
    ], obfuscate_key=0)
    print(f"Packed {len(data)} bytes, magic={data[:4]}, guard={list(data[-2:])}")
    assert data[:4] == b'CAFF'
    assert data[-2:] == bytes([98, 99])
    print("CAFF packer OK!")
