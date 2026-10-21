# Tablet painting demo.  `make` builds boot.dol; `make run` sends it to a Wii
# running the Homebrew Channel (WIILOAD=tcp:<wii-ip>).

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
LIBOGC  ?= $(DEVKITPRO)/libogc
LIB     := ../..

CFLAGS  := -O2 -Wall -Wextra -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float \
           -I$(LIB)/include -I$(LIB)/source -I$(LIBOGC)/include
LDFLAGS := -mrvl -mcpu=750 -meabi -mhard-float -L$(LIBOGC)/lib/wii
LIBS    := -lfat -lwiikeyboard -lwiiuse -lbte -logc -lm

SRCS    := main.c $(wildcard $(LIB)/source/*.c)
OBJS    := $(patsubst %.c,build/%.o,$(notdir $(SRCS)))

vpath %.c . $(LIB)/source

all: boot.dol

boot.dol: boot.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@

boot.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

build/%.o: %.c $(wildcard $(LIB)/include/*.h $(LIB)/source/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

run: boot.dol
	$(DEVKITPRO)/tools/bin/wiiload $<

clean:
	rm -rf build boot.elf boot.dol

.PHONY: all run clean
