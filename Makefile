ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
AR      := $(PREFIX)ar
LIBOGC  ?= $(DEVKITPRO)/libogc

CFLAGS  := -O2 -Wall -Wextra -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float \
           -Iinclude -I$(LIBOGC)/include
WITH_WIIDRC    ?= 0
WITH_WUPC      ?= 0
WITH_SICKSAXIS ?= 0
PORTLIBS       ?= $(CURDIR)/portlibs/ppc
CFLAGS += -I$(PORTLIBS)/include
ifeq ($(WITH_WIIDRC),1)
 CFLAGS += -DHAVE_WIIDRC
endif
ifeq ($(WITH_WUPC),1)
 CFLAGS += -DHAVE_WUPC
endif
ifeq ($(WITH_SICKSAXIS),1)
 CFLAGS += -DHAVE_SICKSAXIS
endif

SRCS    := $(wildcard source/*.c)
OBJS    := $(patsubst source/%.c,build/%.o,$(SRCS))
LIB     := lib/libinput-wii.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $^

build/%.o: source/%.c source/libinput_internal.h include/input_wii.h include/input_wiispeak.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

install: $(LIB)
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include
	cp $(LIB) $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib/
	cp include/input_wii.h include/input_wiispeak.h $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include/

clean:
	rm -rf build lib

controllers:
	./scripts/build_controller_portlibs.sh

.PHONY: all install clean controllers
