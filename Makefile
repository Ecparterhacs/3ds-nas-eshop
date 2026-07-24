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
APP_AUTHOR      :=  3DS NAS eShop contributors

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

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

.PHONY: all clean test

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo "clean ..."
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

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

-include $(DEPSDIR)/*.d

endif
