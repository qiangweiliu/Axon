# Framework 层 Makefile
#
# 核心层：编译本层源文件，追加到 ALL_OBJS。

_FW_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_FW_DIR)Kconfig

_FW_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(FRAMEWORK_SRCS))
ALL_OBJS += $(_FW_OBJS)
