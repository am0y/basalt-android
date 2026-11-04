# Basalt native build (arm64-v8a or x86_64)
NDK    := $(or $(ANDROID_NDK_HOME),$(HOME)/android-tools/ndk/android-ndk-r28b)
ANDROID_API := $(strip $(shell cat android_api.txt))
ABI ?= arm64-v8a
ifeq ($(ABI),arm64-v8a)
TARGET := aarch64-linux-android
else ifeq ($(ABI),x86_64)
TARGET := x86_64-linux-android
else
$(error ABI must be arm64-v8a or x86_64)
endif
CC     := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)$(ANDROID_API)-clang
CXX    := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)$(ANDROID_API)-clang++
MODE ?= release
ARM_ENGINE ?= 743
ARM_FULL ?= 0
ifeq ($(ABI),arm64-v8a)
ifneq ($(filter $(ARM_ENGINE),743 744),$(ARM_ENGINE))
$(error ARM_ENGINE must be 743 or 744)
endif
endif
ifeq ($(filter $(MODE),release development),)
$(error MODE must be release or development)
endif
BUILD_DIR := .build/$(ABI)/$(MODE)/api$(ANDROID_API)/engine$(ARM_ENGINE)/full$(ARM_FULL)
LUAU_ANDROID := luau-android-$(ABI)-api$(ANDROID_API)
NATIVE_ABS := $(abspath .)
LUAU_DIR := ../luau
LUAU_ABS := $(abspath $(LUAU_DIR))
HARDEN := -O2 -fPIC -fvisibility=hidden -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
          -ffunction-sections -fdata-sections -flto=thin \
          -fno-ident \
          -fmerge-all-constants -ffile-prefix-map=$(NATIVE_ABS)=.
CFLAGS := $(HARDEN) -Wall -Wextra -I. -DBASALT_DEV_BRIDGE=$(if $(filter development,$(MODE)),1,0) \
           $(if $(filter development,$(MODE)),-DBASALT_ENABLE_SECURITY_PROBE,) \
           -DBASALT_ARM_744=$(if $(filter 744,$(ARM_ENGINE)),1,0) -DBASALT_ARM_744_FULL=$(ARM_FULL)
LDFLAGS := -llog -ldl
OUT    := $(if $(filter arm64-v8a,$(ABI)),$(if $(filter 744,$(ARM_ENGINE)),libbasalt-744$(if $(filter 1,$(ARM_FULL)),-full,)$(if $(filter development,$(MODE)),-dev,).so,$(if $(filter development,$(MODE)),libbasalt-dev.so,libbasalt.so)),$(if $(filter development,$(MODE)),libdecode-x86_64-dev.so,libdecode-x86_64.so))
SRC    := executor.c engine.c hook.c hook_x86.c x86_executor.c runtime_protection.c loader.c embedded_bytecode.c workspace.c workspace_paths.c http_bridge.c device_http.c vm_api.c rnet.c sha384.c legacy_diagnostics.c linux_bytecode_compat.c
HOST_CXX := c++
LUAU_BUILD := $(LUAU_DIR)/build

all: $(OUT)

.PHONY: all development
development:
	$(MAKE) MODE=development all

ANDROID_LIBS := $(LUAU_ANDROID)/libLuau.Compiler.a $(LUAU_ANDROID)/libLuau.Ast.a \
                $(LUAU_ANDROID)/libLuau.Bytecode.a $(LUAU_ANDROID)/libLuau.Common.a
OBJS := $(addprefix $(BUILD_DIR)/,$(SRC:.c=.o) compiler_bridge.o httpget_rewrite.o)

$(OUT): $(OBJS) $(ANDROID_LIBS)
	$(CXX) -shared -flto=thin -static-libstdc++ -Wl,--gc-sections -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,-z,relro,-z,now,-z,noexecstack \
		-Wl,--icf=safe -Wl,--build-id=none -Wl,-O2 \
		$(OBJS) $(ANDROID_LIBS) -o $(OUT) $(LDFLAGS)
ifeq ($(MODE),release)
	$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all $(OUT)
endif
	@ls -la $(OUT)

$(BUILD_DIR)/%.o: %.c hook.h engine.h vm_layout.h protocol.h process_scope.h process_memory.h instance_scope.h loader.h embedded_bytecode.h workspace.h workspace_paths.h http_bridge.h device_http.h vm_api.h bytecode_layout.h compiler_bridge.h build_config.h legacy_diagnostics.h android_api.txt Makefile | $(BUILD_DIR)
	$(CC) -std=c11 -D_GNU_SOURCE $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/embedded_bytecode.o: scheduler_bytecode.h editor_bytecode.h
$(BUILD_DIR)/device_http.o: sha384.h
$(BUILD_DIR)/workspace.o: workspace_path.h
$(BUILD_DIR)/vm_api.o: security_policy.h
$(BUILD_DIR)/vm_api.o $(BUILD_DIR)/rnet.o: rnet.h
$(BUILD_DIR)/vm_api.o $(BUILD_DIR)/linux_bytecode_compat.o: linux_bytecode_compat.h
$(BUILD_DIR)/runtime_protection.o: runtime_protection.h protection_policy.h
$(BUILD_DIR)/executor.o $(BUILD_DIR)/vm_api.o: runtime_protection.h
$(BUILD_DIR)/compiler_bridge.o: compiler_bridge.cpp compiler_bridge.h httpget_rewrite.h numeric_locale.h Makefile | $(BUILD_DIR)
	$(CXX) -std=c++17 $(CFLAGS) -fvisibility-inlines-hidden \
		-I$(LUAU_DIR)/Compiler/include -I$(LUAU_DIR)/Ast/include \
		-I$(LUAU_DIR)/Bytecode/include -I$(LUAU_DIR)/Common/include -c $< -o $@

$(BUILD_DIR)/httpget_rewrite.o: httpget_rewrite.cpp httpget_rewrite.h Makefile | $(BUILD_DIR)
	$(CXX) -std=c++17 $(CFLAGS) -fvisibility-inlines-hidden \
		-I$(LUAU_DIR)/Compiler/include -I$(LUAU_DIR)/Ast/include \
		-I$(LUAU_DIR)/Bytecode/include -I$(LUAU_DIR)/Common/include -c $< -o $@

$(LUAU_ANDROID)/libLuau.Compiler.a:
	cmake -S $(LUAU_DIR) -B $(LUAU_ANDROID) -DCMAKE_TOOLCHAIN_FILE=$(NDK)/build/cmake/android.toolchain.cmake \
		-DANDROID_ABI=$(ABI) -DANDROID_PLATFORM=android-$(ANDROID_API) -DANDROID_STL=c++_static \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DCMAKE_C_FLAGS='-ffile-prefix-map=$(LUAU_ABS)=.' \
		-DCMAKE_CXX_FLAGS='-fvisibility=hidden -ffile-prefix-map=$(LUAU_ABS)=.' \
		-DLUAU_BUILD_CLI=OFF -DLUAU_BUILD_TESTS=OFF
	cmake --build $(LUAU_ANDROID) --target Luau.Compiler -j 4

$(LUAU_ANDROID)/libLuau.Ast.a $(LUAU_ANDROID)/libLuau.Bytecode.a $(LUAU_ANDROID)/libLuau.Common.a: $(LUAU_ANDROID)/libLuau.Compiler.a

LUAU_LIBS := $(LUAU_BUILD)/libLuau.Compiler.a $(LUAU_BUILD)/libLuau.Ast.a \
             $(LUAU_BUILD)/libLuau.Bytecode.a $(LUAU_BUILD)/libLuau.Common.a

$(LUAU_BUILD)/libLuau.Compiler.a:
	cmake -S $(LUAU_DIR) -B $(LUAU_BUILD) -DCMAKE_BUILD_TYPE=Release \
		-DLUAU_BUILD_CLI=OFF -DLUAU_BUILD_TESTS=OFF
	cmake --build $(LUAU_BUILD) --target Luau.Compiler -j 4

$(LUAU_BUILD)/libLuau.Ast.a $(LUAU_BUILD)/libLuau.Bytecode.a $(LUAU_BUILD)/libLuau.Common.a: $(LUAU_BUILD)/libLuau.Compiler.a

compile_bytecode: compile_bytecode.cpp httpget_rewrite.cpp httpget_rewrite.h numeric_locale.h $(LUAU_LIBS)
	$(HOST_CXX) -std=c++17 -O2 -I$(LUAU_DIR)/Compiler/include \
		-I$(LUAU_DIR)/Bytecode/include -I$(LUAU_DIR)/Ast/include \
		-I$(LUAU_DIR)/Common/include compile_bytecode.cpp httpget_rewrite.cpp \
		$(LUAU_BUILD)/libLuau.Compiler.a $(LUAU_BUILD)/libLuau.Ast.a \
		$(LUAU_BUILD)/libLuau.Bytecode.a $(LUAU_BUILD)/libLuau.Common.a \
		-o $@

scheduler_bytecode.h: scheduler.luau console.luau security.luau encoding.luau extended.luau websocket.luau drawing.luau signals.luau properties.luau mobile_ui.luau instance_http.luau android_compat.luau lifecycle.luau saveinstance.luau input_compat.luau actor_compat.luau bundle_scheduler.py compile_bytecode embed_bytecode.py Makefile
	python3 bundle_scheduler.py
	./compile_bytecode scheduler_bundled.luau scheduler.cmd --opt=0 --debug=0
	python3 embed_bytecode.py scheduler.cmd $@

editor_bytecode.h: ../scripts/editor.luau ../scripts/saved_scripts.luau ../scripts/editor_settings.luau ../scripts/execution_service.luau ../scripts/assets/glass-icons.png ../scripts/assets/basalt-logo.png bundle_editor.py compile_bytecode embed_editor.py embed_bytecode.py
	python3 bundle_editor.py
	./compile_bytecode editor_bundled.luau editor.cmd --opt=1 --debug=0
	python3 embed_editor.py editor.cmd $@

bytecode_layout.h: gen_bytecode_layout.py $(LUAU_DIR)/Common/include/Luau/Bytecode.h $(LUAU_DIR)/Common/include/Luau/BytecodeUtils.h
	python3 gen_bytecode_layout.py

clean:
	rm -f $(OUT) $(OBJS) compile_bytecode scheduler.cmd scheduler_bytecode.h editor.cmd editor_bytecode.h
