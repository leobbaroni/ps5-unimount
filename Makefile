PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

VERSION_TAG := $(shell git describe --abbrev=6 --dirty --always --tags 2>/dev/null || echo unknown)
BUILD_TIME := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
UNIMOUNT_VERSION := $(shell cat VERSION 2>/dev/null || echo unknown)
KRAKEN_MOUNT_REVISION := $(shell head -c 7 vendor/kraken/REVISION 2>/dev/null || echo unknown)

# Compiler and dependency flags
HOMEBREW_ROOT := $(PS5_PAYLOAD_SDK)/target/user/homebrew
JSON_C_PREFIX := $(CURDIR)/.deps/json-c
JSON_C_LIB := $(JSON_C_PREFIX)/lib/libjson-c.a
JSON_C_OBJS := src/sm_api_service.o src/sm_appdb.o src/sm_gameinfo.o src/sm_kraken.o src/sm_pkg.o src/sm_config_file.o
JSON_C_OBJS += src/sm_takeover.o src/sm_kraken_compat.o src/sm_kraken_auto.o src/sm_unimount_logic.o
JSON_C_OBJS += src/sm_diag.o src/sm_diag_core.o src/sm_diag_system.o
HOMEBREW_CFLAGS := -I$(JSON_C_PREFIX)/include -I$(HOMEBREW_ROOT)/include
MHD_LIB := $(HOMEBREW_ROOT)/lib/libmicrohttpd.a
PNG_LIB := $(HOMEBREW_ROOT)/lib/libpng16.a
JPEG_LIB := $(HOMEBREW_ROOT)/lib/libturbojpeg.a
ZLIB_LIB := $(HOMEBREW_ROOT)/lib/libz.a
SMB_LIB := $(HOMEBREW_ROOT)/lib/libsmb2.a
CFLAGS := -O3 -flto=thin -DNDEBUG -ffunction-sections -fdata-sections -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Werror=strict-prototypes -Werror=missing-prototypes -D_BSD_SOURCE -std=gnu11 -Iinclude -Isrc
CFLAGS += -DSHADOWMOUNT_VERSION=\"$(VERSION_TAG)\"
CFLAGS += -DUNIMOUNT_VERSION=\"$(UNIMOUNT_VERSION)\" -DKRAKEN_MOUNT_REVISION=\"$(KRAKEN_MOUNT_REVISION)\"
CFLAGS += -DSM_VFS_ENABLED
CFLAGS += -I$(JSON_C_PREFIX)/include

# Linker
LDFLAGS := -flto=thin -Wl,--gc-sections

# Libraries
LIBS := -lSceNotification -lSceSystemService -lSceUserService -lSceAppInstUtil -lSceNet -lSceSsl -lSceHttp -lSceIpmi -lsqlite3 $(JSON_C_LIB) $(MHD_LIB) $(PNG_LIB) $(JPEG_LIB) $(ZLIB_LIB) $(SMB_LIB) -lpthread -lm
PS5_SCE_STUBS_DIR ?= $(PS5_PAYLOAD_SDK)/src/sce_stubs
KERNEL_SYS_STUB_SO := src/libkernel_sys_ext.so
KERNEL_SYS_STUB_SRCS := $(PS5_SCE_STUBS_DIR)/libkernel_sys.c src/libkernel_sys_ext.c

ASSET_SRCS := src/notify_icon_asset.c src/config_ini_example_asset.c src/web_index_asset.c
ASSET_SRCS += src/shell_icon_param_asset.c src/unimount_icon_asset.c
SRCS := src/main.c $(wildcard src/sm_*.c) $(ASSET_SRCS)
ASM_SRCS := src/sm_shellcore_bridge.S
OBJS := $(SRCS:.c=.o) $(ASM_SRCS:.S=.o)
HEADERS := $(wildcard include/*.h)
L10N_CATALOGS := $(wildcard include/lang/*.inc)
CARGO ?= cargo
KRAKEN_TARGET := x86_64-unknown-freebsd
KRAKEN_LIB := kraken-bridge/target/$(KRAKEN_TARGET)/release/libsmp_kraken_bridge.a
KRAKEN_VENDOR := vendor/kraken
KRAKEN_AMPR := $(KRAKEN_VENDOR)/vendor/ampr-emu-ng
KRAKEN_PLAYGO := vendor/pgo_stub
KRAKEN_SOURCES := $(shell find kraken-bridge/src $(KRAKEN_VENDOR)/crates/kraken-mount-pkg/src -name '*.rs')

# Targets
all: shadowmountplus.elf

.PHONY: json-c
json-c: $(JSON_C_LIB)

# Validate SDK/options and installed headers; cache hits leave the archive intact.
$(JSON_C_LIB): FORCE tools/build_ps5_json_c.py patches/json-c-0.19-release-free.patch
	env -u MAKEFLAGS -u MFLAGS python3 tools/build_ps5_json_c.py --prefix "$(JSON_C_PREFIX)" --sdk "$(PS5_PAYLOAD_SDK)"

$(JSON_C_OBJS): $(JSON_C_LIB)

usb-info.elf: tools/usb_info.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lSceNotification
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

# Build Daemon
shadowmountplus.elf: $(OBJS) $(KERNEL_SYS_STUB_SO) $(JSON_C_LIB) $(KRAKEN_LIB) src/kraken_assets.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) src/kraken_assets.o $(KERNEL_SYS_STUB_SO) $(KRAKEN_LIB) $(LIBS)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

$(KRAKEN_LIB): $(KRAKEN_SOURCES) kraken-bridge/Cargo.toml kraken-bridge/Cargo.lock $(KRAKEN_VENDOR)/src/ampr_index.rs $(KRAKEN_VENDOR)/crates/kraken-mount-pkg/Cargo.toml
	$(CARGO) build --release --locked --target $(KRAKEN_TARGET) --manifest-path kraken-bridge/Cargo.toml

src/kraken_assets.o: src/kraken_assets.S $(KRAKEN_AMPR)/libSceAmpr.sprx $(KRAKEN_AMPR)/SHA256SUMS $(KRAKEN_VENDOR)/REVISION $(KRAKEN_PLAYGO)/libScePlayGo.sprx $(KRAKEN_PLAYGO)/SHA256SUMS
	cd $(KRAKEN_AMPR) && sha256sum --check --ignore-missing --strict SHA256SUMS
	cd $(KRAKEN_PLAYGO) && sha256sum --check --strict SHA256SUMS
	$(CC) $(CFLAGS) -c -o $@ $<

$(KERNEL_SYS_STUB_SO): $(KERNEL_SYS_STUB_SRCS)
	test -f "$(PS5_SCE_STUBS_DIR)/libkernel_sys.c"
	$(CC) -shared -Wl,-soname=libkernel_sys.sprx -o $@ $^

src/notify_icon_asset.c: smp_icon.png
	xxd -i $< > $@

src/config_ini_example_asset.c: config.ini.example
	xxd -i $< > $@

src/web_index_asset.c: web/index.html
	xxd -i $< > $@

src/shell_icon_param_asset.c: assets/shell_icon_param.json
	xxd -i $< > $@

src/unimount_icon_asset.c: assets/unimount_icon.png
	xxd -i $< > $@

src/sm_api_service.o src/sm_appdb.o src/sm_icon_thumb.o src/sm_gameinfo.o src/sm_pkg.o: CFLAGS += $(HOMEBREW_CFLAGS)
src/sm_smb.o src/sm_vfs.o: CFLAGS += $(HOMEBREW_CFLAGS)
src/main.o src/sm_image_index.o: CFLAGS += -DSHADOWMOUNT_BUILD_TIME=\"$(BUILD_TIME)\"
src/main.o src/sm_image_index.o: FORCE
# These embed the version; rebuild them so the reported version never goes stale.
src/sm_api_service.o src/sm_log.o src/sm_sandbox_record.o: FORCE

src/sm_l10n.o: $(L10N_CATALOGS)
src/sm_api_service.o src/sm_takeover.o src/sm_kraken_compat.o: VERSION
src/sm_ampr_updater.o: src/sm_ampr_ca.inc
src/sm_shellcore_offsets.o: src/sm_shellcore_offsets.inc

.PHONY: FORCE
FORCE:

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.o: src/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f shadowmountplus.elf usb-info.elf api-test.elf kill.elf src/*.o $(KERNEL_SYS_STUB_SO) $(ASSET_SRCS)
