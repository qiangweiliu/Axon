#ifndef OS_API_H
#define OS_API_H

/*
 * os_api.h — 平台适配层: 统一 OS API 接口声明
 *
 * 所有操作(包括 C 标准库函数)均通过此接口访问。
 * 上层代码不直接 #include <stdlib.h>、<string.h>、<stdio.h> 等。
 * 当前仅实现 POSIX/Linux 后端，Windows 预留。
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>

/* =========================================================================
 * 文件操作
 * ========================================================================= */

typedef void *os_file_handle_t;

os_file_handle_t os_file_open(const char *path, const char *mode);
size_t os_file_read(os_file_handle_t fh, void *buf, size_t len);
size_t os_file_write(os_file_handle_t fh, const void *buf, size_t len);
int os_file_close(os_file_handle_t fh);
int os_file_seek(os_file_handle_t fh, long offset, int whence);
long os_file_tell(os_file_handle_t fh);

/* =========================================================================
 * 目录操作
 * ========================================================================= */

typedef void *os_dir_handle_t;

os_dir_handle_t os_dir_open(const char *path);
const char *os_dir_next(os_dir_handle_t dh);
int os_dir_close(os_dir_handle_t dh);

/* Create a directory (mkdir -p equivalent for single level) */
int os_dir_create(const char *path);

/* =========================================================================
 * 网络操作
 * ========================================================================= */

typedef int os_socket_t;

os_socket_t os_socket_create(int domain, int type, int protocol);
int os_socket_connect(os_socket_t fd, const char *addr, int port);
int os_socket_bind(os_socket_t fd, const char *addr, int port);
int os_socket_listen(os_socket_t fd, int backlog);
os_socket_t os_socket_accept(os_socket_t fd, char *peer_addr, int peer_addr_len, int *peer_port);
ssize_t os_socket_recv(os_socket_t fd, void *buf, size_t len, int flags);
ssize_t os_socket_send(os_socket_t fd, const void *buf, size_t len, int flags);
int os_socket_close(os_socket_t fd);

/* DNS resolution */
int os_resolve_host(const char *hostname, char *ip_out, size_t ip_len);

/* =========================================================================
 * 进程操作
 * ========================================================================= */

typedef pid_t os_pid_t;

os_pid_t os_proc_fork(void);
int os_proc_exec(const char *path, char *const argv[]);
int os_proc_wait(os_pid_t pid, int *status);

/* Pipe and FD operations */
int os_pipe(int fds[2]);
int os_dup2(int old_fd, int new_fd);
int os_fd_close(int fd);

/* =========================================================================
 * 线程操作
 * ========================================================================= */

typedef void *os_thread_handle_t;
typedef void *(*os_thread_func_t)(void *);

int os_thread_create(os_thread_handle_t *handle, os_thread_func_t func, void *arg);
int os_thread_join(os_thread_handle_t handle);
int os_thread_detach(os_thread_handle_t handle);

/* =========================================================================
 * 同步原语
 * ========================================================================= */

typedef void *os_mutex_handle_t;

os_mutex_handle_t os_mutex_create(void);
int os_mutex_lock(os_mutex_handle_t m);
int os_mutex_unlock(os_mutex_handle_t m);
int os_mutex_destroy(os_mutex_handle_t m);

typedef void *os_cond_handle_t;

os_cond_handle_t os_cond_create(void);
int os_cond_signal(os_cond_handle_t c);
int os_cond_wait(os_cond_handle_t c, os_mutex_handle_t m, int timeout_ms); /* -1 = infinite */
int os_cond_destroy(os_cond_handle_t c);

/* =========================================================================
 * 时钟
 * ========================================================================= */

uint64_t os_clock_ms(void);
uint64_t os_clock_us(void);
struct timespec os_clock_timespec(void);
void os_sleep_ms(uint32_t ms);
int os_time_format(char *buf, size_t buflen, time_t t);

/* =========================================================================
 * 内存分配
 * ========================================================================= */

void *os_alloc(size_t size);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);
void os_free(void *ptr);

/* =========================================================================
 * 字符串与内存操作
 * ========================================================================= */

void *os_memset(void *ptr, int value, size_t num);
void *os_memcpy(void *dest, const void *src, size_t num);
int os_memcmp(const void *s1, const void *s2, size_t n);
size_t os_strlen(const char *s);
char *os_strncpy(char *dest, const char *src, size_t n);
int os_strcmp(const char *s1, const char *s2);
int os_strncmp(const char *s1, const char *s2, size_t n);
char *os_strchr(const char *s, int c);

/* =========================================================================
 * 格式化输出
 * ========================================================================= */

/*
 * os_printf 系列: 所有格式化输出必须通过这些函数。
 * 注意: os_snprintf 返回值为 int(成功写入的字节数), os_vsnprintf 同理。
 * 可变参数版本需配合 stdarg.h 使用。
 */
int os_snprintf(char *str, size_t size, const char *format, ...);
int os_vsprintf(char *str, size_t size, const char *format, va_list ap);
int os_fprintf(os_file_handle_t fh, const char *format, ...);
int os_fprintf_stderr(const char *format, ...);
int os_printf(const char *format, ...);
int os_fputc(int c, os_file_handle_t fh);

/* =========================================================================
 * 可变参数
 * ========================================================================= */

#include <stdarg.h>

typedef va_list os_va_list;
#define os_va_start(ap, val)    va_start(ap, val)
#define os_va_end(ap)           va_end(ap)

/* =========================================================================
 * 排序
 * ========================================================================= */

typedef int (*os_cmp_func_t)(const void *, const void *);
void os_qsort(void *base, size_t nmemb, size_t size, os_cmp_func_t compar);

/* =========================================================================
 * 数学运算
 * ========================================================================= */

int os_abs(int x);

/* =========================================================================
 * 环境变量
 * ========================================================================= */

const char *os_env_get(const char *name);
int os_env_set(const char *name, const char *value);

/* =========================================================================
 * 系统配置查询
 * ========================================================================= */

long os_sysconf(int name);

/* =========================================================================
 * 信号处理
 * ========================================================================= */

typedef void (*os_signal_handler_t)(int);
os_signal_handler_t os_signal_set(int signum, os_signal_handler_t handler);

#endif /* OS_API_H */
