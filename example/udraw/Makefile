# uDraw GameTablet painting demo.  `make` builds boot.dol; `make run` sends it
# to a Wii running the Homebrew Channel (WIILOAD=tcp:<wii-ip>).  `make TEST=1`
# builds the headless test version (see main.c).

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
LIBOGC  ?= $(DEVKITPRO)/libogc
LIB     := ../..

CFLAGS  := -O2 -Wall -Wextra -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float \
           -DHAVE_TABLET -I$(LIB)/include -I$(LIB)/source -I$(LIBOGC)/include
# wiiuse would take the tablet for a Classic Controller without these
LDFLAGS := -mrvl -mcpu=750 -meabi -mhard-float -L$(LIBOGC)/lib/wii \
           -Wl,--wrap=classic_ctrl_handshake,--wrap=parse_event
LIBS    := -lfat -lwiikeyboard -lwiiuse -lbte -logc -lm
BUILD   := build

ifeq ($(TEST),1)
CFLAGS  += -DUDRAW_TEST
BUILD   := build-test
endif

SRCS    := main.c $(wildcard $(LIB)/source/*.c)
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))

vpath %.c . $(LIB)/source

all: boot.dol

boot.dol: boot.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@

boot.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.o: %.c $(wildcard $(LIB)/include/*.h $(LIB)/source/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

run: boot.dol
	$(DEVKITPRO)/tools/bin/wiiload $<

clean:
	rm -rf build build-test boot.elf boot.dol

.PHONY: all run clean
