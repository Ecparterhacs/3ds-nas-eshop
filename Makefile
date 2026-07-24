#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifneq ($(MAKECMDGOALS),test)
  ifeq ($(strip $(DEVKITARM)),)
    $(error "Please set DEVKITARM in your environment")
  endif
  include $(DEVKITARM)/3ds_rules
endif

TOPDIR ?= $(CURDIR)

TARGET          :=  3ds-eshop-client
BUILD           :=  build
SOURCES         :=  source
INCLUDES        :=  include
NAS_HOST        ?=  192.168.1.100
NAS_PORT        ?=  40441

ARCH            :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS          :=  -g -Wall -Wextra -O2 -std=gnu11 -mword-relocations \
                    -ffunction-sections $(ARCH)
CFLAGS          +=  $(INCLUDE) -D__3DS__
CFLAGS          +=  -DNAS_HOST=\"$(NAS_HOST)\" -DNAS_PORT=$(NAS_PORT)
ASFLAGS         :=  -g $(ARCH)
LDFLAGS         :=  -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# Static library order matters: citro2d -> citro3d -> libctru.
LIBS            :=  -lcitro2d -lcitro3d -lctru -lm
LIBDIRS         :=  $(CTRULIB)

APP_TITLE       :=  3DS eShop Client
APP_DESCRIPTION :=  Browse games from your NAS
APP_AUTHOR      :=  Ecparterhacs
APP_ICON        :=  $(TOPDIR)/assets/icon.png
APP_VERSION     :=  1.0.0

MAKEROM         ?=  makerom
BANNERTOOL      ?=  bannertool
CIA_RSF         :=  $(TOPDIR)/assets/app.rsf
CIA_BANNER      :=  $(TOPDIR)/assets/banner.png
CIA_AUDIO       :=  $(TOPDIR)/assets/banner.wav
CIA_ICON        :=  $(TOPDIR)/assets/icon.png

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)
export APP_ICON CIA_RSF CIA_BANNER CIA_AUDIO CIA_ICON
export MAKEROM BANNERTOOL

CFILES          :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES          :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD       :=  $(CC)
export OFILES   :=  $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS := $(OUTPUT).smdh
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh

.PHONY: all 3dsx cia release clean test

all: 3dsx

3dsx: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile \
		$(OUTPUT).3dsx

cia: $(BUILD)
	@command -v "$(MAKEROM)" >/dev/null 2>&1 || \
		{ echo "makerom not found; install it or set MAKEROM=/path/to/makerom"; exit 1; }
	@command -v "$(BANNERTOOL)" >/dev/null 2>&1 || \
		{ echo "bannertool not found; install it or set BANNERTOOL=/path/to/bannertool"; exit 1; }
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile \
		$(OUTPUT).cia

release: $(BUILD)
	@command -v "$(MAKEROM)" >/dev/null 2>&1 || \
		{ echo "makerom not found; install it or set MAKEROM=/path/to/makerom"; exit 1; }
	@command -v "$(BANNERTOOL)" >/dev/null 2>&1 || \
		{ echo "bannertool not found; install it or set BANNERTOOL=/path/to/bannertool"; exit 1; }
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile \
		$(OUTPUT).3dsx $(OUTPUT).cia

$(BUILD):
	@mkdir -p $@

clean:
	@echo "clean ..."
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf \
		$(TARGET).cia

test:
	@cc -std=c11 -Wall -Wextra -Werror -Iinclude \
		source/game.c tests/test_game_parser.c -o /tmp/3ds-eshop-parser-test
	@/tmp/3ds-eshop-parser-test
	@cc -std=c11 -Wall -Wextra -Werror -Iinclude \
		source/cia_util.c tests/test_cia_parser.c -o /tmp/3ds-eshop-cia-test
	@/tmp/3ds-eshop-cia-test

else

$(OUTPUT).3dsx: $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf: $(OFILES)

$(OUTPUT).cia: $(OUTPUT).elf $(CIA_RSF) $(CIA_BANNER) $(CIA_AUDIO) $(CIA_ICON)
	@$(BANNERTOOL) makebanner -i "$(CIA_BANNER)" -a "$(CIA_AUDIO)" \
		-o "$(DEPSDIR)/banner.bnr"
	@$(BANNERTOOL) makesmdh \
		-s "$(APP_TITLE)" \
		-l "$(APP_DESCRIPTION)" \
		-p "$(APP_AUTHOR)" \
		-scs "3DS NAS 商店" \
		-scl "浏览并安装 NAS 中的游戏" \
		-scp "$(APP_AUTHOR)" \
		-i "$(CIA_ICON)" \
		-r regionfree \
		-f visible \
		-o "$(DEPSDIR)/icon.icn"
	@$(MAKEROM) -f cia -target t -exefslogo \
		-o "$(OUTPUT).cia" \
		-elf "$(OUTPUT).elf" \
		-rsf "$(CIA_RSF)" \
		-banner "$(DEPSDIR)/banner.bnr" \
		-icon "$(DEPSDIR)/icon.icn" \
		-major 1 -minor 0 -micro 0

-include $(DEPSDIR)/*.d

endif
