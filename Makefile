RUSTARCH ?= aarch64-unknown-none-softfloat

ifeq ($(shell uname),Darwin)
USE_CLANG ?= 1
$(info INFO: Building on Darwin)

ifeq ($(shell command -v llvm-config 2>/dev/null),)
BREW ?= $(shell command -v brew)
LLVMCONFIG ?= $(shell $(BREW) --prefix llvm)/bin/llvm-config
else
LLVMCONFIG ?= $(shell command -v llvm-config)
endif
TOOLCHAIN ?= $(shell $(LLVMCONFIG) --bindir)/
$(info INFO: Toolchain path: $(TOOLCHAIN))

ifeq ($(shell ls $(TOOLCHAIN)ld.lld 2>/dev/null),)
BREW ?= $(shell command -v brew)
LLDDIR ?= $(shell $(BREW) --prefix lld)/bin/
else
LLDDIR ?= $(TOOLCHAIN)
endif
ifneq ($(TOOLCHAIN),$(LLDDIR))
$(info INFO: LLD path: $(LLDDIR))
endif
endif

ifeq ($(shell uname -m),aarch64)
ARCH ?=
else
ARCH ?= aarch64-linux-gnu-
endif

ifeq ($(USE_CLANG),1)
CC := $(TOOLCHAIN)clang --target=$(ARCH)
AS := $(TOOLCHAIN)clang --target=$(ARCH)
LD := $(LLDDIR)ld.lld
OBJCOPY := $(TOOLCHAIN)llvm-objcopy
CLANG_FORMAT ?= $(TOOLCHAIN)clang-format
EXTRA_CFLAGS ?=
else
CC := $(TOOLCHAIN)$(ARCH)gcc
AS := $(TOOLCHAIN)$(ARCH)gcc
LD := $(TOOLCHAIN)$(ARCH)ld
OBJCOPY := $(TOOLCHAIN)$(ARCH)objcopy
CLANG_FORMAT ?= clang-format
EXTRA_CFLAGS ?= -Wstack-usage=2048
endif

ifeq ($(V),)
QUIET := @
else
ifeq ($(V),0)
QUIET := @
else
QUIET :=
endif
endif

BASE_CFLAGS := -O2 -Wall -g -Wundef -Werror=strict-prototypes -fno-common -fno-PIE \
	-Werror=implicit-function-declaration -Werror=implicit-int \
	-Wsign-compare -Wunused-parameter -Wno-multichar \
	-ffreestanding -fpic -ffunction-sections -fdata-sections \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) -isystem sysinc \
	-Isrc \
	-fno-stack-protector -mstrict-align -march=armv8.2-a \
	$(EXTRA_CFLAGS)

CFLAGS := $(BASE_CFLAGS) -mgeneral-regs-only -DM4_LOADER -Isrc/libfdt

CFG :=
ifeq ($(J616S_SCREEN_DIAGNOSTIC),1)
CFG += J616S_SCREEN_DIAGNOSTIC J616S_MEMORY_DIAGNOSTIC
endif
ifeq ($(RELEASE),1)
CFG += RELEASE
endif

ifeq ($(NO_DISPLAY),1)
CFG += NO_DISPLAY
endif

# DCP bring-up test builds: M4_DCP_STAGE=1|2|3 (see standalone/dcp-fdt.c).
# Unset leaves the default loader unchanged (no DCP code is built).
DCP_FDT_OBJECTS :=
ifneq ($(M4_DCP_STAGE),)
CFLAGS += -DM4_DCP_STAGE=$(M4_DCP_STAGE)
DCP_FDT_OBJECTS := standalone/dcp-fdt.o
endif

# Round11 bring-up/USB-C metadata export on collection boots only (non-DCP
# builds); normal boots skip it. Unset leaves it out.
ifeq ($(M4_BRINGUP_EXPORT),1)
CFLAGS += -DM4_BRINGUP_EXPORT
endif

# Previous fully uncached loader (no cached windows); the rest of R34 stays.
ifeq ($(M4_LOADER_UNCACHED),1)
CFLAGS += -DM4_LOADER_UNCACHED
endif

# Previous one-peer-at-a-time start (wait for READY before the next launch).
ifeq ($(M4_SMP_SEQUENTIAL),1)
CFLAGS += -DM4_SMP_SEQUENTIAL
endif

# R34 scheduler topology: /cpus/cpu-map and capacity-dmips-mhz in the Linux DT.
# Off by default: the pinned GPU module refuses a /cpus child without a reg
# (see src/standalone/direct-fdt.c). Needs diffs/capture-dt.h.diff in the GPU.
ifeq ($(M4_CPU_TOPOLOGY),1)
CFLAGS += -DM4_CPU_TOPOLOGY
endif

# R36 multicore E0: read-only idle/wake register record (standalone/cpu-idle-regs.c), on by default.
# M4_NO_CPU_IDLE_READ=1 reads nothing and publishes nothing.
ifeq ($(M4_NO_CPU_IDLE_READ),1)
CFLAGS += -DM4_NO_CPU_IDLE_READ
endif

# Kernel boots with sched_verbose: domain spans and capacities in the log.
ifeq ($(M4_SCHED_VERBOSE),1)
CFLAGS += -DM4_SCHED_VERBOSE
endif

# Collect firmware memory metadata, then stop before normal hardware startup.
ifeq ($(J616S_MEMORY_DIAGNOSTIC),1)
CFG += J616S_MEMORY_DIAGNOSTIC
endif

# Required for no_std + alloc for now
export RUSTC_BOOTSTRAP=1
RUST_LIB := librust.a
ifeq ($(BUILDSTD),1)
CARGO_FLAGS := -Z build-std=alloc,core
else
CARGO_FLAGS :=
endif

ifeq ($(CHAINLOADING),1)
CFG += CHAINLOADING
CARGO_FLAGS += --features chainload
endif

LDFLAGS := --gc-sections -EL -maarch64elf --no-undefined -X -Bsymbolic \
	-z notext --no-apply-dynamic-relocs --orphan-handling=warn \
	-z nocopyreloc --gc-sections -pie
ifeq ($(J616S_SCREEN_DIAGNOSTIC),1)
LDFLAGS += --wrap=malloc --wrap=calloc --wrap=memalign --wrap=free --wrap=realloc \
	--wrap=printf --wrap=debug_printf --wrap=__assert_fail \
	--wrap=iodev_console_write --wrap=flush_and_reboot
endif

MINILZLIB_OBJECTS := $(patsubst %,minilzlib/%, \
	dictbuf.o inputbuf.o lzma2dec.o lzmadec.o rangedec.o xzstream.o)

TINF_OBJECTS := $(patsubst %,tinf/%, \
	adler32.o crc32.o tinfgzip.o tinflate.o tinfzlib.o)

DLMALLOC_OBJECTS := dlmalloc/malloc.o

LIBFDT_OBJECTS := $(patsubst %,libfdt/%, \
	fdt_addresses.o fdt_empty_tree.o fdt_ro.o fdt_rw.o fdt_strerror.o fdt_sw.o \
	fdt_wip.o fdt.o)

CHICKENS_OBJECTS := $(patsubst %,chickens/%, \
	avalanche.o \
	blizzard.o \
	cyclone_typhoon.o \
	everest.o \
	firestorm.o \
	hurricane_zephyr.o \
	icestorm.o \
	monsoon_mistral.o \
	sawtooth.o \
	twister.o)

DCP_OBJECTS := $(patsubst %,dcp/%, \
	dpav_ep.o \
	dptx_phy.o \
	dptx_port_ep.o \
	parser.o \
	system_ep.o)

OBJECTS := \
	adt.o \
	afk.o \
	aic.o \
	asc.o \
	bootlogo_48.o bootlogo_128.o bootlogo_256.o \
	chainload.o \
	chainload_asm.o \
	chickens.o \
	clk.o \
	cpufreq.o \
	dapf.o \
	dart.o \
	dcp.o \
	dcp_iboot.o \
	devicetree.o \
	display.o \
	dockchannel_uart.o \
	exception.o exception_asm.o \
	fb.o font.o font_retina.o \
	firmware.o \
	gxf.o gxf_asm.o \
	heapblock.o \
	hv.o hv_vm.o hv_exc.o hv_vuart.o hv_wdt.o hv_asm.o hv_aic.o hv_t8140.o hv_virtio.o \
	i2c.o \
	iodev.o \
	iova.o \
	isp.o \
	standalone/loader.o standalone/fixups.o standalone/boot-timing.o standalone/boot-profile.o standalone/direct-smp.o standalone/direct-smp-entry.o standalone/direct-smp-release.o standalone/direct-entry.o standalone/dart-capture.o standalone/display-handoff.o standalone/display-handoff-native.o $(DCP_FDT_OBJECTS) standalone/dart-tables.o standalone/dart-preserve.o standalone/display-ownership.o standalone/dapf.o standalone/fdt-check.o standalone/metadata.o standalone/memory.o standalone/mcc.o standalone/watchdog.o standalone/direct-bluetooth.o standalone/bt-boot-admission.o standalone/bt-boot-current-adt.o standalone/payload.o \
	j616s_diag.o \
	j616s_screen.o \
	j616s_probe.o j616s_probe_asm.o j616s_mmu.o j616s_mmu_asm.o j616s_pt.o j616s_usb.o j616s_usb_plan.o j616s_usb_asm.o j616s_exec_asm.o j616s_proxy.o \
	kboot.o kboot_atc.o \
	kboot_t6020_compat.o \
	main.o \
	mitigations.o \
	mcc.o \
	memory.o memory_asm.o \
	nvme.o \
	payload.o \
	pcie.o \
	pmgr.o \
	proxy.o \
	ringbuffer.o \
	rtkit.o \
	sart.o \
	sep.o \
	sio.o \
	smc.o \
	smp.o \
	spmi.o \
	start.o \
	startup.o \
	string.o \
	tunables.o tunables_static.o \
	tps6598x.o \
	uart.o \
	uartproxy.o \
	usb.o usb_dwc3.o \
	utils.o utils_asm.o \
	vsprintf.o \
	wdt.o \
	$(CHICKENS_OBJECTS) \
	$(DCP_OBJECTS) \
	$(MINILZLIB_OBJECTS) $(TINF_OBJECTS) $(DLMALLOC_OBJECTS) $(LIBFDT_OBJECTS) \
	standalone/loader-cache.o standalone/loader-cache-asm.o standalone/loader-speed.o \
	standalone/cpu-idle-regs.o standalone/pmp-publish.o

FP_OBJECTS := \
	kboot_gpu.o \
	math/expf.o \
	math/exp2f_data.o \
	math/powf.o \
	math/powf_data.o

# R39 Level 1: kernel and initrd as files from the SSD. M4_LEVEL1=1 is the
# `desktop` carrier (tries the SSD first), M4_LEVEL1=0 the `embedded` carrier
# (the attempt is never started). M4_CARRIER_ID is the 16-hex carrier id (the
# build script patches it into the image with tools/carrier-id.py when the
# final initrd is not known at compile time).
M4_LEVEL1 ?= 0
CFLAGS += -DM4_LEVEL1=$(M4_LEVEL1)
# /chosen m4,loader-version: the release id the carrier build stamps (default rev11)
ifneq ($(M4_LOADER_VERSION),)
CFLAGS += -DM4_LOADER_VERSION=\"$(M4_LOADER_VERSION)\"
endif
# The Level 1 code is a position-independent blob: its own compile and link
# (no dynamic relocations, no writable data), embedded into the .l1 section.
L1_NAMES := level1-sha256 level1-ext4 level1-core level1-ans level1-blob level1-libc
L1_FLAGS := -Os -fno-tree-switch-conversion -fno-pic -fno-PIE -mcmodel=tiny -ffreestanding -fno-builtin -fno-stack-protector -mstrict-align \
	-march=armv8.2-a -mgeneral-regs-only -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -fno-common -Wall \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) -Isrc/standalone \
	-DM4_LEVEL1=$(M4_LEVEL1)
L1_OBJS := $(patsubst %,build/level1/%.o,$(L1_NAMES)) build/level1/level1-header.o
build/level1/%.o: src/standalone/%.c
	$(QUIET)echo "  L1CC  $@"
	$(QUIET)mkdir -p build/level1
	$(QUIET)$(CC) -c $(L1_FLAGS) -o $@ $<
build/level1/level1-header.o: src/standalone/level1-header.S
	$(QUIET)mkdir -p build/level1
	$(QUIET)$(CC) -c $(L1_FLAGS) -o $@ $<
build/level1.elf: $(L1_OBJS) src/standalone/level1.ld
	$(QUIET)echo "  L1CHK no absolute relocations in the blob"
	$(QUIET)for o in $(filter-out %level1-header.o,$(L1_OBJS)); do \
	  if $(subst objcopy,readelf,$(OBJCOPY)) -rW $$o | grep -q "R_AARCH64_.*ABS"; then \
	    echo "$$o has an absolute data relocation: the Level 1 blob is never relocated (no pointer tables)"; exit 1; fi; done
	$(QUIET)echo "  L1LD  $@"
	$(QUIET)$(LD) -T src/standalone/level1.ld --gc-sections -static -nostdlib -EL -maarch64elf -o $@ $(L1_OBJS)
build/level1.bin: build/level1.elf
	$(QUIET)echo "  L1BIN $@"
	$(QUIET)$(OBJCOPY) -O binary $< $@
build/standalone/level1-embed.o: build/level1.bin
# the shim adds no GOT entries / dynamic relocations (loader rodata+rela headroom is tiny)
build/standalone/level1-shim.o: CFLAGS += -fvisibility=hidden
# loader-side Level 1 code is size-sensitive (loader .text headroom is the budget): -Os
build/standalone/level1-addr.o: CFLAGS += -Os -fvisibility=hidden
OBJECTS += standalone/level1-shim.o standalone/level1-embed.o standalone/level1-guard.o standalone/level1-addr.o

# R36 media lane: disabled audio/AVD nodes and the guarded register probe
# (standalone/media-probe.c). Part of the stage-3 loader without a flag;
# M4_MEDIA_NO_PROBE=1 keeps the nodes and reservations but reads no register.
OBJECTS += standalone/media-probe.o
ifeq ($(M4_MEDIA_NO_PROBE),1)
CFLAGS += -DM4_MEDIA_NO_PROBE
endif

BUILD_OBJS := $(patsubst %,build/%,$(OBJECTS))
BUILD_FP_OBJS := $(patsubst %,build/%,$(FP_OBJECTS))
BUILD_RUST_LIB := $(patsubst %,build/%,$(RUST_LIB))
BUILD_ALL_OBJS := $(BUILD_OBJS) $(BUILD_FP_OBJS) $(BUILD_RUST_LIB)
NAME := m1n1
TARGET := m1n1.macho
TARGET_RAW := m1n1.bin

DEPDIR := build/.deps

.PHONY: all clean format invoke_cc always_rebuild
all: build/$(TARGET) build/$(TARGET_RAW)
clean:
	rm -rf build/* build/.deps
format:
	$(CLANG_FORMAT) -i src/*.c src/chickens/*.c src/dcp/*.c src/math/*.c src/*.h src/dcp/*.h src/math/*.h sysinc/*.h
format-check:
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/chickens/*.c src/dcp/*.c src/math/*.c src/*.h src/dcp/*.h src/math/*.h sysinc/*.h
rustfmt:
	cd rust && cargo fmt
rustfmt-check:
	cd rust && cargo fmt --check

build/$(RUST_LIB): src/../build/build_cfg.h rust/src/*.rs rust/src/gpu/*.rs rust/src/gpu/hw/*.rs rust/Cargo.toml rust/Cargo.lock
	$(QUIET)echo "  RS    $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)cargo build $(CARGO_FLAGS) --target $(RUSTARCH) --lib --release --manifest-path rust/Cargo.toml --target-dir build
	$(QUIET)cp "build/$(RUSTARCH)/release/${RUST_LIB}" "$@"

build/%.o: src/%.S
	$(QUIET)echo "  AS    $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(AS) -c $(BASE_CFLAGS) -MMD -MF $(DEPDIR)/$(*F).d -MQ "$@" -MP -o $@ $<

$(BUILD_FP_OBJS): build/%.o: src/%.c
	$(QUIET)echo "  CC FP $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(CC) -c $(BASE_CFLAGS) -MMD -MF $(DEPDIR)/$(*F).d -MQ "$@" -MP -o $@ $<

build/%.o: src/%.c build-tag build-cfg
	$(QUIET)echo "  CC    $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(CC) -c $(CFLAGS) -MMD -MF $(DEPDIR)/$(*F).d -MQ "$@" -MP -o $@ $<

# special target for usage by m1n1.loadobjs
invoke_cc:
	$(QUIET)$(CC) -c $(CFLAGS) -Isrc -o $(OBJFILE) $(CFILE)

build/$(NAME).elf: $(BUILD_ALL_OBJS) m1n1.ld
	$(QUIET)echo "  LD    $@"
	$(QUIET)$(LD) -T m1n1.ld $(LDFLAGS) -o $@ $(BUILD_ALL_OBJS)

build/$(NAME)-raw.elf: $(BUILD_ALL_OBJS) m1n1-raw.ld
	$(QUIET)echo "  LDRAW $@"
	$(QUIET)$(LD) -T m1n1-raw.ld $(LDFLAGS) -o $@ $(BUILD_ALL_OBJS)

build/$(NAME).macho: build/$(NAME).elf
	$(QUIET)echo "  MACHO $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@

ifeq ($(LOGO),)
build/$(NAME).bin: build/$(NAME)-raw.elf
	$(QUIET)echo "  RAW   $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@

else
build/$(NAME)-asahi.bin: build/$(NAME)-raw.elf
	$(QUIET)echo "  RAW   $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@

build/$(NAME).bin: build/$(NAME)-asahi.bin build/$(LOGO).logo
	$(QUIET)echo "  RAW   $@"
	$(QUIET)cat $^ > $@
endif

.INTERMEDIATE: build-tag build-cfg
build-tag src/../build/build_tag.h &:
	$(QUIET)mkdir -p build
	$(QUIET)./version.sh > build/build_tag.tmp
	$(QUIET)cmp -s build/build_tag.h build/build_tag.tmp 2>/dev/null || \
	( mv -f build/build_tag.tmp build/build_tag.h && echo "  TAG   build/build_tag.h" )

build-cfg src/../build/build_cfg.h &:
	$(QUIET)mkdir -p build
	$(QUIET)for i in $(CFG); do echo "#define $$i"; done > build/build_cfg.tmp
	$(QUIET)cmp -s build/build_cfg.h build/build_cfg.tmp 2>/dev/null || \
	( mv -f build/build_cfg.tmp build/build_cfg.h && echo "  CFG   build/build_cfg.h" )

build/%.bin: data/%.bin
	$(QUIET)echo "  IMG   $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)cp $< $@

build/%.o: build/%.bin
	$(QUIET)echo "  BIN   $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(OBJCOPY) -I binary -B aarch64 -O elf64-littleaarch64 $< $@

build/%.bin: font/%.bin
	$(QUIET)echo "  CP    $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)cp $< $@

build/%.rgba: data/%.png
	$(eval SIZE := $(lastword $(subst _, ,$*)))
	$(QUIET)echo "  MAGIC $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)magick $< -background black -flatten -depth 8 -crop $(SIZE)x$(SIZE) -resize $(SIZE)x$(SIZE) rgba:$@

build/%.logo: build/%_256.rgba build/%_128.rgba
	$(QUIET)echo "  PAYLOAD $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)echo -n "m1n1_logo_256128" > $@
	$(QUIET)cat $^ >> $@

-include $(DEPDIR)/*
