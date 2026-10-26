BUILD_DIR := build
BUILD_CHANNEL ?= development

.PHONY: all clean

all:
	cmake -S . -B $(BUILD_DIR) -DDISQUISITION_BUILD_CHANNEL=$(BUILD_CHANNEL)
	cmake --build $(BUILD_DIR) --parallel

clean:
	rm -rf $(BUILD_DIR)
