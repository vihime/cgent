# config.mk — cgent build configuration
# Platform detection and shared flags

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Compiler
CC      = gcc
AR      = ar

# Base flags
CFLAGS_BASE  = -std=c11 -Wall -Wextra -Wpedantic -g -Os
CFLAGS_BASE += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

# Size optimization: strip symbols, remove unused sections
CFLAGS_SMALL  = -fdata-sections -ffunction-sections -flto
LDFLAGS_SMALL = -Wl,--gc-sections -s

# Include paths
CFLAGS_BASE += -Iinclude -Ithird_party/cJSON

# Platform-specific flags
ifeq ($(UNAME_S),Linux)
	CFLAGS_BASE  += -DPLATFORM_LINUX
	LDFLAGS_BASE  = -lssl -lcrypto -lpthread -lm -ldl
else ifeq ($(UNAME_S),Darwin)
	CFLAGS_BASE  += -DPLATFORM_MACOS
	LDFLAGS_BASE  = -lssl -lcrypto -lpthread -lm
	# macOS OpenSSL via Homebrew (if not in default path)
	ifneq ($(wildcard /usr/local/opt/openssl/include),)
		CFLAGS_BASE  += -I/usr/local/opt/openssl/include
		LDFLAGS_BASE += -L/usr/local/opt/openssl/lib
	endif
else
	# Windows (MinGW-w64 / MSYS2)
	CFLAGS_BASE  += -DPLATFORM_WINDOWS -D_WIN32_WINNT=0x0601
	LDFLAGS_BASE  = -lssl -lcrypto -lws2_32 -lm
endif

CFLAGS  = $(CFLAGS_BASE)
LDFLAGS = $(LDFLAGS_BASE) -s

# Debug build
ifdef DEBUG
	CFLAGS += -O0 -DDEBUG -fsanitize=address
	LDFLAGS += -fsanitize=address
endif

# Verbose flag
ifdef V
	Q =
else
	Q = @
endif
