# Host (Linux) build of the recompiled Diablo II: make -j16
CC ?= clang
GEN := gen
RT := runtime
CFLAGS_GEN := -O2 -g1 -w -I$(RT) -I$(GEN) -fno-strict-aliasing
CFLAGS_RT := -O1 -g -Wall -Wno-unused -I$(RT) -I$(GEN) -fno-strict-aliasing $(shell sdl2-config --cflags)
LIBS := $(shell sdl2-config --libs) -lGLESv2 -lSDL2_ttf -lm -lpthread -rdynamic
GEN_SRC := $(wildcard $(GEN)/*.c)
RT_SRC := $(wildcard $(RT)/*.c)
OBJ := $(patsubst $(GEN)/%.c,build/obj/%.o,$(GEN_SRC)) $(patsubst $(RT)/%.c,build/rt/%.o,$(RT_SRC))

build/d2recomp: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LIBS)
build/obj/%.o: $(GEN)/%.c $(RT)/cpu.h
	@mkdir -p build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@
build/rt/%.o: $(RT)/%.c $(RT)/cpu.h $(RT)/native.h
	@mkdir -p build/rt
	$(CC) $(CFLAGS_RT) -c $< -o $@
clean:
	rm -rf build
