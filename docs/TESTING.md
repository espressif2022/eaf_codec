# Testing

## Prerequisites

```bash
python3 -m pip install -r requirements.txt
```

Host C verification additionally needs a C11 compiler and zlib development files.

## Canonical commands

| Command | Purpose | Typical use |
| :--- | :--- | :--- |
| `make assets` | Regenerate procedural and master-derived public PNG sequences | After generator changes |
| `make host-test` | Full Python/C roundtrip, CLI and legacy compatibility suite | Required before merge |
| `make host-quick` | Six-frame public smoke benchmark with PASS/FAIL | Fast local/CI acceptance |
| `make host-bench` | Exhaustive EAF4 submode matrix over procedural and visual assets | Performance investigation |
| `make device-s3-pack` | Build the SUB2 480×480 public MBPK image | Before ESP32-S3 LCD board runs |
| `make device-s3-report` | Validate the S3 serial log and generate its public report | After an S3 run |
| `make device-pack` / `make device-report` | Build/report the general procedural pack | Other supported targets |

The legacy targets `make test`, `make quick-bench` and `make bench` are aliases.
Set `PYTHON=/path/to/python` when using a virtual environment.

## Host acceptance rules

`make host-test` must complete all EAF4 submodes, EAF/EAF4 container inspection,
public CLI names for `0x04..0x06`, legacy assets and Python/C CRC verification.
`0x07 EAF Video` depends on the target video stack and is validated in its integration
environment rather than claimed by this Host suite.

`make host-quick` returns non-zero when any of these checks fails:

- EAF Lossless is not byte-exact for RGB and Alpha;
- EAF Bounded exceeds `MaxErr=8` in RGB or Alpha;
- a payload is empty or malformed;
- the Host C decoder rejects a frame or CRC.

Timing fields are regression signals for the same machine, not device predictions.

## Generated outputs

All generated files stay under ignored `build/`:

- `build/eaf4_verify`: Host C verifier;
- `build/host_quick_report.json`: smoke benchmark data;
- `build/matrix_8888_report.json`: full Host matrix;
- `build/s31_bench_pack/`: public device pack, manifest and captured serial log.
- `build/esp32s3_480x480/`: full-screen S3 sources, pack, manifest and hardware log.

Device procedures and pass criteria are in [DEVICE_TESTING.md](DEVICE_TESTING.md).
