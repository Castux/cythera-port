# Delver runtime for Cythera
CC      ?= cc
PYTHON  ?= python3
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2)
ifeq ($(OS),Windows_NT)
# plain main() with a console for the log (see SDL_SetMainReady in host/sdl.c)
SDL_LIBS   := $(filter-out -lSDL2main -mwindows,$(SDL_LIBS))
EXE        := .exe
ifeq ($(GUI),1)
override LDFLAGS += -mwindows  # no console window (packaged builds)
endif
endif
# Optional FreeType for hinted (crisp) TrueType text: tools/build_freetype.sh, else the system's
FT_DIR  := third_party/freetype
ifneq ($(wildcard $(FT_DIR)/lib/libfreetype.a),)
FT_CFLAGS := -DHAVE_FREETYPE -I$(FT_DIR)/include
FT_LIBS   := $(FT_DIR)/lib/libfreetype.a
else ifneq ($(shell pkg-config --exists freetype2 2>/dev/null && echo y),)
FT_CFLAGS := -DHAVE_FREETYPE $(shell pkg-config --cflags freetype2)
FT_LIBS   := $(shell pkg-config --libs freetype2)
endif
# LICENSE_BYPASS=0 runs the original code unaltered (no shareware registration bypass)
LICENSE_BYPASS ?= 1
ifneq ($(LICENSE_BYPASS),0)
OPT_CFLAGS := -DLICENSE_BYPASS
endif
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(SDL_CFLAGS) $(FT_CFLAGS) $(OPT_CFLAGS)
override LDLIBS += $(FT_LIBS) $(SDL_LIBS) -lm -lpthread

BUILD   := build
SRCS    := $(wildcard src/*.c src/cpu/*.c src/loader/*.c src/os/*.c src/host/*.c)
OS_SRCS := $(wildcard src/os/*.c)
GEN     := $(BUILD)/gen/trap_table.c
OBJS    := $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/gen/trap_table.o
DEPS    := $(OBJS:.o=.d)

all: $(BUILD)/cythera$(EXE)

$(BUILD)/cythera$(EXE): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(GEN): $(OS_SRCS) tools/gen_traps.py
	$(PYTHON) tools/gen_traps.py $@ $(OS_SRCS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/gen/trap_table.o: $(GEN)
	$(CC) $(CFLAGS) -c -o $@ $<

# CPU unit tests
$(BUILD)/test_ppc: tests/test_ppc.c src/cpu/ppc.c src/util.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DPPC_TEST -o $@ $^ -lm

test: $(BUILD)/test_ppc
	$(BUILD)/test_ppc

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
-include $(DEPS)
