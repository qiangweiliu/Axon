# Config 模块 Makefile

_CFG_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_CFG_DIR)Kconfig

_CFG_SRCS := $(addprefix $(_CFG_DIR),$(MODULE_SRCS))
_CFG_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_CFG_SRCS))

ALL_OBJS += $(_CFG_OBJS)
