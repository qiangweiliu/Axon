# SkillManager 模块 Makefile

_SK_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_SK_DIR)Kconfig

_SK_SRCS := $(addprefix $(_SK_DIR),$(MODULE_SRCS))
_SK_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_SK_SRCS))

ALL_OBJS += $(_SK_OBJS)
