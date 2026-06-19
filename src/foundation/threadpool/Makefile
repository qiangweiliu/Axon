# ThreadPool 模块 Makefile

_TP_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_TP_DIR)Kconfig

_TP_SRCS := $(addprefix $(_TP_DIR),$(MODULE_SRCS))
_TP_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_TP_SRCS))

ALL_OBJS += $(_TP_OBJS)
