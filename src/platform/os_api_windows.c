/*
 * os_api_windows.c — Windows 平台预留实现(空壳)
 *
 * 当前为占位实现，仅返回错误码。完整实现需使用 Win32 API。
 */

#include "os_api.h"

/* 所有函数返回错误/NULL，表明平台尚未实现 */

os_file_handle_t os_file_open(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
size_t os_file_read(os_file_handle_t fh, void *buf, size_t len) { (void)fh; (void)buf; (void)len; return 0; }
size_t os_file_write(os_file_handle_t fh, const void *buf, size_t len) { (void)fh; (void)buf; (void)len; return 0; }
int os_file_close(os_file_handle_t fh) { (void)fh; return -1; }
int os_file_seek(os_file_handle_t fh, long offset, int whence) { (void)fh; (void)offset; (void)whence; return -1; }
long os_file_tell(os_file_handle_t fh) { (void)fh; return -1; }

os_dir_handle_t os_dir_open(const char *path) { (void)path; return NULL; }
const char *os_dir_next(os_dir_handle_t dh) { (void)dh; return NULL; }
int os_dir_close(os_dir_handle_t dh) { (void)dh; return -1; }

os_socket_t os_socket_create(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; return -1; }
int os_socket_bind(os_socket_t fd, const char *addr, int port) { (void)fd; (void)addr; (void)port; return -1; }
int os_socket_listen(os_socket_t fd, int backlog) { (void)fd; (void)backlog; return -1; }
os_socket_t os_socket_accept(os_socket_t fd, char *peer_addr, int peer_addr_len, int *peer_port) { (void)fd; (void)peer_addr; (void)peer_addr_len; (void)peer_port; return -1; }
ssize_t os_socket_recv(os_socket_t fd, void *buf, size_t len, int flags) { (void)fd; (void)buf; (void)len; (void)flags; return -1; }
ssize_t os_socket_send(os_socket_t fd, const void *buf, size_t len, int flags) { (void)fd; (void)buf; (void)len; (void)flags; return -1; }
int os_socket_close(os_socket_t fd) { (void)fd; return -1; }

os_pid_t os_proc_fork(void) { return -1; }
int os_proc_exec(const char *path, char *const argv[]) { (void)path; (void)argv; return -1; }
int os_proc_wait(os_pid_t pid, int *status) { (void)pid; (void)status; return -1; }

int os_thread_create(os_thread_handle_t *handle, os_thread_func_t func, void *arg) { (void)handle; (void)func; (void)arg; return -1; }
int os_thread_join(os_thread_handle_t handle) { (void)handle; return -1; }
int os_thread_detach(os_thread_handle_t handle) { (void)handle; return -1; }

os_mutex_handle_t os_mutex_create(void) { return NULL; }
int os_mutex_lock(os_mutex_handle_t m) { (void)m; return -1; }
int os_mutex_unlock(os_mutex_handle_t m) { (void)m; return -1; }
int os_mutex_destroy(os_mutex_handle_t m) { (void)m; return -1; }

os_cond_handle_t os_cond_create(void) { return NULL; }
int os_cond_signal(os_cond_handle_t c) { (void)c; return -1; }
int os_cond_wait(os_cond_handle_t c, os_mutex_handle_t m, int timeout_ms) { (void)c; (void)m; (void)timeout_ms; return -1; }
int os_cond_destroy(os_cond_handle_t c) { (void)c; return -1; }

uint64_t os_clock_ms(void) { return 0; }
uint64_t os_clock_us(void) { return 0; }
struct timespec os_clock_timespec(void) { struct timespec ts = {0, 0}; return ts; }

void *os_alloc(size_t size) { (void)size; return NULL; }
void os_free(void *ptr) { (void)ptr; }

const char *os_env_get(const char *name) { (void)name; return NULL; }
int os_env_set(const char *name, const char *value) { (void)name; (void)value; return -1; }
