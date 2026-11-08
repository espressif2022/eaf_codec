# ESP EAF 压缩模式指南

本文描述当前仓库已经收敛的 `0x00 ~ 0x07` 编码体系。旧资产可能携带
`0x08`、`0x09`、`0x0A`、`0x0C` 等历史字节码；C 解码器保留兼容入口，
但统一 CLI 和新编码器不再生成这些编号。

## 1. 容器、codec ID 与子模式

仓库支持两种容器：

- **EAF**：文件头为 `0x89 "EAF"`，主要承载 RGB565、RGB565A8、JPEG
  和 H.264 数据。
- **EAF4**：文件头为 `"EAF4"`，原生输出 BGRA8888，并带帧索引和 CRC。

统一 codec ID 描述产品能力；EAF4 索引里的 `mode 0..6` 是帧级子模式，
两者不能混用：

- EAF4 子模式 `0..5` 都属于顶层 codec `0x04 LOSSLESS`；
- EAF4 子模式 `6` 属于顶层 codec `0x05 BTC_SAFE`。

## 2. 统一编码表

| ID | 名称 | 算法与定位 | Alpha | 保真度 |
| :---: | :--- | :--- | :---: | :---: |
| `0x00` | `PALETTE_RLE` | 4/8-bit 调色板 + RLE，旧资产兼容 | 8-bit 调色板 Alpha | 调色板量化 |
| `0x01` | `PALETTE_HUFF_RLE` | 调色板 + Huffman + RLE，旧资产兼容 | 8-bit 调色板 Alpha | 调色板量化 |
| `0x02` | `JPEG_KEYFRAME` | 整帧 JPEG，也供 Quad-JPEG 关键帧使用 | 不支持 | 有损 |
| `0x03` | `PALETTE_HUFF` | 调色板 + 直接 Huffman，旧资产兼容 | 8-bit 调色板 Alpha | 调色板量化 |
| `0x04` | **EAF Lossless** (`LOSSLESS`) | 帧间差分、平面重排、变化块和联合小差分 + Deflate | 完整 8-bit | 逐比特无损 |
| `0x05` | **EAF Bounded** (`BTC_SAFE`) | `4×4` Skip/Flat/双色/四色/Raw 逃逸块 | 8888 完整 8-bit；565 可用 Color-Key | 有界误差 |
| `0x06` | **EAF JPEG Delta** (`QUAD_JPEG`) | `16×16/8×8` 变化检测 + Abbreviated-JPEG 条带 | 不支持 | 有损 |
| `0x07` | **EAF Video** (`H264`) | Constrained Baseline Annex-B I/P 帧 | 不支持 | 有损 |

公开名称刻意描述能力而非某一版实现：`Bounded` 强调可配置最大误差，避免把
`BTC` 这一内部块算法暴露成产品概念；`JPEG Delta` 说明“变化区 + JPEG”，比
`QUAD_JPEG` 更直接；`Video` 则允许未来替换底层视频实现而不改变用户配置。
协议枚举继续保留，以保证文件和固件兼容。

## 3. 色深、输出格式与透明度

### 3.1 像素格式

| 格式 | 每像素有效位数 | 通道布局 | Alpha 语义 | 典型用途 |
| :--- | :---: | :--- | :--- | :--- |
| `RGB565` | 16 bit | `R5:G6:B5` | 无 | LCD 帧缓冲、低内存不透明动画 |
| `RGB565A8` | 24 bit | RGB565 颜色平面 + 独立 A8 平面 | 0=透明，255=不透明 | LVGL 半透明前景 |
| `RGB888` | 24 bit | 每通道 8 bit RGB | 无 | JPEG/H.264 不透明输出、PPA 输入 |
| `BGRA8888` | 32 bit | B、G、R、A 各 8 bit | Straight Alpha | 高质量透明前景、EAF4 原生输出 |

EAF4 返回的是 **Straight Alpha（非预乘 Alpha）**。颜色通道保存原始颜色值，
不会预先乘以 Alpha；混合阶段再计算前景与背景。因此 `Alpha=0` 的像素仍可能
带有非零 RGB。`0x04 LOSSLESS` 会无损保留这些隐藏 RGB，不能假定透明像素颜色为黑色。

`RGB565A8` 中的“无损”是相对于已经量化后的 RGB565+A8 数据：A8 可逐字节无损，
但 RGB565 相对于原始 RGB888 天然存在 5/6-bit 量化误差。

### 3.2 Codec、色深与 Alpha 能力

| Codec | 原生编码内容 | 可选解码输出 | 连续 8-bit Alpha | 1-bit 透明 |
| :---: | :--- | :--- | :---: | :---: |
| `0x00/01/03` | 4/8-bit 索引 + BGRA 调色板 | 4-bit：RGB565/RGB565A8；8-bit：另可直接输出 BGRA8888 | 是，精度受调色板颜色数约束 | 可表达 |
| `0x02` | JPEG YCbCr/RGB | RGB565 | 否 | 否 |
| `0x04` | RGB565+A8 或 BGRA8888 | 对应载荷的原生格式，不做 565/8888 互转 | 是，无损 | 是 |
| `0x05` | RGB565 或 BGRA8888 BTC | RGB565、BGRA8888 | 仅 BGRA8888 路径支持，且受误差阈值约束 | RGB565 可使用运行时 Color-Key |
| `0x06` | JPEG 关键帧/变化条带 | RGB565 | 否 | 否 |
| `0x07` | H.264 I420 | RGB565、BGRA8888 | 否；BGRA 输出补 `A=255` | 否 |

`0x07` 的 BGRA8888 由 I420 直接生成，并补 `A=255`，不经过 RGB565。
透明素材必须预先与背景合成，或者改用 `0x04`、BGRA8888 `0x05`、调色板模式。

经典 EAF 的输出格式由每个播放器实例的
`esp_eaf_player_config_t.output_format` 在运行时选择，不使用 Kconfig。
默认值是 RGB565，以兼容现有应用；请求 BGRA8888 时，仅 8-bit Palette、
24-bit BGR 和 H.264 等具备直接 32-bit 输出的 codec 可以成功。其余 codec
返回 `ESP_ERR_NOT_SUPPORTED`，不会先解码为 RGB565 再扩展。EAF4 始终输出
原生 BGRA8888。`ESP_EAF_OUT_RGB888` 当前仅为预留值。

### 3.3 Alpha 与 Color-Key 规则

- **调色板 Alpha**：`0x00/0x01/0x03` 的每个调色板项可以携带 A8，适合少色透明图；
  颜色或 Alpha 种类很多时容易受 16/256 色上限影响。
- **完整连续 Alpha**：优先使用 BGRA8888 `0x04`；允许每通道默认最大误差 8 时，
  可用 BGRA8888 `0x05` 换取更小体积和更快解码。
- **RGB565A8**：`0x04` 将 RGB565 与 A8 分平面编码，Alpha 无损；适合帧缓冲和
  LVGL 已经采用 RGB565 的设备。
- **Color-Key**：`lv_eaf_set_color_key(obj, rgb565)` 在播放端把指定 RGB565 值映射为
  `A=0`，其他颜色映射为 `A=255`。它是运行时 1-bit 抠图，不是连续 Alpha，也不是
  BTC/JPEG/H.264 码流中的 Alpha 通道；`-1` 表示关闭。
- **输入 Alpha 丢弃规则**：使用 `--format 565 --codec bounded`、`jpeg-delta` 或
  `video` 时，PNG/APNG 的连续 Alpha 不会进入对应颜色码流。若仍需透明，只能对
  RGB565 BTC 使用 Color-Key，或改用 `lossless`/BGRA8888 编码。
- **透明边缘**：Color-Key 无法表达抗锯齿半透明边缘。带柔边、阴影、发光或玻璃效果的
  素材应使用 A8/BGRA8888，或在编码前预合成到固定背景。

```c
/* RGB565 纯色抠图：0x0000 作为全透明黑色；传 -1 可关闭。 */
lv_eaf_set_color_key(player, 0x0000);
```

### 3.4 素材选择

| 素材特征 | 建议格式与 codec |
| :--- | :--- |
| 少色、透明状态图标，已有旧资产 | 调色板 `0x00/0x01/0x03` |
| 任意背景上的透明前景，要求严格一致 | BGRA8888 `0x04 LOSSLESS` |
| 透明前景，允许轻微误差，优先速度/体积 | BGRA8888 `0x05 BTC_SAFE` |
| RGB565 设备上的无损半透明前景 | RGB565A8 `0x04 LOSSLESS` |
| 只有全透明/全不透明两态的抠图 | RGB565 + Color-Key |
| 不透明自然光影或实拍 | `0x06 QUAD_JPEG` |
| 不透明长视频、Flash 最优先 | `0x07 H264` |

## 4. `0x04 EAF Lossless`

### BGRA8888 / EAF4

编码器逐帧构造多个候选表示，再按压缩体积和解码成本选择：

1. Raw：整帧 BGRA8888，作为关键帧；
2. XOR：整帧与前帧做 32-bit 字 XOR；
3. Planar：XOR 后拆为连续 B/G/R/A 四平面；
4. Tile-Planar：`4×4` 变化块位图 + 变化块四平面 XOR；
5. Tile-Packed：变化块位图 + 交错 BGRA 字 XOR；
6. Joint-Delta：四通道 `[-1,1]` 差分编码为 `0..80`，其他像素走逃逸流。

所有候选最终使用 zlib Deflate。关键帧直接 inflate 到输出帧缓冲；帧间模式
按需分配 scratch，以降低 PSRAM 峰值。

### RGB565+A8 / EAF

颜色拆为 RGB565 低、高字节平面，Alpha 保持独立 A8 平面。帧间仅保存
发生变化的 `4×4` 块；颜色使用 XOR，Alpha 使用模 256 差分。小差分可进入
联合符号流，其余值进入逃逸流，两个流均使用 Raw Deflate。

适用场景：要求完整透明度或像素严格一致的 UI、前景动画和测试基准。

## 5. `0x05 EAF Bounded`

每个 `4×4` 块从以下表示中选择：

- `0 Skip`：沿用上一帧；
- `1 Flat`：保存一个颜色；
- `2 Two-color`：两个端点 + 16-bit 1bpp 索引；
- `3 Four-color`：两个端点 + 32-bit 2bpp 线性插值索引；
- `4 Raw`：候选不能满足误差阈值时保存原始像素。

Raw 逃逸块是误差保证的关键：BGRA8888 保存 64 字节，RGB565 保存 32 字节。
默认 `--max-error 8` 时，8888 路径保证重建后任意通道最大绝对误差不超过 8。
RGB565 的误差约束以量化后的 RGB565 源为基准；若再与原始 RGB888 比较，还会包含
5/6-bit 量化误差。

适用场景：卡通、文字、锐利 UI、透明前景，以及优先考虑解码速度的动画。

## 6. `0x06 EAF JPEG Delta`

编码过程：

1. 按 `16×16` 宏块检测与上一重建帧的差异；
2. 对变化宏块进一步生成四个 `8×8` 象限的 4-bit 掩码；
3. 将变化的 `8×8` 块拼成高 16 像素的一维 JPEG 条带；
4. 关键帧保存完整 JPEG 并缓存 DQT/DHT/SOF/SOS；
5. 差分帧仅保存掩码、象限信息和 JPEG entropy scan；
6. 若差分条带不比整帧 JPEG 小，自动回退到整帧 JPEG。

为适配 ESP32-S31/P4 硬件 JPEG 2D-DMA，编码器会补齐条带 MCU 数量；解码器
在宽高和 Cache Line 对齐满足要求时启用硬件，否则回退软件 JPEG。

该模式不保存 Alpha。适合预合成背景后的自然光影、渐变、粒子和实拍动画。

## 7. `0x07 EAF Video`

使用 Constrained Baseline、无 B 帧的 Annex-B 访问单元。首帧包含 SPS/PPS/IDR，
后续使用 P-Slice；解码得到 I420 后，通过 BT.601 limited-range LUT 转为 RGB565
或 BGRA8888。

它通常具有最小 Flash 体积，但软件解码 CPU 开销最高，也不支持 Alpha。适合长动画、
全屏运动和 Flash 容量优先的产品。

## 8. 选型速查

| 需求 | 推荐 |
| :--- | :--- |
| BGRA8888、完整 Alpha、严格无损 | `--format 8888 --codec lossless --lossless-profile v2` |
| 完整 Alpha、允许轻微有界误差、追求速度 | `--format 8888 --codec bounded` |
| RGB565 卡通、文字、扁平 UI | `--format 565 --codec bounded` |
| RGB565+A8 严格无损 | `--format 565 --codec lossless` |
| 不透明自然光影或实拍 | `--format 565 --codec jpeg-delta` |
| 长视频、极限压缩 | `--codec video` |
| 已有调色板老资产 | 保留 `0x00/0x01/0x03`，新资产按上述模式重编码 |

## 9. 常用命令

```bash
python3 tools/eaf_cli.py encode -o out.eaf4 --format 8888 --codec lossless \
  --lossless-profile v2 frames/*.png

python3 tools/eaf_cli.py encode -o out.eaf4 --format 8888 --codec bounded \
  --max-error 8 frames/*.png

python3 tools/eaf_cli.py encode -o out.eaf --format 565 --codec jpeg-delta \
  frames/*.png

python3 tools/eaf_cli.py inspect out.eaf4
```

详细二进制布局参见 [FORMAT.md](FORMAT.md)，硬件实测数据参见
[BENCHMARK_REPORT.md](BENCHMARK_REPORT.md)。

## 10. 历史字节码兼容边界

C 解码器目前会规范化以下旧字节码：

| 历史字节码 | 当前 codec |
| :---: | :---: |
| `0x0C Lossless-RGBA` | `0x04 LOSSLESS` |
| `0x09 BTC-Safe` | `0x05 BTC_SAFE` |
| `0x08 Quad-Abbrev` | `0x06 QUAD_JPEG` |

`0x0A Planar-Delta` 仍保留专用兼容代码和性能统计接口，但不属于统一 CLI 的新资产
输出集合。兼容分支不得当作当前编号写入新文档或新编码器。
