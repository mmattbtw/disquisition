BUILD_DIR := build

.PHONY: all clean

all:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) --parallel

clean:
	rm -rf $(BUILD_DIR)
