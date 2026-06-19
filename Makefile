# Agent Framework — 顶层 Makefile
#
# 职责：读 Kconfig → include 各层 Makefile → 链接。
# 不直接处理任何源文件，所有编译规则由层/模块 Makefile 定义。

include Kconfig

# ── 工具链 ──────────────────────────────────────────────────────────
CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -O2
LDFLAGS  = -lm -T agent.ld

BUILDDIR = build
TARGET   = $(BUILDDIR)/agent

# ── 全局对象列表（由层/模块 Makefile 追加） ─────────────────────────
ALL_OBJS =

# ── 加载所有层 ──────────────────────────────────────────────────────
-include $(addsuffix /Makefile,$(LAYERS))

# ── 编译规则（所有源文件统一使用） ──────────────────────────────────
$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── 链接 ────────────────────────────────────────────────────────────
$(TARGET): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ── 目标 ────────────────────────────────────────────────────────────
.PHONY: all clean

all: $(TARGET)

clean:
	rm -rf $(BUILDDIR)
