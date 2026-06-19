# HttpClient 模块 Makefile

_HC_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_HC_DIR)Kconfig

_HC_SRCS := $(addprefix $(_HC_DIR),$(MODULE_SRCS))
_HC_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_HC_SRCS))

ALL_OBJS += $(_HC_OBJS)
