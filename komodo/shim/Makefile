# Makefile — komodo shim (bare-metal AArch64 loader for `fastboot boot`).
#   make                 -> Image            (arm64 shim image)
#   make diag            -> shim-boot-diag.img  (shim only, no payload: M2 test)
#   make bootimg KERNEL=kernel.development.komodo
#                        -> payload.bin + shim-boot.img (needs an XNU kernel)
#   make test            -> ./test.sh (all offline checks)
#   make clean
#
# Flashing nothing: `fastboot boot` is RAM-only. See ../README.md.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm
READELF := $(CROSS)readelf

CFLAGS  := -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only \
           -mstrict-align -fno-stack-protector -Wall -Wextra
# Stop the hardware watchdogs in the shim (see wdt.c). Default ON: measured on
# komodo 2026-11-02, ABL leaves both armed and clocked (WTCON cl0 0x1af39,
# cl1 0x18021, bit5 set) -> the phone resets into Android. WDT_OFF=0 to keep.
WDT_OFF ?= 1
ifeq ($(WDT_OFF),1)
CFLAGS  += -DSHIM_WDT_OFF
endif
LDFLAGS := -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N

OBJS := start.o shim.o fb.o fdt.o console.o mmu.o loader.o vectors.o exceptions.o wdt.o

# Tools (companions live one dir up).
MKPAYLOAD   ?= ../mkpayload_komodo.py
MKBOOTIMG   ?= ../mk_bootimg_komodo.py
ADT         ?= ../komodo.adt

# Boot parameters.
# XNU RAM window: the largest hole in komodo's /reserved-memory below 4 GiB.
# 0x80000000+1G (the old default) covered gsa (TrustZone), tpu/gxp firmware,
# the always-on AoC and xHCI DMA. 0x98000000..0xc3000000 has none of them and
# sits above 0x80000000-0x90000000, where the bootloader loads images.
PHYS     ?= 0x98000000
MEM      ?= 0x2b000000
# HIGHMEM=1: XNU gets the upper DDR bank instead — 0x880000000, 14 GiB, no
# reserved holes (live /proc/iomem 2026-11-02). The ADT must match (--highmem).
# verified on komodo 2026-11-02 (PID 1 with 14 GiB)
HIGHMEM ?= 1
ifeq ($(HIGHMEM),1)
PHYS     := 0x880000000
MEM      := 0x380000000
ADTFLAGS := --highmem
ADT      := ../komodo-hi.adt
endif
# boot-args the ios-sweet/tegu lineage used on hardware: kprintf to the fb
# console (serial=3), one CPU first, symbolicated panics, watchdog off.
CMDLINE  ?= debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1
# optional md0 image (e.g. the static tegu init for a mockfs root): add rd=md0
RAMDISK  ?=
PREFIX   ?= xnu-boot

all: Image

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

shim.elf: $(OBJS) shim.ld
	@# refuse absolute relocations: the image runs wherever ABL loads it.
	@abs=$$($(READELF) -r shim.o fb.o fdt.o console.o mmu.o loader.o exceptions.o wdt.o 2>/dev/null | grep -c ABS64 || true); \
	 if [ "$$abs" != 0 ]; then echo "error: $$abs ABS64 relocations in C objects"; exit 1; fi
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

Image: shim.elf
	$(OBJCOPY) -O binary $< $@
	@# pad to _end so .bss + stack exist as zeroed bytes (matches image_size).
	@size=$$($(NM) $< | awk '$$3=="_end"{print strtonum("0x"$$1)}'); \
	 truncate -s $$size $@; \
	 echo "OK -> $@ ($$(stat -c%s $@) bytes, image_size $$size)"

# --- diagnostic boot: shim only, empty ramdisk (proves the shim runs: M2) ---
diag: Image
	python3 -I $(MKBOOTIMG) Image shim-boot-diag.img \
		--cmdline "$(CMDLINE)" --os_version 15.0.0 --os_patch 2025-05
	@echo "flash-free test: fastboot boot shim-boot-diag.img"

# --- full boot: shim + XNU payload as the ramdisk (needs KERNEL=...) ---
payload.bin:
	@if [ -z "$(KERNEL)" ]; then echo "set KERNEL=<xnu Mach-O> (from macOS/CI build, M1b)"; exit 1; fi
	python3 -I ../mkadt_komodo.py $(ADT) $(ADTFLAGS)
	python3 -I $(MKPAYLOAD) "$(KERNEL)" $(ADT) payload.bin \
		--phys $(PHYS) --mem $(MEM) --cmdline="$(CMDLINE)" $(if $(RAMDISK),--ramdisk $(RAMDISK))

bootimg: Image payload.bin
	python3 -I $(MKBOOTIMG) Image shim-boot.img --ramdisk payload.bin \
		--cmdline "$(CMDLINE)" --os_version 15.0.0 --os_patch 2025-05
	@echo "fastboot boot shim-boot.img   (prefer: make xnuboot)"

# --- the real thing: shim + payload EMBEDDED in the kernel image, repacked
#     into the stock boot.img container (raw + stock-like MZ/LZ4 variants) ---
STOCK_BOOT ?= ../../ota/out/boot.img
xnuboot: Image payload.bin
	python3 -I ../mkxnuboot.py $(STOCK_BOOT) Image payload.bin $(PREFIX)
	@echo "RAM-only test: fastboot boot $(PREFIX)-raw.img   (or $(PREFIX)-stocklike.img)"

test:
	./test.sh

clean:
	rm -f *.o shim.elf Image payload.bin shim-boot.img shim-boot-diag.img xnu-boot-*.img

.PHONY: all diag bootimg xnuboot test clean
