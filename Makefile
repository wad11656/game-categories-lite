TARGET = category_lite
# Release builds should not synchronously write the very chatty XMB diagnostic
# log. Use `make DEBUG=1` only when producing a troubleshooting build.
DEBUG ?= 0
STUBS = imports.o scePaf.o func_stubs.o
CATEGORY_MODES = multims.o context.o vshitem.o mode.o selection.o
HELPER = clearcache.o  logger.o utils.o config.o filter.o
OBJS = main.o category.o gcread.o gcpatches.o sysconf.o language.o
OBJS += $(CATEGORY_MODES) $(HELPER) $(STUBS) 
LIBS =  -lpsprtc -lpspreg

EXTRA_TARGETS = category_lang
EXTRA_CLEAN = category_lite_??.h

ifeq (x$(CONFIG_LANG), x)
CONFIG_LANG = en
endif

# use a psp-filer with VSH support or the plugin won't load on PRO firmware
# edit: 6.20 Pro won't work with this. Bugs, bugs everywhere....
all: category_lang install-release
#	-psp-packer category_lite.prx

category_lang:
	bin2c lang/category_lite_$(CONFIG_LANG).txt category_lite_lang.h category_lite_lang

# Drop a copy of the built PRX into release/ so it's the single canonical
# location for the ready-to-flash binary.
.PHONY: install-release
install-release: $(TARGET).prx
	@mkdir -p release
	@cp -f $(TARGET).prx release/$(TARGET).prx
	@echo "  -> release/$(TARGET).prx"

EXTRA_WARNS= -Wextra -Wfloat-equal -Wundef -Wshadow -Wpointer-arith -Wwrite-strings -Wunreachable-code
CFLAGS =-O2 -Wall -G0 -std=c99 -fshort-wchar -fcommon $(EXTRA_WARNS)

ifeq ($(DEBUG), 1)
CFLAGS+=-DDEBUG -DGCLITE_LOGGING=1
endif

ifeq ($(BENCHMARK), 1)
CFLAGS+=-DBENCHMARK
endif

ASFLAGS = $(CFLAGS)
LDFLAGS = -nostartfiles

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_USER_LIBS = 1
USE_USER_LIBC = 1

PSP_FW_VERSION=620

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
