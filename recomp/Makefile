# Recomp build (forked from LAS Recomp's Makefile; see UPSTREAM.md).
#
#   make smm2                      the whole pipeline for Super Mario Maker 2 v3.0.3 from your own dump:
#                                  set up the game files, recompile main + sdk to C, build, (then `make run-smm2`)
#   make GAME=smm2 <target>        one step: setup | gen | all | run | runtime-objs | clean-gen
#
# The shared runtime is runtime/ (kernel, IPC, services, NVN on Metal / Vulkan, platform layers) and the recompiler
# is tools/. A game is games/<game>/: game.mk (identity, modules, dump layout) and its own glue code.
# Everything made from the dump goes to private/ (game files), generated/ (recompiled C) and build/; git ignores
# them and ../scripts/check_no_proprietary.py blocks them. They may be symlinks to another disk.
#
# macOS: Homebrew clang/SDL3/FFmpeg, Metal renderer.  Linux: clang + Vulkan.  Windows: MSYS2 UCRT64 + Vulkan.
# Cross builds (from macOS or any host, with zig as the compiler: tools/fetch_zig.sh, or ZIG=<zig>):
#   make TARGET=linux-aarch64 | linux-x86_64 | windows-x86_64 GAME=smm2 all   (DEPS=<headers + link libraries>)
# iOS / iPadOS (Xcode's clang; the game code is linked into the app - nothing compiles on the device):
#   make TARGET=ios-arm64 | iossim-arm64 GAME=smm2 DEPS=<SDL3 for iOS> app   (tools/make_ios_app.sh signs it)
# Android (the NDK's clang; the game code is built here and packaged into the APK as libsmm2game.so):
#   make TARGET=android-arm64 GAME=smm2 DEPS=<SDL3 + shaderc for Android> apk install   (tools/make_android_apk.sh)
UNAME := $(shell uname -s)
comma := ,
TARGET ?=
ifneq ($(TARGET),)
PLATFORM := $(word 1,$(subst -, ,$(TARGET)))
ARCH     := $(word 2,$(subst -, ,$(TARGET)))
else ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Linux)
PLATFORM := linux
else
PLATFORM := macos
# (macOS: Apple silicon or Intel - built natively, there is no cross target)
ARCH     := $(patsubst arm64,aarch64,$(shell uname -m))
endif

GAME     ?= smm2
GAMEDIR  := games/$(GAME)
GAME_MOVIES := 1  # (runtime/hle/movie.c + FFmpeg: games whose code uses the movie library; game.mk may say 0)
include $(GAMEDIR)/game.mk

# Game files (set up by `make setup` from the dump), recompiled code, build products: one folder each per game.
DATA     ?= private/$(GAME)
GAMEFILES := $(DATA)/extracted
GEN      ?= generated/$(GAME)
B        ?= build/$(GAME)

# The guest window's base (runtime/rt.h GUEST_ORIGIN): the game code is generated for it. Another base gets its own
# generated/ and build/ folders.
ifeq ($(PLATFORM),android)
# Android arm64 kernels often give apps a 39-bit address space with libraries mapped (randomly) from its top down,
# over 0x7000000000; the window sits low instead, where nothing maps by default (DECISIONS.md, "Android").
GUEST_ORIGIN ?= 0x800000000
endif
GUEST_ORIGIN ?= 0x7000000000

IOS := $(filter ios iossim,$(PLATFORM))
ifneq ($(IOS),)
# iOS / the Simulator: Xcode's clang for the SDK. The game code is generated/<game> (arm64, as macOS), compiled into
# the app's executable (iOS loads no code it didn't ship with: no game code library, no first-launch build).
IOS_SDK  := $(if $(filter iossim,$(PLATFORM)),iphonesimulator,iphoneos)
IOS_MIN  ?= 17.0
IOS_TRIPLE := arm64-apple-ios$(IOS_MIN)$(if $(filter iossim,$(PLATFORM)),-simulator)
CROSS_CC := xcrun --sdk $(IOS_SDK) clang -target $(IOS_TRIPLE)
CROSS_CXX := xcrun --sdk $(IOS_SDK) clang++ -target $(IOS_TRIPLE)
B        := build/$(GAME)-$(TARGET)
else ifeq ($(PLATFORM),android)
# Android: the NDK's clang (ANDROID_NDK, or the newest in $(ANDROID_HOME)/ndk). The runtime is libmain.so (SDL's
# activity loads it and calls SDL_main), the game code libsmm2game.so next to it; both go into the APK. aarch64 only
# (arm64-v8a): the generated code is the arm64 one (FP/SIMD as inline asm), at the Android GUEST_ORIGIN.
ANDROID_HOME ?= $(or $(ANDROID_SDK_ROOT),$(HOME)/Library/Android/sdk)
ANDROID_NDK ?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/ndk/*)))
ANDROID_API ?= 33  # (Android 13: the first with Vulkan 1.3 in its loader, which the backend needs)
NDK_BIN  := $(firstword $(wildcard $(ANDROID_NDK)/toolchains/llvm/prebuilt/*/bin))
CROSS_CC := $(NDK_BIN)/clang --target=aarch64-linux-android$(ANDROID_API)
CROSS_CXX := $(NDK_BIN)/clang++ --target=aarch64-linux-android$(ANDROID_API)
B        := build/$(GAME)-$(TARGET)
GENSIMD  := --simd asm
else ifneq ($(TARGET),)
ifeq ($(PLATFORM),macos)
$(error macOS has no cross target - build it natively: make -C recomp smm2)
endif
# zig cc / c++ for the target; the generated code depends only on the CPU's FP/SIMD lowering (asm on aarch64, C on
# x86-64), so targets share it: generated/<game> (aarch64, as macOS) or generated/<game>-c (x86-64)
ZIG      ?= $(firstword $(wildcard build/zig/zig) zig)
ZTARGET  := $(ARCH)-$(if $(filter windows,$(PLATFORM)),windows-gnu,linux-gnu.2.36)
CROSS_CC := $(ZIG) cc -target $(ZTARGET)
CROSS_CXX := $(ZIG) c++ -target $(ZTARGET)
B        := build/$(GAME)-$(TARGET)
ifeq ($(ARCH),x86_64)
GEN      := generated/$(GAME)-c
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
else ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated/$(GAME)-$(ARCH)
B        := build/$(GAME)-$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
else ifeq ($(PLATFORM),macos)
# macOS: Apple's clang, Metal. Built natively (no cross target): the game code's FP/SIMD lowering follows the host -
# the asm one (the guest's own NEON, bit-exact) on Apple silicon, the portable C one on x86-64 (as on Windows / Linux x64).
ifeq ($(ARCH),x86_64)
GEN      := generated/$(GAME)-c
# AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
GENARCH  ?= -march=x86-64-v3
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

MACOS_MIN ?=
DEPS      ?=
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := $(or $(CROSS_CC),clang $(MINFLAG) $(CROSS))
CXX      := $(or $(CROSS_CXX),clang++ $(MINFLAG) $(CROSS))
LLD      := $(if $(CROSS_CC),,-fuse-ld=lld)  # (zig links with its own lld)
PLATFORM_SRC := runtime/platform/$(if $(IOS),macos,$(patsubst android,linux,$(PLATFORM))).c  # (iOS: the macOS layer, TARGET_OS_IPHONE inside; Android: the Linux one, __ANDROID__ inside)
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime
# the name players see: the game's, a shorter one on phones and tablets (game.mk GAME_MOBILE_TITLE)
GAME_TITLE := $(if $(filter ios iossim android,$(PLATFORM)),$(or $(GAME_MOBILE_TITLE),$(GAME_TITLE)),$(GAME_TITLE))
GAMEDEFS := -DGAME_TITLE_ID=$(GAME_TITLE_ID)ull -DGAME_ID='"$(GAME_ID)"' -DGAME_TITLE='"$(GAME_TITLE)"' -DGAME_BUNDLE_ID='"$(GAME_BUNDLE_ID)"' \
            -DGAME_DISPLAY_VERSION='"$(GAME_DISPLAY_VERSION)"'

ifneq ($(GUEST_ORIGIN),0x7000000000)
GEN      := $(GEN)-$(GUEST_ORIGIN)
B        := $(B)-$(GUEST_ORIGIN)
GAMEDEFS += -DGUEST_ORIGIN=$(GUEST_ORIGIN)ull
endif
GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) $(PLATFORM_SRC) \
            $(wildcard runtime/net/*.c) \
            $(GAME_SRC)
ifeq ($(PLATFORM),windows)
RT_SRC   += runtime/platform/windows_exports.c  # (what the game code DLL calls in the exe)
endif
ifeq ($(strip $(GAME_MOVIES)),0)
RT_SRC   := $(filter-out runtime/hle/movie.c,$(RT_SRC))
FFMPEG_LIBS :=
else
FFMPEG_LIBS := -lavformat -lavcodec -lavutil -lswscale
endif
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h) \
            $(wildcard runtime/net/*.h) \
            $(GAMEDIR)/game.mk
UI_SRC   := $(filter-out runtime/ui/nextendo_ios.mm,$(wildcard runtime/ui/*.mm))
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifneq ($(IOS),)
SDL      := $(or $(DEPS),/Volumes/SSD/smm-work/deps/ios-$(if $(filter iossim,$(PLATFORM)),sim,device))
EXE      := $(B)/$(GAME_ID)recomp
DEPINC   := -I$(SDL)/include
OSFLAGS  := -DGAMELIB_STATIC
RT_SRC   := $(filter-out runtime/gamebuild.c,$(RT_SRC))
LDFLAGS  := -L$(SDL)/lib -lSDL3 -framework CoreFoundation -framework Foundation -framework Metal $(if $(filter ios,$(PLATFORM)),-framework MetalFX) \
            -framework QuartzCore -framework UIKit -framework CoreText -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers \
            -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreHaptics -framework CoreMotion \
            -framework GameController -framework CoreBluetooth -framework OpenGLES -framework CoreVideo -framework CoreMedia -framework CoreServices
BACKEND_SRC := $(wildcard runtime/nvn/*.m)
BACKEND_OBJ := $(patsubst runtime/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
IMGUI_OBJ += $(B)/imgui/imgui_impl_metal.o
UIFLAGS  := -fobjc-arc
GAMELIB  :=
EXE_GAME_OBJ := $(GEN_OBJ)  # (linked in)
APP      := $(B)/SMM2Recomp.app
else ifeq ($(PLATFORM),macos)
# (Intel's Homebrew is /usr/local, Apple silicon's is /opt/homebrew)
HOMEBREW := $(if $(filter x86_64,$(ARCH)),/usr/local,/opt/homebrew)
SDL      := $(or $(DEPS),$(HOMEBREW)/opt/sdl3)
FFMPEG   := $(or $(DEPS),$(HOMEBREW)/opt/ffmpeg)
# (only when set: an empty append would leave a trailing space in GENFLAGS, which is part of GAMELIB_ID, and make
# every Apple silicon player's game code library be rebuilt)
ifneq ($(GENARCH),)
GENFLAGS += $(GENARCH)
endif
EXE      := $(B)/$(GAME_ID)recomp
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib $(if $(FFMPEG_LIBS),-L$(FFMPEG)/lib $(FFMPEG_LIBS) -Wl$(comma)-rpath$(comma)$(FFMPEG)/lib) \
            -framework CoreFoundation -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore -framework AppKit -framework CoreText -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
BACKEND_SRC := $(wildcard runtime/nvn/*.m)
BACKEND_OBJ := $(patsubst runtime/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
IMGUI_OBJ += $(B)/imgui/imgui_impl_metal.o
UIFLAGS  := -fobjc-arc
GAMELIB  := $(B)/$(GAME_ID)game.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup
else ifeq ($(PLATFORM),android)
# libmain.so: the runtime with SDL's entry (SDL_main), Vulkan, shaderc (static), libc++ static. The game code library
# links against it (its runtime calls resolve there) and is loaded by its soname from the APK (runtime/gamelib.c).
SDL      := $(or $(DEPS),/Volumes/SSD/smm-work/deps/android-arm64)
EXE      := $(B)/libmain.so
DEPINC   := -I$(SDL)/include
OSFLAGS  := -Wno-format -Wno-nullability-completeness -fPIC
RT_SRC   := $(filter-out runtime/gamebuild.c,$(RT_SRC))
GENFLAGS += -fPIC
LDFLAGS  := -shared -Wl,-soname,libmain.so -Wl,--no-undefined -Wl,-z,max-page-size=16384 -L$(SDL)/lib -lSDL3 -lshaderc_combined -lvulkan \
            -landroid -lnativewindow -llog -lm -ldl -static-libstdc++  # (nativewindow: ANativeWindow_setFrameRate)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++
GAMELIB  := $(B)/lib$(GAME_ID)game.so
GAMELIB_LDFLAGS := -shared -Wl,-soname,lib$(GAME_ID)game.so -Wl,-z,max-page-size=16384 $(EXE)
GAMELIB_SHARED := -shared
APK      := $(B)/SMM2Recomp.apk
else ifeq ($(PLATFORM),linux)
EXE      := $(B)/$(GAME_ID)recomp
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= $(if $(CROSS_CC),-mcpu=x86_64_v2,-march=x86-64-v2)  # baseline for the lifted FP/SIMD code (gamebuild.c upgrades to v3 where the CPU has AVX2/FMA/BMI2; startup only requires v2)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format
SHADERC  ?= -lshaderc
LDFLAGS  := $(LLD) -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 $(FFMPEG_LIBS) \
            -lvulkan $(SHADERC) -lpthread -ldl -lm $(if $(CROSS_CC),-Wl$(comma)--allow-shlib-undefined -Wl$(comma)-rpath$(comma)'$$ORIGIN/lib')
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++
GAMELIB  := $(B)/$(GAME_ID)game.so
GAMELIB_LDFLAGS := -shared $(LLD)
GAMELIB_SHARED := -shared
else
EXE      := $(B)/$(GAME_ID)recomp.exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= $(if $(CROSS_CC),-mcpu=x86_64_v2,-march=x86-64-v2)  # (baseline; gamebuild.c upgrades to v3 where available)
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := $(LLD) -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 $(FFMPEG_LIBS) \
            -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared -lws2_32 -lbcrypt
RES_OBJ  := $(B)/windows.res  # (the exe's icon: runtime/platform/windows.rc)
# (vulkan-1.dll is looked up at run time, not imported: without it, the exe still starts and says what to install -
# tools/gen_vk_shim.py, runtime/platform/windows.c)
RES_OBJ  += $(B)/rt/vk_shim.o
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)
GAMELIB  := $(B)/$(GAME_ID)game.dll
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/$(GAME_ID)recomp.dll.a
GAMELIB_LDFLAGS := -shared $(LLD) $(B)/$(GAME_ID)recomp.dll.a
GAMELIB_SHARED := -shared
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c $(GAME_HLE) $(wildcard $(GAME_PATCHES)) $(GAME_PATCH_DEPS) \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
# The generated C must come from the same generator and patches: `make gen` stamps $(GEN)/.gen-<GAMELIB_ID>. Built from
# older generated code, a library carried the new id without the new patches (the iPad's game code was from
# 2026-10-18: no OpenCourse World handshake, no relaxed checks), so a stale tree stops the build instead.
ifneq ($(GEN_SRC),)
ifeq ($(wildcard $(GEN)/.gen-$(GAMELIB_ID)),)
ifneq ($(filter all app,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)  # (the goals that compile the game code)
$(error the generated game code in $(GEN) is older than the generator and patches (GAMELIB_ID $(GAMELIB_ID)): run `make GAME=$(GAME) gen` first)
endif
endif
endif

RT_OBJ   := $(patsubst %.c,$(B)/rt/%.o,$(patsubst runtime/%,%,$(RT_SRC)))
ifneq ($(IOS),)
UI_OBJ   += $(B)/rt/ui/nextendo_ios.o  # (the sign-in sheet: ASWebAuthenticationSession)
LDFLAGS  += -framework AuthenticationServices
endif
ifneq ($(filter ios iossim macos,$(PLATFORM)),)
RT_OBJ   += $(B)/rt/net/secure_store_apple.o  # (the Keychain backend of runtime/net/secure_store.h)
LDFLAGS  += -framework Security
endif
# TLS for the game's own ssl service (runtime/services/ssl.c): Mbed TLS 3.6 LTS, third_party/mbedtls (Apache-2.0),
# built with runtime/net/mbedtls_user_config.h (a TLS 1.2 client)
MBEDTLS  := third_party/mbedtls
MBEDTLS_FLAGS := -I$(MBEDTLS)/include -Iruntime/net -DMBEDTLS_USER_CONFIG_FILE='"mbedtls_user_config.h"'
MBEDTLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(B)/mbedtls/%.o,$(wildcard $(MBEDTLS)/library/*.c))
# The course server inside the app (core/crates/embed, runtime/net/embed_server.h): linked when its library is built
# (`make embed-server`: cargo; Windows / Linux cross builds compile its C parts with zig, tools/zigcc.sh) or
# EMBED_SERVER=1; a build never needs Rust otherwise.
EMBED_TRIPLE := $(if $(filter ios,$(PLATFORM)),aarch64-apple-ios,$(if $(filter iossim,$(PLATFORM)),aarch64-apple-ios-sim,$(if $(filter macos,$(PLATFORM)),$(if $(filter x86_64,$(ARCH)),x86_64-apple-darwin,aarch64-apple-darwin))))
EMBED_TRIPLE := $(or $(EMBED_TRIPLE),$(if $(filter windows,$(PLATFORM)),x86_64-pc-windows-gnu),$(if $(filter linux,$(PLATFORM)),$(ARCH)-unknown-linux-gnu))
EMBED_LIB := $(if $(EMBED_TRIPLE),../core/target/$(EMBED_TRIPLE)/release/libmmk_embed.a)
EMBED_SERVER ?= $(if $(wildcard $(EMBED_LIB)),1,0)
# (what Rust's standard library needs from the system there)
EMBED_SYSLIBS := $(if $(filter windows,$(PLATFORM)),-lws2_32 -luserenv -lbcrypt -lntdll -ladvapi32,$(if $(filter linux,$(PLATFORM)),-lpthread -ldl -lm))
ifeq ($(EMBED_SERVER),1)
OSFLAGS  += -DRECOMP_EMBED_SERVER
LDFLAGS  += $(EMBED_LIB) $(EMBED_SYSLIBS)
endif

RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn $(DEPINC) $(OSFLAGS) $(GAMEDEFS)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(DEPINC) $(GAMEDEFS)
CXXFLAGS += $(if $(filter android,$(PLATFORM)),-fPIC)  # (Android: everything goes into shared libraries)

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' $(GAMEDEFS) -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"' -DGAMEBUILD_HLE='"$(GAME_HLE)"' \
                                -DGAMEBUILD_PATCHES='"$(GAME_PATCHES)"'

$(B)/rt/net/embed_server.o: $(wildcard $(EMBED_LIB))  # (built again when the library appears)

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(B)/mbedtls/%.o: $(MBEDTLS)/library/%.c runtime/net/mbedtls_user_config.h
	@mkdir -p $(dir $@)
	@$(CC) -O2 -g $(MBEDTLS_FLAGS) $(filter -D% -fPIC,$(OSFLAGS)) -c $< -o $@  # (-fPIC: Android's libmain.so)
$(B)/rt/services/ssl.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/services/ssl.o: runtime/net/mbedtls_user_config.h
$(B)/rt/net/token.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/net/http.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/net/http.o: runtime/net/mbedtls_user_config.h
$(B)/rt/net/roots.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/net/nextendo.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/net/nextendo.o: runtime/net/mbedtls_user_config.h

# a game's own glue (games/<game>/*.c), compiled like the runtime; its own headers too (a struct that grows in one
# must rebuild every file that includes it)
GAME_HDRS := $(shell find $(GAMEDIR) -name '*.h' -not -path '*/generated/*' 2>/dev/null)
$(B)/rt/games/%.o: games/%.c $(RT_HDRS) $(GAME_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

# linked to a temp file, then renamed over: a copy still running keeps its (old) file
$(EXE): $(RT_OBJ) $(MBEDTLS_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ) $(EXE_GAME_OBJ) $(RES_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@
	$(if $(and $(CROSS_CC),$(filter windows,$(PLATFORM))),(echo LIBRARY $(notdir $@); cat runtime/platform/windows_exports.def) > $(B)/exports.def && \
	    $(ZIG) dlltool -m i386:x86-64 -d $(B)/exports.def -l $(B)/$(GAME_ID)recomp.dll.a)

# Windows: the Vulkan loader's entry points, forwarded to vulkan-1.dll loaded at run time (its export list:
# runtime/platform/vulkan-1.exports; the prototypes from the Vulkan headers)
$(B)/vk_shim.c: tools/gen_vk_shim.py runtime/platform/vulkan-1.exports
	@mkdir -p $(dir $@)
	$(or $(PY),python3) tools/gen_vk_shim.py $(if $(DEPS),$(DEPS)/include,/usr/include) runtime/platform/vulkan-1.exports $@
$(B)/rt/vk_shim.o: $(B)/vk_shim.c
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -Wno-unused-parameter -c $< -o $@

# Windows: the app icon as a resource (docs/branding/app-icon.ico, tools/make_icons.sh), compiled by zig's rc
$(B)/windows.res: runtime/platform/windows.rc ../docs/branding/app-icon.ico
	@mkdir -p $(dir $@)
	$(ZIG) rc /fo $@ /i ../docs/branding $<

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

# The release this build is (the setup screen shows it; its update check compares it with GitHub's latest release):
# the tag, e.g. v0.1.0 (release builds pass RECOMP_VERSION=<tag>), else git's description, else "dev"
RECOMP_VERSION ?= $(shell git describe --tags --match 'v[0-9]*' 2>/dev/null || echo dev)
$(B)/rt/ui/launcher.o: CXXFLAGS += -DRECOMP_VERSION='"$(RECOMP_VERSION)"'
$(B)/rt/ui/menu.o: CXXFLAGS += -DRECOMP_VERSION='"$(RECOMP_VERSION)"'  # (the menu's About row)
$(B)/rt/ui/launcher.o: runtime/gamebuild.h runtime/paths.h
$(B)/rt/services/am.o: RTFLAGS += -DRECOMP_VERSION='"$(RECOMP_VERSION)"'  # (the Game tab's version display toggle)

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_metal.o: $(IMGUI)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

# ------------------------------------------------------------------ the pipeline
# Python for the recompiler: tools/venv (made by `make venv`: capstone, unicorn, lz4)
PY := $(firstword $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))
venv:
	@[ -n "$(PY)" ] || python3 -m venv tools/venv
	tools/venv/bin/pip install -q lz4 capstone unicorn

# Game files from the dump: games/<game>/setup.py checks them and lays them out in $(GAMEFILES) (exefs/ and romfs/;
# an update's romfs over the base game's, as links). Paths come from DUMP_* (game.mk defaults; override on the
# command line).
setup:
	$(if $(PY),,$(error run `make venv` first))
	$(PY) $(GAMEDIR)/setup.py $(SETUP_ARGS) --out $(GAMEFILES)

# Recompile every module to C (AArch64 -> C, on this machine, from the player's own files).
gen:
	$(if $(PY),,$(error run `make venv` first))
	cd tools && ../$(PY) gen.py sdk ../$(GAMEFILES)/exefs/sdk $(shell printf '%#x' $$(($(GUEST_ORIGIN) + 0x4000000))) ../$(GEN)/sdk --hle ../$(GAME_HLE) $(GENSIMD)
	cd tools && ../$(PY) gen.py main ../$(GAMEFILES)/exefs/main $(GUEST_ORIGIN) ../$(GEN)/main \
	    $(if $(wildcard $(GAME_PATCHES)),--patches ../$(GAME_PATCHES)) $(GENSIMD)
	rm -f $(GEN)/.gen-* && touch $(GEN)/.gen-$(GAMELIB_ID)

clean-gen:
	rm -rf $(GEN) $(B)/obj

# iOS: the .app bundle around the executable (Info.plist, icon, signing: tools/make_ios_app.sh)
app: $(EXE)
	$(if $(IOS),,$(error app: pass TARGET=ios-arm64 | iossim-arm64))
	tools/make_ios_app.sh $(PLATFORM) $(EXE) $(APP)

# Android: the APK around libmain.so + the game code library (SDL's Java activity, manifest, debug-signed:
# tools/make_android_apk.sh); `install` puts it on the device adb sees (ADB_SERIAL=<serial> to pick one), `push-data`
# copies the game files there (tools/android_push_data.sh; PUSH_INTERNAL=1: into the app's internal folder, for a
# debuggable build made with RECOMP_ANDROID_DEBUGGABLE=1, where Android 11+ won't let the app read adb's files)
apk: all
	$(if $(filter android,$(PLATFORM)),,$(error apk: pass TARGET=android-arm64))
	tools/make_android_apk.sh $(B) $(APK) $(SDL) $(ANDROID_HOME) $(ANDROID_API)
install: apk
	tools/make_android_apk.sh --install $(APK)
push-data:
	tools/android_push_data.sh $(if $(filter 1,$(PUSH_INTERNAL)),--internal) $(DATA)

# A player package of a cross build (runtime with its setup screen, Python, recompiler, zig, hactool; nothing from the
# game): dist/SMM2Recomp-windows-x86_64.zip, dist/SMM2Recomp-linux-<arch>.tar.gz
#   make TARGET=windows-x86_64 GAME=smm2 DEPS=<deps> package
package: $(EXE)
	$(if $(TARGET),,$(error package: pass TARGET=windows-x86_64 | linux-x86_64 | linux-aarch64 (macOS: make app-macos)))
	RECOMP_VERSION=$(RECOMP_VERSION) tools/package.sh $(TARGET) $(B) $(GAME_ID) $(DEPS)

# The macOS player package: dist/SMM2Recomp-macos-arm64.zip with SMM2Recomp.app (runtime built for MACOS_MIN against
# SDL3 from tools/build_deps_macos.sh; the rest as the other packages). RECOMP_MACOS_IDENTITY=<Developer ID> signs it
# with that identity, else ad hoc. (macOS 15: what zig 0.17 itself needs.)
APP_MACOS_MIN ?= 15.0
app-macos:
	$(if $(filter macos,$(PLATFORM)),,$(error app-macos: on macOS))
	[ -f build/deps/lib/libSDL3.0.dylib ] || MACOS_MIN=$(APP_MACOS_MIN) tools/build_deps_macos.sh
	$(MAKE) GAME=$(GAME) B=build/$(GAME)-app DEPS=$(realpath build)/deps MACOS_MIN=$(APP_MACOS_MIN) \
	    RECOMP_VERSION=$(RECOMP_VERSION) build/$(GAME)-app/$(GAME_ID)recomp
	RECOMP_VERSION=$(RECOMP_VERSION) MACOS_MIN=$(APP_MACOS_MIN) tools/bundle_macos.sh build/$(GAME)-app/$(GAME_ID)recomp

# The course server inside the app (core/crates/embed) for this target, with cargo (no debug info: it would add ~80 MB
# to each package)
EMBED_ENV := $(if $(ZTARGET),ZIG="$(abspath $(ZIG))" ZIGCC_TARGET=$(ZTARGET) \
             CC_$(subst -,_,$(EMBED_TRIPLE))="$(CURDIR)/tools/zigcc.sh" AR_$(subst -,_,$(EMBED_TRIPLE))="$(CURDIR)/tools/zigar.sh" \
             $(if $(filter windows,$(PLATFORM)),CARGO_ENCODED_RUSTFLAGS="-Cdlltool=$(CURDIR)/tools/zigdlltool.sh"))
embed-server:
	$(if $(EMBED_TRIPLE),,$(error embed-server: no Rust target for $(TARGET)))
	cd ../core && rustup target add $(EMBED_TRIPLE) >/dev/null 2>&1; $(EMBED_ENV) CARGO_PROFILE_RELEASE_DEBUG=0 cargo build -p mmk-embed --release --target $(EMBED_TRIPLE)

# Run from the data folder (saves, settings and shader caches go there); RUN_ARGS / env vars as the runtime reads them
run: all
	cd $(DATA) && $(CURDIR)/$(EXE) extracted $(RUN_ARGS)

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check, needs no game files
runtime-objs: $(RT_OBJ) $(MBEDTLS_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

# Recompiler unit tests (no game files)
test-tools:
	cd tools && ../$(PY) test_lift.py

# One command per game: `make smm2` = setup + gen + build. (Then `make run-smm2`.)
smm2:
	$(MAKE) GAME=smm2 setup
	$(MAKE) GAME=smm2 gen
	$(MAKE) GAME=smm2 -j$(JOBS) all
run-smm2:
	$(MAKE) GAME=smm2 run
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Shader prefetch's build states / waits / pending table (runtime/nvn/buildsync.c), no game files
build/test_buildsync: tools/test_buildsync.c runtime/nvn/buildsync.c runtime/nvn/buildsync.h
	@mkdir -p build
	$(HOSTCC_TEST) -O1 -Wall -pthread '-DLOG(...)=printf(__VA_ARGS__)' -o $@ tools/test_buildsync.c runtime/nvn/buildsync.c
test-buildsync: build/test_buildsync
	./build/test_buildsync
HOSTCC_TEST ?= cc

# Frame generation (runtime/nvn/framegen*): the per-frame decision, and the passes on synthetic images (Metal)
build/test_framegen_logic: tools/test_framegen_logic.c runtime/nvn/framegen.c runtime/nvn/framegen.h
	@mkdir -p build
	clang -O1 -DFG_NO_SDL -I runtime -o $@ tools/test_framegen_logic.c runtime/nvn/framegen.c
build/test_framegen: tools/test_framegen.m runtime/nvn/framegen_metal.m runtime/nvn/framegen_src.c runtime/nvn/framegen.h
	@mkdir -p build
	clang -O1 -fobjc-arc -DFG_NO_SDL -I runtime -framework Metal -framework Foundation -o $@ tools/test_framegen.m \
	    runtime/nvn/framegen_metal.m runtime/nvn/framegen_src.c
# frame generation on a captured strip (LARECOMP_FRAMEGEN_SHOT's prev | generated | cur): the passes again, offline
build/framegen_png: tools/framegen_png.m runtime/nvn/framegen_metal.m runtime/nvn/framegen_src.c runtime/nvn/framegen.h
	@mkdir -p build
	clang -O1 -fobjc-arc -DFG_NO_SDL -I runtime -framework Metal -framework Foundation -framework ImageIO \
	    -framework CoreGraphics -framework UniformTypeIdentifiers -o $@ tools/framegen_png.m runtime/nvn/framegen_metal.m \
	    runtime/nvn/framegen_src.c
test-framegen: build/test_framegen_logic build/test_framegen
	build/test_framegen_logic
	build/test_framegen

# The overlay menu's font file (runtime/ui/system_font.mm)
build/test_system_font: tools/test_system_font.mm runtime/ui/system_font.mm runtime/ui/system_font.h
	@mkdir -p build
	clang++ -O1 -fobjc-arc -I runtime -framework CoreText -framework CoreFoundation -framework Foundation -o $@ \
	    tools/test_system_font.mm runtime/ui/system_font.mm
test-ui: build/test_system_font
	build/test_system_font

# Shader compile-ahead alone: scan a RomFS folder, translate, compile (tools/test_shader_ahead.m)
build/test_shader_ahead: tools/test_shader_ahead.m runtime/nvn/shader_ahead.c runtime/nvn/shader_ahead.h runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/program.c runtime/nvn/maxwell/shader.h runtime/zstd_lib.c
	clang -O2 -fobjc-arc -framework Metal -framework Foundation -o $@ tools/test_shader_ahead.m runtime/nvn/shader_ahead.c \
	    runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/program.c runtime/zstd_lib.c

build/test_shaders: tools/test_shaders.m runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
	clang -O1 -fobjc-arc -framework Metal -framework Foundation -o $@ tools/test_shaders.m runtime/nvn/maxwell/translate.c

.PHONY: all venv setup gen clean-gen run gen-objs runtime-objs test-tools test-buildsync test-framegen test-ui smm2 run-smm2 package embed-server app app-macos apk install push-data
