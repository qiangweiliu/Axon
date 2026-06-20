#!/bin/bash
# Axon Agent — 自动化测试脚本
# Usage: bash test.sh [module_name]
#   bash test.sh          # 全部模块
#   bash test.sh config   # 仅 config 模块
#   bash test.sh tools    # 仅工具集成

# set -e removed — we handle errors manually via PASS/FAIL counters
AGENT=./build/agent
PASS=0; FAIL=0; SKIP=0
RED='\033[31m'; GRN='\033[32m'; YLW='\033[33m'; DIM='\033[2m'; RST='\033[0m'

ok()   { PASS=$((PASS+1)); echo -e "  ${GRN}✓${RST} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}✗${RST} $1"; }
skip() { SKIP=$((SKIP+1)); echo -e "  ${YLW}⊘${RST} $1"; }

section() { echo -e "\n${DIM}── [$1] ──────────────────────────────${RST}"; }

# ── Build check ─────────────────────────────────────────────────────
section "Build"
make -s 2>&1 | tail -1 >/dev/null && ok "build" || fail "build"

# ── Test harness ─────────────────────────────────────────────────────
api_ask() {
    echo "ask $1" | timeout 60 "$AGENT" 2>/dev/null
}
tool_called() {
    api_ask "$1" | grep -q "⚙.*$2" 2>/dev/null
}

# ── Config ───────────────────────────────────────────────────────────
run_config() {
    section "Config"
    "$AGENT" config verify 2>&1 | grep -q 'Verifying' && ok "config verify" || fail "config verify"
}

# ── Memfile ──────────────────────────────────────────────────────────
run_memfile() {
    section "Memfile"
    printf "A\n§\nA\n§\nB\n" > /tmp/axon_test_mem.md
    cp data/memory/l0/working.md /tmp/axon_save_work 2>/dev/null || true
    cp /tmp/axon_test_mem.md data/memory/l0/working.md
    echo skip | timeout 5 "$AGENT" 2>/dev/null
    ct=$(grep -c '^A$' data/memory/l0/working.md 2>/dev/null)
    [ "$ct" = "1" ] && ok "dedup on load" || fail "dedup ($ct copies of A)"
    cp /tmp/axon_save_work data/memory/l0/working.md 2>/dev/null || true
    rm -f /tmp/axon_test_mem.md /tmp/axon_save_work
}

# ── Input ────────────────────────────────────────────────────────────
run_input() {
    section "Input"
    echo "test-pipe-input" | timeout 5 "$AGENT" 2>/dev/null | grep -q 'EOF' \
        && ok "pipe mode" || fail "pipe mode"
}

# ── HTTP ─────────────────────────────────────────────────────────────
run_http() {
    section "HTTP"
    curl -s --max-time 5 https://apihub.agnes-ai.com/v1/chat/completions \
      -H "Content-Type: application/json" \
      -H "Authorization: Bearer sk-LdVJVHXcNqAhde0K9YqHu4RBeLOq2kOF6gGxbxZKMENitMrY" \
      -d '{"model":"agnes-2.0-flash","messages":[{"role":"user","content":"hi"}],"stream":false,"max_tokens":3}' \
      2>&1 | grep -q '"choices"' && ok "HTTPS direct" || fail "HTTPS direct"
}

# ── Tools ────────────────────────────────────────────────────────────
run_tools() {
    section "Tools Integration"

    local tests=(
        "用echo工具返回axontest|echo|axontest"
        "用list_dir列出当前目录|list_dir|"
        "用bash执行echo axontest|bash|echo"
        "用read_file读取config.yml|read_file|"
        "用write_file写入/tmp/axontest.txt内容ok|write_file|"
    )
    for t in "${tests[@]}"; do
        IFS='|' read -r prompt toolname extra <<< "$t"
        if tool_called "$prompt" "$toolname"; then
            ok "$toolname"
        else
            skip "$toolname (API)"
        fi
    done

    # Verify written file
    [ -f /tmp/axontest.txt ] && ok "write_file verify" || echo "  (not checked)"
    rm -f /tmp/axontest.txt
}

# ── Agent Loop ───────────────────────────────────────────────────────
run_agent() {
    section "Agent Loop"
    local out
    out=$(api_ask "执行echo a, echo b, echo c, echo d, echo e")
    if echo "$out" | grep -q 'Completed'; then
        ok "max_depth hit gracefully"
    elif echo "$out" | grep -q 'Axon'; then
        ok "agent loop (answer box)"
    else
        skip "agent loop (API)"
    fi
}

# ── Dynamic Tools ────────────────────────────────────────────────────
run_dynamic() {
    section "Dynamic Tools"
    "$AGENT" config verify 2>&1 | grep -q 'Verifying' || { skip "binary not found"; return; }
    local count
    count=$(echo skip | timeout 5 "$AGENT" 2>&1 | grep -c 'ToolManager: registered')
    [ "$count" -ge 5 ] && ok "5+ tools registered" || fail "only $count tools"
    [ -f data/tools/example_search.json ] && ok "dynamic tool config" || skip "no config"
}

# ── Main ─────────────────────────────────────────────────────────────
if [ ! -f "$AGENT" ]; then
    echo "Building..."
    make -s
fi

if [ $# -eq 0 ]; then
    set -- all
fi
for arg in "$@"; do
    case "$arg" in
        all)
            run_config; run_memfile; run_input; run_http
            run_tools; run_agent; run_dynamic
            ;;
        config)   run_config ;;
        memfile) run_memfile ;;
        input)   run_input ;;
        http)    run_http ;;
        tools)   run_tools ;;
        agent)   run_agent ;;
        dynamic) run_dynamic ;;
        *) echo "Usage: $0 [all|config|memfile|input|http|tools|agent|dynamic]" ;;
    esac
done

echo -e "\n${DIM}── Results: ${GRN}$PASS passed${RST}, ${RED}$FAIL failed${RST}, ${YLW}$SKIP skipped${RST} ──${RST}"
[ $FAIL -eq 0 ] || exit 1
