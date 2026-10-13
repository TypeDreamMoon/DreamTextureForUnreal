#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""写 UE 5.8 的 LocRes v3（Optimized_CityHash64_UTF16）与 LocMeta，不需要编辑器。

这是整条链的最后一环：loctext.json（源文）+ translations/*.json（译文）-> .locres / .locmeta。

    python build_locres.py                    校验后写出 en/ 与 zh-Hans/ 两份 .locres 和 .locmeta
    python build_locres.py --check            不写文件；产物和磁盘上的不一致就退出 1（sync_loctext.py --check 会调它）
    python build_locres.py --allow-incomplete 缺译文时也写（缺的条目不进 zh-Hans 的 .locres，界面上显示英文源文）
    python build_locres.py --verify-hashes F  读任意一个 .locres（比如引擎自带的 Editor.locres），重算每个键的哈希、
                                              与文件里存的逐个对照。用来验证这里的 CityHash 与引擎一致，或在引擎升级后
                                              确认哈希算法没变

关键事实（都在引擎源码里核实过，见 Tools/Localization/README.md 的「为什么能不开编辑器」一节）：

  * 命名空间与 key 的哈希是 CityHash64(UTF-16LE 字节) 折成 32 位：lo32 + hi32 * 23
    （Core/Private/Internationalization/TextKey.cpp 的 HashString、Templates/TypeHash.h 的 GetTypeHash64）。
    加载时 FTextKey::SerializeWithHash 直接信文件里的哈希，不重算——写错了不报错，只是查不到。
  * 每条记录里存的 src 不是源文本身，是 FCrc::StrCrc32<TCHAR>(源文)（TextLocalizationResource.h 的 HashString）。
    运行时 FTextLocalizationManager::FindDisplayString_Internal 拿代码里 LOCTEXT 的源文算同一个哈希、对不上就**静默丢弃译文**。
    所以 loctext.json 的 src 必须是「运行时看到的字符串」（C 转义已解码），而不是源码里写的样子。
  * TCHAR 在 Windows 上是 UTF-16 码元：非 BMP 字符（emoji）是两个码元，要逐码元算 CRC，不是逐码点。
    （DreamShader 里 ord() 的版本对 BMP 字符正确、对 emoji 不对；这里修了。）
  * 原生文化（en）也要写一份 en/<目标>.locres（原文 -> 原文）。引擎切到原生文化时不会加载「原生资源」，
    而是只加载 en/ 下的 .locres 去覆盖实时表；没有这份的话，从 zh-Hans 切回 en 后实时表里还是中文。
    这份文件 Localization Dashboard 本来就会生成，DreamShader 没带，所以它在运行时切回英文会留着中文。

CRC32 的表与 CityHash64 取自 pylocres（crc_hash.py / city_hash.py，MIT），DreamShader 里已在真实 UE 文件上验证过；
这里在此基础上又对引擎自带的 Editor.locres 做了全量哈希复核。
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loc_common as common  # noqa: E402

# ================= CRC32 (verbatim from pylocres crc_hash.py) =================
CRCTable = [
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
]

def str_crc32(value, crc=0):
    """FCrc::StrCrc32<TCHAR>。TCHAR 是 UTF-16 码元，每个码元按 4 字节（高位补零）喂进 CRC，遇到 NUL 就停。"""
    raw = value.encode("utf-16-le", "surrogatepass")
    crc = ~crc & 0xFFFFFFFF
    for (unit,) in struct.iter_unpack("<H", raw):
        if unit == 0:
            break
        ch = unit
        for _ in range(4):
            crc = (crc >> 8) ^ CRCTable[(crc ^ ch) & 0xFF]
            ch >>= 8
    return ~crc & 0xFFFFFFFF

# ================= CityHash64 (verbatim from pylocres city_hash.py) =================
K0 = 0xc3a5c85c97cb3127; K1 = 0xb492b66fbe98f273; K2 = 0x9ae16a3b2f90404f; K3 = 0xc949d7c7509e6557
C1 = 0xcc9e2d51; C2 = 0x1b873593; HASH_MUL = 0x9ddfea08eb382d69; BIG_ENDIAN = False

class CityHash:
    @staticmethod
    def _uint64(x): return x & 0xFFFFFFFFFFFFFFFF
    @staticmethod
    def _uint32(x): return x & 0xFFFFFFFF
    @staticmethod
    def byte_swap_uint32(x):
        x = CityHash._uint32(x)
        return ((x >> 24) | ((x & 0x00ff0000) >> 8) | ((x & 0x0000ff00) << 8) | (x << 24))
    @staticmethod
    def byte_swap_uint64(x):
        x = CityHash._uint64(x)
        return ((x >> 56) | ((x & 0x00ff000000000000) >> 40) | ((x & 0x0000ff0000000000) >> 24) |
                ((x & 0x000000ff00000000) >> 8) | ((x & 0x00000000ff000000) << 8) |
                ((x & 0x0000000000ff0000) << 24) | ((x & 0x000000000000ff00) << 40) | (x << 56))
    @staticmethod
    def hash128_to_64(x):
        a = CityHash._uint64((x[0] ^ x[1]) * HASH_MUL); a ^= (a >> 47)
        b = CityHash._uint64((x[1] ^ a) * HASH_MUL); b ^= (b >> 47)
        return CityHash._uint64(b * HASH_MUL)
    @staticmethod
    def fetch32(data, index):
        x = struct.unpack_from('<I', data, index)[0]
        return CityHash._uint64(CityHash.byte_swap_uint32(x) if BIG_ENDIAN else x)
    @staticmethod
    def fetch64(data, index):
        x = struct.unpack_from('<Q', data, index)[0]
        return CityHash._uint64(CityHash.byte_swap_uint64(x) if BIG_ENDIAN else x)
    @staticmethod
    def rotate(val, shift):
        val = CityHash._uint64(val)
        if shift == 0: return CityHash._uint64(val)
        return CityHash._uint64((val >> shift) | (val << (64 - shift)))
    @staticmethod
    def shift_mix(val):
        val = CityHash._uint64(val)
        return CityHash._uint64(val ^ (val >> 47))
    @staticmethod
    def hash_len16(u, v, mul=None):
        if mul is None: return CityHash.hash128_to_64([u, v])
        u, v = CityHash._uint64(u), CityHash._uint64(v); mul = CityHash._uint64(mul)
        a = CityHash._uint64((u ^ v) * mul); a ^= (a >> 47)
        b = CityHash._uint64((v ^ a) * mul); b ^= (b >> 47)
        return CityHash._uint64(b * mul)
    @staticmethod
    def hash_len0_to_16(data, offset):
        length = len(data) - offset
        if length >= 8:
            mul = K2 + length * 2
            a = CityHash.fetch64(data, offset) + K2
            b = CityHash.fetch64(data, offset + length - 8)
            c = CityHash._uint64(CityHash.rotate(b, 37) * mul + a)
            d = CityHash._uint64((CityHash.rotate(a, 25) + b) * mul)
            return CityHash.hash_len16(c, d, mul)
        if length >= 4:
            mul = K2 + length * 2
            a = CityHash.fetch32(data, offset)
            return CityHash.hash_len16(length + (a << 3), CityHash.fetch32(data, offset + length - 4), mul)
        if length > 0:
            a = data[offset]; b = data[offset + (length >> 1)]; c = data[offset + (length - 1)]
            y = a + (b << 8); z = length + (c << 2)
            return CityHash._uint64(CityHash.shift_mix((y * K2) ^ (z * K0)) * K2)
        return K2
    @staticmethod
    def hash_len17_to_32(data):
        length = len(data); mul = CityHash._uint64(K2 + length * 2)
        a = CityHash._uint64(CityHash.fetch64(data, 0) * K1)
        b = CityHash.fetch64(data, 8)
        c = CityHash._uint64(CityHash.fetch64(data, length - 8) * mul)
        d = CityHash._uint64(CityHash.fetch64(data, length - 16) * K2)
        return CityHash.hash_len16(
            CityHash._uint64(CityHash.rotate(a + b, 43) + CityHash.rotate(c, 30) + d),
            CityHash._uint64(a + CityHash.rotate(b + K2, 18) + c), mul)
    @staticmethod
    def hash_len33_to_64(data):
        length = len(data); mul = CityHash._uint64(K2 + length * 2)
        a = CityHash._uint64(CityHash.fetch64(data, 0) * K2); b = CityHash.fetch64(data, 8)
        c = CityHash.fetch64(data, length - 24); d = CityHash.fetch64(data, length - 32)
        e = CityHash._uint64(CityHash.fetch64(data, 16) * K2); f = CityHash._uint64(CityHash.fetch64(data, 24) * 9)
        g = CityHash.fetch64(data, length - 8); h = CityHash._uint64(CityHash.fetch64(data, length - 16) * mul)
        u = CityHash._uint64(CityHash.rotate(a + g, 43) + (CityHash.rotate(b, 30) + c) * 9)
        v = CityHash._uint64((a + g) ^ d) + f + 1
        w = CityHash._uint64(CityHash.byte_swap_uint64((u + v) * mul) + h)
        x = CityHash._uint64(CityHash.rotate(e + f, 42) + c)
        y = CityHash._uint64(CityHash.byte_swap_uint64((v + w) * mul) + g) * mul
        z = CityHash._uint64(e + f + c)
        a = CityHash._uint64(CityHash.byte_swap_uint64((x + z) * mul + y) + b)
        b = CityHash._uint64(CityHash.shift_mix((z + a) * mul + d + h) * mul)
        return CityHash._uint64(b + x)
    @staticmethod
    def weak_hash_len32_with_seeds(w, x, y, z, a, b):
        a += w; b = CityHash.rotate(b + a + z, 21); c = a
        a += x; a += y; b += CityHash.rotate(a, 44)
        return [CityHash._uint64(a + z), CityHash._uint64(b + c)]
    @staticmethod
    def weak_hash_len32_with_seeds_from_bytes(data, offset, a, b):
        return CityHash.weak_hash_len32_with_seeds(
            CityHash.fetch64(data, offset), CityHash.fetch64(data, offset + 8),
            CityHash.fetch64(data, offset + 16), CityHash.fetch64(data, offset + 24), a, b)
    @staticmethod
    def city_hash_64_utf16_to_uint32(s):
        if not s: return 0
        h = CityHash.city_hash_64(s)
        return CityHash._uint32((h & 0xFFFFFFFF) + (((h >> 32) & 0xFFFFFFFF) * 23))
    @staticmethod
    def city_hash_64(s):
        data = s.encode('utf-16le'); length = len(data)
        if length <= 32:
            if length <= 16: return CityHash.hash_len0_to_16(data, 0)
            return CityHash.hash_len17_to_32(data)
        if length <= 64: return CityHash.hash_len33_to_64(data)
        x = CityHash.fetch64(data, length - 40)
        y = CityHash.fetch64(data, length - 16) + CityHash.fetch64(data, length - 56)
        z = CityHash.hash_len16(CityHash.fetch64(data, length - 48) + length, CityHash.fetch64(data, length - 24))
        v = CityHash.weak_hash_len32_with_seeds_from_bytes(data, length - 64, length, z)
        w = CityHash.weak_hash_len32_with_seeds_from_bytes(data, length - 32, y + K1, x)
        x = CityHash._uint64(x * K1 + CityHash.fetch64(data, 0))
        chunk_length = (len(data) - 1) & ~63
        offset = 0
        while chunk_length != 0:
            x = CityHash._uint64(CityHash.rotate(x + y + v[0] + CityHash.fetch64(data, offset + 8), 37) * K1)
            y = CityHash._uint64(CityHash.rotate(y + v[1] + CityHash.fetch64(data, offset + 48), 42) * K1)
            x ^= w[1]
            y = CityHash._uint64(y + v[0] + CityHash.fetch64(data, offset + 40))
            z = CityHash._uint64(CityHash.rotate(z + w[0], 33) * K1)
            v = CityHash.weak_hash_len32_with_seeds_from_bytes(data, offset, v[1] * K1, x + w[0])
            w = CityHash.weak_hash_len32_with_seeds_from_bytes(data, offset + 32, z + w[1], y + CityHash.fetch64(data, offset + 16))
            z, x = x, z
            offset += 64; chunk_length -= 64
        return CityHash.hash_len16(
            CityHash.hash_len16(v[0], w[0]) + CityHash.shift_mix(y) * K1 + z,
            CityHash.hash_len16(v[1], w[1]) + x)


# ================= LocRes v3 / LocMeta 写入 =================
# 魔数是 FGuid 的四个 uint32 按小端依次写出。
LOCRES_MAGIC = bytes.fromhex("0e147475674a03fc4a15909dc3377f1b")    # FGuid(0x7574140E, 0xFC034A67, 0x9D90154A, 0x1B7F37C3)
LOCMETA_MAGIC = bytes.fromhex("4fee4ca1684855836c4c46bd70da507c")   # FGuid(0xA14CEE4F, 0x83554868, 0xBD464C6C, 0x7C50DA70)
LOCRES_VERSION_CITYHASH64_UTF16 = 3     # ELocResVersion::Optimized_CityHash64_UTF16
LOCMETA_VERSION_ADDED_ISUGC = 2         # ELocMetaVersion::AddedIsUGC（5.8 的 Latest）


def fstring(s):
    """FString 的序列化：纯 ASCII 走 ANSI（长度为正，含结尾 NUL），否则 UTF-16（长度为负）。"""
    if s.isascii():
        return struct.pack("<i", len(s) + 1) + s.encode("ascii") + b"\x00"
    b = s.encode("utf-16-le", "surrogatepass")
    return struct.pack("<i", -(len(b) // 2 + 1)) + b + b"\x00\x00"


def build_locres_bytes(entries):
    """entries：[(命名空间, key, 源文, 本文化译文)]。同一输入产出字节完全相同的文件（命名空间、key 都排序），
    这样 --check 才能用逐字节比较判断「产物是不是最新」。"""
    groups = {}
    for ns, key, src, tr in entries:
        groups.setdefault(ns, {})[key] = (src, tr)

    lut_index = {}
    lut = []            # [字符串, 引用计数]

    def intern(s):
        if s not in lut_index:
            lut_index[s] = len(lut)
            lut.append([s, 0])
        i = lut_index[s]
        lut[i][1] += 1
        return i

    body = bytearray()
    total = 0
    for ns in sorted(groups):
        keys = groups[ns]
        body += struct.pack("<I", CityHash.city_hash_64_utf16_to_uint32(ns)) + fstring(ns)
        body += struct.pack("<I", len(keys))
        for key in sorted(keys):
            src, tr = keys[key]
            body += struct.pack("<I", CityHash.city_hash_64_utf16_to_uint32(key)) + fstring(key)
            body += struct.pack("<I", str_crc32(src))
            body += struct.pack("<i", intern(tr))
            total += 1

    head_len = 16 + 1 + 8 + 4 + 4
    lut_offset = head_len + len(body)
    out = bytearray(LOCRES_MAGIC)
    out += bytes([LOCRES_VERSION_CITYHASH64_UTF16])
    out += struct.pack("<q", lut_offset)
    out += struct.pack("<II", total, len(groups))
    out += body
    out += struct.pack("<I", len(lut))
    for s, refs in lut:
        out += fstring(s) + struct.pack("<i", refs)
    return bytes(out)


def build_locmeta_bytes(native_culture, native_locres, compiled_cultures):
    """FTextLocalizationMetaDataResource::SaveToArchive 的布局：魔数、版本、原生文化、原生 LocRes 路径、
    编译了哪些文化、bIsUGC（bool 在 FArchive 里占 4 字节）。"""
    out = bytearray(LOCMETA_MAGIC)
    out += bytes([LOCMETA_VERSION_ADDED_ISUGC])
    out += fstring(native_culture)
    out += fstring(native_locres)
    out += struct.pack("<i", len(compiled_cultures))
    for c in compiled_cultures:
        out += fstring(c)
    out += struct.pack("<I", 0)
    return bytes(out)


# ================= 独立的读取器（验证用，不共用写入代码的任何一段） =================
class Reader(object):
    def __init__(self, buf):
        self.buf = buf
        self.off = 0

    def u8(self):
        v = self.buf[self.off]
        self.off += 1
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.buf, self.off)[0]
        self.off += 4
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.buf, self.off)[0]
        self.off += 4
        return v

    def i64(self):
        v = struct.unpack_from("<q", self.buf, self.off)[0]
        self.off += 8
        return v

    def raw(self, n):
        v = self.buf[self.off:self.off + n]
        self.off += n
        return v

    def fstring(self):
        n = self.i32()
        if n > 0:
            return self.raw(n)[:-1].decode("ascii")
        if n < 0:
            return self.raw(-n * 2)[:-2].decode("utf-16-le", "surrogatepass")
        return ""


def read_locres(buf):
    """-> (entries, strings)；entries：[(ns, key, ns 哈希, key 哈希, 源文 CRC, 译文)]"""
    r = Reader(buf)
    if r.raw(16) != LOCRES_MAGIC:
        raise ValueError("不是 LocRes（魔数不对）")
    version = r.u8()
    if version != LOCRES_VERSION_CITYHASH64_UTF16:
        raise ValueError("只认 v%d（Optimized_CityHash64_UTF16），这个文件是 v%d" % (LOCRES_VERSION_CITYHASH64_UTF16, version))
    lut_offset = r.i64()
    total = r.u32()
    ns_count = r.u32()

    rows = []
    for _ in range(ns_count):
        ns_hash = r.u32()
        ns = r.fstring()
        for _ in range(r.u32()):
            key_hash = r.u32()
            key = r.fstring()
            crc = r.u32()
            idx = r.i32()
            rows.append((ns, key, ns_hash, key_hash, crc, idx))
    if r.off != lut_offset:
        raise ValueError("键表结束于 %d，但文件头说字符串表在 %d" % (r.off, lut_offset))
    if len(rows) != total:
        raise ValueError("文件头说有 %d 条，实际读到 %d 条" % (total, len(rows)))

    strings = []
    refs_declared = []
    for _ in range(r.u32()):
        strings.append(r.fstring())
        refs_declared.append(r.i32())
    if r.off != len(buf):
        raise ValueError("字符串表之后还有 %d 字节多余内容" % (len(buf) - r.off))

    refs_actual = [0] * len(strings)
    entries = []
    for ns, key, ns_hash, key_hash, crc, idx in rows:
        refs_actual[idx] += 1
        entries.append((ns, key, ns_hash, key_hash, crc, strings[idx]))
    if refs_actual != refs_declared:
        raise ValueError("字符串表里记的引用计数与实际引用不符")
    return entries, strings


def read_locmeta(buf):
    r = Reader(buf)
    if r.raw(16) != LOCMETA_MAGIC:
        raise ValueError("不是 LocMeta（魔数不对）")
    version = r.u8()
    native_culture = r.fstring()
    native_locres = r.fstring()
    compiled = []
    is_ugc = False
    if version >= 1:
        compiled = [r.fstring() for _ in range(r.i32())]
    if version >= 2:
        is_ugc = bool(r.u32())
    return {"version": version, "native_culture": native_culture, "native_locres": native_locres,
            "compiled_cultures": compiled, "is_ugc": is_ugc, "trailing": len(buf) - r.off}


def verify_locres(buf, expected):
    """读回刚写的字节，逐条核对：哈希是不是按引擎的算法算的、译文是不是对的。expected：[(ns, key, src, tr)]"""
    entries, _ = read_locres(buf)
    if len(entries) != len(expected):
        raise ValueError("读回 %d 条，应当 %d 条" % (len(entries), len(expected)))
    want = {(ns, key): (src, tr) for ns, key, src, tr in expected}
    for ns, key, ns_hash, key_hash, crc, tr in entries:
        src, expected_tr = want[(ns, key)]
        if ns_hash != CityHash.city_hash_64_utf16_to_uint32(ns):
            raise ValueError("命名空间哈希不对：%s" % ns)
        if key_hash != CityHash.city_hash_64_utf16_to_uint32(key):
            raise ValueError("key 哈希不对：%s|%s" % (ns, key))
        if crc != str_crc32(src):
            raise ValueError("源文 CRC 不对：%s|%s" % (ns, key))
        if tr != expected_tr:
            raise ValueError("译文读回不一致：%s|%s" % (ns, key))


# ================= 要写哪些文件 =================

def outputs(entries, translations, allow_incomplete):
    """-> {相对 Content/Localization/<目标> 的路径: 字节}"""
    files = {}

    native = [(e["ns"], e["key"], e["src"], e["src"]) for e in entries]
    native_path = "%s/%s.locres" % (common.NATIVE_CULTURE, common.TARGET)
    files[native_path] = build_locres_bytes(native)
    verify_locres(files[native_path], native)

    for culture in common.TRANSLATED_CULTURES:
        rows = []
        for e in entries:
            tr = translations.lookup(e)     # 语言中立的条目在这里拿到源文本身
            if tr is None:
                if not allow_incomplete:
                    raise ValueError("缺译文：%s" % common.entry_id(e))
                continue        # 不写：缺的条目在运行时回落到源文，比写一个空串强
            rows.append((e["ns"], e["key"], e["src"], tr))
        path = "%s/%s.locres" % (culture, common.TARGET)
        files[path] = build_locres_bytes(rows)
        verify_locres(files[path], rows)

    compiled = [common.NATIVE_CULTURE] + list(common.TRANSLATED_CULTURES)
    files["%s.locmeta" % common.TARGET] = build_locmeta_bytes(
        common.NATIVE_CULTURE, native_path, compiled)
    return files


def stale_outputs(files):
    """和磁盘比较：缺的、内容不同的，以及磁盘上多出来的（比如删掉一种文化之后残留的目录）。"""
    problems = []
    for rel, data in sorted(files.items()):
        path = os.path.join(common.LOCALIZATION_DIR, *rel.split("/"))
        if not os.path.exists(path):
            problems.append("缺文件：%s" % rel)
            continue
        with open(path, "rb") as f:
            if f.read() != data:
                problems.append("内容过期：%s" % rel)
    if os.path.isdir(common.LOCALIZATION_DIR):
        for root, _, names in os.walk(common.LOCALIZATION_DIR):
            for name in names:
                rel = os.path.relpath(os.path.join(root, name), common.LOCALIZATION_DIR).replace(os.sep, "/")
                if rel not in files:
                    problems.append("多余文件：%s" % rel)
    return problems


def check_outputs(allow_incomplete=False):
    """sync_loctext.py --check 调这个。返回问题列表；校验没过、写不出文件时也算问题。"""
    entries = common.load_loctext()
    translations = common.load_translations()
    report = common.validate(entries, translations)
    if report.error_count and not allow_incomplete:
        return ["译文校验没过，没法生成 .locres（细节见上面的校验输出）"]
    try:
        files = outputs(entries, translations, allow_incomplete)
    except ValueError as e:
        return [str(e)]
    return stale_outputs(files)


# ================= 验证别人的 .locres 里的哈希 =================

def verify_hashes_of(path):
    with open(path, "rb") as f:
        buf = f.read()
    entries, _ = read_locres(buf)
    bad_ns = bad_key = 0
    checked_ns = set()
    longest_key = 0
    for ns, key, ns_hash, key_hash, _, _ in entries:
        if (ns, ns_hash) not in checked_ns:
            checked_ns.add((ns, ns_hash))
            if CityHash.city_hash_64_utf16_to_uint32(ns) != ns_hash:
                bad_ns += 1
                print("命名空间哈希不符：%r  文件 %08x  重算 %08x" % (ns, ns_hash, CityHash.city_hash_64_utf16_to_uint32(ns)))
        longest_key = max(longest_key, len(key.encode("utf-16-le")))
        if CityHash.city_hash_64_utf16_to_uint32(key) != key_hash:
            bad_key += 1
            if bad_key <= 10:
                print("key 哈希不符：%s|%s  文件 %08x  重算 %08x" % (ns, key, key_hash, CityHash.city_hash_64_utf16_to_uint32(key)))
    print("%s：%d 条、%d 个命名空间，最长的 key 是 %d 字节（UTF-16）；命名空间哈希不符 %d，key 哈希不符 %d"
          % (os.path.basename(path), len(entries), len(checked_ns), longest_key, bad_ns, bad_key))
    return 0 if bad_ns == 0 and bad_key == 0 else 1


# ================= 入口 =================

def main(argv=None):
    ap = argparse.ArgumentParser(description="生成 DreamTextureForUnreal 的 .locres / .locmeta")
    ap.add_argument("--check", action="store_true", help="不写文件，只看产物是不是最新")
    ap.add_argument("--allow-incomplete", action="store_true", help="缺译文也写（缺的条目回落到英文源文）")
    ap.add_argument("--verify-hashes", metavar="LOCRES", help="复核任意 .locres 里每个键的哈希")
    args = ap.parse_args(argv)
    common.setup_output()

    if args.verify_hashes:
        return verify_hashes_of(args.verify_hashes)

    entries = common.load_loctext()
    if not entries:
        print("loctext.json 是空的或不存在，先跑 python sync_loctext.py")
        return 1
    translations = common.load_translations()
    report = common.validate(entries, translations)
    if report.error_count and not args.allow_incomplete:
        common.print_report(report, entries)
        print("\n校验没过（%d 个问题），没有生成 .locres。补完译文再来；只想看看效果可以加 --allow-incomplete。" % report.error_count)
        return 1
    if report.error_count:
        common.print_report(report, entries, limit=10)
        print("\n（--allow-incomplete：带着上面的问题继续写。）")

    files = outputs(entries, translations, args.allow_incomplete)

    if args.check:
        problems = stale_outputs(files)
        for p in problems:
            print("STALE: " + p)
        if problems:
            print("产物不是最新：python Tools/Localization/build_locres.py")
            return 1
        print("产物是最新：%d 个文件，%d 条源文。" % (len(files), len(entries)))
        return 0

    stale = stale_outputs(files)
    for rel, data in sorted(files.items()):
        path = os.path.join(common.LOCALIZATION_DIR, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        print("写 %s（%d 字节）" % (os.path.relpath(path, common.PLUGIN).replace(os.sep, "/"), len(data)))
    # 删掉磁盘上多出来的（比如不再支持的文化），免得残留的旧译文还被引擎加载。
    for p in stale:
        if p.startswith("多余文件："):
            rel = p.split("：", 1)[1]
            os.remove(os.path.join(common.LOCALIZATION_DIR, *rel.split("/")))
            print("删多余的 %s" % rel)
    print("完成：%d 条源文，%d 个文件，缺译文 %d。" % (len(entries), len(files), len(report.missing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
