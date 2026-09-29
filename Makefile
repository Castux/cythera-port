# Delver runtime for Cythera
CC      ?= cc
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2)
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(SDL_CFLAGS)
override LDLIBS += $(SDL_LIBS) -lm -lpthread

BUILD   := build
SRCS    := $(wildcard src/*.c src/cpu/*.c src/loader/*.c src/os/*.c src/host/*.c)
OS_SRCS := $(wildcard src/os/*.c)
GEN     := $(BUILD)/gen/trap_table.c
OBJS    := $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/gen/trap_table.o
DEPS    := $(OBJS:.o=.d)

all: $(BUILD)/cythera

$(BUILD)/cythera: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(GEN): $(OS_SRCS) tools/gen_traps.py
	python3 tools/gen_traps.py $@ $(OS_SRCS)

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
