#!/usr/bin/env python3
"""
读取 Spike 运行 shen_test_yolov11n_letterbox 产生的 hex dump，
重建 BGR 图像并保存为 JPG。

用法：
  spike --extension=gemmini .../shen_test_yolov11n_letterbox-baremetal > spike_out.txt 2>/dev/null
  python3 shen_letterbox_to_jpg.py spike_out.txt [output.jpg]

输入格式（在标记行之间）：
  ===LETTERBOX_DUMP_BEGIN===
  640 640          # width height
  BBGGRR...        # 每行 640 像素，每像素 6 hex 字符（BGR）
  ...              # 共 640 行
  ===LETTERBOX_DUMP_END===
"""
import sys
import os

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <spike_output.txt> [output.jpg]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "shen_letterbox_result.jpg"

    with open(input_file, 'r', errors='replace') as f:
        lines = f.readlines()

    # 找到标记行
    begin_idx = None
    end_idx = None
    for i, line in enumerate(lines):
        if '===LETTERBOX_DUMP_BEGIN===' in line:
            begin_idx = i
        elif '===LETTERBOX_DUMP_END===' in line:
            end_idx = i
            break

    if begin_idx is None or end_idx is None:
        print("ERROR: 未找到 LETTERBOX_DUMP_BEGIN/END 标记")
        sys.exit(1)

    # 解析宽高
    wh_line = lines[begin_idx + 1].strip().split()
    width, height = int(wh_line[0]), int(wh_line[1])
    print(f"Image size: {width}x{height}")

    # 解析像素数据（BGR hex）
    pixel_lines = lines[begin_idx + 2 : end_idx]
    if len(pixel_lines) != height:
        print(f"WARNING: 期望 {height} 行数据，实际 {len(pixel_lines)} 行")

    bgr_bytes = bytearray()
    for row_idx, line in enumerate(pixel_lines):
        hex_str = line.strip()
        if len(hex_str) != width * 6:
            print(f"WARNING: 第 {row_idx} 行长度 {len(hex_str)}，期望 {width * 6}")
        for x in range(width):
            offset = x * 6
            b = int(hex_str[offset:offset + 2], 16)
            g = int(hex_str[offset + 2:offset + 4], 16)
            r = int(hex_str[offset + 4:offset + 6], 16)
            bgr_bytes.extend([b, g, r])

    # 尝试用 PIL 保存（BGR → RGB）
    try:
        from PIL import Image
        rgb_bytes = bytearray()
        for i in range(0, len(bgr_bytes), 3):
            rgb_bytes.extend([bgr_bytes[i + 2], bgr_bytes[i + 1], bgr_bytes[i]])
        img = Image.frombytes('RGB', (width, height), bytes(rgb_bytes))
        img.save(output_file, quality=95)
        print(f"Saved: {output_file} ({os.path.getsize(output_file)} bytes)")
    except ImportError:
        # 无 PIL 时保存为 raw，用户可用其他工具打开
        raw_file = output_file.replace('.jpg', '.raw')
        with open(raw_file, 'wb') as f:
            f.write(bgr_bytes)
        print(f"PIL not available. Saved raw BGR: {raw_file} ({width}x{height}x3)")

if __name__ == '__main__':
    main()
