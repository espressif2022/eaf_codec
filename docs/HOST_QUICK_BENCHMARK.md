# Host 快速验收基线

这是一组开发机 smoke benchmark，不代表 ESP32 板端性能。它用于快速确认：

- `0x04 EAF Lossless v2` 逐字节往返一致；
- `0x05 EAF Bounded` 的 RGB/Alpha 最大误差均不超过 8；
- EAF4 容器、CRC 与 Host C 解码器可以完整解码；
- 不同内容类型的压缩率没有出现明显回退。

## 运行方法

```bash
python3 -m pip install -r requirements.txt
python3 bench/generate_synthetic_assets.py
make host-quick
```

默认从 9 个程序化场景和 3 个生成视觉场景中各均匀抽取 6 帧，运行两种主力
BGRA8888 codec，结果写入
`build/host_quick_report.json`。任一无损往返、有界误差、有效载荷或 C 解码检查
失败，命令都会返回非零状态。

## 参考结果

测试日期：2026-11-08。环境：WSL2 Linux x86-64，Intel Core i5-13500H，
Python 3.10.12，NumPy 2.2.6，Pillow 12.3.0。每组 6 帧；C 时间为 Host
`eaf4_verify` 的每帧中位耗时，因此只能用于同一台机器上的回归比较。

| 场景 | Codec | 压缩比 | 编码 FPS | C 解码 μs/帧 | PSNR | RGB/A MaxErr |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| alpha_glow | Lossless v2 | 6.85× | 96.2 | 158.3 | Inf | 0/0 |
| alpha_glow | EAF Bounded | 28.13× | 133.8 | 33.3 | 46.02 | 2/8 |
| color_key | Lossless v2 | 231.50× | 542.2 | 57.8 | Inf | 0/0 |
| color_key | EAF Bounded | 350.49× | 271.2 | 10.8 | 52.88 | 8/0 |
| dark_gradient | Lossless v2 | 19.89× | 65.8 | 82.3 | Inf | 0/0 |
| dark_gradient | EAF Bounded | 266.78× | 284.3 | 10.5 | 40.85 | 8/0 |
| fine_lines | Lossless v2 | 21.48× | 72.8 | 74.2 | Inf | 0/0 |
| fine_lines | EAF Bounded | 26.90× | 56.2 | 69.2 | Inf | 0/0 |
| flat_ui | Lossless v2 | 440.75× | 574.0 | 18.3 | Inf | 0/0 |
| flat_ui | EAF Bounded | 688.79× | 288.3 | 6.8 | Inf | 0/0 |
| high_frequency | Lossless v2 | 2.31× | 55.2 | 295.0 | Inf | 0/0 |
| high_frequency | EAF Bounded | 2.31× | 59.0 | 306.8 | Inf | 0/0 |
| micro_motion | Lossless v2 | 97.29× | 269.7 | 20.2 | Inf | 0/0 |
| micro_motion | EAF Bounded | 267.71× | 388.4 | 7.5 | 51.75 | 2/0 |
| natural_motion | Lossless v2 | 3.04× | 47.1 | 334.2 | Inf | 0/0 |
| natural_motion | EAF Bounded | 29.26× | 87.6 | 36.0 | 43.88 | 8/0 |
| pan_scroll | Lossless v2 | 16.33× | 43.7 | 170.7 | Inf | 0/0 |
| pan_scroll | EAF Bounded | 12.38× | 24.7 | 190.2 | 40.45 | 8/0 |
| landscape_pan | Lossless v2 | 1.45× | 22.4 | 1030.7 | Inf | 0/0 |
| landscape_pan | EAF Bounded | 1.75× | 23.1 | 879.8 | 44.63 | 8/0 |
| landscape_water | Lossless v2 | 2.63× | 36.3 | 613.5 | Inf | 0/0 |
| landscape_water | EAF Bounded | 3.12× | 41.5 | 505.2 | 45.06 | 8/0 |
| robot_float | Lossless v2 | 4.78× | 143.1 | 183.2 | Inf | 0/0 |
| robot_float | EAF Bounded | 5.43× | 152.3 | 123.0 | 54.52 | 8/8 |

验收结果：**PASS（48 项检查）**。

这组数据也刻意保留了两个负向案例：随机高频纹理下有界模式大量进入 Raw 逃逸，
与无损模式同为 2.31×；全屏平移纹理中有界模式为 12.38×，反而小于无损的
16.33×。因此不能只用扁平 UI 素材评价 codec。
照片级 `landscape_pan` 上无损仅为 1.45×、有界模式为 1.75×，进一步说明
真实纹理、相机运动和程序化 UI 必须分组报告。
