# eaf_codec

统一的 **ESP EAF 动画与图像压缩算法套件**。本仓库将 16-bit (`RGB565 / RGB565A8`) 与 32-bit (`BGRA8888 / RGB888`) 两大位深体系收敛为**一套连续无空洞的编码表 (`0x00 ~ 0x07`)**、**一个 C 解码组件 (`components/esp_lv_eaf_player`)** 与 **一个命令行入口 (`tools/eaf_cli.py`)**。

---

## 1. 统一紧凑编码表（`0x00 ~ 0x07`，零空缺）

| 编码 ID | 模式名称 | 定位 | 565 支持 (`RGB565 / 565A8`) | 888/8888 支持 (`RGB888 / BGRA8888`) | Alpha 通道 | 硬件加速支持 |
| :---: | :--- | :--- | :---: | :---: | :---: | :--- |
| `0x00` | **`PALETTE_RLE`** | 旧版兼容 | 支持 | **支持**（调色板原生 BGRA8888 直出） | 8-bit 调色板 | 调色板 Cache |
| `0x01` | **`PALETTE_HUFF_RLE`** | 旧版兼容 | 支持 | **支持**（调色板原生 BGRA8888 直出） | 8-bit 调色板 | 调色板 Cache |
| `0x02` | **`JPEG_KEYFRAME`** | 内部关键帧 | 支持 | **支持** (`RGB888 / BGRA8888`) | **不支持** | **S31 / P4 硬件 JPEG 2D-DMA** (`hb` 对齐) |
| `0x03` | **`PALETTE_HUFF`** | 旧版兼容 | 支持 | **支持**（调色板原生 BGRA8888 直出） | 8-bit 调色板 | 调色板 Cache |
| **`0x04`** | **`LOSSLESS`** | **主力 1：100% 无损** | **原生 `565+A8` 三平面/联合符号** | **原生 `BGRA8888` 自适应无损**（含 32-bit 字 XOR 与 81 态符号） | **完整 8-bit 无损** | ROM `tinfl` / zlib + PPA |
| **`0x05`** | **`BTC_SAFE`** | **主力 2：空域有界误差** | **原生 `RGB565` (32B 逃逸)** | **原生 `BGRA8888` (64B 逃逸)** | **8888 支持 8-bit**<br>565 支持 1-bit Key | 极速位运算 ($\text{maxErr}\le 8$) |
| **`0x06`** | **`QUAD_JPEG`** | **主力 3：变化块抽 JPEG** | **原生 `RGB565`** | **原生 `RGB888 / BGRA8888`** | **不支持**（仅不透明背景/视频） | **S31 / P4 硬件 JPEG 2D-DMA**（编码端自动对齐 `hb=32/48`） |
| **`0x07`** | **`H264`** | **主力 4：极限视频流** | **原生 `I420 -> RGB565`** | **原生 `I420 -> BGRA8888`** | **不支持**（仅不透明视频） | 双核 `tinyh264` + 免分支 LUT |

---

## 2. 目录结构

```text
eaf_codec/
├── components/
│   └── esp_lv_eaf_player/          # 统一 ESP-IDF / Host C 解码器与 LVGL 控件组件
│       ├── include/
│       │   ├── esp_eaf_codec.h     # 0x00..0x07 统一编码枚举与双文件头探测 esp_eaf_container_probe()
│       │   ├── esp_eaf_dec.h       # EAF 0x00..0x07 解码接口（支持设置 RGB565 / BGRA8888 输出）
│       │   ├── eaf4.h              # EAF4 (BGRA8888) 流式/零拷贝解码接口
│       │   └── lv_eaf.h            # LVGL v8/v9 统一播放控件（自动识别 .eaf 与 .eaf4）
│       └── src/
│           ├── esp_eaf_dec.c       # 0x00..0x07 解码实现（含 S31/P4 硬件 JPEG hb 对齐检查与 565/8888 双输出）
│           ├── eaf4.c              # EAF4 BGRA8888 解码实现
│           └── lv_eaf.c            # 双容器自适应 LVGL 控件实现
├── encoder/
│   ├── eaf4.py                     # 0x04 LOSSLESS & 0x05 BTC_SAFE (BGRA8888) 编码器与参考解码器
│   ├── btc8888.py                  # 0x05 BTC_SAFE (BGRA8888) 4x4 有界误差色胞编码器
│   ├── quad_jpeg.py                # 0x06 QUAD_JPEG 四叉树变化块条带 JPEG 编码器（含硬件 DMA 宽度对齐）
│   ├── eaf565.py                   # 0x04..0x07 (RGB565 / RGB565A8) 编码器
│   └── eaf565_lossless_rgba.py     # 0x04 LOSSLESS (RGB565+A8 联合符号无损) 批量打包器
├── tools/
│   ├── eaf_cli.py                  # 统一命令行工具：--format {8888,565} --codec {lossless,btc-safe,quad-jpeg,h264}
│   ├── eaf_inspect.py              # EAF 帧/块结构检查器
│   └── eaf4_verify.c               # 主机端 C 解码 CRC 校验与微秒级计时工具
├── bench/
│   ├── compare_modes.py            # 单序列各模式压缩率对比
│   └── run_matrix.py               # 测试动图全矩阵评测（体积 / 压缩比 / C 解码耗时 / PSNR / MaxErr）
├── assets/
│   ├── xiaobai/                    # 测试动画 PNG 帧序列
│   └── eaf_samples/                # 0x00..0x03 旧版兼容 .eaf 样本
├── tests/
│   └── test_roundtrip.py           # 0x00..0x07 全模式 + 双格式 CLI 自动化回归测试
└── docs/
    ├── COMPRESSION_SUMMARY.md      # 0x00..0x07 统一编码表、565/8888 支持声明与选型指南
    ├── FORMAT.md                   # EAF4 二进制格式规范
    └── EAF_COMPRESSION_MODES_GUIDE.md # 各模式底层算法原理详解
```

---

## 3. 快速使用

```bash
# 1. 编译 C 校验器并运行 0x00..0x07 全模式回归测试
make test

# 2. 运行测试动图全矩阵评测
make bench

# 3. 统一 CLI 编码（4 大主力模式：lossless / btc-safe / quad-jpeg / h264）
python3 tools/eaf_cli.py encode -o out_lossless_8888.eaf4 --format 8888 --codec lossless assets/xiaobai/happy_loop/*.png
python3 tools/eaf_cli.py encode -o out_btc_8888.eaf4      --format 8888 --codec btc-safe assets/xiaobai/ui_demo/*.png
python3 tools/eaf_cli.py encode -o out_lossless_565.eaf   --format 565  --codec lossless assets/xiaobai/happy_loop/*.png
python3 tools/eaf_cli.py encode -o out_btc_565.eaf        --format 565  --codec btc-safe assets/xiaobai/happy_loop/*.png
python3 tools/eaf_cli.py encode -o out_quad_jpeg.eaf      --format 565  --codec quad-jpeg assets/xiaobai/ui_demo/*.png

# 4. 自动识别文件头并查看信息
python3 tools/eaf_cli.py inspect out_lossless_8888.eaf4
python3 tools/eaf_cli.py inspect out_quad_jpeg.eaf
```
