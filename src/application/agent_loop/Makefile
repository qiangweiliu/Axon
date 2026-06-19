# AgentLoop 模块 Makefile

_AL_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_AL_DIR)Kconfig

_AL_SRCS := $(addprefix $(_AL_DIR),$(MODULE_SRCS))
_AL_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_AL_SRCS))

ALL_OBJS += $(_AL_OBJS)
