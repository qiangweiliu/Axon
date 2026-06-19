# Platform 层 Makefile
#
# 核心层：编译本层源文件，追加到 ALL_OBJS。

_PLATFORM_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_PLATFORM_DIR)Kconfig

_PLATFORM_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(PLATFORM_SRCS))
ALL_OBJS       += $(_PLATFORM_OBJS)
