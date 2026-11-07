# EAF/AAF 文件检查工具

这个工具按设备端 `gfx_eaf_dec.c` 的解析布局检查 EAF/AAF 文件，用来定位资源导入后在设备端解码失败、花屏、CRC 错误、block 越界等问题。

## 使用方式

```bash
python scripts/eaf_inspect.py path/to/file.eaf
python scripts/eaf_inspect.py --frames path/to/file.eaf
python scripts/eaf_inspect.py -v path/to/file.eaf
python scripts/eaf_inspect.py --json path/to/file.eaf
```

也可以使用包入口：

```bash
python -m scripts.eaf_inspector path/to/file.eaf
```

## 检查内容

- 文件头：`0x89` magic、`EAF/AAF` 格式字符串、帧数、`table_len`。
- 整体校验：按设备端算法计算 `EAF_TABLE_OFFSET` 后 `table_len` 字节的 checksum。
- 帧表：`frame_size/frame_offset` 是否能定位到合法帧，帧 magic 是否为 `0x5A5A`。
- 图片帧：`_S` header、bit depth、尺寸、block 数、palette 和 block length 表。
- block 数据：检查 block 是否越过 payload，encoding 是否有效。
- 解码验证：支持 RLE、RAW、Huffman Direct、Huffman+RLE 的离线解码和大小检查。

## 说明

JPEG 和 Heatshrink block 默认只做结构和边界检查，不在 Python 里完整解码。正常文件默认只打印摘要和异常位置；要看每帧信息用 `--frames`，要看每个 block 的细节用 `-v`。
