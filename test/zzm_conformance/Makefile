ARM_CC ?= arm-none-eabi-gcc
ARM_READELF ?= arm-none-eabi-readelf
BUILD_DIR := build
FIXTURE := fixture.c
LINKER := ../../util/zzm_module.ld
CHECK := ../../util/check_zzm_conformance.sh
MODULE_BUILD := ../../ZZ9000_proto.sdk/ZZ9000OS/build/modules
MPEG2_ELF ?= $(MODULE_BUILD)/mpeg2/MPEG2.elf
WEBM_ELF ?= $(MODULE_BUILD)/webm/WEBM.elf
WEBP_ELF ?= $(MODULE_BUILD)/webp/WEBP.elf

COMMON_FLAGS := -mcpu=cortex-a9 -marm -ffreestanding -fno-builtin -fno-pic \
	-nostdlib -Wl,--gc-sections -Wl,-T,$(LINKER) \
	-Wl,--undefined=zzm_rt_identity -Wl,--undefined=zzm_module_descriptor \
	-Wl,--defsym,ZZM_SLOT=0x09000000 -Wl,--defsym,ZZM_SLOT_SIZE=0x00400000

FIXTURES := $(BUILD_DIR)/valid.elf $(BUILD_DIR)/local_heap.elf \
	$(BUILD_DIR)/stub_syscall.elf $(BUILD_DIR)/unresolved_allocator.elf \
	$(BUILD_DIR)/undefined_symbol.elf $(BUILD_DIR)/unwind_throw.elf \
	$(BUILD_DIR)/outside_slot.elf $(BUILD_DIR)/bad_header.elf

.PHONY: test clean

test: $(FIXTURES) run.sh $(CHECK)
	sh ./run.sh $(CHECK) $(ARM_READELF) $(MPEG2_ELF) $(WEBM_ELF) $(WEBP_ELF)

$(BUILD_DIR)/valid.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -o $@ $(FIXTURE)

$(BUILD_DIR)/local_heap.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -DLOCAL_HEAP -o $@ $(FIXTURE)

$(BUILD_DIR)/stub_syscall.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -DSTUB_SYSCALL -o $@ $(FIXTURE)

$(BUILD_DIR)/unresolved_allocator.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -DUNRESOLVED_ALLOCATOR \
		-Wl,--unresolved-symbols=ignore-all -o $@ $(FIXTURE)

$(BUILD_DIR)/undefined_symbol.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -DUNDEFINED_SYMBOL \
		-Wl,--unresolved-symbols=ignore-all -o $@ $(FIXTURE)

$(BUILD_DIR)/unwind_throw.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -DUNWIND_THROW -o $@ $(FIXTURE)

$(BUILD_DIR)/outside_slot.elf: $(FIXTURE) $(LINKER)
	@mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(COMMON_FLAGS) -DOUTSIDE_SLOT \
		-Wl,--section-start,.outside=0x08000000 -o $@ $(FIXTURE)

$(BUILD_DIR)/bad_header.elf: $(BUILD_DIR)/valid.elf
	cp $< $@
	printf '\000' | dd of=$@ bs=1 seek=0 conv=notrunc status=none

clean:
	rm -rf $(BUILD_DIR)
