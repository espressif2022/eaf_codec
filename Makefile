CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Icomponents/esp_lv_eaf_player/include
BUILD := build

.PHONY: all test bench clean

all: $(BUILD)/eaf4_verify

$(BUILD)/eaf4_verify: tools/eaf4_verify.c components/esp_lv_eaf_player/src/eaf4.c components/esp_lv_eaf_player/include/eaf4.h components/esp_lv_eaf_player/include/esp_eaf_codec.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/eaf4_verify.c components/esp_lv_eaf_player/src/eaf4.c -lz

test: $(BUILD)/eaf4_verify
	python3 tests/test_roundtrip.py

bench: $(BUILD)/eaf4_verify
	python3 bench/run_matrix.py

clean:
	rm -rf $(BUILD)
