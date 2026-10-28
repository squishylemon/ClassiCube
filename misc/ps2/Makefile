ifeq ($(strip $(PS2SDK)),)
$(error "PS2SDK must be set in your environment")
endif

.SUFFIXES:


#---------------------------------------------------------------------------------
# Configurable options
#---------------------------------------------------------------------------------
TARGET 		:= ClassiCube-ps2
SOURCE_DIRS	:= src src/ps2 third_party/bearssl
BUILD_DIR 	:= build/ps2
RUN_PROGRAM	?= pcsx2

default: $(TARGET)-min.elf


#---------------------------------------------------------------------------------
# Code generation
#---------------------------------------------------------------------------------
LIBS	:= -lpatches -lpad -ldma -lgraph -lc -lps2ip -lnetman -lmc -lmouse -lkbd -laudsrv

CFLAGS 	:= -D_EE -G0 -O2 -Wall -gdwarf-2 -gz -DPLAT_PS2 \
				-I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include
LDFLAGS := -L$(PS2SDK)/ee/lib -Wl,-zmax-page-size=128 \
				-T$(PS2SDK)/ee/startup/linkfile


#---------------------------------------------------------------------------------
# Compiler tools
#---------------------------------------------------------------------------------
CC		:= mips64r5900el-ps2-elf-gcc
PP 	 	:= mips64r5900el-ps2-elf-cpp
LINK	:= $(CC)

STRIP	:= mips64r5900el-ps2-elf-strip
DVP 	:= dvp-as
VCL 	:= openvcl/openvcl


#---------------------------------------------------------------------------------
# IRX modules
#---------------------------------------------------------------------------------
IOP_SRCS := ps2dev9 netman smap \
			bdm bdmfs_fatfs \
			usbd usbmass_bd usbhdfsd \
			ps2mouse ps2kbd \
			libsd audsrv

IOP_MODS := $(patsubst %,$(BUILD_DIR)/%_irx.o,$(IOP_SRCS))
OBJECTS += $(IOP_MODS)

$(BUILD_DIR)/%_irx.c: $(PS2SDK)/iop/irx/%.irx
	bin2c $< $@ $*_irx

$(BUILD_DIR)/%_irx.o: $(BUILD_DIR)/%_irx.c
	$(CC) $(CFLAGS) -c $< -o $@

src/ps2/Platform_PS2.c: $(IOP_MODS)


#---------------------------------------------------------------------------------
# shader object generation
#---------------------------------------------------------------------------------
SHDR_DIR  := misc/ps2
SHDR_OBJS := #$(BUILD_DIR)/vu1_DrawTextured.o

.PRECIOUS: $(SHDR_DIR)/vu1_%.vsm $(SHDR_DIR)/vu1_%.vcl
# prevent the .vsm files from being deleted

$(SHDR_DIR)/vu1_%.vcl: $(SHDR_DIR)/vu1_%.vcp
	$(PP) -P -C $< -o $@

$(SHDR_DIR)/vu1_%.vsm: $(SHDR_DIR)/vu1_%.vcl
	$(VCL) $< -o $@

$(SHDR_DIR)/vu1_%.o: $(SHDR_DIR)/vu1_%.vsm
	$(DVP) $< -o $@

src/ps2/Graphics_PS2.c: $(SHDR_OBJS)


#---------------------------------------------------------------------------------
# executable generation
#---------------------------------------------------------------------------------
OEXT		:= .elf
GEN_FILES 	:= $(TARGET)-min.elf $(TARGET).iso
include misc/makefiles/common_build.mk

# ClassiCube textures and sounds are embedded into the executable using .incbin
$(BUILD_DIR)/src/TexturePack.o: misc/cc_textures.zip
$(BUILD_DIR)/src/Audio.o: misc/cc_audio.zip

$(TARGET)-min.elf: $(TARGET).elf
	$(STRIP) $< -o $@


#---------------------------------------------------------------------------------
# disc image generation
#---------------------------------------------------------------------------------
ISO_DIR := $(BUILD_DIR)/iso

# Disc only needs the executable, as textures and sounds are built into it
# (needs xorriso, e.g. 'apk add xorriso' in the ps2dev Docker image)
$(TARGET).iso: $(TARGET)-min.elf misc/ps2/SYSTEM.CNF
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)
	cp misc/ps2/SYSTEM.CNF $(ISO_DIR)/SYSTEM.CNF
	cp $(TARGET)-min.elf $(ISO_DIR)/CLASSICU.ELF
	xorriso -as mkisofs -quiet -iso-level 1 -sysid PLAYSTATION -V CLASSICUBE -o $@ $(ISO_DIR)

iso: $(TARGET).iso


#---------------------------------------------------------------------------------
# common targets
#---------------------------------------------------------------------------------
include misc/makefiles/common_targets.mk
