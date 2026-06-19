# Memory 模块 Makefile

_MEM_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_MEM_DIR)Kconfig

_MEM_SRCS := $(addprefix $(_MEM_DIR),$(MODULE_SRCS))
_MEM_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_MEM_SRCS))

ALL_OBJS += $(_MEM_OBJS)
