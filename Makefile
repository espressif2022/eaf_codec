CC ?= gcc
PYTHON ?= python3
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Icomponents/esp_lv_eaf_player/include -Icomponents/esp_lv_eaf_player/src/codec
BUILD := build

.PHONY: all assets test bench quick-bench host-test host-quick host-bench device-pack device-s3-pack device-report device-s3-report clean

all: $(BUILD)/eaf4_verify

EAF4_DECODER_SRCS := components/esp_lv_eaf_player/src/container/eaf4/esp_eaf4_container.c \
	components/esp_lv_eaf_player/src/codec/esp_eaf_codec_registry.c \
	components/esp_lv_eaf_player/src/codec/esp_eaf_codec_bounded.c \
	components/esp_lv_eaf_player/src/codec/eaf4/lossless.c

$(BUILD)/eaf4_verify: tools/eaf4_verify.c $(EAF4_DECODER_SRCS) components/esp_lv_eaf_player/include/eaf4.h components/esp_lv_eaf_player/include/esp_eaf_codec.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/eaf4_verify.c $(EAF4_DECODER_SRCS) -lz

assets:
	$(PYTHON) bench/generate_synthetic_assets.py
	$(PYTHON) bench/generate_visual_assets.py
	$(PYTHON) bench/generate_test_sample.py

host-test: $(BUILD)/eaf4_verify
	$(PYTHON) tests/test_roundtrip.py

host-quick: $(BUILD)/eaf4_verify
	$(PYTHON) bench/quick_host_bench.py

host-bench: $(BUILD)/eaf4_verify
	$(PYTHON) bench/run_matrix.py assets/synthetic assets/visual

device-pack:
	$(PYTHON) bench/pack_s31_bench.py

device-s3-pack:
	$(PYTHON) bench/pack_esp32s3_480x480.py

device-report:
	$(PYTHON) bench/generate_report.py

device-s3-report:
	$(PYTHON) bench/generate_report.py \
		--manifest build/esp32s3_480x480/manifest.json \
		--log build/esp32s3_480x480/esp32s3_hardware.log \
		-o docs/ESP32S3_480X480_BENCHMARK.md

# Backward-compatible short targets.
test: host-test
quick-bench: host-quick
bench: host-bench

clean:
	rm -rf $(BUILD)
