# ESP32 动画压缩模式全景指南：技术栈、使用特点与适用场景

**文档更新日期**：2026-10-26  
**实测硬件平台**：
- **ESP-BOX-3**（`/dev/ttyACM0`，ESP32-S3 @ 240MHz，16MB QIO Flash + 16MB Octal PSRAM，320×240 SPI LCD + GT911）
- **ESP32-S3-LCD-EV-Board**（`/dev/ttyUSB0`，ESP32-S3-WROOM-1-N16R16V，480×480 RGB LCD `GC9503CV` + `FT5x06` / 800×480 `ST7262` + `GT1151`）

---

## 1. 总体架构与容器体系概览

当前工程同时支持两大动画容器体系，并统一通过 LVGL 控件（`lv_eaf` 与 `lv_efc`）向应用层提供播放接口：

1. **EAF 容器体系（`esp_lv_eaf_player`）**：
   - 外层文件头：`0x89 "EAF"` + `total_frames (4B)` + `checksum (4B)` + `payload_len (4B)` + 帧偏移表；
   - 单帧头：`0x5A5A` + `"_S\0" "00V1.0"` + `bit_depth (4/8/24)` + `(width, height, blocks, block_height)` + 可选调色板；
   - **多模态分块编码**：通过每个 Block 首字节 `encoding_type (0x00 ~ 0x0C)` 支持从无损调色板、色胞截断（BTC）、连续 8-bit Alpha 色胞（`0x0B BTC-Alpha`）、100% 逐比特无损 RGBA 三平面帧间差分（`0x0C Lossless-RGBA`）、缩略图集 JPEG、位平面重排（Planar）到 H.264 视频流的 **13 种编码模式**。
2. **EFC 容器体系（`esp_efc_codec`）**：
   - 外层文件头：`"EFC1"`，专为 `4×4` 像素块（Tile=4）设计的**针对 RGB565 源零误差无损**指令流编码器，结合全文件全局 Raw Deflate 压缩。

---

## 2. 各压缩模式技术栈与二进制协议总览（已按“6 主力 + 2 老版兼容 + 6 淘汰/合并”分级）

| 状态分类 | 编码标识 | 模式名称 | 核心技术栈（编码端 $\rightarrow$ 解码端） | 保真度类型 | 透明度支持 | 帧间依赖 |
| :---: | :---: | :--- | :--- | :--- | :--- | :--- |
| **★ 核心主力** | **`0x0C`** | `EAF Lossless-RGBA` | **100% 精确 `4×4` 位图 (`1800B`) + 三平面差分（`RGB565_Lo XOR` + `RGB565_Hi XOR` + `A8 Delta`）** + ROM `tinfl` | **100% 逐比特无损**（与源 `RGB565A8` `CRC32` 完全一致） | **8-bit 连续无损 Alpha**（`RGB565A8`） | 依赖前帧 |
| **★ 核心主力** | **`0x09`** | `EAF BTC-Safe` | `4×4` 自适应色胞截断（Skip / 单色 / 1-bit / 2-bit DXT1）+ **`mode=4` 原始 RGB565 逃逸块（32B）** + ROM `tinfl` | **有界误差准无损**（$\text{maxErr} \le 8$） | 可选 1-bit Color-Key | 依赖前帧 |
| **★ 核心主力** | **`0x08`** | `EAF Quad-Abbrev` | 两级四叉树（外层 `16×16` 掩码 + 内层 4-bit `8×8` 象限掩码）+ 1D Abbrev-JPEG（HQ 采用 `4:4:4`） | 有损（频域 DCT） | 不支持（需背景预合成） | 依赖前帧 + 首帧表头 |
| **★ 核心主力** | **`0x0A`** | `EAF Planar-Delta` | **RGB565 低/高字节平面分离** + 可选行水平差分 + 帧间 XOR + 持久 `zlib` 流 + 32-bit 对齐还原 | **无损重排**（与输入 RGB565 100% 一致） | 可选 1-bit Color-Key | 依赖前帧（每 30 帧刷新） |
| **★ 核心主力** | **`0x07`** | `EAF H.264` | Constrained Baseline Annex-B（`SPS+PPS+IDR` / `P-Slice`，BT.601 limited-range） $\rightarrow$ **双核 `tinyh264` + 零分支 LUT** | 有损（YUV 4:2:0 运动补偿） | 不支持（需背景预合成） | 强依赖前帧（GOP） |
| **★ 核心主力** | **`EFC1` (1B)** | `ESP EFC (<=256色)` | `4×4` 块游程复用（前帧/左邻/上邻/纯色）+ 1字节调色板 Token + 全文件 Deflate + SIMD | **RGB565 逐像素无损**（限 $\le 256$ 色） | **1-bit Color-Key**（`Opcode 19`） | 强依赖前帧 + 全局流 |
| **老版本兼容** | **`0x00`** | `EAF RLE` | 4/8-bit 调色板量化 + `(count, value)` 双字节游程 $\rightarrow$ 查表还原（保留用于兼容旧版 v1.0/v2.0 资产） | 有损（$\le 256$ 色调色板） | **8-bit Alpha**（RGBA 调色板） | 无（单帧独立） |
| **老版本兼容** | **`0x01` / `0x03`** | `EAF Huffman` | 4/8-bit 调色板 + RLE（可选）+ 显式霍夫曼树字典 $\rightarrow$ 逐比特遍历树（保留用于兼容旧版 v1.0/v2.0 资产） | 有损（$\le 256$ 色调色板） | **8-bit Alpha**（RGBA 调色板） | 无（单帧独立） |
| *内部子帧* | *`0x02`* | *`EAF JPEG`* | *仅作为 `0x08 Quad-Abbrev` 的第 0 帧（提供共享 `DQT/DHT` 表头）或全屏回退帧内部使用* | *有损（频域 DCT）* | *不支持* | *仅条带跳过时依赖前帧* |
| *备选有损* | *`0x0B`* | *`EAF BTC-Alpha`* | *`4×4` DXT5/BC3 变体（2bpp RGB565 + 2bpp BC4 Alpha 4级插值），建议优先使用无损 `0x0C`* | *有损（4级 Alpha 插值）* | *8-bit Alpha* | *依赖前帧* |
| ~~已合并~~ | ~~`0x06`~~ | ~~`EAF BTC-Zlib`~~ | ~~无逃逸块的早期 BTC，已完全合并进 `0x09 BTC-Safe`（通过 `--btc-max-error` 控制）~~ | ~~有损~~ | — | — |
| ~~已淘汰~~ | ~~`0x04` / `0x05`~~ | ~~`Tile-JPEG / Abbrev`~~ | ~~早期 `16×16` 宏块图集 JPEG，已被 `0x08 Quad-Abbrev` 全面上位替代~~ | ~~有损~~ | — | — |

---

## 3. 核心压缩模式深度解析：技术栈、特点与适用场景

### 3.1 `EAF BTC` 系列（`0x06 BTC_ZLIB` / `0x09 BTC_SAFE`）—— 极速解码与有界误差主力

#### 技术栈与编码原理
- **块划分**：将画面按 `4×4` 像素（16 个 RGB565 像素，原始 32 字节）划分为网格（240×240 共 3,600 块，支持按最大负载自动切分水平条带）。
- **5 种块级编码原语（以 `0x09 BTC_SAFE` 为准）**：
  - `mode = 0`（Skip，**0 B**）：与上一帧重建像素相比误差在阈值内，直接跳过；
  - `mode = 1`（Flat 单色，**2 B**）：存 1 个 `uint16_t` RGB565 均值，解码时拼成 `uint32_t` 双像素 4 行直写；
  - `mode = 2`（1-bit 双色 BTC，**6 B**）：沿块内 RGB 跨度最大的通道切分高低两组，存 2 个 RGB565 端点色（4B）+ 16-bit 掩码（2B）；
  - `mode = 3`（2-bit 四色 DXT1 线性插值，**8 B**）：存 2 个极值端点色 $c_0, c_3$（4B）+ 32-bit 2bpp 索引（4B），解码器用整数运算实时插值出 $c_1 = (2c_0+c_3)/3$ 与 $c_2 = (c_0+2c_3)/3$；
  - `mode = 4`（Raw 原始逃逸块，**32 B**，仅 `0x09`）：当复杂高频边缘在 `mode 1~3` 下的重建单通道最大误差 $> 8$ 时，直接原样保存 16 个 RGB565 像素，彻底杜绝肉眼可见的方块伪影。
- **熵编码与解码加速**：每帧独立打包为 Raw Deflate 流（`wbits = -15`），解码端直接调用 **ESP32-S3 掩膜 ROM 内置的 `tinfl_decompress_mem_to_mem()`**，零 Flash 库体积开销、无需 JPEG 解码器（省去约 19KB DRAM）、无需 IDCT 和 YUV 转 RGB。

#### 使用特点
- **优势**：
  1. **解码速度全场最快**：纯位运算与移位查表，Emoji 场景下仅 **1.04 ms（PSRAM）/ 2.62 ms（HQ）**，二次元动漫仅 **2.61 ms / 5.73 ms（HQ）**，比 EFC 快约 **1.8 ~ 2.2 倍**；
  2. **动漫描边无振铃噪点**：空域插值不会像 JPEG DCT 那样在锐利黑线旁产生“蚊式噪点（Mosquito Noise）”；
  3. **硬误差上限保证**：`0x09 BTC_SAFE` 保证全序列任意像素的 RGB888 展开误差 $\le 8$（PSNR 高达 $36.5 \sim 55.2\text{ dB}$）；
  4. **流式零常驻内存**：按帧独立解压，解压完即释放临时缓冲，局部脏矩形（`esp_eaf_dirty_rect_t`）精准触发 LVGL 局部刷新。
- **局限**：
  - 在全屏自然实拍视频或超复杂高频纹理（如未重排的 Tarot HQ）下，由于大量块触发 `mode=3` 或 `mode=4`，体积会大于频域 `Quad-JPEG`。

#### 适合场景
- **首选场景**：**机器人扁平表情（Emoji）**、**二次元赛璐璐/卡通动画（Anime）**、**带锐利文字或线条的 UI 动效**、**3D 渲染家电特效（配合 `0x0A Planar` 重排）**。

---

### 3.2 `EAF Planar-Delta`（`0x0A`）—— 无损位平面重排加速器

#### 技术栈与编码原理
- **位平面分离（Byte-Plane Splitting）**：RGB565 每个像素占 2 字节（小端字节序下低字节含 `G_low + B`，高字节含 `R + G_high`）。相邻像素或帧间差分的**高字节往往高度相似，低字节包含细微抖动**。若交错存储（`L0 H0 L1 H1...`），LZ77 滑窗难以匹配；`0x0A` 将一帧拆分为连续的 **`Low Plane (W×H bytes)` + `High Plane (W×H bytes)`**。
- **双预测器标志（`flags` 字节）**：
  - `bit 0`：**帧间前帧 XOR**（`current_rgb565 ^ prev_rgb565`，每 30 帧强制完整刷新）；
  - `bit 1`：**行内水平差分**（`uint16_t` 相邻像素累减）。
- **极速还原管线**：
  - 解压阶段：复用持久化的 `z_stream`（每帧 `inflateReset`），避免重复分配字典；
  - 还原阶段：针对 4 字节对齐的 Framebuffer，使用 `__may_alias__` 16-bit 对齐读取高低平面，单条位运算拼出 32-bit 双像素并执行 32-bit 写入或 XOR，将还原耗时从 `16.0 ms` 压缩至 **`3.9 ms`**。

#### 使用特点
- **优势**：
  1. **100% 像素无损**：与输入基底（无论是 BTC 还是 EFC 像素序列）逐帧逐像素完全一致（板端数千帧 FNV-1a 校验 100% 匹配）；
  2. **在特定 3D/AIGC 场景同时实现「大幅缩容 + 解码提速」**：
     - **咖啡机萃取 BTC**：`399.0 KiB → 127.6 KiB`（**缩小 68.03%**），PSRAM 解码 `9.06 → 6.75 ms`（**快 25.5%**）；
     - **咖啡机蒸汽 BTC**：`539.8 KiB → 362.8 KiB`（**缩小 32.79%**），PSRAM 解码 `10.13 → 7.43 ms`（**快 26.6%**）；
     - **Tarot BTC / EFC**：体积分别缩小 **42.55%**（`3097.4 → 1779.6 KiB`）和 **28.07%**（`2502.4 → 1799.8 KiB`），PSRAM 解码从 `16.05 / 20.07 ms` 降至 **`11.31 / 11.38 ms`**（**快 29.5% / 43.3%**）。
- **局限**：
  - 每个解码器实例需常驻 **`115,200 Bytes (112.5 KiB)`** 工作缓冲 + `zlib` 状态；
  - 对全屏高速剧烈旋转的画面（如洗衣机滚筒特写 `washer_spin`），重排后体积反而增大 21%~28%，且在 SRAM Framebuffer（工作缓冲回退至 PSRAM）下不占优，因此**必须按素材实测择优启用，不可无脑全量替换**。

#### 适合场景
- **首选场景**：**PSRAM Framebuffer 配置下的 3D 渲染流体/蒸汽家电动画（BTC 基底）**、**AIGC 复杂插画局部特效（Tarot BTC/EFC 基底）**。

---

### 3.3 `EAF Quad-Abbrev`（`0x08`）与 `Tile-Abbrev`（`0x05`）—— 频域缩略图集 JPEG

#### 技术栈与编码原理
- **两级四叉树稀疏提取（`0x08`）**：
  - 第一级：29 字节（225 bits）位图标记 `240×240` 中发生变化的 `16×16` 宏块；
  - 第二级：对每个变化宏块附加 1 个 `4-bit Nibble`（`1..15`），精确指示左上、右上、左下、右下 **4 个 `8×8` 象限**中哪几个发生了变化，仅提取变化的 `8×8` 象限，每 4 个拼成 1 个 `16×16` MCU，组成 `width = 16 × M, height = 16` 的 **1D 水平条带图集**。
- **Abbreviated-JPEG 共享表头**：
  - 第 0 帧使用标准 JPEG（`0x02`），解码器自动提取并缓存 590 字节的 `DQT + SOF0 + DHT + SOS` 模板；
  - 后续差分帧**剥离全部 JPEG 表头（每帧省 609 字节）**，仅存掩码 + 纯熵编码扫描流，解码时在内存动态回填 `(atlas_w, atlas_h)` 后交由 `esp_new_jpeg`（或 ESP32-P4 硬件 JPEG）一次解码完成。
- **自适应整帧回退**：编码器每帧自动对比「整帧 JPEG」与「四叉树差分图集」大小，择小写入，并按 `--keyframe-interval`（默认 60）定期刷新。
- **HQ 模式增强**：采用 **JPEG `quality=84` + `4:4:4` 原生色度（无色度下采样）**，彻底消除 `8×8` 异源小块拼接时的色度边缘污染。

#### 使用特点
- **优势**：
  1. **连续自然光影与实拍视频的体积/画质甜点**：频域 DCT 对自然渐变、海浪、夕阳、散景光晕的能量集中度极高。在 HQ 实拍视频（`Video`）下仅 **488.8 KiB**（而 BTC HQ 为 `981.0 KiB`、EFC 为 `1,789.7 KiB`，**比 BTC 小一半、比 EFC 小 73%**）；
  2. **支持芯片硬件 JPEG 加速**：在带硬件 JPEG 引擎的芯片（如 ESP32-P4）上，全帧/图集 JPEG 可进一步卸载 CPU 算力。
- **局限**：
  - 依赖 `esp_new_jpeg` 解码器组件（运行时约 19KB DRAM 开销）；
  - 不适合极高对比度的纯平涂细线图标（除非开到 `q=84 4:4:4`，否则 DCT 会有轻微振铃）。

#### 适合场景
- **首选场景**：**自然相机实拍视频（`Video`）**、**柔光径向渐变/散景光球 UI（`Poke`）**、**中低体积预算下的复杂插画动效**。

---

### 3.4 `ESP EFC Codec`（`EFC1`）—— 面向少色 UI 的 RGB565 严格无损编码

#### 技术栈与编码原理
- **块级游程与微调色板（`4×4` Tile）**：
  - 块游程拷贝：`Opcode 1`（复用前帧同位置连续 $N$ 块）、`Opcode 2`（复用左邻块）、`Opcode 18`（复用上邻块）、`Opcode 0`（连续 $N$ 个同色纯色块）；
  - 块内微调色板：`Opcode 4`（$\le 2$ 色，1-bit 索引）、`Opcode 5`（$\le 4$ 色，2-bit 索引）、`Opcode 6`（$\le 8$ 色，3-bit 索引）；
  - 帧间块内局部差分：`Opcode 7`（变化像素索引列表）、`Opcode 8`（16-bit 变化位图掩码 + 变化像素色值）；
  - 原始块兜底：`Opcode 3`（块内 $>8$ 色且与前帧差异大时，直接存 16 个像素 Token）。
- **全局字典与 SIMD 加速**：全视频序列颜色数 $\le 256$ 时自动切换为 1 字节调色板 Token，否则为 2 字节 RGB565 Token；全文件所有帧的 Token Stream 合并做一次 `zlib` Raw Deflate（Level 9），解码端在 ESP32-S3 上启用 `CONFIG_ESP_EFC_USE_SIMD=y`（PIE 指令集加速块拷贝）及 `Opcode 19` 1-bit Color-Key 透明支持。

#### 使用特点
- **优势**：
  1. **对源 RGB565 100% 逐像素零误差**：在取消前置 GIF 调色板缩减、使用 `tools/encode_rgb565_efc.cjs` 直转后，所有帧与原始 RGB565 完全一致（$\text{maxErr} = 0$）；
  2. **原生支持 1-bit Color-Key 透明度**：通过 `esp_efc_dec_frame_with_alpha()` 直接输出 `RGB565A8`；
  3. **资源打开耗时极短**：`open` 仅约 `0.06 ~ 0.11 ms`；在平涂少色场景（如 `Emoji` 96.4 KiB、静态背景占比大的 `espresso_flow` 152.6 KiB）下压缩率优秀。
- **局限**：
  1. **对连续渐变/实拍噪点极度敏感**：只要 `4×4` 块内出现 $>8$ 种不完全相等的 RGB565 值，微调色板即失效。因此实拍视频（`Video`）体积高达 **1,789.7 KiB**，`Poke` 达 **1,304.7 KiB**；
  2. **解码耗时约为 BTC 的 1.8~2.2 倍**：大量变长 Opcode 与 `varint` 分支解析导致复杂画面解码需 `13.7 ~ 17.1 ms`；且 `lv_efc` 会将整个 EFC Blob 拷贝/常驻在 PSRAM 中。

#### 适合场景
- **首选场景**：**要求 100% 逐像素零误差的少色扁平 UI 图标/表情**、**需要原生 1-bit Color-Key 透明抠图的静态背景局部动效**。

---

### 3.5 `EAF H.264`（`0x07`）—— 极限超小体积视频流模式

#### 技术栈与编码原理
- **编码端**：使用 `libx264` 生成 Constrained Baseline Profile、无 B 帧（首帧 `SPS+PPS+IDR`，后续帧 `P-Slice`）、显式标记 **BT.601 Limited-Range (`tv / smpte170m`)**、HQ 模式采用 `CRF 18` 并启用环路滤波（Deblocking），每帧一个独立 Annex-B Access Unit 封装进 EAF Block。
- **解码端（ESP32-S3 双核 + 零分支 LUT）**：
  - 启用 `CONFIG_ESP_H264_DECODER_IRAM=y` 与 `CONFIG_ESP_H264_DUAL_TASK=y`（双核协同解码，Core 1 优先级 17）；
  - 解码输出 `I420 (YUV420P)` 后，通过 `esp_eaf_i420_to_rgb565()` 预计算的 **BT.601 Limited-Range 查表（`s_y_lut`, `s_rv_lut`, `s_gu_lut`, `s_gv_lut`, `s_bu_lut`）+ 1024 项免分支饱和截断表（`s_r565_clamp` 等）**，每次循环同時转换 `2×2` 共 4 个像素并以 32-bit Word 写入 Framebuffer。

#### 使用特点
- **优势**：
  - **全场体积极限冠军（比其他所有格式小 3 ~ 10 倍）**：跨帧亚像素运动矢量补偿（Motion Compensation）使其在所有场景下体积最小：
    - `Emoji`：**26.2 KiB**（旧版 CRF20 仅 **12.1 KiB**，压缩比高达 **465x**）
    - `Anime`：**86.3 KiB**（旧版 **35.2 KiB**）
    - `Poke`：**120.7 KiB**（旧版 **54.9 KiB**）
    - `Video`：**199.9 KiB**（旧版 **82.1 KiB**）
    - `Tarot`：**277.4 KiB**（旧版 **84.2 KiB**）
- **局限**：
  - **CPU 解码开销最高**：ESP32-S3 纯软件解码 240×240 H.264 单帧平均耗时 **24 ~ 35 ms**（约占 15 FPS 周期下 36% ~ 52% 的 CPU 时间），不适合在同一时间还要跑重度软件渲染或多路并发解码的场景；
  - **YUV 4:2:0 色度下采样**：在高反差细彩线边缘不如 RGB565 原生的 BTC/EFC 锐利。

#### 适合场景
- **首选场景**：**Flash 容量极度受限（如 4MB/8MB Flash 需塞入数十个长动画）**、**全屏运镜/背景滚动的实拍或 3D 宣传片**。

---

### 3.6 传统格式：`EAF RLE` (`0x00`) 与 `EAF Huffman` (`0x01`/`0x03`)

#### 技术栈与使用特点
- **原理**：每帧携带 16 色（4-bit）或 256 色（8-bit）`BGRA` 调色板，像素索引流使用 `(repeat_count, repeat_val)` 双字节游程或 Huffman 树压缩；解码器通过 `esp_eaf_palette_cache_t` 缓存 RGB565 与 Alpha 转换结果。
- **优势**：结构极其简单，无帧间状态依赖。
- **局限**：
  1. **无帧间差分**：哪怕背景 90% 静止，每帧仍需重复编码全屏像素（例如 `Emoji RLE8` 体积 227.8 KiB，是 `BTC` 的 2.2~5.6 倍）；
  2. **极其害怕抖动（Dithering）与渐变**：一旦输入带抖动噪点，`repeat_count` 退化为 1，体积膨胀 200%（如 `Tarot` RLE 高达 4.3 MB）。

#### 适合场景
- **适合场景**：**仅需几帧的小尺寸扁平 UI 状态挂件**。

---

### 3.7 `EAF Lossless-RGBA` (`0x0C`) 与 `EAF BTC-Alpha` (`0x0B`) —— 连续 8-bit Alpha 前景解耦专用

#### 技术栈与编码原理
针对 `foreground.apng`（`480×480`、150 帧、多层半透明咖啡波浪叠加于独立静态背景 `background.png` 之上）的严苛场景，提供了**前景与背景 100% 解耦**的两套连续 8-bit Alpha (`LV_COLOR_FORMAT_RGB565A8`) 压缩方案：

1. **`0x0C ESP_EAF_ENCODING_LOSSLESS_RGBA`（`--mode lossless-rgba`，100% 比特级无损主力）**：
   - **零容忍 `4×4` 块掩码（`1,800 Bytes`）**：将 `480×480` 划分为 `14,400` 个 `4×4` 像素块；仅当块内 16 个 `RGB565` 像素 **且** 16 个 `A8` 字节与上一帧 **100% 逐比特相等** 时才置 `mask = 0` 跳过；
   - **三平面帧间差分重排（Planar Temporal-XOR / Delta）**：对发生变化的 `n_changed` 个 `4×4` 块，将其拆分为三个连续同质平面：
     - `c_low[n_changed * 16]`：`RGB565` 低字节帧间 XOR（`cur_low ^ prev_low`）；
     - `c_high[n_changed * 16]`：`RGB565` 高字节帧间 XOR（`cur_high ^ prev_high`）；
     - `a_plane[n_changed * 16]`：`A8` 透明度字节帧间减法差分（`(cur_a - prev_a) & 0xFF`）；
   - **为什么比 `diehl_efc_evaluation`（将 `RGB565 + A8` 拼成 `480×720` `uint16_t` EFC）小一半以上？**
     - 原始 EFC 将下半屏的相邻两个 8-bit Alpha 像素硬拼成一个 `uint16_t` 样本，一旦流体波浪产生连续 8-bit Alpha 渐变，每个 `4×4` 块内的 `uint16_t` 组合数立刻超过 8 种，导致 EFC 微调色板失效并退化为 2 字节原始 Token；
     - 而 `0x0C` 将 `A8` 独立为单字节帧间差分平面，平滑波浪的帧间差高度集中在 `0, ±1, ±2` 附近，经 Raw Deflate (`wbits=-15`) 压缩后体积从 **`8,198.7 KiB` 降至 `3,902.6 KiB`（缩小 `52.4%`，节省 `4.30 MB` Flash）**，且 **150 帧 `CRC32` 与原始 EFC 100% 逐比特一致**！
2. **`0x0B ESP_EAF_ENCODING_BTC_ALPHA`（`--mode btc-alpha`，轻量有损 DXT5 变体）**：
   - 每个 `4×4` 块使用 2-bit 模式：`0=Skip`、`1=单色 RGBA (3B)`、`3=2bpp RGB565 + 2bpp BC4/DXT5 8-bit Alpha 4级插值 (14B)`；
   - 体积进一步压至 **`2,891.5 KiB` (`35.0x`)**，解码耗时 `18.30 ms` (`16.0 fps`)，但在多层陡峭半透明波浪处会有轻微 4 级 Alpha 量化误差。

#### `ESP32-S3-LCD-EV-Board` (`480×480` RGB) 双板 1:1 实测对比（`/dev/ttyUSB0` vs `/dev/ttyUSB1`）

| 评估指标（`480×480` · 150 帧 · `RGB565A8` 实时混合） | `/dev/ttyUSB1`：`diehl_efc_evaluation` (原始 EFC `480×720`) | `/dev/ttyUSB0`：`evboard_apng_demo` (`0x0B BTC-Alpha` 有损) | `/dev/ttyUSB0`：`evboard_apng_demo` (**`0x0C Lossless-RGBA` 无损**) |
| :--- | :---: | :---: | :---: |
| **前景与背景解耦** | 是（`background.rgb565` + 前景） | 是（`background.rgb565` + 前景） | **是（`background.rgb565` + 前景）** |
| **150 帧 `RGB565+A8` `CRC32` 校验** | `150/150` 通过（基准） | 有损（Alpha 4级插值） | **`150/150` 100% 逐比特匹配（零误差）** |
| **前景动画打包体积** | `8,198.7 KiB` (`8.01 MB`) | `2,891.5 KiB` (`2.82 MB`) | **`3,902.6 KiB` (`3.81 MB`)（缩小 52.4%，省 4.30 MB）** |
| **相对未压缩比 (`101.25 MB`)** | `12.3x` | `35.0x` | **`25.9x`（比原 11.03 MB APNG 小 65.5%）** |
| **全链路实测渲染帧率 (`fps`)** | `11.98 ~ 12.02 fps` | `15.96 ~ 16.01 fps` | **`14.46 ~ 14.71 fps`（全链路快 +21.5%）** |
| **单帧纯解码耗时 (`decode_ms`)** | ~`36 ms` | `18.30 ms` (`54.6 fps`) | **`24.97 ms` (`40.1 fps`)** |
| **运行时剩余可用 PSRAM** | `4.08 MB` (`4,282,484 B`) | `9.49 MB` | **`8.83 MB` (`9,261,972 B`，多省出 4.75 MB PSRAM）** |

---

## 4. 场景化选型决策矩阵（速查表）

在实际产品开发中，请根据**视觉内容特征**、**透明度需求**与**Flash/CPU 预算**按下表直接选型：

| 业务场景类型 | 画面特征 | 推荐首选方案（主力平衡） | 极限体积方案（Flash 紧缺） | 最高画质/无损方案 | 避坑指南 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. 机器人表情 / 扁平少色 UI** | 纯色背景、几何图形、颜色少 | **`EAF BTC (0x06 / 0x09)`**<br>40~101 KB，1.0~2.6 ms/帧 | **`EAF H.264 (0x07)`**<br>12~26 KB（24 ms/帧） | **`EFC RGB565`**<br>96.4 KB，4.4 ms，零误差 | 避免用无帧间差分的 `RLE8`（重复存黑底，体积大 2~4 倍） |
| **2. 二次元动漫 / 赛璐璐角色** | 平涂色块 + 锐利黑线描边 + 局部动作 | **`EAF BTC-Safe (0x09)`**<br>387 KB（或 `0x06` 148 KB），无描边噪点，5.7 ms | **`EAF H.264 (0x07)`**<br>35~86 KB | **`EFC RGB565`**<br>423 KB，12.1 ms | 避免低码率 JPEG（黑线旁必出蚊式噪点）或抖动 GIF |
| **3. 自然相机实拍视频** | 全屏连续光影、海浪/云层、传感器底噪 | **`EAF Quad-Abbrev (0x08)`**<br>181~489 KB，6.1~12.6 ms | **`EAF H.264 (0x07)`**<br>82~200 KB | **`EAF BTC-Safe (0x09)`**<br>981 KB，误差 $\le 8$ | **严禁使用 RLE8 / 默认 EFC**（调色板与 4×4 精确匹配在实拍视频上完全失效，体积 $>1.5\text{ MB}$） |
| **4. 柔光渐变 / 散景光效 (`Poke`)** | 高密度径向柔光过渡、无硬边界 | **`EAF Quad-Abbrev (0x08)`**<br>197~454 KB，无色阶断层 | **`EAF H.264 (0x07)`**<br>55~121 KB | **`EAF BTC-Safe (0x09)`**<br>685 KB，8.9 ms | 严禁经由 256 色抖动 GIF 中转（麻点既毁画质又让体积暴涨 3 倍） |
| **5. AIGC 复杂插画特效 (`Tarot`)** | 精细静态底图 + 局部星云/粒子 + AI 亚像素底噪 | **`EAF Quad (0x08)`** 或 **`BTC + Planar (0x0A)`**<br>165 KB（去噪） / 1779 KB（HQ 无损重排 11.3 ms） | **`EAF H.264 (0x07)`**<br>84~277 KB | **`EFC + Planar (0x0A)`**<br>1800 KB，11.4 ms | AIGC 视频编码前建议开启连续软阈值稳像（或用 `BTC + Planar` 吸收底噪） |
| **6. 写实 3D 家电渲染（蒸汽/流体）** | 金属玻璃高光 + 半透明蒸汽/水流 | **`EAF BTC + Planar (0x0A)`**<br>萃取 127.6 KB (6.75 ms)、蒸汽 362.8 KB (7.43 ms) | **`EAF Quad-Abbrev (0x08)`** | **`EFC RGB565`**（滚筒特写等全屏快动场景保留原版） | `0x0A Planar` 在 PSRAM FB 下对咖啡萃取/蒸汽缩容 33%~68%，但全屏快转（滚筒特写）不要开 Planar |
| **7. 带透明通道的悬浮动效 (`foreground.apng`)** | 前景与背景解耦，叠加在任意底层 UI 上 | **8-bit 连续透明有损**：`0x0B BTC-Alpha` (`2,891.5 KiB`, `16.0 fps`)<br>**1-bit 抠图**：`0x06 BTC-Zlib` (`1,008.9 KiB`) | **固定背景预合成**：<br>`0x07 H.264` (`205.6 KiB`) / `0x08 Quad` (`444.9 KiB`) | **8-bit 连续透明 100% 无损**：<br>**`0x0C Lossless-RGBA` (`3,902.6 KiB`, `14.5 fps`，比 EFC `8,198.7 KiB` 小 52.4%)** | 需要与原始 `RGB565A8` 零误差一致时直接用 `0x0C Lossless-RGBA`，勿用有损 `0x0B` 跑慢速波浪 |

---

## 5. 工具链常用命令速查

### 5.1 PC 端视频/帧序列转换 (`video_to_eaf_opt.py` & `encode_rgb565_efc.cjs`)

```bash
# 1. BTC-Safe (0x09，推荐主力：有界误差 <= 8，极速解码)
python3 video_to_eaf_opt.py input.mp4 -o out_btc_hq.eaf --mode btc-safe --btc-max-error 8

# 2. BTC-Zlib (0x06，轻量极速版：体积更小，1~4ms 解码)
python3 video_to_eaf_opt.py input.mp4 -o out_btc_fast.eaf --mode btc-zlib

# 3. Quad-Abbrev (0x08，实拍视频/柔光渐变首选：4:4:4 高画质或 4:2:0 小体积)
python3 video_to_eaf_opt.py input.mp4 -o out_quad_hq.eaf --mode quad-abbrev --quality 84 --jpeg-subsampling 0
python3 video_to_eaf_opt.py input.mp4 -o out_quad_compact.eaf --mode quad-abbrev --quality 52 --jpeg-subsampling 2

# 4. EAF H.264 (0x07，极限最小体积)
python3 video_to_eaf_opt.py input.mp4 -o out_h264.eaf --mode h264 --h264-crf 18

# 5. EFC RGB565 无损直转 (零误差，不经 GIF 量化)
node tools/encode_rgb565_efc.cjs input.rgb565 out.efc 240 240 60

# 6. Lossless-RGBA (0x0C，100% 比特级无损 RGB565 + A8 连续半透明前景，比 EFC 小 52.4%)
python3 video_to_eaf_opt.py foreground.apng -o fg_lossless_rgba.eaf --mode lossless-rgba

# 7. BTC-Alpha (0x0B，轻量有损 8-bit 连续 Alpha DXT5 前景)
python3 video_to_eaf_opt.py foreground.apng -o fg_btc_alpha.eaf --mode btc-alpha
```

### 5.2 固件编译与多板烧录 (`ESP-BOX-3` & `ESP32-S3-LCD-EV-Board`)

当前 `box3_eaf_demo` 固件已支持**运行时自动识别硬件**（启动时通过 I2C 探测 `TCA9554 + FT5x06/GT1151`，自动在 `ESP32-S3-LCD-EV-Board 480×480/800×480 RGB` 与 `ESP-BOX-3 320×240 SPI` 之间自适应切换，右侧栏显示 **序号、压缩方式、压缩率、开机测得的纯解码能力、LVGL 刷新统计**）。最后一项包含侧栏重绘，不等同于动画帧率或接口扫描频率：

```bash
# 加载 ESP-IDF 5.5 环境
source /home/xuxin/esp_work/esp-idf/export.sh

# 编译标准 21 项五大场景对比固件
idf.py -C box3_eaf_demo -D EAF_APPLIANCE_BENCH=OFF -D EAF_TAROT_SPEED_BENCH=OFF -D EAF_LOSSLESS_BENCH=OFF build

# 烧录至 ESP32-S3-LCD-EV-Board (/dev/ttyUSB0) 或 ESP-BOX-3 (/dev/ttyACM0)
idf.py -C box3_eaf_demo -p /dev/ttyUSB0 flash monitor
```

### 5.3 USB0 的解码／显示帧率差异（2026-10-26，解除限速前）

本次串口采样确认 `/dev/ttyUSB0` 是 **ESP32-S3-LCD-EV-Board + GC9503／FT5x06，480×480 RGB**。运行标准 21 项固件，编译时间 `Oct 26 2026 10:11:15`，启动 ELF 摘要前缀 `b2512f528` 与本地构建一致。110 秒日志包含完整启动基准及第 1–15 项播放，未覆盖整轮 21 项。原始日志和结构化记录在 `validation/usb0-fps-analysis/`。

| USB0 素材 | 开机纯解码能力 | 解码均值 | 播放期间 LVGL FPS 样本 |
| --- | ---: | ---: | --- |
| Emoji BTC HQ | 381 fps | 2.62 ms | 稳定样本 16、16、17；另有启动 0 |
| Emoji EFC RGB565 | 228 fps | 4.37 ms | 16、17、16、9 |
| Anime H.264 | 33 fps | 30.26 ms | 16、16、16、16、16 |
| Video H.264 | 29 fps | 34.47 ms | 16、15、16、16、16 |

原因按证据强度区分：

1. **确定的首要限制：动画固定每 66 ms 推进一帧，约 15.15 fps。** `main.c` 对 EAF、EFC 均设置这一周期；纯解码能力是启动阶段连续解码所得，且在 RGB LCD 初始化前测量。381 fps 与约 15 fps 分别是能力与播放目标，不能用它们的比值定义显示效率。
2. **确定的统计混合：Display FPS 是 LVGL 每轮最后一次 flush 完成的次数。** 侧栏每 500 ms 重新设置标题和统计标签，也触发界面重绘，因此约 15 fps 的动画可出现 16–17 fps 的界面统计。RGB 重复扫描同一幅图的次数不在这个计数中。
3. **确定的显示滞后：切换素材不清空旧 FPS，且零值被忽略。** 本次第 4 项 H.264 切入时栏上是 9 fps，随后样本为 17、15、16、17、16。切入时的 9 来自此前统计窗口，不能作为该 H.264 模式的稳定显示能力。记录中的瞬时 9／11／13 等低值仍值得单独测量，现有日志不足以确定每次阻塞来源。
4. **RGB 路径的额外成本确实存在，但占比尚未分段实测。** RGB 默认 triple-partial 模式包含 LVGL 软件绘制、分块复制到 PSRAM 后缓冲、必要的缓冲修复，以及等待空闲缓冲。解码也在 LVGL timer 回调中同步执行，慢帧会延后其他界面任务。不能直接把剩余时间全算成 RGB 传输损耗。
5. **本板不适用 40 MHz SPI 的 43 fps 理论上限。** 配置为 RGB PCLK 16 MHz，包含 porch 的周期计算约 `16,000,000 / (520 × 510) = 60.33 Hz`；这是配置推算，未测 VSYNC。480×480 RGB565 在该扫描率下的有效像素读取约 27.8 MB/s，另有 bounce／绘制／复制开销。扫描约 60 Hz、动画约 15 fps 可以同时成立。

后续应分别记录：纯解码能力、目标动画 FPS、实际推进／呈现的源帧数、LVGL 刷新 FPS、面板 VSYNC 频率。用同一时间窗口的源帧计数评估播放达成率。再增加解码、绘制、复制和等待的分段耗时，以及独立的不限速显示测试；正常 15 fps 素材直接加速推进会改变动效时长。

### 5.4 全模式尽力播放（2026-10-26）

按用户要求，Demo 全部模式改为 **best effort**：EAF 与 EFC 均使用 `frame_delay=0`，显示刷新 timer 也设为 0。EFC 的零延迟不再映射到 `LV_DEF_REFR_PERIOD=15`；EAF 默认帧间延迟改为 0，并支持加载前配置。正数 API 参数仍可显式启用定时播放。

解码 timer 和显示刷新随 LVGL worker 调度推进，保留操作系统调度及显示缓冲回压；没有额外添加帧率上限、sleep 或忙等。加载完成后先立即绘制已解码的首帧，再由后续调度推进帧序列。原先素材的播放时长会随设备吞吐量缩短。

侧栏改名为 **Decode cap／UI FPS**；切换素材清空旧 FPS，接受零值，只在统计变化时更新侧栏。UI FPS 仍是 LVGL 刷新完成统计，不能当作严格的独立源帧呈现计数或面板 VSYNC。

构建与解码器 17 项 ASan／UBSan 回归日志保存于 `validation/usb0-best-effort/`。此模式对标准 21 项以及 Tarot／家电测试配置统一生效。

已烧录并在 **USB0／MAC `90:e5:b1:d0:a9:d8`／480×480 RGB** 上完成标准 21 项完整轮播，回到第 1 项；无解码失败、断言或崩溃。新 ELF 摘要前缀为 `f6e6d195e`。启动基准的 **126 个采样帧 RGB565 摘要与修改前全部一致**。

| 模式 | 修改前 LVGL 刷新统计 fps | 尽力播放 LVGL 刷新统计 fps |
| --- | ---: | ---: |
| Emoji BTC | 16 | 57 |
| Emoji Quad | 15.5 | 50.5 |
| Emoji EFC | 16 | 41 |
| Emoji H.264 | 16.5 | 19.5 |
| Anime BTC | 16.5 | 54 |
| Video H.264 | 16 | 16.5 |

表格是每项稳定窗口内 LVGL 刷新统计的中位数；排除切入后前 1.5 秒及下一次切换前 300 ms，采样仍是整数 FPS，半数值来自中位数计算。它不是独立源帧呈现计数。快速模式已明显超过原先约 15 fps 的节拍，慢速 H.264 仍主要受完整解码／绘制路径限制；尽力播放不保证所有模式接近 RGB 扫描频率。

**统计口径补充：表中的 `Emoji BTC 16 → 57` 不是实测接口／面板上屏帧率。** 修改前选中的三个日志读数是 `16、16、17`，修改后是 `51、58、57`，分别取中位数得到 16 与 57。这是短时窗口统计，不是长时间平均。适配器在 `lv_display_flush_is_last()` 为真、调用 `display_manager_flush_ready()` 时累计一次，再用 `count × 1,000,000 / elapsed_us` 计算整数速率；包含侧栏等界面更新。该计数点不验证不同源帧是否已被面板扫描。

本次没有独立测量 VSYNC、RGB framebuffer 切换次数和带源帧编号的实际呈现次数。`16 MHz / (520 × 510) ≈ 60.33 Hz` 只是当前 RGB 时序配置的推算，因此 57 在配置上不越界，但不能据此把它确认为实际显示 57 fps。要与接口能力比较，必须补齐这些硬件／源帧计数，不能用该表直接认定上屏吞吐量。

全部 21 项数据、原始日志及前后对照 CSV 位于 `validation/usb0-best-effort/`，可运行 `python3 tools/analyze_best_effort_board.py` 复核。修改前日志仅覆盖第 1–15 项播放，第 16–21 项不虚构旧实测值。源码仍保留既有的 OS 调度让出时间和显示缓冲等待，这些不属于素材播放节拍。

---

## 6. 全栈压缩技术收敛与淘汰指南（“6 收 8 汰”标准产品线）

在完成了五大场景 21 项 HQ、写实 3D 家电 16 项、以及 `foreground.apng (480×480 RGB565A8)` 双板 1:1 实测后，当前 `0x00 ~ 0x0C` 与 `EFC1` 共 14 种编码模式中，有一部分属于演进过程中的历史中间态，或被新模式在**「体积、解码速度、画质」三个维度同时严格支配（Pareto Dominated）**。

为降低工程维护成本与用户选型心智负担，建议将全栈算法收敛为 **6 个正交主力模式**，并淘汰/降级其余 **8 个过渡模式**：

### 6.1 正式收敛保留的 6 大核心主力模式（生产级标准库）

1. **`0x0C Lossless-RGBA` —— 【带 8-bit 连续 Alpha · 100% 无损主力】**
   - **适用场景**：前景与背景解耦、带半透明渐变/波浪/羽化阴影的动态图层（如 `foreground.apng`）。
   - **收敛理由**：150 帧硬件 `CRC32` 与原始 `Diehl EFC` 100% 逐字节一致，但体积缩小 **52.4%**（`3,902.6 KB` vs `8,198.7 KB`，省 `4.3 MB` Flash），纯解码快 **38.3%**（`40.2 fps` vs `29.1 fps`）。直接终结了 EFC 拼 `480×720` 的做法。
2. **`0x09 BTC-Safe` —— 【不带 Alpha / 1-bit Color-Key · 空域极速主力】**
   - **适用场景**：机器人表情（`Emoji`）、二次元赛璐璐动漫（`Anime`）、锐利线条 UI、以及内部预混后的 1-bit 透明抠图前景。
   - **收敛理由**：全场解码最快（`1~6 ms`，高达 `381 fps`）、零常驻内存、无 JPEG 描边蚊式噪点；通过 `mode=4` 32B 原始逃逸块将最大误差死锁在 `maxErr <= 8`，并且完全向下涵盖 `0x06 BTC-Zlib`（只需调大 `--btc-max-error` 阈值即可）。
3. **`0x08 Quad-Abbrev` —— 【不带 Alpha · 频域自然画质主力】**
   - **适用场景**：相机自然实拍视频（`Video`）、柔光径向渐变/散景光球（`Poke`）、固定背景下的水波/光晕。
   - **收敛理由**：利用两级四叉树（`16×16` + `8×8` 象限）+ 剥离表头 Abbreviated-JPEG，在自然连续渐变场景下体积比 `BTC` 小 50%、比 `EFC` 小 73%，且支持 ESP32-P4 等芯片的硬件 JPEG 加速。
4. **`0x0A Planar-Delta` —— 【不带 Alpha · 3D/AIGC 位平面无损重排器】**
   - **适用场景**：PSRAM Framebuffer 下的 3D 家电局部流体/蒸汽特效（如咖啡萃取、蒸汽）、AIGC 插画局部动效（`Tarot`）。
   - **收敛理由**：在不损失任何 1 比特画质的前提下，将 `RGB565` 拆分为高低字节平面并做帧间 XOR，使咖啡萃取缩小 **68%** 且解码提速 **25.5%**，使 `Tarot` 缩小 **42.5%** 且解码提速 **29.5% ~ 43.3%**。
5. **`0x07 EAF H.264` —— 【不带 Alpha · 极限超小体积视频流】**
   - **适用场景**：Flash 极度受限（如 4MB/8MB Flash 需放多段长动画）、全屏运镜宣传片、背景固定的 `foreground.apng`（150 帧仅 `205.6 KB`，压缩比 `328x`）。
   - **收敛理由**：跨帧亚像素运动矢量补偿使其在所有场景下体积比块编码小 **3 ~ 10 倍**，在 CPU 算力允许（`24~35 ms/帧`）时是极限省 Flash 的唯一解。
6. **`EFC1 (1-Byte Palette 模式)` —— 【全片 <= 256 色纯平涂图标专用】**
   - **适用场景**：全动画序列唯一颜色数 $\le 256$ 色的纯几何平涂 UI、带原生 1-bit Color-Key (`Opcode 19`) 的少色动效。
   - **收敛理由**：当颜色数控制在 256 色以内时，1 字节 Token + 邻居块拷贝指令（`Opcode 1/2/18`）依然具备极佳的紧凑度与 SIMD 块拷贝速度。

---

### 6.2 老版本兼容保留与其余 6 个冗余/过渡模式的处置方案

1. **【老版本兼容保留】`0x00 RLE` 与 `0x01 / 0x03 Huffman`**：
   - 在解码器（`esp_eaf_dec.c`）中**完整保留**，确保历史已发布的 EAF `v1.0 / v2.0` 调色板动画资源 100% 正常播放；
   - **新制作资产不再选用**：因缺少帧间差分且渐变动效体积膨胀严重，新项目带 8-bit Alpha 一律改用 **`0x0C Lossless-RGBA`**，少色图标一律改用 **`0x09 BTC-Safe`** 或 **`EFC 1B`**。
2. **【直接淘汰】`0x04 Tile-JPEG` 与 `0x05 Tile-Abbrev`**：
   - 属于 `16×16` 宏块图集的早期过渡方案，缺乏 `8×8` 四叉树象限筛选，同画质下已被 **`0x08 Quad-Abbrev`** 100% 上位替代（体积小 15%~30%）。
3. **【合并隐藏】`0x06 BTC-Zlib` $\rightarrow$ 统一归入 `0x09 BTC-Safe`**：
   - `0x06` 只是没有 `mode=4` 原始逃逸块的早期 BTC。工具链默认统一输出 **`0x09 BTC-Safe`**（想要极小体积时只需调大 `--btc-max-error` 即可等价于 `0x06`）。
4. **【降为内部子帧】`0x02 Standard JPEG`**：
   - 不再对用户暴露为独立模式，仅作为 **`0x08 Quad-Abbrev`** 的第 0 帧（传递共享 `DQT/DHT` 表头）或全屏剧变帧的内部自动回退分支。
5. **【降级备选】`0x0B BTC-Alpha (DXT5)`**：
   - 在慢速半透明流体波浪上，4 级 Alpha 插值与跳块阈值会产生 `4×4` 阶梯波浪块，且体积（`2.82 MB`）相比 100% 无损的 **`0x0C`**（`3.81 MB`）仅省 25%，推荐直接上 **`0x0C Lossless-RGBA`**。
6. **【严禁误用】`EFC 2-Byte / 480×720 A8 拼接`**：
   - 一旦画面带渐变/实拍噪点（块内 $>8$ 色）或把两个 `A8` 硬拼成 `uint16_t`，EFC 微调色板立即失效并退化为 2 字节原样存储（`foreground.efc` 膨胀至 `8.01 MB`，解码慢 38%）。带 Alpha 请统一使用 **`0x0C Lossless-RGBA`**。

