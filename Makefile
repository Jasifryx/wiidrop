#-------------------------------------------------------------------------------
# WiiDrop - builds wiidrop.rpx and wiidrop.wuhb (Aroma)
# Needs devkitPro with: wut wut-tools wiiu-curl wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_mixer wiiu-sdl2_image
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/wut/share/wut_rules

APP_NAME      := WiiDrop
APP_SHORTNAME := WiiDrop
APP_AUTHOR    := WiiDrop contributors

TARGET   := wiidrop
BUILD    := build
SOURCES  := source source/lzma source/zstd/common source/zstd/decompress
DATA     := data          # files here are embedded into the app (data/foo.mp3 -> foo_mp3[])
INCLUDES := source
ICON     := $(wildcard $(TOPDIR)/meta/icon.png)

#-------------------------------------------------------------------------------
CFLAGS   := -g -Wall -O2 -ffunction-sections $(MACHDEP)
CFLAGS   += $(INCLUDE) -D__WIIU__ -D__WUT__ -DHAVE_BZIP2 -DZSTD_DISABLE_ASM
CXXFLAGS := $(CFLAGS)
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

# SDL2 (GUI + music) and its deps, then libcurl (8.7.x: mbedtls, zlib, brotli).
# pkg-config isn't wired up for the Wii U portlibs, so the static link order is listed by hand.
LIBS     := -lSDL2_image -ljpeg -lwebpdemux -lwebp -lsharpyuv \
            -lSDL2_mixer -lmodplug -lmpg123 -lvorbisfile -lvorbis -logg \
            -lSDL2_ttf -lharfbuzz -lfreetype -lpng16 -lbz2 \
            -lSDL2 \
            -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lbrotlidec -lbrotlicommon -lz \
            -lstdc++ -lwut -lm

LIBDIRS  := $(PORTLIBS) $(WUT_ROOT)

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(strip $(DATA)),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
BINFILES := $(foreach dir,$(strip $(DATA)),$(notdir $(wildcard $(dir)/*.*)))

# harfbuzz/modplug are C++, so link with the C++ driver
export LD          := $(CXX)
export OFILES_BIN  := $(addsuffix .o,$(BINFILES))
export OFILES_SRC  := $(CFILES:.c=.o)
export OFILES      := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN  := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export INCLUDE     := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                      $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                      $(foreach dir,$(PORTLIBS),-I$(dir)/include/SDL2) \
                      -I$(CURDIR)/$(BUILD)
export LIBPATHS    := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

WUHB_ARGS := --name="$(APP_NAME)" --short-name="$(APP_SHORTNAME)" --author="$(APP_AUTHOR)"
ifneq ($(ICON),)
WUHB_ARGS += --icon="$(ICON)"
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)
	@echo creating $(TARGET).wuhb
	@wuhbtool $(OUTPUT).rpx $(OUTPUT).wuhb $(WUHB_ARGS)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).rpx $(TARGET).elf $(TARGET).wuhb

#-------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).rpx

$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

# embed data files: data/foo.mp3 -> foo_mp3.o + foo_mp3.h
%.mp3.o %_mp3.h: %.mp3
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
