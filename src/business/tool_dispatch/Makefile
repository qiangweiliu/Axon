# Tool Dispatch 模块 Makefile

_TD_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_TD_DIR)Kconfig

_TD_SRCS := $(addprefix $(_TD_DIR),$(MODULE_SRCS))
_TD_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_TD_SRCS))

ALL_OBJS += $(_TD_OBJS)
