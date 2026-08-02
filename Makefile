# Speed Haste 32X
# Override GENDEV when the Chilly Willy / 32XDK toolchain is elsewhere.
GENDEV ?= /opt/toolchains/sega
TARGET := release/SpeedHaste32X
BUILD := build

SHPREFIX := $(GENDEV)/sh-elf/bin/sh-elf-
MDPREFIX := $(GENDEV)/m68k-elf/bin/m68k-elf-
SHCC := $(SHPREFIX)gcc
SHAS := $(SHPREFIX)as
SHOBJC := $(SHPREFIX)objcopy
MDAS := $(MDPREFIX)as
MDLD := $(MDPREFIX)ld
HOSTCC ?= cc

CPPFLAGS := -Isrc -Isrc/platform -DENABLE_QA_HOOKS=1
AUDIO ?= 0
ifeq ($(AUDIO),1)
CPPFLAGS += -DENABLE_PWM_AUDIO=1
else
CPPFLAGS += -DENABLE_DUAL_SH2_RENDER=1
endif
CFLAGS := -std=c11 -m2 -mb -Os -Wall -Wextra -Werror \
          -fomit-frame-pointer -ffunction-sections -fdata-sections \
          -fno-common
ASFLAGS := --small
LDFLAGS := -T src/platform/mars-32x.ld -nostdlib \
           -Wl,-Map=$(TARGET).map
LIBS := -L$(GENDEV)/sh-elf/sh-elf/lib \
        -L$(GENDEV)/sh-elf/lib/gcc/sh-elf/12.1.0 \
        -lc -lgcc -lgcc-Os-4-200 -lnosys

C_SOURCES := src/main.c src/sh_assets.c src/sh_game.c src/sh_render.c \
             src/platform/platform_32x.c src/platform/slave.c
ASSET_BIN := assets/generated/speed_haste_assets.bin
ASSET_OBJ := $(BUILD)/assets/speed_haste_assets.o
# The ROM/security header in sh2_crt0.o must be the first linked .text input.
OBJECTS := $(BUILD)/src/platform/sh2_crt0.o \
           $(BUILD)/src/platform/sh2_math.o \
           $(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) $(ASSET_OBJ)

.PHONY: all rom clean test test-unit test-static test-emulator check-toolchain
all: rom
rom: check-toolchain $(TARGET).32x

check-toolchain:
	@test -x "$(SHCC)" || { \
	  echo "32X compiler not found under GENDEV=$(GENDEV)" >&2; \
	  echo "Run tools/setup_toolchain.sh, then follow the command it prints." >&2; \
	  exit 1; \
	}

$(BUILD)/src/platform/m68k_crt0.bin: src/platform/m68k_crt0.s
	@mkdir -p $(@D)
	$(MDAS) -m68000 --register-prefix-optional -o $(BUILD)/src/platform/m68k_crt0.o $<
	$(MDLD) -T $(GENDEV)/ldscripts/md.ld --oformat binary -o $@ $(BUILD)/src/platform/m68k_crt0.o

$(BUILD)/src/platform/m68k_crt1.bin: src/platform/m68k_crt1.s
	@mkdir -p $(@D)
	$(MDAS) -m68000 --register-prefix-optional -o $(BUILD)/src/platform/m68k_crt1.o $<
	$(MDLD) -T $(GENDEV)/ldscripts/md.ld --oformat binary -o $@ $(BUILD)/src/platform/m68k_crt1.o

$(BUILD)/src/platform/sh2_crt0.o: src/platform/sh2_crt0.s \
                                      $(BUILD)/src/platform/m68k_crt0.bin \
                                      $(BUILD)/src/platform/m68k_crt1.bin
	@mkdir -p $(@D)
	cd $(BUILD)/src/platform && $(abspath $(SHAS)) $(ASFLAGS) \
	  -o sh2_crt0.o $(abspath $<)

$(BUILD)/src/sh_render.o: CFLAGS += -O2
$(BUILD)/src/sh_game.o: CFLAGS += -O2

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(SHCC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(@D)
	$(SHAS) $(ASFLAGS) -o $@ $<

$(ASSET_OBJ): $(ASSET_BIN)
	@mkdir -p $(@D)
	$(SHOBJC) -I binary -O elf32-sh -B sh \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@

$(TARGET).elf: $(OBJECTS) src/platform/mars-32x.ld
	@mkdir -p $(@D)
	$(SHCC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

$(TARGET).32x: $(TARGET).elf
	$(SHOBJC) -O binary $< $(BUILD)/SpeedHaste32X.bin
	dd if=$(BUILD)/SpeedHaste32X.bin of=$@ bs=512K conv=sync status=none
	python3 tools/fix_rom_header.py $@
	@echo "Built $@ ($$(stat -c %s $@) bytes)"

test-unit:
	python3 tests/test_assets.py
	python3 tests/test_projection.py
	python3 tests/test_collision.py
	python3 tests/test_optimization.py

test-static: rom
	python3 tests/test_rom_static.py $(TARGET).32x

test-emulator: rom
	./tools/test_emulator.sh

test-split: rom
	./tools/test_split.sh

test: test-unit test-static test-emulator test-split

clean:
	rm -rf $(BUILD) release/*.elf release/*.32x release/*.map test-results
