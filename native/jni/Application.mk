APP_ABI := x86_64 arm64-v8a
APP_CFLAGS := -std=gnu99 ${MAGISK_DEBUG} \
	-DMAGISK_VERSION="${MAGISK_VERSION}" -DMAGISK_VER_CODE=${MAGISK_VER_CODE} 
APP_CPPFLAGS := -std=c++11
APP_PLATFORM := android-16
APP_CFLAGS += -Wno-implicit-function-declaration

# Busybox require some additional settings
ifdef B_BB
APP_SHORT_COMMANDS := true
NDK_TOOLCHAIN_VERSION := 4.9
APP_PLATFORM := android-21
endif
