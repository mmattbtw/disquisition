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
# The console community client is shared with the 3DS app.
SOURCES := source ../console ../console/third_party/monocypher
INCLUDES := include ../../include ../console
CFLAGS := -g -O2 -Wall -Wextra -Wpedantic -ffunction-sections $(MACHDEP)
CFLAGS += $(INCLUDE) -D__WIIU__ -D__WUT__
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)
LIBS := -lmbedtls -lmbedx509 -lmbedcrypto -lwut -lm
LIBDIRS := $(WUT_ROOT) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
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
