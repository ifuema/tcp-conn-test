/*
 * server.c —— 最大并发连接测试的"受端"
 *
 * 设计要点:
 *   1. 单进程 + 单线程 + epoll/kqueue, 无锁无线程栈开销, 便于撑到百万连接
 *   2. 默认 discard 模式(收到数据直接丢弃), 不回显, 避免带宽成为瓶颈
 *   3. 每个连接把 SO_RCVBUF / SO_SNDBUF 调到很小, 大幅降低内核内存占用
 *   4. 支持同时监听多个端口, 与客户端配合扩展四元组容量
 *
 * 编译: cc -O2 -Wall -o server server.c
 */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/resource.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#  include <sys/sysctl.h>
#endif

#include "ev.h"

#define MAX_PORTS 4096

static nb_loop g_loop;
static volatile sig_atomic_t g_quit = 0;

typedef struct {
    const char *bind_ip;    /* NULL = 0.0.0.0 */
    int  ports[MAX_PORTS];
    int  nports;
    int  echo;              /* 1: 收到数据后回显(占带宽, 仅供调试) */
    int  quiet;
    int  rcvbuf;
    int  sndbuf;
    int  backlog;
    int  no_keepalive;
    int  ka_idle;
} cfg_t;

static cfg_t cfg = {
    .bind_ip = NULL,
    .nports  = 0,
    .echo    = 0,
    .quiet   = 0,
    .rcvbuf  = 4096,
    .sndbuf  = 4096,
    .backlog = 65535,
    .no_keepalive = 0,
    .ka_idle = 600,
};

/* 监听 fd 标记表: 以 fd 为下标, O(1) 判定事件来自监听口还是数据连接 */
static unsigned char *g_is_listen;
static int g_is_listen_cap = 0;

static int is_listen_ensure(int fd)
{
    if (fd < g_is_listen_cap) return 0;
    int ncap = g_is_listen_cap ? g_is_listen_cap : 4096;
    while (ncap <= fd) ncap *= 2;
    unsigned char *p = realloc(g_is_listen, (size_t)ncap);
    if (!p) return -1;
    memset(p + g_is_listen_cap, 0, (size_t)(ncap - g_is_listen_cap));
    g_is_listen = p;
    g_is_listen_cap = ncap;
    return 0;
}

/* ---------------- 统计 ---------------- */
static uint64_t g_accepted = 0, g_closed = 0, g_read_err = 0;
static uint64_t g_rx_bytes = 0, g_tx_bytes = 0, g_accept_err = 0;
static uint64_t g_last_accepted = 0, g_last_closed = 0;
static int g_cur = 0, g_peak = 0, g_max_fd_seen = 0;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* 解析端口规格: "8000" / "8000,8001" / "8000-8010" / "8000-8010,9000" */
static int parse_ports(const char *s, int *out, int maxn)
{
    int n = 0;
    const char *p = s;
    while (*p && n < maxn) {
        if (*p == ',' || *p == ' ') { p++; continue; }
        char *end = NULL;
        long a = strtol(p, &end, 10);
        if (end == p) return -1;
        if (*end == '-') {
            p = end + 1;
            long b = strtol(p, &end, 10);
            if (end == p || b < a) return -1;
            for (long v = a; v <= b && n < maxn; v++) out[n++] = (int)v;
        } else {
            out[n++] = (int)a;
        }
        p = end;
        if (*p == ',') p++;
    }
    return n;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "  -b <ip>        监听网卡 IP, 默认 0.0.0.0\n"
        "  -p <ports>     监听端口, 支持 8000 / 8000,8001 / 8000-8031, 默认 8000\n"
        "  -e             回显模式(收到什么发回什么), 默认丢弃(更省带宽)\n"
        "  -B <n>         listen backlog, 默认 65535\n"
        "  -R <n>         每连接 SO_RCVBUF, 默认 4096\n"
        "  -S <n>         每连接 SO_SNDBUF, 默认 4096\n"
        "  -k             关闭 TCP keepalive(默认开启, idle=600s)\n"
        "  -K <sec>       keepalive idle 秒数, 默认 600\n"
        "  -q             静默, 不打印每秒进度\n"
        "  -?             帮助\n", prog);
}


/* listen(fd, backlog) 的 backlog 会被内核静默截断到系统的 somaxconn。
   很多人设了 65535 却实际只有 128, 大并发建连时 accept 队列溢出、
   连接"看起来建上了其实被服务端丢弃"。这里显式读出来做校验。 */
static int read_somaxconn(void)
{
    int v = -1;
#ifdef __linux__
    FILE *fp = fopen("/proc/sys/net/core/somaxconn", "r");
    if (fp) {
        if (fscanf(fp, "%d", &v) != 1) v = -1;
        fclose(fp);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    int val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname("kern.ipc.somaxconn", &val, &len, NULL, 0) == 0) v = val;
#endif
    return v;
}

static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* 把一个已 accept 的 fd 纳入管理 */
static void setup_conn(int fd)
{
    if (fd > g_max_fd_seen) g_max_fd_seen = fd;
    set_nonblock(fd);

    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    if (!cfg.no_keepalive) {
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_KEEPIDLE
        int idle = cfg.ka_idle;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
        int intvl = 30;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
    }
    /* 关键: 百万连接时, 默认 16KB~64KB 的收发缓冲会直接吃光内存。
       这里压到 4KB(内核会翻倍并受 rmem_min 限制, 实际约 4~8KB)。*/
    if (cfg.rcvbuf > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));
    if (cfg.sndbuf > 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cfg.sndbuf, sizeof(cfg.sndbuf));

    if (is_listen_ensure(fd) < 0 || nb_ev_add(&g_loop, fd, EV_READ) < 0) {
        close(fd);
        g_closed++;
        return;
    }
    g_cur++;
    if (g_cur > g_peak) g_peak = g_cur;
}

static void close_conn(int fd)
{
    nb_ev_del(&g_loop, fd);
    close(fd);
    g_cur--;
    g_closed++;
}

static void do_accept(int lfd)
{
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* fd 耗尽: 这是最真实的"容量上限"信号, 必须显式告知用户 */
                static int warned = 0;
                if (!warned) {
                    warned = 1;
                    fprintf(stderr,
                        "\n[!] accept 失败: %s —— 进程/系统 fd 已耗尽, 已达本机容量上限\n",
                        strerror(errno));
                }
                g_accept_err++;
                return;
            }
            g_accept_err++;
            return;
        }
        g_accepted++;
        setup_conn(cfd);
    }
}

/* 收到数据: 丢弃或回显 */
static void do_read(int fd)
{
    char buf[8192];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            g_rx_bytes += (uint64_t)n;
            if (cfg.echo) {
                ssize_t w = send(fd, buf, (size_t)n, 0);
                if (w > 0) g_tx_bytes += (uint64_t)w;
                else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    g_read_err++;
                    close_conn(fd);
                    return;
                }
            }
            if ((size_t)n < sizeof(buf)) return;   /* 已读空 */
            continue;
        }
        if (n == 0) {           /* 对端 FIN, 正常关闭 */
            close_conn(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        g_read_err++;
        close_conn(fd);
        return;
    }
}

static void print_progress(uint64_t now, uint64_t start)
{
    double el = (double)(now - start) / 1e6;
    uint64_t da = g_accepted - g_last_accepted;
    uint64_t dc = g_closed - g_last_closed;
    g_last_accepted = g_accepted;
    g_last_closed = g_closed;
    printf("[%7.1fs] 当前=%-9d 峰值=%-9d accept累计=%-11llu close累计=%-9llu "
           "accept/s=%-7llu close/s=%-6llu rx=%.1fMB maxfd=%d\n",
           el, g_cur, g_peak,
           (unsigned long long)g_accepted, (unsigned long long)g_closed,
           (unsigned long long)da, (unsigned long long)dc,
           (double)g_rx_bytes / 1048576.0, g_max_fd_seen);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int opt;
    const char *portspec = "8000";

    while ((opt = getopt(argc, argv, "b:p:eB:R:S:kK:q?")) != -1) {
        switch (opt) {
        case 'b': cfg.bind_ip = optarg; break;
        case 'p': portspec = optarg; break;
        case 'e': cfg.echo = 1; break;
        case 'B': cfg.backlog = atoi(optarg); break;
        case 'R': cfg.rcvbuf = atoi(optarg); break;
        case 'S': cfg.sndbuf = atoi(optarg); break;
        case 'k': cfg.no_keepalive = 1; break;
        case 'K': cfg.ka_idle = atoi(optarg); break;
        case 'q': cfg.quiet = 1; break;
        case '?': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    cfg.nports = parse_ports(portspec, cfg.ports, MAX_PORTS);
    if (cfg.nports <= 0) {
        fprintf(stderr, "端口规格解析失败: %s\n", portspec);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        printf("RLIMIT_NOFILE: soft=%llu hard=%llu\n",
               (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
        if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 100000)
            fprintf(stderr, "[提示] soft nofile 偏低(%llu), 建议先执行: ulimit -n 1048576\n",
                    (unsigned long long)rl.rlim_cur);
    }

    if (nb_ev_create(&g_loop, 8192) < 0) {
        perror("ev_create");
        return 1;
    }

    /* 建监听套接字 */
    int nlisteners = 0;
    for (int i = 0; i < cfg.nports; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }

        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
        set_nonblock(fd);

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)cfg.ports[i]);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (cfg.bind_ip && inet_pton(AF_INET, cfg.bind_ip, &sa.sin_addr) != 1) {
            fprintf(stderr, "非法 IP: %s\n", cfg.bind_ip);
            return 1;
        }

        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            fprintf(stderr, "bind %s:%d 失败: %s\n",
                    cfg.bind_ip ? cfg.bind_ip : "0.0.0.0", cfg.ports[i], strerror(errno));
            return 1;
        }
        if (listen(fd, cfg.backlog) < 0) {
            fprintf(stderr, "listen %d 失败: %s\n", cfg.ports[i], strerror(errno));
            return 1;
        }
        if (is_listen_ensure(fd) < 0 || nb_ev_add(&g_loop, fd, EV_READ) < 0) {
            perror("ev_add");
            return 1;
        }
        g_is_listen[fd] = 1;
        nlisteners++;
    }

    printf("监听 %d 个端口: %s %s", nlisteners,
           cfg.bind_ip ? cfg.bind_ip : "0.0.0.0", portspec);
    printf(" (backlog=%d, %s模式)\n", cfg.backlog, cfg.echo ? "echo" : "discard");

    int somax = read_somaxconn();
    if (somax > 0) {
        printf("系统 somaxconn : %d\n", somax);
        if (somax < cfg.backlog)
            fprintf(stderr,
                "[警告] backlog 被内核截断为 %d (你请求了 %d)。大并发建连时 accept 队列会溢出,\n"
                "       表现为客户端报「建连成功」但服务端并未收到。修复:\n"
                "         Linux: sysctl -w net.core.somaxconn=65535\n"
                "         macOS: sudo sysctl -w kern.ipc.somaxconn=65535\n",
                somax, cfg.backlog);
    }
    fflush(stdout);

    uint64_t start = now_us();
    uint64_t last_print = start;

    while (!g_quit) {
        int n = nb_ev_wait(&g_loop, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("ev_wait");
            break;
        }
        int fd, mask;
        while (nb_ev_next(&g_loop, &fd, &mask)) {
            if (fd < g_is_listen_cap && g_is_listen[fd]) {
                do_accept(fd);
            } else if (mask & (EV_READ | EV_ERR)) {
                do_read(fd);
            }
        }

        uint64_t now = now_us();
        if (!cfg.quiet && now - last_print >= 1000000) {
            print_progress(now, start);
            last_print = now;
        }
    }

    printf("\n=== 服务端汇总 ===\n");
    printf("峰值并发连接: %d\n", g_peak);
    printf("累计 accept : %llu\n", (unsigned long long)g_accepted);
    printf("累计 close  : %llu\n", (unsigned long long)g_closed);
    printf("accept 失败 : %llu\n", (unsigned long long)g_accept_err);
    printf("读错误关闭  : %llu\n", (unsigned long long)g_read_err);
    printf("最大 fd     : %d\n", g_max_fd_seen);

    nb_ev_destroy(&g_loop);
    free(g_is_listen);
    return 0;
}
