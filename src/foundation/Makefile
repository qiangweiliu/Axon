# Foundation 层 Makefile
#
# 模块层：自动发现子目录中的模块，include 各模块的 Makefile。
# 被顶层 Makefile include，不独立运行。

_LAYER_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_LAYER_DIR)Kconfig

# 发现所有模块子目录（包含 Makefile 的目录）
_FOUNDATION_MODULES := $(dir $(wildcard $(_LAYER_DIR)*/Makefile))

# 加载每个模块的 Makefile
-include $(addsuffix Makefile,$(_FOUNDATION_MODULES))
