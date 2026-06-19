/*
 * framework_main.c — Axon agent main entry point.
 *
 * Modes:
 *   build/agent                — interactive REPL (default)
 *   build/agent config <...>   — configuration commands
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "config.h"
#include "agent_loop.h"
#include <signal.h>
#include <stdatomic.h>

static atomic_int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    atomic_store(&g_running, 0);
}

/* ── Normal Agent Run (prompt.txt) ───────────────────────────────── */

static int run_agent(void)
{
    os_signal_set(SIGINT, signal_handler);
    os_signal_set(SIGTERM, signal_handler);

    int rc = framework_init();
    if (rc != 0) { return 1; }

    framework_monitor_print();

    while (atomic_load(&g_running)) {
        framework_loop_tick();
        os_sleep_ms(1000);
    }

    framework_shutdown();
    return 0;
}

/* ── REPL Interactive Mode ───────────────────────────────────────── */

static int repl_main(void)
{
    os_signal_set(SIGINT, signal_handler);
    os_signal_set(SIGTERM, signal_handler);

    int rc = framework_init();
    if (rc != 0) { return 1; }

    framework_monitor_print();
    os_printf("\n");

    agent_loop_repl();

    framework_shutdown();
    return 0;
}

/* ── Config Subcommand ────────────────────────────────────────────── */

static int config_cmd(int argc, char *argv[]);

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) return repl_main();

    if (os_strcmp(argv[1], "--help") == 0 || os_strcmp(argv[1], "-h") == 0) {
        os_printf("Usage: build/agent [config|--help]\n");
        os_printf("  agent              — Axon interactive chat (default)\n");
        os_printf("  agent config ...   — set/show/verify configuration\n");
        return 0;
    }

    if (os_strcmp(argv[1], "repl") == 0) return repl_main();
    if (os_strcmp(argv[1], "config") == 0) return config_cmd(argc - 1, argv + 1);

    os_printf("Unknown: %s (try --help)\n", argv[1]);
    return 1;
}

/* ── Framework Core (no changes below) ────────────────────────────── */

int framework_init(void)
{
    fw_log_bind_module("framework");
    fw_log_init(NULL, FW_LOG_INFO);

    int count = framework_discover_modules();
    if (count <= 0) {
        LOG_ERROR("%s", "Framework: no modules found");
        return -1;
    }
    LOG_INFO("Framework: discovered %d module(s)", count);

    framework_bus_init();
    framework_sort_modules();

    if (call_init_all() != 0) {
        LOG_ERROR("%s", "Framework: init phase failed, aborting");
        return -1;
    }

    if (call_start_all() != 0) {
        LOG_ERROR("%s", "Framework: start phase failed, aborting");
        call_stop_all();
        call_deinit_all();
        fw_log_shutdown();
        return -1;
    }

    return 0;
}

void framework_shutdown(void)
{
    framework_event_publish(FW_EVENT_SHUTDOWN, NULL, 0);
    LOG_INFO("%s", "Framework: shutting down");
    call_stop_all();
    call_deinit_all();
    framework_leak_report();
    LOG_INFO("%s", "Framework: shutdown complete");
    fw_log_shutdown();
}

static int config_cmd(int argc, char *argv[])
{
    if (argc < 2) {
        os_printf("Usage: agent config <set|show|verify>\n");
        os_printf("  agent config set <key> <value>\n");
        os_printf("  agent config show\n");
        os_printf("  agent config verify\n");
        return 1;
    }

    if (os_strcmp(argv[1], "show") == 0) {
        os_file_handle_t fh = os_file_open("config.yml", "r");
        if (fh) {
            char buf[8192];
            size_t n = os_file_read(fh, buf, sizeof(buf) - 1);
            os_file_close(fh);
            if (n > 0) { buf[n] = '\0'; os_printf("%s", buf); }
        }
        return 0;
    }

    if (os_strcmp(argv[1], "set") == 0 && argc >= 4)
        return config_set(argv[2], argv[3]);

    if (os_strcmp(argv[1], "verify") == 0) {
        config_t cfg;
        os_memset(&cfg, 0, sizeof(cfg));
        cfg.log_level = FW_LOG_INFO;
        os_file_handle_t fh = os_file_open("config.yml", "r");
        if (fh) {
            char buf[8192];
            size_t n = os_file_read(fh, buf, sizeof(buf) - 1);
            os_file_close(fh);
            if (n > 0) {
                buf[n] = '\0';
                // Parse for llm section only
                const char *p = buf;
                char section[64] = "";
                while (*p) {
                    while (*p == ' ' || *p == '\t') p++;
                    const char *ls = p;
                    while (*p && *p != '\n') p++;
                    size_t ll = (size_t)(p - ls);
                    if (ll >= 512) { if (*p) p++; continue; }
                    char tmp[512]; os_memcpy(tmp, ls, ll); tmp[ll] = '\0';
                    if (*p == '\n') p++;
                    char *t = tmp; while (*t == ' ' || *t == '\t') t++;
                    if (!*t || *t == '#') continue;
                    size_t tl = os_strlen(t);
                    if (tl > 0 && t[tl-1] == ':') {
                        t[tl-1] = '\0';
                        char *s = t; while (*s == ' ' || *s == '\t') s++;
                        size_t sl = os_strlen(s);
                        if (sl < sizeof(section)) { os_memcpy(section, s, sl); section[sl] = '\0'; }
                        continue;
                    }
                    char *colon = NULL;
                    for (char *c = t; *c; c++) { if (*c == ':') { colon = c; break; } }
                    if (!colon) continue;
                    *colon = '\0';
                    char *key = t; while (*key == ' ' || *key == '\t') key++;
                    char *kend = key + os_strlen(key) - 1; while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
                    char *val = colon + 1; while (*val == ' ' || *val == '\t') val++;
                    if (*val == '"') { val++; char *ve = val + os_strlen(val) - 1; while (ve > val && *ve == '"') *ve-- = '\0'; }
                    if (os_strcmp(section, "llm") == 0) {
                        if (os_strcmp(key, "endpoint") == 0) { size_t vl = os_strlen(val); if (vl < sizeof(cfg.llm_endpoint)) { os_memcpy(cfg.llm_endpoint, val, vl); cfg.llm_endpoint[vl] = '\0'; } }
                        else if (os_strcmp(key, "api_key") == 0) { size_t vl = os_strlen(val); if (vl < sizeof(cfg.llm_api_key)) { os_memcpy(cfg.llm_api_key, val, vl); cfg.llm_api_key[vl] = '\0'; } }
                        else if (os_strcmp(key, "model") == 0) { size_t vl = os_strlen(val); if (vl < sizeof(cfg.llm_model)) { os_memcpy(cfg.llm_model, val, vl); cfg.llm_model[vl] = '\0'; } }
                    }
                }
            }
        }
        if (!cfg.llm_endpoint[0]) {
            os_printf("FAIL: llm.endpoint not set\n"); return 1;
        }
        const char *url = cfg.llm_endpoint;
        os_printf("Verifying: %s\n", url);
        os_printf("  model:  %s\n", cfg.llm_model[0] ? cfg.llm_model : "(unset)");
        os_printf("  api_key: %s\n", cfg.llm_api_key[0] ? "***" : "(unset)");
        const char *pu = url;
        int port = 80, is_https = 0;
        if (os_strncmp(pu, "https://", 8) == 0) { pu += 8; port = 443; is_https = 1; }
        else if (os_strncmp(pu, "http://", 7) == 0) { pu += 7; }
        char host[256]="", rpath[512]="/";
        const char *hs = pu; while (*pu && *pu != ':' && *pu != '/') pu++;
        size_t hl = (size_t)(pu - hs); if (hl >= sizeof(host)) { os_printf("FAIL: host too long\n"); return 1; }
        os_memcpy(host, hs, hl); host[hl] = '\0';
        if (*pu == ':') { pu++; port = 0; while (*pu >= '0' && *pu <= '9') { port = port * 10 + (*pu - '0'); pu++; } }
        if (*pu == '/') { size_t pl = os_strlen(pu); if (pl < sizeof(rpath)) os_memcpy(rpath, pu, pl + 1); }
        os_printf("  host: %s  port: %d%s\n", host, port, is_https ? " (TLS)" : "");
        char ip[64];
        if (os_resolve_host(host, ip, sizeof(ip)) != 0) { os_printf("FAIL: DNS\n"); return 1; }
        os_printf("  resolved: %s\n", ip);
        os_socket_t fd = os_socket_create(2, 1, 0);
        if (fd < 0) { os_printf("FAIL: socket\n"); return 1; }
        if (os_socket_connect(fd, ip, port) != 0) { os_socket_close(fd); os_printf("FAIL: TCP connect\n"); return 1; }
        os_printf("  TCP: connected\n");
        if (is_https) { os_socket_close(fd); os_printf("PASS\n"); return 0; }
        char probe[512]; os_snprintf(probe, sizeof(probe), "%smodels", rpath);
        char req[2048]; int rlen;
        if (cfg.llm_api_key[0]) rlen = os_snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nConnection: close\r\n\r\n", probe, host, cfg.llm_api_key);
        else rlen = os_snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", probe, host);
        os_socket_send(fd, req, (size_t)rlen, 0);
        char rbuf[4096]; size_t total = 0;
        for (int i = 0; i < 10; i++) { ssize_t nr = os_socket_recv(fd, rbuf + total, sizeof(rbuf) - total - 1, 0); if (nr <= 0) break; total += (size_t)nr; if (total >= sizeof(rbuf) - 1) break; os_sleep_ms(200); }
        rbuf[total] = '\0'; os_socket_close(fd);
        int status = 0; const char *sp = rbuf; while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') { sp++; while (*sp >= '0' && *sp <= '9') { status = status * 10 + (*sp - '0'); sp++; } }
        if (status == 200) { os_printf("PASS: HTTP 200\n"); return 0; }
        else if (status == 401 || status == 403) { os_printf("FAIL: HTTP %d (auth)\n", status); return 1; }
        else if (total == 0) { os_printf("FAIL: no response\n"); return 1; }
        else { os_printf("FAIL: HTTP %d\n", status); return 1; }
    }

    os_printf("Unknown config command: %s\n", argv[1]);
    return 1;
}
