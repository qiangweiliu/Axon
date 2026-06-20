# Archive 模块 Makefile

_AR_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_AR_DIR)Kconfig

_AR_SRCS := $(addprefix $(_AR_DIR),$(MODULE_SRCS))
_AR_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_AR_SRCS))

ALL_OBJS += $(_AR_OBJS)
