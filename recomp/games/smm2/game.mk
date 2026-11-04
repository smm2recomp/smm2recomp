# Super Mario Maker 2 (title 01009B90006DC000), version 3.0.3: what the shared Makefile needs to know.
GAME_ID        := smm2
GAME_TITLE     := Super Mario Maker 2
# (iOS / Android: home screens cut names past ~12 characters)
GAME_MOBILE_TITLE := SMMaker 2
GAME_BUNDLE_ID := org.smcraft.smm2recomp
GAME_TITLE_ID  := 0x01009B90006DC000
GAME_DISPLAY_VERSION := 3.0.3
# SMM2's code has no movie library: no runtime/hle/movie.c, no FFmpeg
GAME_MOVIES    := 0

# The game's glue, built into the executable with the shared runtime
GAME_SRC       := $(wildcard games/smm2/*.c) $(wildcard games/smm2/mods/*.c)
# The host-drawn SMM1 100 Mario screens (layout player, flow, ending): games/smm2/mods/smm1
GAME_SRC       += $(wildcard games/smm2/mods/smm1/*.c)
# SMM1's title letters on SMM2's title screen (a mod, off by default): games/smm2/mods/title_letters
GAME_SRC       += $(wildcard games/smm2/mods/title_letters/*.c)
# Guest symbols replaced by host code before recompiling (one mangled name per line; empty: none)
GAME_HLE       := games/smm2/hle_symbols.txt
# Per-instruction patches for the generator (none yet)
GAME_PATCHES   := games/smm2/patches.py
# Files the patches import (part of what the game code library is built from)
GAME_PATCH_DEPS := $(wildcard games/smm2/mods/*.py)

# Your own dump, extracted: the base game (v1.0.0) and the v3.0.3 update. `make smm2` reads them from here unless you
# pass other paths, e.g. make smm2 DUMP_BASE_ROMFS=/path/to/romfs. Only the update's exefs is used (an update ships
# the whole exefs); its romfs goes over the base game's.
DUMP_ROOT         ?= $(HOME)/smm-work/extracted
DUMP_BASE_ROMFS   ?= $(DUMP_ROOT)/smm2-romfs
DUMP_UPDATE_EXEFS ?= $(DUMP_ROOT)/smm2-v303-exefs
DUMP_UPDATE_ROMFS ?= $(DUMP_ROOT)/smm2-v303-romfs
SETUP_ARGS        ?= --base-romfs "$(DUMP_BASE_ROMFS)" --update-exefs "$(DUMP_UPDATE_EXEFS)" \
                     --update-romfs "$(DUMP_UPDATE_ROMFS)"
