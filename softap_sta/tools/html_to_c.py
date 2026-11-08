#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
html_to_c.py — 把 HTML 文件 gzip 压缩并转换成 C 语言字节数组

用法:
    python tools/html_to_c.py main/web/index.html --out-dir main

生成:
    <out-dir>/webpage_gz.h   (声明)
    <out-dir>/webpage_gz.c   (gzip 字节数组)

修改 HTML 后重新运行本脚本即可更新 C 数组。
"""
import argparse
import gzip
import os
import sys


def main():
    parser = argparse.ArgumentParser(description="gzip 压缩 HTML 并生成 C 数组")
    parser.add_argument("html", help="源 HTML 文件路径")
    parser.add_argument("--out-dir", default="main", help="输出目录（默认 main）")
    parser.add_argument("--name", default="webpage_gz", help="C 数组名（默认 webpage_gz）")
    args = parser.parse_args()

    with open(args.html, "rb") as f:
        raw = f.read()

    # mtime=0 保证同样的 HTML 每次生成完全相同的字节（可重复构建）
    gz = gzip.compress(raw, compresslevel=9, mtime=0)

    os.makedirs(args.out_dir, exist_ok=True)
    name = args.name
    macro_len = name.upper() + "_LEN"

    h_path = os.path.join(args.out_dir, name + ".h")
    with open(h_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/html_to_c.py 自动生成，请勿手动修改！ */\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        f.write(f"#define {macro_len} ({len(gz)}u)\n")
        f.write(f"extern const uint8_t {name}[{macro_len}];\n\n")
        f.write("#ifdef __cplusplus\n}\n#endif\n")

    c_path = os.path.join(args.out_dir, name + ".c")
    with open(c_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/html_to_c.py 自动生成，请勿手动修改！ */\n")
        f.write(f"/* 源文件: {os.path.basename(args.html)}: {len(raw)} B -> gzip: {len(gz)} B */\n")
        f.write(f"#include \"{name}.h\"\n\n")
        f.write(f"const uint8_t {name}[{macro_len}] = {{\n")
        for i in range(0, len(gz), 16):
            chunk = gz[i:i + 16]
            line = ", ".join("0x%02x" % b for b in chunk)
            sep = "," if i + 16 < len(gz) else ""
            f.write("    %s%s\n" % (line, sep))
        f.write("};\n")

    print("HTML %d B -> gzip %d B (%.1f%%)" % (len(raw), len(gz), 100.0 * len(gz) / len(raw)))
    print("生成: %s" % h_path)
    print("生成: %s" % c_path)


if __name__ == "__main__":
    sys.exit(main())
