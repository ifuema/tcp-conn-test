/*
 * wincompat.h —— Windows 兼容层（MinGW-w64 / MSYS2 / clang-cl）
 *
 * 只在 _WIN32 下生效，Linux/macOS 下本头文件等同空文件。
 *
 * 设计要点：
 *   1) Windows 的套接字错误走 WSAGetLastError()，不走 errno。
 *      本文件把常用的 socket/connect/recv/... 包一层，失败时把错误翻译成
 *      对应的 errno，这样上层的 errno 判断和 strerror() 全部保持原样。
 *      宏必须定义在 winsock2.h 之后，否则会连系统声明一起改掉。
 *   2) Windows 没有 epoll/kqueue，事件循环用 select() 后端（见 ev.h）。
 *      select 的复杂度是 O(FD_SETSIZE)，所以原生 Windows 版适合几千到
 *      一两万连接的场景；更大规模请用 WSL2（README 有说明）。
 *   3) fcntl/O_NONBLOCK、clock_gettime、getrlimit、signal 这几个在
 *      调用点用 #ifdef 分开处理，不在这里做宏替换（避免和系统声明打架）。
 */
#ifndef NB_WINCOMPAT_H
#define NB_WINCOMPAT_H

#ifdef _WIN32

#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600          /* Vista+: InetPton / WSAPoll */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

/* fd_set 的大小必须在包含 winsock2.h 之前定义 —— 它同时也是 select()
   后端的连接数上限。可用 -DNB_WIN_FD_SETSIZE=16384 覆盖。 */
#ifndef NB_WIN_FD_SETSIZE
#  define NB_WIN_FD_SETSIZE 8192
#endif
#define FD_SETSIZE NB_WIN_FD_SETSIZE

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>                   /* SIO_KEEPALIVE_VALS / tcp_keepalive */
#include <windows.h>
#include <signal.h>                /* sig_atomic_t / SIGINT */
#include <errno.h>
#include <stdint.h>

/* ---------- WSA 错误 -> errno ---------- */
static int nb_wsa_errno(int e)
{
    switch (e) {
    case WSAEWOULDBLOCK:   return EAGAIN;
    case WSAEINPROGRESS:   return EINPROGRESS;
    case WSAEINTR:         return EINTR;
    case WSAETIMEDOUT:     return ETIMEDOUT;
    case WSAECONNREFUSED:  return ECONNREFUSED;
    case WSAECONNRESET:    return ECONNRESET;
    case WSAECONNABORTED:  return ECONNABORTED;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAEADDRINUSE:    return EADDRINUSE;
    case WSAEMFILE:        return EMFILE;
    case WSAENOBUFS:       return ENOBUFS;
    case WSAEHOSTUNREACH:  return EHOSTUNREACH;
    case WSAENETUNREACH:   return ENETUNREACH;
    case WSAEACCES:        return EACCES;
    case WSAEISCONN:       return EISCONN;
    case WSAEALREADY:      return EALREADY;
    case WSAENOTCONN:      return ENOTCONN;
    default:               return EIO;
    }
}

/* 取错误码并同时写进 errno，让上层 strerror(errno) 能正常显示 */
static int nb_lasterr(void)
{
    int e = nb_wsa_errno(WSAGetLastError());
    errno = e;
    return e;
}

/* ---------- 带 errno 翻译的套接字包装 ----------
   注意函数体里用 (xxx)(...) 的写法阻止宏展开。 */
static inline int nb_socket(int af, int type, int proto)
{
    SOCKET s = (socket)(af, type, proto);
    if (s == INVALID_SOCKET) { nb_lasterr(); return -1; }
    return (int)s;
}

static inline int nb_connect(int fd, const struct sockaddr *addr, int len)
{
    if ((connect)((SOCKET)fd, addr, len) == 0) return 0;
    nb_lasterr();
    return -1;
}

static inline int nb_bind(int fd, const struct sockaddr *addr, int len)
{
    if ((bind)((SOCKET)fd, addr, len) == 0) return 0;
    nb_lasterr();
    return -1;
}

static inline int nb_listen(int fd, int backlog)
{
    if ((listen)((SOCKET)fd, backlog) == 0) return 0;
    nb_lasterr();
    return -1;
}

static inline int nb_accept(int fd, struct sockaddr *addr, int *len)
{
    SOCKET s = (accept)((SOCKET)fd, addr, len);
    if (s == INVALID_SOCKET) { nb_lasterr(); return -1; }
    return (int)s;
}

static inline long nb_send(int fd, const void *buf, size_t len, int flags)
{
    int r = (send)((SOCKET)fd, (const char *)buf, (int)len, flags);
    if (r < 0) { nb_lasterr(); return -1; }
    return (long)r;
}

static inline long nb_recv(int fd, void *buf, size_t len, int flags)
{
    int r = (recv)((SOCKET)fd, (char *)buf, (int)len, flags);
    if (r < 0) { nb_lasterr(); return -1; }
    return (long)r;
}

static inline int nb_setsockopt(int fd, int level, int name,
                                const void *val, int len)
{
    if ((setsockopt)((SOCKET)fd, level, name, (const char *)val, len) == 0) return 0;
    nb_lasterr();
    return -1;
}

static inline int nb_getsockopt(int fd, int level, int name, void *val, int *len)
{
    if ((getsockopt)((SOCKET)fd, level, name, (char *)val, len) == 0) return 0;
    nb_lasterr();
    return -1;
}

static inline int nb_close(int fd)
{
    if ((closesocket)((SOCKET)fd) == 0) return 0;
    nb_lasterr();
    return -1;
}

#define socket     nb_socket
#define connect    nb_connect
#define bind       nb_bind
#define listen     nb_listen
#define accept     nb_accept
#define send       nb_send
#define recv       nb_recv
#define setsockopt nb_setsockopt
#define getsockopt nb_getsockopt
#define close      nb_close

/* ---------- SO_ERROR 翻译 ----------
   Windows 的 getsockopt(SO_ERROR) 返回的是 WSA 错误码(如 10061 = WSAECONNREFUSED),
   而不是 errno。不翻译的话, 上层的失败归因(超时/拒绝/重置/端口耗尽)会全部落空,
   报告里只会显示一堆 "Unknown error"。 */
static inline int nb_so_error(int fd, int *out)
{
    int e = 0;
    int len = (int)sizeof(e);
    if (nb_getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len) < 0) return -1;
    *out = (e == 0) ? 0 : nb_wsa_errno(e);
    errno = *out;
    return 0;
}

/* ---------- 非阻塞 ---------- */
static inline int nb_set_nonblock(int fd)
{
    u_long mode = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0) { nb_lasterr(); return -1; }
    return 0;
}

/* ---------- TCP keepalive（Windows 用 SIO_KEEPALIVE_VALS，没有 TCP_KEEPIDLE） ---------- */
static inline int nb_set_keepalive(int fd, int on, int idle_sec, int intvl_sec)
{
    if (!on) {
        int off = 0;
        return nb_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &off, sizeof(off));
    }
    int one = 1;
    if (nb_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) < 0) return -1;

    struct tcp_keepalive ka;
    DWORD returned = 0;
    ka.onoff = 1;
    ka.keepalivetime    = (u_long)(idle_sec > 0 ? idle_sec : 600) * 1000UL;
    ka.keepaliveinterval = (u_long)(intvl_sec > 0 ? intvl_sec : 10) * 1000UL;
    if (WSAIoctl((SOCKET)fd, SIO_KEEPALIVE_VALS, &ka, sizeof(ka),
                 NULL, 0, &returned, NULL, NULL) != 0) {
        nb_lasterr();
        return -1;
    }
    return 0;
}

/* ---------- 单调时钟（QueryPerformanceCounter） ---------- */
static inline uint64_t nb_now_us(void)
{
    LARGE_INTEGER freq, cnt;
    static double us_per_tick = 0.0;
    if (us_per_tick == 0.0) {
        QueryPerformanceFrequency(&freq);
        us_per_tick = 1000000.0 / (double)freq.QuadPart;
    }
    QueryPerformanceCounter(&cnt);
    return (uint64_t)((double)cnt.QuadPart * us_per_tick);
}

/* ---------- 最小 getopt 实现 ----------
   MinGW 各发行版对 getopt 的声明不统一, MSVC 干脆没有, 这里自带一份。
   只支持本工具用到的形式: -x / -xvalue / -x value, 以及 "--" 结束。 */
static char *optarg = NULL;
static int   optind = 1;
static int   opterr = 1;
static int   optopt = 0;

static inline int getopt(int argc, char *const argv[], const char *opts)
{
    static int sp = 1;              /* 当前参数内的字符下标 */
    const char *cp;

    optarg = NULL;
    if (sp == 1) {
        if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0')
            return -1;
        if (strcmp(argv[optind], "--") == 0) { optind++; return -1; }
    }
    int c = (unsigned char)argv[optind][sp];
    optopt = c;
    if (c == ':' || (cp = strchr(opts, c)) == NULL) {
        if (opterr) fprintf(stderr, "未知选项: -%c\n", c);
        if (argv[optind][++sp] == '\0') { optind++; sp = 1; }
        return '?';
    }
    if (cp[1] == ':') {
        if (argv[optind][sp + 1] != '\0') {
            optarg = &argv[optind][sp + 1];
        } else if (++optind < argc) {
            optarg = argv[optind];
        } else {
            if (opterr) fprintf(stderr, "选项 -%c 缺少参数\n", c);
            sp = 1;
            return '?';
        }
        optind++;
        sp = 1;
    } else {
        if (argv[optind][++sp] == '\0') { optind++; sp = 1; }
    }
    return c;
}

/* Windows 下 select() 后端的连接数上限，用于替代 getrlimit(RLIMIT_NOFILE) */
#define NB_WIN_MAX_CONNS NB_WIN_FD_SETSIZE

/* 初始化 / 清理 Winsock，main() 开头调用一次 */
static inline int nb_wsa_init(void)
{
    WSADATA wd;
    return WSAStartup(MAKEWORD(2, 2), &wd);
}

#endif /* _WIN32 */
#endif /* NB_WINCOMPAT_H */
