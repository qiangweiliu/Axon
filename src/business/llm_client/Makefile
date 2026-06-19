# LLM Client 模块 Makefile

_LLM_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_LLM_DIR)Kconfig

_LLM_SRCS := $(addprefix $(_LLM_DIR),$(MODULE_SRCS))
_LLM_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_LLM_SRCS))

ALL_OBJS += $(_LLM_OBJS)
