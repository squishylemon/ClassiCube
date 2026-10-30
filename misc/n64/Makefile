BUILD_DIR		= build-n64
SOURCE_DIR	 	= src/n64
N64_ROM_TITLE 	= "ClassiCube"
N64_ROM_RTC 	= true
N64_ROM_SAVETYPE = flashram
TARGET			= ClassiCube-n64
N64_MKDFS_ROOT	= $(BUILD_DIR)/romfs
SRC_DIRS		= 

CFILES 	:= $(notdir $(wildcard src/*.c)) $(notdir $(wildcard src/n64/*.c))
OFILES 	:= $(CFILES:.c=.o) rsp_gpu.o
OBJS 	:= $(addprefix $(BUILD_DIR)/,$(OFILES))
CFLAGS	:= -Wno-error=missing-braces -Wno-error=strict-aliasing -Wno-error=incompatible-pointer-types -DPLAT_N64


default: $(BUILD_DIR) $(TARGET).z64

clean:
	rm -rf $(BUILD_DIR) $(TARGET).z64

$(BUILD_DIR): 
	mkdir -p $@


$(BUILD_DIR)/%.o: src/%.c 
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<

include $(N64_INST)/include/n64.mk

# Works around libdragon hanging on real hardware when accessing cartridge registers (see Platform_N64.c)
LDFLAGS += --wrap=io_read --wrap=io_write
# The code is loaded entirely into RAM (of which there's only 4 MB without the Expansion Pak),
#  so optimise for size, and don't fill in every uninitialised local variable either
$(TARGET).z64: CFLAGS += -Os -ftrivial-auto-var-init=uninitialized

$(TARGET).z64: N64_ROM_TITLE = "ClassiCube"
$(TARGET).z64: $(BUILD_DIR)/filesystem.dfs

# ClassiCube's own textures and sounds, and the free music, are stored in the ROM filesystem
#  (rather than in the executable, which is loaded entirely into RAM)
MUSIC_FILES	:= $(wildcard misc/music/*.wav)
ROMFS_FILES	:= $(N64_MKDFS_ROOT)/cc_textures.zip $(BUILD_DIR)/sounds.extracted \
			   $(patsubst misc/music/music%.wav,$(N64_MKDFS_ROOT)/music/MUSIC%.WAV,$(MUSIC_FILES))

$(N64_MKDFS_ROOT)/%.zip: misc/%.zip
	@mkdir -p $(dir $@)
	cp $< $@

# Sounds are stored uncompressed, so they can be played directly from the ROM (see Audio_N64.c)
$(BUILD_DIR)/sounds.extracted: misc/cc_audio.zip
	@mkdir -p $(N64_MKDFS_ROOT)/sounds
	python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" $< $(N64_MKDFS_ROOT)/sounds
	touch $@

$(N64_MKDFS_ROOT)/music/MUSIC%.WAV: misc/music/music%.wav
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/filesystem.dfs: $(ROMFS_FILES)

$(BUILD_DIR)/ClassiCube-n64.elf: $(OBJS)

-include $(wildcard $(BUILD_DIR)/*.d)
