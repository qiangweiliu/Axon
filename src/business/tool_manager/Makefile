# ToolManager 模块 Makefile

_TM_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_TM_DIR)Kconfig

_TM_SRCS := $(addprefix $(_TM_DIR),$(MODULE_SRCS))
_TM_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_TM_SRCS))

ALL_OBJS += $(_TM_OBJS)
