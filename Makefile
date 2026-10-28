BUILD_DIR := build
BUILD_CHANNEL ?= development
DESKTOP_BUILD_DIR ?= $(BUILD_DIR)-desktop
RELAY_BUILD_DIR ?= $(BUILD_DIR)-relay

.PHONY: all desktop relay clean

all:
	cmake -S . -B $(BUILD_DIR) -DDISQUISITION_BUILD_CHANNEL=$(BUILD_CHANNEL)
	cmake --build $(BUILD_DIR) --parallel

desktop:
	cmake -S . -B "$(DESKTOP_BUILD_DIR)" -DBUILD_DESKTOP_APP=ON -DBUILD_RELAY=OFF -DBUILD_TESTING=OFF -DDISQUISITION_BUILD_CHANNEL="$(BUILD_CHANNEL)"
	cmake --build "$(DESKTOP_BUILD_DIR)" --target disquisition-desktop --parallel

relay:
	cmake -S . -B "$(RELAY_BUILD_DIR)" -DBUILD_DESKTOP_APP=OFF -DBUILD_RELAY=ON -DBUILD_TESTING=OFF
	cmake --build "$(RELAY_BUILD_DIR)" --target relay --parallel

clean:
	rm -rf "$(BUILD_DIR)" "$(DESKTOP_BUILD_DIR)" "$(RELAY_BUILD_DIR)"
