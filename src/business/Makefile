# Business 层 Makefile
#
# 模块层：自动发现子目录中的模块，include 各模块的 Makefile。

_LAYER_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_LAYER_DIR)Kconfig

_BUSINESS_MODULES := $(dir $(wildcard $(_LAYER_DIR)*/Makefile))
-include $(addsuffix Makefile,$(_BUSINESS_MODULES))
