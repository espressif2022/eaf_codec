# eaf_codec

统一的 **ESP EAF 动画与图像压缩算法套件**。本仓库将 RGB565、
RGB565+A8 与 BGRA8888 收敛为**一套连续无空洞的编码表
(`0x00 ~ 0x07`)**、**一个 C 解码组件 (`components/esp_lv_eaf_player`)**
与**一个命令行入口 (`tools/eaf_cli.py`)**。

---

## 1. 统一紧凑编码表（`0x00 ~ 0x07`，零空缺）

| 编码 ID | 模式名称 | 定位 | 565 支持 (`RGB565 / 565A8`) | 888/8888 支持 (`RGB888 / BGRA8888`) | Alpha 通道 | 硬件加速支持 |
| :---: | :--- | :--- | :---: | :---: | :---: | :--- |
| `0x00` | **`PALETTE_RLE`** | 旧版兼容 | 支持 | **8-bit 支持**（调色板原生 BGRA8888 直出） | 8-bit 调色板 | 调色板 Cache |
| `0x01` | **`PALETTE_HUFF_RLE`** | 旧版兼容 | 支持 | **8-bit 支持**（调色板原生 BGRA8888 直出） | 8-bit 调色板 | 调色板 Cache |
| `0x02` | **`JPEG_KEYFRAME`** | 内部关键帧 | 支持 | 不支持（当前后端原生输出 RGB565） | **不支持** | **S31 / P4 硬件 JPEG 2D-DMA** (`hb` 对齐) |
| `0x03` | **`PALETTE_HUFF`** | 旧版兼容 | 支持 | **8-bit 支持**（调色板原生 BGRA8888 直出） | 8-bit 调色板 | 调色板 Cache |
| **`0x04`** | **EAF Lossless** (`LOSSLESS`) | **100% 无损** | **原生 `565+A8` 三平面/联合符号** | **原生 `BGRA8888` 自适应无损** | **完整 8-bit 无损** | ROM `tinfl` / zlib + PPA |
| **`0x05`** | **EAF Bounded** (`BTC_SAFE`) | **空域有界误差** | **原生 `RGB565` (32B 逃逸)** | **原生 `BGRA8888` (64B 逃逸)** | **8888 支持 8-bit**<br>565 支持 1-bit Key | 极速位运算 ($\text{maxErr}\le 8$) |
| **`0x06`** | **EAF JPEG Delta** (`QUAD_JPEG`) | **变化区域 JPEG** | **原生 `RGB565`** | 不支持（不做 RGB565→8888 扩色） | **不支持** | **S31 / P4 硬件 JPEG 2D-DMA** |
| **`0x07`** | **EAF Video** (`H264`) | **视频流/极限体积** | **原生 `I420 -> RGB565`** | **原生 `I420 -> BGRA8888`** | **不支持**（BGRA 输出固定 `A=255`） | 双核 `tinyh264` + LUT |

> 公共名称描述能力；括号内是协议/实现枚举，保持二进制兼容。色深、
> Straight Alpha、RGB565A8、Color-Key 以及透明度边界，
> 统一说明见[《色深、输出格式与透明度》](docs/EAF_COMPRESSION_MODES_GUIDE.md#3-色深输出格式与透明度)。

---

## 2. 目录结构

```text
eaf_codec/
├── components/
│   └── esp_lv_eaf_player/          # 统一 ESP-IDF / Host C 解码器与 LVGL 控件组件
│       ├── include/
│       │   ├── esp_eaf_codec.h     # 0x00..0x07 统一编码枚举与双文件头探测 esp_eaf_container_probe()
│       │   ├── eaf4.h              # EAF4 (BGRA8888) 流式/零拷贝解码接口
│       │   ├── esp_eaf_player.h    # GUI 无关统一播放接口
│       │   └── lv_eaf.h            # LVGL v8/v9 统一播放控件（自动识别 .eaf 与 .eaf4）
│       └── src/
│           ├── core/
│           │   ├── esp_eaf_player.c      # GUI 无关播放状态与容器选择
│           │   └── esp_eaf_decoder.c     # 每流状态、Scratch 与脏区管理
│           ├── container/
│           │   ├── eaf/                  # EAF/AAF 头、帧表、Block 分发
│           │   └── eaf4/                 # EAF4 头、索引、CRC、Seek 与 zlib 外层
│           ├── codec/
│           │   ├── esp_eaf_codec_registry.c # 双容器统一 codec 回调分发
│           │   ├── esp_eaf_codec_bounded.c  # EAF/EAF4 共用 Bounded 核心
│           │   ├── common/               # JPEG/Huffman/RLE/YUV 公共基础设施
│           │   ├── eaf/                  # EAF 专属载荷实现
│           │   └── eaf4/                 # EAF4 专属载荷实现
│           └── adapter/lvgl/
│               └── lv_eaf.c              # 可选 LVGL v8/v9 绑定
├── encoder/
│   ├── eaf4.py                     # 0x04 LOSSLESS & 0x05 BTC_SAFE (BGRA8888) 编码器与参考解码器
│   ├── btc8888.py                  # 0x05 BTC_SAFE (BGRA8888) 4x4 有界误差色胞编码器
│   ├── quad_jpeg.py                # 0x06 QUAD_JPEG 四叉树变化块条带 JPEG 编码器（含硬件 DMA 宽度对齐）
│   ├── eaf565.py                   # 0x04..0x07 (RGB565 / RGB565A8) 编码器
│   └── eaf565_lossless_rgba.py     # 0x04 LOSSLESS (RGB565+A8 联合符号无损) 批量打包器
├── tools/
│   ├── eaf_cli.py                  # 统一 CLI：--codec {lossless,bounded,jpeg-delta,video}
│   ├── eaf_inspect.py              # EAF 帧/块结构检查器
│   └── eaf4_verify.c               # 主机端 C 解码 CRC 校验与微秒级计时工具
├── bench/
│   ├── compare_modes.py            # 单序列各模式压缩率对比
│   ├── generate_synthetic_assets.py # 无版权依赖、可复现的多场景 RGBA 测试素材生成器
│   ├── generate_visual_assets.py    # 从固定母版派生透明角色和照片级风景动画
│   ├── quick_host_bench.py          # 快速 Host 验收：压缩率/编解码 FPS/画质/C 解码器
│   └── run_matrix.py               # 指定素材目录的全矩阵评测（体积 / C 解码耗时 / PSNR / MaxErr）
├── assets/
│   ├── synthetic/                  # 程序生成的公开 benchmark：透明、暗部、纹理、平移、细线等
│   ├── generated_sources/          # 生成式角色与照片级风景母版（固定 SHA-256）
│   ├── visual/                     # 从母版确定性派生的角色/风景动画序列
│   └── eaf_samples/                # 0x00..0x03 旧版兼容 .eaf 样本
├── tests/
│   └── test_roundtrip.py           # EAF4 Mode 0..6、0x04..0x06 CLI 与旧格式自动化回归
└── docs/
    ├── README.md                   # 文档索引与公共命名说明
    ├── TESTING.md                  # Host/设备测试入口与验收规则
    ├── DEVICE_TESTING.md           # LVGL 测试与 ESP32-S3 480×480 Benchmark 流程
    ├── BENCHMARK_REPORT.md         # 可复现的公开设备报告索引
    ├── ESP32S3_480X480_BENCHMARK.md # S3 SUB2 真机压缩/解码/内存基线
    ├── HOST_QUICK_BENCHMARK.md     # 可公开复现的 Host 快速验收方法与参考基线
    ├── COMPRESSION_SUMMARY.md      # 0x00..0x07 统一编码表、565/8888 支持声明与选型指南
    ├── FORMAT.md                   # EAF4 二进制格式规范
    └── EAF_COMPRESSION_MODES_GUIDE.md # 各模式底层算法原理详解
```

---

## 3. 快速使用

```bash
# 1. Host 测试与 Benchmark
make host-test                    # 完整 Python/C/CLI/兼容性回归
make host-quick                   # 数秒级公开素材验收，带 PASS/FAIL
make host-bench                   # 完整 EAF4 子模式矩阵

# 可选：重新生成无版权依赖的公开测试素材，再显式运行矩阵
make assets

# 2. 统一 CLI 编码（lossless / bounded / jpeg-delta / video）
python3 tools/eaf_cli.py encode -o out_lossless_8888.eaf4 --format 8888 --codec lossless assets/synthetic/micro_motion/*.png
python3 tools/eaf_cli.py encode -o out_bounded_8888.eaf4  --format 8888 --codec bounded assets/synthetic/flat_ui/*.png
python3 tools/eaf_cli.py encode -o out_lossless_565.eaf   --format 565  --codec lossless assets/synthetic/alpha_glow/*.png
python3 tools/eaf_cli.py encode -o out_bounded_565.eaf     --format 565  --codec bounded assets/synthetic/flat_ui/*.png
python3 tools/eaf_cli.py encode -o out_jpeg_delta.eaf     --format 565  --codec jpeg-delta assets/synthetic/natural_motion/*.png

# 3. 自动识别文件头并查看信息
python3 tools/eaf_cli.py inspect out_lossless_8888.eaf4
python3 tools/eaf_cli.py inspect out_jpeg_delta.eaf

# 完整测试、设备测试与 Benchmark 说明
# 见 docs/TESTING.md 与 docs/DEVICE_TESTING.md
```
