# EAF4 统一容器与帧子模式（0..9）格式规范

`EAF4` 是支持 `BGRA8888`、`RGB565` 与 `RGB565A8` 的统一带索引、带 CRC 动画容器。
这里的 `mode 0..9` 是 EAF4 索引中的**帧级子模式**，不是统一编码表的
顶层 codec ID：子模式 `0..5, 7, 8` 属于 `0x04 LOSSLESS`，子模式 `6, 9` 属于
`0x05 BTC_SAFE`。

支持全四通道 8-bit (`B, G, R, A`) 与 16-bit (`RGB565` / `RGB565+A8`) 无损与有界误差压缩容器。Alpha 为
**Straight Alpha（非预乘）**：RGB 不预乘 A，且 `A=0` 下的隐藏 RGB 仍属于
帧数据。宽和高必须是 4 的倍数，且各自不超过 1024。文件小于 32 MiB，
帧数不超过 10000。完整色深和透明度语义见
[《色深、输出格式与透明度》](EAF_COMPRESSION_MODES_GUIDE.md#3-色深输出格式与透明度)。

## 1. 文件布局

```
[64-byte header][24-byte index × N][zlib payload × N]
```

多字节整数均为小端（Little-Endian）。

### Header（64 字节）

| 偏移 | 类型 | 字段 | 约束 |
| ---: | --- | --- | --- |
| 0 | 4s | magic | `EAF4` |
| 4 | u16 | version | `1` |
| 6 | u16 | header_size | `64` |
| 8 | u16 | width | 4 的倍数 |
| 10 | u16 | height | 4 的倍数 |
| 12 | u32 | frames | 1..10000 |
| 16 | u32 | period_ms | 10..10000 |
| 20 | u32 | frame_bytes | `width * height * bpp` (`bpp` = 4 for BGRA8888, 3 for RGB565A8, 2 for RGB565) |
| 24 | u32 | index_offset | `64` |
| 28 | u32 | data_offset | `64 + frames * 24` |
| 32 | u32 | file_size | 整个文件长度 |
| 36 | u16 | tile_w | `4` |
| 38 | u16 | tile_h | `4` |
| 40 | u32 | header_crc | 本字段置 0 后，对 64 字节头做 IEEE CRC-32 |
| 44 | u32 | index_crc | 索引区 CRC-32 |
| 48 | u32 | flags | `1` |
| 52 | u8 | pixel_format | `0 = BGRA8888`（v1 默认兼容），`1 = RGB565`，`2 = RGB565A8` |
| 53 | u8 | codec_id | `0 = 由帧索引决定（v1 兼容）`，或 `0x04 (LOSSLESS)` / `0x05 (BTC_SAFE)` |
| 54 | 10s | reserved | 全 0 |

### Index Entry（24 字节）

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | u32 | payload_offset |
| 4 | u32 | payload_length |
| 8 | u32 | decoded_size（zlib 解压后的字节数） |
| 12 | u32 | frame_crc32（重建后的 BGRA8888 整帧 CRC32） |
| 16 | u8 | mode（`0..6`） |
| 17 | u8 | 0 |
| 18 | u16 | 0 |
| 20 | u32 | payload_crc32（压缩字节流 CRC32） |

---

## 2. 全部 7 种 8888 编码模式 (`mode = 0..6`)

每一帧先构造对应模式的未压缩字节流，再用 zlib level 6 压缩。C 容器解析器（`components/esp_lv_eaf_player/src/container/eaf4/esp_eaf4_container.c`）通过统一 codec registry 调用对应后端；Python 参考解码器位于 `encoder/eaf4.py`：

| Mode | 名称 | 保真度 | 未压缩数据布局 | 对应的旧 EAF 算法血统 |
| :---: | --- | :---: | --- | --- |
| **0** | `MODE_RAW` | 100% 无损 | `W×H×4` 整帧交错 BGRA8888 | 关键帧（每 `key_interval` 帧强制插入以支持快速 Seek） |
| **1** | `MODE_XOR` | 100% 无损 | `W×H×4` 整帧交错 `cur ^ prev` | 32-bit 整字帧间 XOR |
| **2** | `MODE_PLANAR` | 100% 无损 | `B[W×H] + G[W×H] + R[W×H] + A[W×H]` 四平面分离 `cur ^ prev` | `0x04 LOSSLESS` 的全帧四平面表示 |
| **3** | `MODE_TILE_PLANAR` | 100% 无损 | 小端 `4×4` 变化块位图 + 变化块 `B/G/R/A` 四平面分离 XOR | `0x04 LOSSLESS` 的稀疏四平面表示 |
| **4** | `MODE_TILE_PACKED` | 100% 无损 | 小端 `4×4` 变化块位图 + 变化块交错 32-bit BGRA 字 XOR | 省去四通道移位拼字，直接执行 32-bit 字异或；收益取决于内容与目标芯片 |
| **5** | `MODE_JOINT_DELTA` | 100% 无损 | 小端 `4×4` 变化块位图 + 变化像素三进制联合符号 $(\Delta B,\Delta G,\Delta R,\Delta A)\in[-1,1]^4 \rightarrow 0..80$（`255` 为逃逸）+ 四平面逃逸流 | `0x04 LOSSLESS` 的联合小差分表示 |
| **6** | `MODE_BTC8888_SAFE` | 有界误差 ($\text{maxErr}\le 8$) | `modes[num_tiles]` + 非跳过块载荷：<br>• `0`: Skip (`0B`)<br>• `1`: Flat 单色 (`4B` BGRA)<br>• `2`: 1-bit 双色 (`8B` 端点 + `2B` 掩码)<br>• `3`: 2-bit 四色线性插值 (`8B` 端点 + `4B` 掩码)<br>• `4`: Raw 逃逸块 (`64B` 原始 BGRA8888) | `0x05 BTC_SAFE` 的 BGRA8888 表示 |
| **7** | `MODE_565A8_TILES` | 100% 565+A8 无损 | 小端 `4×4` 变化块位图 + `c_low` + `c_high` XOR + `A8` 加法差分 | `0x04 LOSSLESS` 的 RGB565+A8 块差分表示 |
| **8** | `MODE_565A8_JOINT` | 100% 565+A8 无损 | 小端 `4×4` 变化块位图 + 81 态三进制联合符号 + 三平面逃逸流 | `0x04 LOSSLESS` 的 RGB565+A8 联合符号表示 |
| **9** | `MODE_BTC565_SAFE` | 有界误差 ($\text{maxErr}\le 8$) | `modes[num_tiles]` + 非跳过块载荷（`0B` / `2B` / `6B` / `8B` / `32B` 原始 RGB565 逃逸块） | `0x05 BTC_SAFE` 的 RGB565 表示 |

---

## 3. 编码 Profile 选择

- `--profile v1`：在无损 `Mode 0..3` 中逐帧自适应择优，用于旧版 v1 比特流兼容。
- `--profile v2`：在全部无损模式 `Mode 0..5` 中做带解码成本权重的逐帧选择（加入 `Tile-Packed` 与 `Joint-Delta`）；体积和速度收益必须按素材实测。
- `--profile tile_packed`：关键帧用 `Mode 0`，帧间固定用 `Mode 4 (Tile-Packed)`，追求极致的无损解码速度。
- `--profile btc8888`：`0x05 EAF Bounded` 的内部 profile，使用 `Mode 6`；CLI 通过 `--max-error`（默认 8）控制单通道最大绝对误差上限。
