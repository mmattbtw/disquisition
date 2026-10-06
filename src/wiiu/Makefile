# Native RPX/WUHB build using devkitPro's packaged wut rules.
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error Please set DEVKITPRO or build using the documented devkitPPC container)
endif
TOPDIR ?= $(CURDIR)
APP_NAME := Disquisition
APP_SHORTNAME := Disquisition
APP_AUTHOR := Disquisition contributors
include $(DEVKITPRO)/wut/share/wut_rules

TARGET := disquisition
BUILD := build
SOURCES := source ../3ds/source ../common
INCLUDES := include ../3ds/include ../../include
CFLAGS := -g -O2 -Wall -Wextra -Wpedantic -ffunction-sections $(MACHDEP)
CFLAGS += $(INCLUDE) -D__WIIU__ -D__WUT__
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)
LIBS := -lwut -lm
LIBDIRS := $(WUT_ROOT)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
# Use the platform-independent session from the parent 3DS branch. Do not
# wildcard its platform sources, or CMake/desktop/SQLite sources.
CPPFILES := $(notdir $(wildcard source/*.cpp)) session.cpp protocol.cpp
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
    $(foreach dir,$(LIBDIRS),-isystem $(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: all clean $(BUILD)
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).rpx $(TARGET).wuhb
else
.PHONY: all
all: $(OUTPUT).wuhb
$(OUTPUT).wuhb: $(OUTPUT).rpx
$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
