ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
AR      := $(PREFIX)ar
LIBOGC  ?= $(DEVKITPRO)/libogc

CFLAGS  := -O2 -Wall -Wextra -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float \
           -Iinclude -I$(LIBOGC)/include
SRCS    := $(wildcard source/*.c)
OBJS    := $(patsubst source/%.c,build/%.o,$(SRCS))
LIB     := lib/libinput-wii.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $^

build/%.o: source/%.c source/internal.h include/input_wii.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

install: $(LIB)
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include
	cp $(LIB) $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib/
	cp include/input_wii.h $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include/

clean:
	rm -rf build lib

.PHONY: all install clean
