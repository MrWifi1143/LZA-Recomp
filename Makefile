# LZ&A Recomp build (from Let's Go Recomp's / LAS Recomp's). Linux (clang + Vulkan) is the development platform; the
# Windows release is cross-built from Linux with zig (tools/windows/README.md).
#   make gen           translate your own game files (extracted/exefs) into generated/
#   make -j20 all      build/lasrecomp (runtime) and build/lasgame.so (the game code)
#   make dist          the Windows release zip (LZA_WINBUILD=<folder>, tools/windows/README.md): the exe cross-built,
#                      then $(LZA_WINBUILD)/dist/LZA-Recomp-<tag>-windows.zip
UNAME := $(shell uname -s)
comma := ,
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else
PLATFORM := linux
endif

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. DEPS: a prefix with SDL3 / FFmpeg to use instead of the system's (MSYS2's).
B         ?= build
DEPS      ?=
# EXE_NAME: the exe's name (the release's: WIN_EXE); the game code library imports from the exe by it
EXE_NAME  ?= lasrecomp
CC       := clang $(CROSS)
CXX      := clang++ $(CROSS)
IMGUI    := third_party/imgui
# -fdenormal-fp-math=dynamic: the guest's FPCR.FZ runs as the host's own flush to zero (MXCSR FTZ/DAZ, simd.h), so the
# compiler must not assume IEEE denormals (it turned a bit test into a compare with 0.0, which DAZ makes true)
GENFLAGS := -O1 -fno-strict-aliasing -fdenormal-fp-math=dynamic -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c) $(wildcard $(GEN)/subsdk0/*.c) $(wildcard $(GEN)/subsdk1/*.c)
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/$(PLATFORM).c
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifeq ($(PLATFORM),linux)
EXE      := $(B)/lasrecomp
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows: AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format  # (%llx for uint64_t, which is unsigned long here: the same size)
SHADERC  ?= -lshaderc  # (Debian / Ubuntu's libshaderc.so; a shaderc built from source: -lshaderc_shared)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++
GAMELIB  := $(B)/lasgame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared
else
EXE      := $(B)/$(EXE_NAME).exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)  # release: no console window (log -> lasrecomp.log)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o  # the .exe's icon (Explorer, taskbar)
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++
GAMELIB  := $(B)/lasgame.dll
# the runtime functions it calls: exported by the .exe, linked through its import library
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/lasrecomp.dll.a
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/lasrecomp.dll.a
GAMELIB_SHARED := -shared  # (+ the import library: runtime/gamebuild.c)
endif

# Research build (LARECOMP_TRACE_STORES): make TRACE=1 B=build/trace GEN=generated-trace gen / -j20 all.
# Every store of the game reports its instruction to runtime/storetrace.c.
ifdef TRACE
GENFLAGS += -DSTORE_TRACE
GENTRACE := --trace-stores
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
RTFLAGS  := -O2 -g -fno-strict-aliasing -fdenormal-fp-math=dynamic -Wall -Wno-unused-function -Iruntime -Iruntime/nvn -Ithird_party $(DEPINC) $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn -Ithird_party $(DEPINC)

all: $(EXE) $(if $(strip $(GEN_SRC)),$(GAMELIB))  # (strip: an empty wildcard list is spaces, which $(if) took as true)

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

# The shader caches (cache/xlat, cache/spirv, the warm-up's marker) key on the translator's own source, not on when it
# was compiled: any header change recompiles it, which wiped the caches on almost every build (runtime/nvn/maxwell)
XLAT_SRCS := runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
XLAT_HASH := $(shell cat $(XLAT_SRCS) | sha256sum | cut -c1-16)
$(B)/rt/nvn/maxwell/translate.o: RTFLAGS += -DXLAT_SRC_HASH='"$(XLAT_HASH)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file. The temp file keeps
# the real name (in a folder of its own): on Windows the import library records the name the game code library loads
# it by.
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Python: .venv (Windows: Scripts/python.exe; Linux: bin/python)
PY := $(firstword $(wildcard .venv/Scripts/python.exe) $(wildcard .venv/bin/python) $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))

gen:
	cd tools && $(abspath $(PY)) gen.py sdk ../extracted/exefs/sdk 0x7008000000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt $(GENSIMD) $(GENTRACE)
	cd tools && $(abspath $(PY)) gen.py main ../extracted/exefs/main 0x7000000000 ../$(GEN)/main $(GENSIMD) $(GENTRACE)
	if [ -f extracted/exefs/subsdk0 ]; then cd tools && $(abspath $(PY)) gen.py subsdk0 ../extracted/exefs/subsdk0 0x700A000000 ../$(GEN)/subsdk0 $(GENSIMD) $(GENTRACE); fi
	if [ -f extracted/exefs/subsdk1 ]; then cd tools && $(abspath $(PY)) gen.py subsdk1 ../extracted/exefs/subsdk1 0x700C000000 ../$(GEN)/subsdk1 $(GENSIMD) $(GENTRACE); fi

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc docs/icon/icon.ico runtime/platform/windows.manifest
	@mkdir -p $(dir $@)
	$(WINDRES) -I docs/icon -I runtime/platform -O coff $< -o $@

# Windows release, cross-built from Linux (tools/windows/README.md): build/win/$(WIN_EXE).exe and its import library,
# then $(LZA_WINBUILD)/dist/LZA-Recomp-$(WIN_TAG)-windows.zip with what the start screen builds the game code with on
# the player's PC (runtime/gamebuild.c). No game code, no game files.
WIN_EXE  ?= ZARecomp
WIN_TAG  ?= ZA
win-exe:
	tools/windows/build_exe.sh $(WIN_EXE)
dist: win-exe
	tools/windows/make_zip.sh $(WIN_TAG) $(WIN_EXE)

.PHONY: all gen gen-objs runtime-objs win-exe dist
