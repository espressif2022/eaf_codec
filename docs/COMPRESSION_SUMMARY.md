# EAF 压缩算法收敛与选型汇总（RGB565 `0x89 EAF` & BGRA8888 `EAF4`）

本仓库将 **RGB565 / RGB565A8 (`0x89 EAF`)** 与 **全四通道 8-bit BGRA8888 (`EAF4`)** 两套同源压缩算法收敛在同一个组件 `components/esp_lv_eaf_player` 和同一个命令行工具 `tools/eaf_cli.py` 中，通过文件头前 4 字节（`0x89 "EAF"` vs `"EAF4"`）自动识别并分发解码。

---

## 一、同源算法架构与演进映射

`EAF4 (BGRA8888)` 的核心模式直接从 `0x89 EAF (RGB565)` 的主力算法升维演进而来：

1. **通道平面分离帧间差分（`0x0A Planar-Delta` $\rightarrow$ `EAF4 Mode 2 Planar-8888`）**
   - **RGB565 (`0x0A`)**：将 16-bit 像素拆分为 `Low Byte` 与 `High Byte` 两个连续平面并做帧间 XOR。
   - **BGRA8888 (`Mode 2`)**：将 32-bit 像素拆分为 `B, G, R, A` 四个独立 8-bit 平面并做帧间 XOR。
2. **4×4 变化块掩码 + 无损差分（`0x0C Lossless-RGBA` $\rightarrow$ `EAF4 Modes 3 / 4 / 5`）**
   - **RGB565A8 (`0x0C`)**：使用小端 4×4 变化块位图跳过静止区域，变化块支持三平面分离 (`Lo / Hi / A8`) 或 81 态三进制联合符号 (`Joint-Inter`)。
   - **BGRA8888 (`Mode 3 Tile-Planar`)**：4×4 变化块位图 + B/G/R/A 四平面分离 XOR。
   - **BGRA8888 (`Mode 4 Tile-Packed`)**：4×4 变化块位图 + **交错 32-bit BGRA 字直接 XOR**。免去四平面读取与移位拼字，复杂大面积动画还原速度提升 **30% ~ 42%**。
   - **BGRA8888 (`Mode 5 Joint-Delta`)**：4×4 变化块位图 + 四通道小差分 $(\Delta B,\Delta G,\Delta R,\Delta A)\in[-1,1]^4$ 编码为单字节 81 态符号 (`0..80`，`255` 为逃逸) + 四平面逃逸流。在局部渐变/呼吸动效上体积比 `Mode 3` 再省 **15% ~ 30%**。
3. **4×4 有界误差色胞截断（`0x09 BTC-Safe` $\rightarrow$ `EAF4 Mode 6 BTC8888-Safe`）**
   - **RGB565 (`0x09`)**：每块 1 字节模式（`0=Skip`, `1=Flat 2B`, `2=2色 6B`, `3=4色线性插值 8B`, `4=Raw 32B RGB565 逃逸块`）。
   - **BGRA8888 (`Mode 6`)**：完全同构扩展至 32-bit BGRA（`0=Skip 0B`, `1=Flat 4B`, `2=2色 10B`, `3=4色线性插值 12B`, `4=Raw 64B BGRA8888 逃逸块`）。严格锁定四通道单像素最大绝对误差 $\text{maxErr} \le 8$，消除色胞方块伪影，同时将复杂动画体积压缩至无损方案的 **22% ~ 35%**。

---

## 二、收敛后的生产选型标准

### 1. 当显示链路为 24/32-bit（需要全 8-bit Alpha 叠加或 PPA 硬件混合）$\rightarrow$ 选用 `EAF4` 容器
- **通用 100% 无损首选**：`--container eaf4 --profile v2`（逐帧在 `Modes 0..5` 中自适应择优，兼顾最小无损体积与局部块跳过）。
- **高负载双图层极速无损**：`--container eaf4 --profile tile_packed`（关键帧 `Mode 0` + 帧间 `Mode 4` 32-bit 整字异或，最大化 CPU 还原吞吐）。
- **Flash 容量受限的复杂全屏动画**：`--container eaf4 --profile btc8888 --btc-max-error 8`（`Mode 6 BTC8888-Safe`，34x~72x 压缩比，PSNR `45~87 dB`）。

### 2. 当显示链路为 16-bit `RGB565` $\rightarrow$ 选用 `0x89 EAF` 容器
- **带 8-bit 连续 Alpha 前景无损**：`--container eaf565 --mode565 lossless-rgba` (`0x0C`)。
- **不透明 / 1-bit 透明卡通与 UI 动效**：`--container eaf565 --mode565 btc-safe` (`0x09`)。
- **自然实拍或柔光渐变**：`--container eaf565 --mode565 quad-abbrev` (`0x08`) 或 `h264` (`0x07`)。
