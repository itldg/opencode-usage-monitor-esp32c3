#!/usr/bin/env python3
"""把 audio/*.wav 打包成 audio.bin(头部索引 + 数据),烧录到 audio 分区。

用法:python pack_audio.py <音频目录> <输出路径>
头部:magic "AUDI"(u32) + version(u16) + count(u16) + reserved(u32)
      然后 count 条 {u32 offset, u32 size, char name[16]}
文件顺序与 main/audio_player.h 的 AUDIO_* 枚举一致:
  boot, reset, 1_30..1_100, 2_30..2_100, 3_30..3_100
"""

import os
import struct
import sys

FILES = ["boot", "reset"] + [
    "%d_%s" % (w, t) for w in (1, 2, 3) for t in ("30", "50", "70", "85", "95", "100")
]


def main():
    src_dir, out_path = sys.argv[1], sys.argv[2]
    hdr_size = 12 + len(FILES) * 24
    entries = []
    blob = b""
    for name in FILES:
        path = os.path.join(src_dir, name + ".wav")
        with open(path, "rb") as f:
            data = f.read()
        entries.append((name, hdr_size + len(blob), len(data)))
        blob += data

    hdr = struct.pack("<4sHHI", b"AUDI", 1, len(entries), 0)
    hdr += b"".join(
        struct.pack("<II16s", off, size, name.encode().ljust(16, b"\0"))
        for name, off, size in entries
    )
    with open(out_path, "wb") as f:
        f.write(hdr + blob)
    print("audio.bin: %d bytes, %d files" % (len(hdr) + len(blob), len(entries)))


if __name__ == "__main__":
    main()
