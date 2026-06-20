/*
 * os_api_posix.c — POSIX/Linux 平台适配实现
 *
 * 所有系统调用和 C 标准库函数均在此层封装。
 * 上层代码不应直接 #include <stdlib.h>、<string.h>、<stdio.h> 等。
 */

#define _DEFAULT_SOURCE

#include "os_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/time.h>

/* =========================================================================
 * 文件操作
 * ========================================================================= */

struct os_file_impl {
    FILE *fp;
};

os_file_handle_t os_file_open(const char *path, const char *mode) {
    FILE *fp = fopen(path, mode);
    if (!fp) return NULL;
    struct os_file_impl *fh = os_alloc(sizeof(*fh));
    if (!fh) { fclose(fp); return NULL; }
    fh->fp = fp;
    return fh;
}

size_t os_file_read(os_file_handle_t fh, void *buf, size_t len) {
    if (!fh) return 0;
    return fread(buf, 1, len, ((struct os_file_impl *)fh)->fp);
}

size_t os_file_write(os_file_handle_t fh, const void *buf, size_t len) {
    if (!fh) return 0;
    return fwrite(buf, 1, len, ((struct os_file_impl *)fh)->fp);
}

int os_file_close(os_file_handle_t fh) {
    if (!fh) return -1;
    struct os_file_impl *impl = (struct os_file_impl *)fh;
    int ret = fclose(impl->fp);
    os_free(impl);
    return ret;
}

int os_file_seek(os_file_handle_t fh, long offset, int whence) {
    if (!fh) return -1;
    return fseek(((struct os_file_impl *)fh)->fp, offset, whence);
}

long os_file_tell(os_file_handle_t fh) {
    if (!fh) return -1;
    return ftell(((struct os_file_impl *)fh)->fp);
}

/* =========================================================================
 * 目录操作
 * ========================================================================= */

struct os_dir_impl {
    DIR *dirp;
    struct dirent *entry;
};

os_dir_handle_t os_dir_open(const char *path) {
    DIR *dirp = opendir(path);
    if (!dirp) return NULL;
    struct os_dir_impl *dh = os_alloc(sizeof(*dh));
    if (!dh) { closedir(dirp); return NULL; }
    dh->dirp = dirp;
    dh->entry = NULL;
    return dh;
}

const char *os_dir_next(os_dir_handle_t dh) {
    if (!dh) return NULL;
    struct os_dir_impl *impl = (struct os_dir_impl *)dh;
    impl->entry = readdir(impl->dirp);
    if (!impl->entry) return NULL;
    return impl->entry->d_name;
}

int os_dir_close(os_dir_handle_t dh) {
    if (!dh) return -1;
    struct os_dir_impl *impl = (struct os_dir_impl *)dh;
    int ret = closedir(impl->dirp);
    os_free(impl);
    return ret;
}

int os_dir_create(const char *path) {
    if (!path) return -1;
    return mkdir(path, 0755);
}

/* =========================================================================
 * 网络操作
 * ========================================================================= */

os_socket_t os_socket_create(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}

int os_socket_connect(os_socket_t fd, const char *addr, int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(addr);
    sa.sin_port = htons((uint16_t)port);
    return connect(fd, (struct sockaddr *)&sa, sizeof(sa));
}

int os_resolve_host(const char *hostname, char *ip_out, size_t ip_len) {
    if (!hostname || !ip_out || ip_len < 16) return -1;
    struct hostent *he = gethostbyname(hostname);
    if (!he || !he->h_addr_list[0]) return -1;
    const char *ip = inet_ntoa(*(struct in_addr *)he->h_addr_list[0]);
    if (!ip) return -1;
    size_t len = strlen(ip);
    if (len >= ip_len) return -1;
    memcpy(ip_out, ip, len + 1);
    return 0;
}

int os_socket_bind(os_socket_t fd, const char *addr, int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = addr ? inet_addr(addr) : INADDR_ANY;
    sa.sin_port = htons((uint16_t)port);
    return bind(fd, (struct sockaddr *)&sa, sizeof(sa));
}

int os_socket_listen(os_socket_t fd, int backlog) {
    return listen(fd, backlog);
}

os_socket_t os_socket_accept(os_socket_t fd, char *peer_addr, int peer_addr_len, int *peer_port) {
    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    os_socket_t client = accept(fd, (struct sockaddr *)&sa, &sa_len);
    if (client < 0) return -1;
    if (peer_addr && peer_addr_len > 0) {
        strncpy(peer_addr, inet_ntoa(sa.sin_addr), (size_t)(peer_addr_len - 1));
        peer_addr[peer_addr_len - 1] = '\0';
    }
    if (peer_port) *peer_port = ntohs(sa.sin_port);
    return client;
}

ssize_t os_socket_recv(os_socket_t fd, void *buf, size_t len, int flags) {
    return recv(fd, buf, len, flags);
}

ssize_t os_socket_send(os_socket_t fd, const void *buf, size_t len, int flags) {
    return send(fd, buf, len, flags);
}

int os_socket_close(os_socket_t fd) {
    return close(fd);
}

int os_socket_set_timeout(os_socket_t fd, int timeout_sec) {
    if (timeout_sec <= 0) return 0;
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    return 0;
}

/* =========================================================================
 * 进程操作
 * ========================================================================= */

os_pid_t os_proc_fork(void) {
    return fork();
}

int os_proc_exec(const char *path, char *const argv[]) {
    return execvp(path, argv);
}

int os_proc_wait(os_pid_t pid, int *status) {
    return waitpid(pid, status, 0);
}

int os_pipe(int fds[2]) {
    return pipe(fds);
}

int os_dup2(int old_fd, int new_fd) {
    return dup2(old_fd, new_fd);
}

int os_fd_close(int fd) {
    return close(fd);
}

/* =========================================================================
 * 线程操作
 * ========================================================================= */

struct os_thread_wrapper {
    os_thread_func_t func;
    void *arg;
};

static void *os_thread_trampoline(void *arg) {
    struct os_thread_wrapper *w = (struct os_thread_wrapper *)arg;
    void *ret = w->func(w->arg);
    os_free(w);
    return ret;
}

int os_thread_create(os_thread_handle_t *handle, os_thread_func_t func, void *arg) {
    pthread_t *tid = os_alloc(sizeof(*tid));
    if (!tid) return -1;
    struct os_thread_wrapper *w = os_alloc(sizeof(*w));
    if (!w) { os_free(tid); return -1; }
    w->func = func;
    w->arg = arg;
    int ret = pthread_create(tid, NULL, os_thread_trampoline, w);
    if (ret != 0) { os_free(w); os_free(tid); return ret; }
    *handle = (os_thread_handle_t)*tid;
    return 0;
}

int os_thread_join(os_thread_handle_t handle) {
    return pthread_join(*(pthread_t *)handle, NULL);
}

int os_thread_detach(os_thread_handle_t handle) {
    return pthread_detach(*(pthread_t *)handle);
}

/* =========================================================================
 * 同步原语
 * ========================================================================= */

struct os_mutex_impl {
    pthread_mutex_t mutex;
};

os_mutex_handle_t os_mutex_create(void) {
    struct os_mutex_impl *m = os_calloc(1, sizeof(*m));
    if (!m) return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int ret = pthread_mutex_init(&m->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (ret != 0) { os_free(m); return NULL; }
    return m;
}

int os_mutex_lock(os_mutex_handle_t m) {
    if (!m) return -1;
    return pthread_mutex_lock(&((struct os_mutex_impl *)m)->mutex);
}

int os_mutex_unlock(os_mutex_handle_t m) {
    if (!m) return -1;
    return pthread_mutex_unlock(&((struct os_mutex_impl *)m)->mutex);
}

int os_mutex_destroy(os_mutex_handle_t m) {
    if (!m) return -1;
    int ret = pthread_mutex_destroy(&((struct os_mutex_impl *)m)->mutex);
    os_free(m);
    return ret;
}

struct os_cond_impl {
    pthread_cond_t cond;
};

os_cond_handle_t os_cond_create(void) {
    struct os_cond_impl *c = os_calloc(1, sizeof(*c));
    if (!c) return NULL;
    pthread_cond_init(&c->cond, NULL);
    return c;
}

int os_cond_signal(os_cond_handle_t c) {
    if (!c) return -1;
    return pthread_cond_signal(&((struct os_cond_impl *)c)->cond);
}

int os_cond_wait(os_cond_handle_t c, os_mutex_handle_t m, int timeout_ms) {
    if (!c || !m) return -1;
    struct os_cond_impl *ci = (struct os_cond_impl *)c;
    struct os_mutex_impl *mi = (struct os_mutex_impl *)m;

    if (timeout_ms < 0) {
        return pthread_cond_wait(&ci->cond, &mi->mutex);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    return pthread_cond_timedwait(&ci->cond, &mi->mutex, &ts);
}

int os_cond_destroy(os_cond_handle_t c) {
    if (!c) return -1;
    int ret = pthread_cond_destroy(&((struct os_cond_impl *)c)->cond);
    os_free(c);
    return ret;
}

/* =========================================================================
 * 时钟
 * ========================================================================= */

uint64_t os_clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

uint64_t os_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

struct timespec os_clock_timespec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

void os_sleep_ms(uint32_t ms) {
    usleep(ms * 1000);
}

int os_time_format(char *buf, size_t buflen, time_t t) {
    struct tm tm_buf;
    struct tm *result = localtime_r(&t, &tm_buf);
    if (!result) return -1;
    return strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", result);
}

/* =========================================================================
 * 内存分配
 * ========================================================================= */

void *os_alloc(size_t size) {
    return malloc(size);
}

void *os_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

void *os_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void os_free(void *ptr) {
    free(ptr);
}

/* =========================================================================
 * 字符串与内存操作
 * ========================================================================= */

void *os_memset(void *ptr, int value, size_t num) {
    return memset(ptr, value, num);
}

void *os_memcpy(void *dest, const void *src, size_t num) {
    return memcpy(dest, src, num);
}

int os_memcmp(const void *s1, const void *s2, size_t n) {
    return memcmp(s1, s2, n);
}

size_t os_strlen(const char *s) {
    return strlen(s);
}

char *os_strncpy(char *dest, const char *src, size_t n) {
    return strncpy(dest, src, n);
}

int os_strcmp(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

int os_strncmp(const char *s1, const char *s2, size_t n) {
    return strncmp(s1, s2, n);
}

char *os_strchr(const char *s, int c) {
    return strchr(s, c);
}

/* =========================================================================
 * 格式化输出
 * ========================================================================= */

int os_snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int os_vsprintf(char *str, size_t size, const char *format, va_list ap) {
    return vsnprintf(str, size, format, ap);
}

int os_fprintf(os_file_handle_t fh, const char *format, ...) {
    if (!fh) return -1;
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(((struct os_file_impl *)fh)->fp, format, ap);
    va_end(ap);
    return ret;
}

int os_fprintf_stderr(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stderr, format, ap);
    va_end(ap);
    return ret;
}

int os_printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}

int os_fputc(int c, os_file_handle_t fh) {
    if (!fh) return EOF;
    return fputc(c, ((struct os_file_impl *)fh)->fp);
}

/* =========================================================================
 * 排序
 * ========================================================================= */

void os_qsort(void *base, size_t nmemb, size_t size, os_cmp_func_t compar) {
    qsort(base, nmemb, size, compar);
}

/* =========================================================================
 * 数学运算
 * ========================================================================= */

int os_abs(int x) {
    return abs(x);
}

/* =========================================================================
 * 环境变量
 * ========================================================================= */

const char *os_env_get(const char *name) {
    return getenv(name);
}

int os_env_set(const char *name, const char *value) {
    return setenv(name, value, 1);
}

/* =========================================================================
 * 系统配置查询
 * ========================================================================= */

long os_sysconf(int name) {
    return sysconf(name);
}

/* =========================================================================
 * 信号处理
 * ========================================================================= */

os_signal_handler_t os_signal_set(int signum, os_signal_handler_t handler) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    struct sigaction old;
    return sigaction(signum, &sa, &old) == 0 ? old.sa_handler : SIG_ERR;
}
