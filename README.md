# eaf_codec

统一的 **ESP EAF 动画与图像压缩算法套件**。本仓库将 16-bit (`RGB565 / RGB565A8`) 与 32-bit (`BGRA8888`) 两大位深体系合并进同一套 C 组件 (`components/esp_lv_eaf_player`) 与同一套 Python 编码工具链 (`encoder/` + `tools/eaf_cli.py`)，通过文件头自动识别容器格式：

- **`0x89 "EAF"` 容器（RGB565 / RGB565A8，Modes `0x00 ~ 0x0C`）**：面向 16-bit 显示链路。
- **`"EAF4"` 容器（BGRA8888 直通 Alpha，Modes `0 ~ 6`）**：由 `0x0A / 0x0C / 0x09` 升维扩展至全四通道 8-bit，面向 24/32-bit 显示与 PPA 硬件 Alpha 混合链路。

## 1. 目录结构

```text
eaf_codec/
├── components/
│   └── esp_lv_eaf_player/          # 统一 ESP-IDF / Host C 解码器与 LVGL 控件组件
│       ├── include/
│       │   ├── esp_eaf_codec.h     # 双文件头自动探测接口 esp_eaf_container_probe()
│       │   ├── esp_eaf_dec.h       # 0x89 EAF (RGB565/565A8 0x00..0x0C) 解码接口
│       │   ├── eaf4.h              # EAF4 (BGRA8888 Modes 0..6) 流式/零拷贝解码接口
│       │   └── lv_eaf.h            # LVGL v8/v9 统一播放控件（自动识别 .eaf 与 .eaf4）
│       └── src/
│           ├── esp_eaf_dec.c       # 565 多模态解码器
│           ├── eaf4.c              # 8888 多模态解码器 (Modes 0..6)
│           └── lv_eaf.c            # 双容器自适应 LVGL 控件实现
├── encoder/
│   ├── eaf4.py                     # EAF4 (BGRA8888 Modes 0..6) 编码器与参考解码器
│   ├── btc8888.py                  # Mode 6 (BTC-8888 Safe) 4x4 有界误差色胞编码器
│   ├── eaf565.py                   # 0x89 EAF (RGB565 0x07/0x08/0x09/0x0B/0x0C) 编码器
│   └── eaf565_lossless_rgba.py     # 0x89 EAF 0x0C (RGB565+A8 联合符号无损) 批量打包器
├── tools/
│   ├── eaf_cli.py                  # 统一命令行工具：支持 eaf4 / eaf565 编码与自动文件头检查
│   ├── eaf_inspect.py              # 0x89 EAF 帧/块结构检查器
│   └── eaf4_verify.c               # 主机端 C 解码 CRC 校验与微秒级计时工具
├── bench/
│   ├── compare_modes.py            # 单序列 Modes 0..6 压缩率对比
│   └── run_matrix.py               # 测试资产全矩阵评测（体积 / 压缩比 / C 解码耗时 / PSNR / MaxErr）
├── assets/
│   ├── xiaobai/                    # 测试动画 PNG 帧序列
│   └── eaf_samples/                # 预编译 .eaf 样本文件
├── tests/
│   └── test_roundtrip.py           # Modes 0..6 + 双容器 CLI 自动化回归测试
└── docs/
    ├── FORMAT.md                   # EAF4 (BGRA8888 Modes 0..6) 二进制规范
    ├── EAF_COMPRESSION_MODES_GUIDE.md # 0x89 EAF (RGB565 0x00..0x0C) 详细技术指南
    └── COMPRESSION_SUMMARY.md      # 565 与 8888 同源算法映射及选型汇总
```

## 2. 同源算法映射（RGB565 $\leftrightarrow$ BGRA8888）

| 算法类别 | `0x89 "EAF"` (RGB565 / RGB565A8) | `"EAF4"` (BGRA8888 全 8-bit) | 保真度与核心特点 |
| :--- | :--- | :--- | :--- |
| **整帧关键帧 / 帧间 XOR** | — | `Mode 0 (Raw)` / `Mode 1 (Word XOR)` | 100% 无损，周期插入 `Mode 0` 支持快速 Seek |
| **通道平面分离差分** | `0x0A Planar-Delta` (Lo/Hi 2 平面) | `Mode 2 Planar-8888` (B/G/R/A 4 平面) | 100% 无损，提升同质通道 LZ77 匹配率 |
| **4×4 变化块位图 + 平面差分** | `0x0C Lossless-RGBA` (Lo/Hi/A8 3 平面) | `Mode 3 Tile-Planar` (4×4 掩码 + 4 平面) | 100% 无损，跳过静止背景块 |
| **4×4 变化块位图 + 交错字 XOR** | — | `Mode 4 Tile-Packed` (4×4 掩码 + 32-bit 字) | 100% 无损，免去四平面移位拼字，复杂动画还原提速 30%~42% |
| **4×4 变化块 + 三进制联合符号** | `0x0C Joint-Inter` ($[-1,1]^4 \rightarrow 0..80$) | `Mode 5 Joint-Delta` ($[-1,1]^4$ + 四平面逃逸) | 100% 无损，局部微动/渐变动画体积最小 |
| **4×4 有界误差色胞截断 (BTC)** | `0x09 BTC-Safe` (Skip/Flat/2色/4色 + 32B 逃逸) | `Mode 6 BTC8888-Safe` (Skip/Flat/2色/4色 + 64B 逃逸) | 有界误差 ($\text{maxErr} \le 8$)，极速解码，体积比无损小 60%~78% |
| **频域四叉树 / 视频流** | `0x08 Quad-Abbrev` / `0x07 H.264` | —（无透明背景视频直接使用 565 容器） | 面向自然实拍视频与极限小体积场景 |

## 3. 快速使用

```bash
# 1. 编译主机 C 校验器并运行双容器 + Modes 0..6 回归测试
make test

# 2. 运行测试动图全模式基准测试
make bench

# 3. 使用统一 CLI 编码 EAF4 (BGRA8888) 或 EAF (RGB565)
python3 tools/eaf_cli.py encode -o out_8888.eaf4 --container eaf4 --profile v2 assets/xiaobai/happy_loop/*.png
python3 tools/eaf_cli.py encode -o out_btc8888.eaf4 --container eaf4 --profile btc8888 --btc-max-error 8 assets/xiaobai/ui_demo/*.png
python3 tools/eaf_cli.py encode -o out_565.eaf --container eaf565 --mode565 btc-safe assets/xiaobai/happy_loop/*.png

# 4. 自动识别文件头并查看容器信息（支持 .eaf 与 .eaf4）
python3 tools/eaf_cli.py inspect out_8888.eaf4
python3 tools/eaf_cli.py inspect out_565.eaf
```
