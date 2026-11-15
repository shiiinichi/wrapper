OUT ?= ../../build/native-poc
CXX := xcrun clang++
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
FLAGS := -target arm64-apple-macos15.0 -isysroot "$(SDK)" -std=c++17 -O1 -Wall -Wextra -Werror -MMD -MP -ffile-prefix-map="$(CURDIR)/../.."=.
WORKER := $(patsubst %.cpp,$(OUT)/obj/%.o,$(wildcard *.cpp)) $(OUT)/obj/guest_call.o
KIT := $(patsubst %.cpp,$(OUT)/obj/%.o,$(wildcard kit/*.cpp))
SERVER := $(OUT)/obj/standalone/server.o
DESKTOP := $(OUT)/obj/standalone/desktop.o
TESTS := $(patsubst %.cpp,$(OUT)/obj/%.o,$(wildcard tests/*_tests.cpp tests/*_contract.cpp))
.PHONY: all runtime
all: runtime $(OUT)/native-music-wrapper-server $(OUT)/NativeMusicWrapper $(OUT)/native-endpoint-check
runtime: $(OUT)/wrapper-native-poc $(OUT)/libNativeMusicWrapperKit.a $(OUT)/native-asset-tool
$(OUT)/obj/%.o: %.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(FLAGS) -c "$<" -o "$@"
$(OUT)/obj/guest_call.o: guest_call.S
	@mkdir -p "$(@D)"
	xcrun clang -target arm64-apple-macos15.0 -isysroot "$(SDK)" -c "$<" -o "$@"
$(OUT)/wrapper-native-poc: $(WORKER)
	$(CXX) -target arm64-apple-macos15.0 $(WORKER) -lcurl -lsqlite3 -framework CoreFoundation -o "$@"
$(OUT)/libNativeMusicWrapperKit.a: $(KIT)
	xcrun libtool -static -o "$@" $(KIT)
$(OUT)/native-music-wrapper-server: $(SERVER) $(OUT)/libNativeMusicWrapperKit.a
	$(CXX) -target arm64-apple-macos15.0 $(SERVER) "$(OUT)/libNativeMusicWrapperKit.a" -lcurl -o "$@"
$(OUT)/NativeMusicWrapper: $(DESKTOP) $(OUT)/libNativeMusicWrapperKit.a
	$(CXX) -target arm64-apple-macos15.0 $(DESKTOP) "$(OUT)/libNativeMusicWrapperKit.a" -lobjc -lcurl -framework AppKit -framework CoreGraphics -o "$@"
$(OUT)/native-asset-tool: $(OUT)/obj/tools/assets.o
	$(CXX) -target arm64-apple-macos15.0 "$<" -o "$@"
$(OUT)/native-endpoint-check: $(OUT)/obj/tools/check_endpoints.o $(OUT)/libNativeMusicWrapperKit.a
	$(CXX) -target arm64-apple-macos15.0 "$<" "$(OUT)/libNativeMusicWrapperKit.a" -lcurl -o "$@"
$(OUT)/native-wrapper-tests: $(TESTS) $(OUT)/libNativeMusicWrapperKit.a
	$(CXX) -target arm64-apple-macos15.0 $(TESTS) "$(OUT)/libNativeMusicWrapperKit.a" -lcurl -lsqlite3 -o "$@"
.PHONY: verify
verify: all $(OUT)/native-wrapper-tests
	"$(OUT)/native-asset-tool" assets "$(OUT)/assets"
	"$(OUT)/native-wrapper-tests" "$(OUT)" "$(OUT)/assets" tests/fixtures
-include $(WORKER:.o=.d) $(KIT:.o=.d) $(SERVER:.o=.d) $(DESKTOP:.o=.d) $(TESTS:.o=.d) $(OUT)/obj/tools/assets.d
