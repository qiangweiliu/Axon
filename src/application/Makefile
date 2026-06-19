# Application 层 Makefile

_LAYER_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_LAYER_DIR)Kconfig

_APP_MODULES := $(dir $(wildcard $(_LAYER_DIR)*/Makefile))
-include $(addsuffix Makefile,$(_APP_MODULES))
