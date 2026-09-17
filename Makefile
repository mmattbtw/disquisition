BUILD_DIR := build

.PHONY: all clean test

all:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) --parallel

clean:
	rm -rf $(BUILD_DIR)

test: all
	python3 tests/p2p_smoke.py
