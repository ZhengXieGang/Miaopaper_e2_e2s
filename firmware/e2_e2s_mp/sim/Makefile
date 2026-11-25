# sim/Makefile — host build of the real e2s_mp UI (no ESP-IDF, no SDL).
#
# Builds the *real* ../main/mp_ui.c + ../main/mp_hints.c + ../main/mp_font.c
# together with the real gfx.c / text component on a Linux host, and links the
# sim harness. If either UI file is missing (or UI=dummy/HINTS=dummy is
# forced), the corresponding schematic placeholder in sim/ is compiled instead
# and the binary prints a warning.
#
#   make -C sim                 # auto: real files when present, stub otherwise
#   make -C sim UI=real         # force the real renderer (fails if absent)
#   make -C sim UI=dummy        # force the schematic placeholder
#   make -C sim HINTS=real|dummy
#   make -C sim print-config
#   make -C sim clean
#
# Asset paths are baked in as the default for sim_text_init(); override at
# runtime with MP_SIM_ASSETS=/path/to/assets.

SIM_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(SIM_DIR)/..)
COMPONENTS := $(ROOT)/components
MAIN    := $(ROOT)/main
BUILD   := $(SIM_DIR)/build
BIN     := $(BUILD)/sim_mp

# The ask requires gcc; honor an explicit CC=... from the command line or the
# environment, but override make's built-in default ("cc").
CC ?= gcc
ifeq ($(origin CC),default)
CC := gcc
endif
TEXT_ASSETS ?= $(COMPONENTS)/text/assets

# Frozen host flags from docs/PORT_SPEC.md §9.3. -include stdbool.h is kept
# because gfx.c historically used bool without including <stdbool.h>.
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror
HOST_DEFS := -DTEXT_HOST_BUILD -include stdbool.h

override CPPFLAGS += -I$(MAIN) -I$(SIM_DIR) \
            -I$(COMPONENTS)/common/include \
            -I$(COMPONENTS)/display/include \
            -I$(COMPONENTS)/text/include \
            -I$(COMPONENTS)/text \
            -DSIM_ASSET_DIR_DEFAULT='"$(TEXT_ASSETS)"'

# --------------------------------------------------------------------------
# UI / hints source selection (independent: real UI may exist without hints).
# --------------------------------------------------------------------------
UI ?= auto
UI_SRC :=
UI_KIND :=
ifeq ($(UI),auto)
  ifeq ($(wildcard $(MAIN)/mp_ui.c),)
    UI_SRC := $(SIM_DIR)/stub_ui.c
    UI_KIND := dummy
  else
    UI_SRC := $(MAIN)/mp_ui.c
    UI_KIND := real
  endif
else ifeq ($(UI),real)
  ifeq ($(wildcard $(MAIN)/mp_ui.c),)
    $(error UI=real but $(MAIN)/mp_ui.c does not exist yet)
  endif
  UI_SRC := $(MAIN)/mp_ui.c
  UI_KIND := real
else ifeq ($(UI),dummy)
  UI_SRC := $(SIM_DIR)/stub_ui.c
  UI_KIND := dummy
else
  $(error UI must be auto, real or dummy (got '$(UI)'))
endif

HINTS ?= auto
HINTS_SRC :=
HINTS_KIND :=
ifeq ($(HINTS),auto)
  ifeq ($(wildcard $(MAIN)/mp_hints.c),)
    HINTS_SRC := $(SIM_DIR)/stub_hints.c
    HINTS_KIND := dummy
  else
    HINTS_SRC := $(MAIN)/mp_hints.c
    HINTS_KIND := real
  endif
else ifeq ($(HINTS),real)
  ifeq ($(wildcard $(MAIN)/mp_hints.c),)
    $(error HINTS=real but $(MAIN)/mp_hints.c does not exist yet)
  endif
  HINTS_SRC := $(MAIN)/mp_hints.c
  HINTS_KIND := real
else ifeq ($(HINTS),dummy)
  HINTS_SRC := $(SIM_DIR)/stub_hints.c
  HINTS_KIND := dummy
else
  $(error HINTS must be auto, real or dummy (got '$(HINTS)'))
endif

# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------
# sim_main.c is the harness (view models -> mp_ui_render -> PGM). sim_text.c
# injects the two font assets; sim_store.c holds the in-memory fixtures.
# mp_page.c IS compiled: READ fixtures go through the real paginator, and
# --check asserts its line widths; expected.h pins the rendered pixels.
SRCS := $(SIM_DIR)/sim_main.c \
        $(SIM_DIR)/sim_text.c \
        $(SIM_DIR)/sim_store.c \
        $(UI_SRC) \
        $(HINTS_SRC) \
        $(MAIN)/mp_font.c \
        $(MAIN)/mp_page.c \
        $(COMPONENTS)/display/gfx.c \
        $(COMPONENTS)/text/text_font.c \
        $(COMPONENTS)/text/text_encode.c \
        $(COMPONENTS)/text/font8x16.c \
        $(COMPONENTS)/common/crc32.c

HEADERS := $(wildcard $(SIM_DIR)/*.h) $(wildcard $(MAIN)/*.h) \
           $(wildcard $(MAIN)/fonts/*.h) \
           $(wildcard $(COMPONENTS)/text/*.h) \
           $(wildcard $(COMPONENTS)/text/include/*.h) \
           $(wildcard $(COMPONENTS)/display/include/*.h) \
           $(wildcard $(COMPONENTS)/common/include/*.h)

# Switching UI=dummy/real changes the prerequisite set but not the timestamps,
# so an explicit stamp is needed to force a relink when the configuration (or
# the compiler flags that matter) changes.
CONFIG_STAMP := $(BUILD)/config.stamp

.DEFAULT_GOAL := all

.PHONY: all clean print-config FORCE

$(CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD)
	@printf 'ui=%s\nhints=%s\ncc=%s\ncflags=%s\ndefs=%s\n' \
	    '$(UI_KIND)' '$(HINTS_KIND)' '$(CC)' '$(CFLAGS)' '$(CPPFLAGS)' > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

all: $(BIN)

$(BIN): $(SRCS) $(HEADERS) $(CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(HOST_DEFS) $(CPPFLAGS) \
	    -DSIM_UI_KIND='"$(UI_KIND)"' -DSIM_HINTS_KIND='"$(HINTS_KIND)"' \
	    $(SRCS) -o $@

print-config:
	@echo "sim_dir      $(SIM_DIR)"
	@echo "root         $(ROOT)"
	@echo "components   $(COMPONENTS)"
	@echo "ui           $(UI_KIND)  ($(UI_SRC))"
	@echo "hints        $(HINTS_KIND)  ($(HINTS_SRC))"
	@echo "assets       $(TEXT_ASSETS)"
	@echo "bin          $(BIN)"

clean:
	rm -rf $(BUILD)
