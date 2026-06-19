# Logger 模块 Makefile

_LOGGER_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_LOGGER_DIR)Kconfig

_LOGGER_SRCS := $(addprefix $(_LOGGER_DIR),$(MODULE_SRCS))
_LOGGER_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(_LOGGER_SRCS))

ALL_OBJS += $(_LOGGER_OBJS)
