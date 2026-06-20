# Agent 模块 Makefile

_AG_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_AG_DIR)Kconfig

_AG_SRCS := $(addprefix $(_AG_DIR),$(MODULE_SRCS))
_AG_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_AG_SRCS))

ALL_OBJS += $(_AG_OBJS)
