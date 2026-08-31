/*
 * client.c —— 最大并发 TCP 连接数测试客户端
 *
 * 核心目标: 测的是"能同时Hold住多少条连接", 而不是"每秒能新建多少条连接(CPS)"。
 * 很多系统/中间设备(云厂商安全组、LVS、nf_conntrack、SLB、网卡队列、SYN cookie)
 * 都会对 CPS 单独限流, 如果疯狂建连, 撞到的往往是 CPS 上限而非并发上限。
 * 因此本程序:
 *
 *   1. 定速建连: 默认 50 条/秒(可用 -r 调整), 逐条发起, 不用突发洪峰
 *   2. 非阻塞 connect + 单线程 epoll/kqueue, 未完成握手数受 -I 限制
 *   3. 连接建立后长期保持, 不收发业务数据, 只做低频心跳探测存活
 *   4. 失败自动归因: 区分 fd 耗尽 / 本地端口(四元组)耗尽 / 对端拒绝 / 超时
 *   5. 自适应降速: 出现连续非致命失败时自动减半速率, 避免把限流误判成容量上限
 *
 * 编译: cc -O2 -Wall -o client client.c
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
#include <poll.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/resource.h>

#include "ev.h"

#define MAX_PORTS 4096
#define MAX_SRCS  256

/* 连接状态 */
#define ST_CONNECTING   0
#define ST_ESTABLISHED  1

/* 测试模式 */
#define MODE_CONN   0     /* 并发容量: 慢速建连并长期保持 */
#define MODE_CPS    1     /* 新建速率: 阶梯加压找 CPS 拐点, 连上即关 */

/* 单档判定 */
#define VERDICT_PASS 0    /* 成功率 >= 95% */
#define VERDICT_WARN 1    /* 80% ~ 95%, 临界 */
#define VERDICT_FAIL 2    /* < 80% */
#define VERDICT_UNREACHED 3  /* 客户端根本没发出这么多, 速率未达成 */

#define MAX_STEPS   64
#define DEFAULT_CPS_STEPS "200,500,1000,2000,4000,8000,16000"

/* 一档加压的统计快照与结果 */
typedef struct {
    double   target;              /* 本档目标速率(条/秒) */
    uint64_t base_attempted, base_established, base_failed, base_timeout;
    uint64_t base_errno[256];
    uint64_t end_errno[256];
    uint64_t base_lat_n;
    uint64_t attempts, success, failed, timeout;
    double   actual_cps, success_rate, rate_achieved, attempt_rate;
    double   window_full_ratio;   /* 握手窗口被打满的时间占比 */
    double   p99_ratio;           /* 相对首个 PASS 档的 p99 恶化倍数 */
    double   p50, p99, p999;
    int      verdict;
} step_t;

/* 结束原因 */
enum {
    STOP_RUNNING = 0,
    STOP_GOAL,
    STOP_FD_EXHAUSTED,
    STOP_PORTS_EXHAUSTED,
    STOP_TIMEOUT,
    STOP_SIGNAL,
    STOP_STALLED,     /* 连接数长时间不再增长, 判定已达上限 */
    STOP_FATAL
};

typedef struct {
    const char *host;
    int   ports[MAX_PORTS];
    int   nports;
    struct in_addr srcs[MAX_SRCS];
    int   nsrcs;
    const char *portspec;     /* 原始端口规格, 仅用于报告展示 */

    int   max_conns;      /* 目标并发连接数 */
    double rate;          /* 新建连接速率(条/秒) */
    int   max_inflight;   /* 未完成握手上限 */
    int   conn_timeout;   /* 单次 connect 超时(秒), 0=交给内核(约127s) */
    int   hb_interval;    /* 心跳周期(秒), 0=关闭 */
    int   hb_batch;       /* 心跳每秒最多探测条数 */
    int   payload;        /* 建连成功后立即发送的字节数 */
    int   hold_sec;       /* 达标后继续观察秒数 */
    int   time_limit;     /* 总时限(秒), 0=不限 */
    int   keepalive;
    int   ka_idle;
    int   rcvbuf;
    int   sndbuf;
    int   adaptive;
    int   quiet;
    const char *report;

    int   mode;               /* MODE_CONN / MODE_CPS */
    step_t steps[MAX_STEPS];  /* CPS 档位 */
    int   nsteps;
    int   cps_hold;           /* 每档持续秒数 */
    int   linger_rst;         /* 建连后强制 RST 关闭, 避免 TIME_WAIT 堆积 */
    int   stall_sec;          /* 存活数多久不净增长就判定触顶并自动停止, 0=不自动停 */
    int   max_batch;          /* 单次循环最多补发多少条(令牌桶), 默认 512 */
} cfg_t;

static cfg_t cfg = {
    .host = NULL,
    .nports = 0,
    .nsrcs = 0,
    .max_conns = 10000,
    .rate = 50.0,
    .max_inflight = 64,
    .conn_timeout = 15,
    .hb_interval = 10,
    .hb_batch = 200000,
    .payload = 0,
    .hold_sec = 30,
    .time_limit = 0,
    .keepalive = 0,
    .ka_idle = 600,
    .rcvbuf = 4096,
    .sndbuf = 4096,
    .adaptive = 1,
    .quiet = 0,
    .report = NULL,

    .mode = MODE_CONN,
    .nsteps = 0,
    .cps_hold = 10,
    .linger_rst = -1,         /* -1 = 未指定, 由模式决定默认值 */
    .stall_sec = 60,          /* 默认 60 秒无增长即判定触顶 */
    .max_batch = 512,
};

/* -------------------- 连接与全局状态 -------------------- */
typedef struct conn {
    int      fd;
    int      state;
    uint64_t t0_us;
    uint16_t dport;
    int      dst_idx;
    int      hb_ok;
    int      pend_idx;    /* 在握手队列中的下标, -1 表示不在队列里 */
} conn_t;

static conn_t **g_conns = NULL;      /* 连接列表(用于遍历心跳) */
static int g_nconns = 0, g_cap = 0;
static conn_t **g_fd2conn = NULL;    /* fd -> conn, O(1) 反查 */
static int g_fd2conn_cap = 0;

static nb_loop g_loop;
static volatile sig_atomic_t g_sigint = 0;

static struct sockaddr_in g_dst[MAX_PORTS];
static int g_dst_idx = 0;
static int g_src_idx = 0;

static double *g_lat = NULL;         /* 建连延迟样本(ms) */
static uint64_t g_lat_n = 0, g_lat_cap = 0;

/* 握手中连接的队列: 只存 ST_CONNECTING, 规模受 -I 限制(默认64), 每次超时检查是 O(64)。
   没有它, 一旦上游静默丢 SYN, 挂死的连接会永久占满 inflight 槽位, 程序直接停摆。 */
static conn_t **g_pend = NULL;
static int g_pend_n = 0, g_pend_cap = 0;
static uint64_t g_conn_timeout_us = 0;

typedef struct {
    uint64_t attempted;
    uint64_t established;
    uint64_t failed;
    uint64_t peer_closed;   /* 对端 FIN/RST 导致的关闭 */
    uint64_t timeout;       /* connect 超时(SYN 发出去没有任何响应) */
    uint64_t hb_sent;
    uint64_t hb_dead;       /* 心跳时发现已失效的连接 */
    uint64_t rx_bytes;
    uint64_t errno_cnt[256];
} stats_t;

static stats_t g_st;
static int  g_inflight = 0;
static int  g_cur_est = 0;
static int  g_peak_est = 0;
static int  g_stop_reason = STOP_RUNNING;
static int  g_fatal = 0;
static double g_cur_rate;
static uint64_t g_interval_us;
static double   g_debt = 0;           /* 令牌桶欠账: 应发而未发的条数(可为小数) */
static uint64_t g_last_tick_us = 0;   /* 上次发放额度的时间 */
static int  g_consec_fail = 0;
static uint64_t g_goal_us = 0;
static uint64_t g_start_us = 0;

static int      g_cps_mode = 0;       /* CPS 模式生效中 */
static int      g_step_idx = 0;       /* 当前档位下标 */
static uint64_t g_step_start_us = 0;  /* 当前档开始时间 */
static double   g_cps_ceiling = 0;    /* 保守上限: 最后一个 PASS 档 */
static double   g_cps_critical = 0;   /* 临界值: 最后一个非 FAIL 档 */
static double   g_cps_critical_actual = 0;  /* 临界档实际跑到的速率 */
static int      g_cps_done = 0;       /* 档位跑完(或提前终止) */
static uint64_t g_step_samples = 0;   /* 本档采样次数 */
static uint64_t g_step_full = 0;      /* 其中握手窗口被打满的次数 */
static double   g_base_p99 = 0;       /* 首个 PASS 档的 p99, 作为延迟基线 */
static uint64_t g_last_growth_us = 0; /* 上次"净增长"的时刻(用于停滞检测) */
static int      g_stall_ref_est = 0;  /* 停滞窗口起点的存活数, 用于算净增长 */
static double   g_stall_elapsed = 0;  /* 停止时的停滞时长(秒) */
static uint64_t g_loop_iters = 0;     /* 主循环迭代次数(观测客户端自身是否吃力) */
static uint64_t g_loop_batch_sum = 0; /* 累计实际发起条数 */

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

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) g_sigint = 1;
}

/* -------------------- 解析 -------------------- */
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

static int parse_srcs(const char *s, struct in_addr *out, int maxn)
{
    int n = 0;
    const char *p = s;
    while (*p && n < maxn) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        char buf[64];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        if (inet_pton(AF_INET, buf, &out[n]) != 1) return -1;
        n++;
        if (!q) break;
        p = q + 1;
    }
    return n;
}

/* 解析档位表: "200,500,1000" -> 速率数组 */
static int parse_steps(const char *s, step_t *out, int maxn)
{
    int n = 0;
    const char *p = s;
    while (*p && n < maxn) {
        if (*p == ',' || *p == ' ') { p++; continue; }
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p || v <= 0) return -1;
        out[n++].target = v;
        p = end;
        if (*p == ',') p++;
    }
    return n;
}

static int resolve_host(const char *host, struct in_addr *out)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, NULL, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "解析主机失败 %s: %s\n", host, gai_strerror(r));
        return -1;
    }
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s -s <host> [选项]\n"
        "\n"
        "=== 基本 ===\n"
        "  -s <host>      目标主机(IP 或域名), 必填\n"
        "  -p <ports>     目标端口, 支持 8000 / 8000,8001 / 8000-8031, 默认 8000\n"
        "  -c <n>         目标并发连接数, 默认 10000\n"
        "  -r <rate>      新建连接速率(条/秒), 默认 50 —— 调低可规避 CPS 限流\n"
        "  -I <n>         最大未完成握手数, 默认 64\n"
        "  -T <sec>       单次 connect 超时(秒), 默认 15, 0=交给内核(约127s)\n"
        "  -G <sec>       存活数多久不\"净增长\"就判定触顶并自动停止, 默认 60, 0=不自动停\n"
        "                 覆盖两种触顶形态: 建连失败(SYN 被丢), 以及建得上但旧连接不断\n"
        "                 被对端关闭(会话表满后踢旧建新) —— 后者失败数一直是 0\n"
        "\n"
        "=== 测试模式 (-m) ===\n"
        "  -m conn        并发容量模式(默认): 慢速建连并长期保持, 测能Hold住多少条\n"
        "  -m cps         新建速率模式: 阶梯加压找 CPS 拐点, 连上即关, 自动出结论与原因\n"
        "  -Z <rates>     CPS 档位表, 逗号分隔\n"
        "                 默认 200,500,1000,2000,4000,8000,16000\n"
        "  -Y <sec>       CPS 每档持续秒数, 默认 10\n"
        "  -L             短连接强制 RST 关闭(避免 TIME_WAIT 占满端口), CPS 模式默认开\n"
        "  -M <n>         单次循环最多补发条数(令牌桶), 默认 512\n"
        "                 定时器精度约1ms, 高速档必须靠批量补发才能逼近目标速率\n"
        "\n"
        "=== 突破四元组限制 ===\n"
        "  -b <ips>       绑定源 IP 列表, 逗号分隔, 例: 10.0.0.5,10.0.0.6\n"
        "                 (需这些 IP 已配置在本机网卡上)\n"
        "                 四元组容量 ≈ 源IP数 × 本地端口数 × 目标端口数\n"
        "\n"
        "=== 连接保持与探测 ===\n"
        "  -B <sec>       心跳周期(秒), 0=关闭, 默认 10\n"
        "  -P <n>         心跳每秒最多探测条数, 默认 200000\n"
        "  -n <bytes>     建连成功后立即发送的字节数, 默认 0\n"
        "  -w <sec>       达标后继续观察多少秒, 默认 30(用于观察是否有连接被悄悄断开)\n"
        "  -t <sec>       总时限(秒), 0=不限\n"
        "  -k             开启 TCP keepalive(idle 默认 600s)\n"
        "  -K <sec>       keepalive idle 秒数\n"
        "  -R/-S <n>      SO_RCVBUF / SO_SNDBUF, 默认 4096/4096\n"
        "\n"
        "=== 其它 ===\n"
        "  -A             关闭自适应降速\n"
        "  -q             静默(只输出最终报告)\n"
        "  -o <file>      把最终报告写入文件\n"
        "  -?             帮助\n",
        prog);
}

/* -------------------- 连接管理 -------------------- */
static int fd2conn_ensure(int fd)
{
    if (fd < g_fd2conn_cap) return 0;
    int ncap = g_fd2conn_cap ? g_fd2conn_cap : 4096;
    while (ncap <= fd) ncap *= 2;
    conn_t **p = realloc(g_fd2conn, (size_t)ncap * sizeof(conn_t *));
    if (!p) return -1;
    memset(p + g_fd2conn_cap, 0, (size_t)(ncap - g_fd2conn_cap) * sizeof(conn_t *));
    g_fd2conn = p;
    g_fd2conn_cap = ncap;
    return 0;
}

static int conns_reserve(void)
{
    if (g_nconns < g_cap) return 0;
    int ncap = g_cap ? g_cap * 2 : 1024;
    conn_t **p = realloc(g_conns, (size_t)ncap * sizeof(conn_t *));
    if (!p) return -1;
    g_conns = p;
    g_cap = ncap;
    return 0;
}

static void pend_remove(conn_t *c)
{
    if (c->pend_idx < 0) return;
    int i = c->pend_idx;
    if (i < g_pend_n && g_pend[i] == c) {
        g_pend[i] = g_pend[--g_pend_n];
        g_pend[i]->pend_idx = i;
    }
    c->pend_idx = -1;
}

static int pend_push(conn_t *c)
{
    if (g_pend_n >= g_pend_cap) {
        int ncap = g_pend_cap ? g_pend_cap * 2 : 256;
        conn_t **np = realloc(g_pend, (size_t)ncap * sizeof(conn_t *));
        if (!np) return -1;
        g_pend = np;
        g_pend_cap = ncap;
    }
    g_pend[g_pend_n] = c;
    c->pend_idx = g_pend_n;
    g_pend_n++;
    return 0;
}

static void remove_conn(conn_t *c);
static void on_connect_fail(int e);

/* 从后往前遍历 + swap-remove: 被换到位置 i 的元素此前已检查过, 不会漏检也不会重复 */
static void check_conn_timeout(uint64_t now)
{
    if (g_conn_timeout_us == 0 || g_pend_n == 0) return;
    for (int i = g_pend_n - 1; i >= 0; i--) {
        conn_t *c = g_pend[i];
        if (now - c->t0_us < g_conn_timeout_us) continue;
        pend_remove(c);          /* 先出队, remove_conn 内部还会再调一次(幂等) */
        g_inflight--;
        g_st.timeout++;
        remove_conn(c);
        on_connect_fail(ETIMEDOUT);
    }
}

#define LAT_MAX_SAMPLES 4000000   /* 400万样本 = 32MB, 到顶后不再记录分位数 */

static void lat_push(double ms)
{
    if (g_lat_n >= LAT_MAX_SAMPLES) return;
    if (g_lat_n >= g_lat_cap) {
        int ncap = g_lat_cap ? (int)(g_lat_cap * 2) : 8192;
        double *p = realloc(g_lat, (size_t)ncap * sizeof(double));
        if (!p) return;
        g_lat = p;
        g_lat_cap = (uint64_t)ncap;
    }
    g_lat[g_lat_n++] = ms;
}

static int lat_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void remove_conn(conn_t *c)
{
    pend_remove(c);          /* 幂等, 保证任何关闭路径都不会在队列里留残骸 */
    for (int i = 0; i < g_nconns; i++) {
        if (g_conns[i] == c) {
            g_conns[i] = g_conns[--g_nconns];
            break;
        }
    }
    nb_ev_del(&g_loop, c->fd);
    close(c->fd);
    if (c->fd < g_fd2conn_cap) g_fd2conn[c->fd] = NULL;
    if (c->state == ST_ESTABLISHED) g_cur_est--;
    free(c);
}

static void count_errno(int e)
{
    if (e >= 0 && e < 256) g_st.errno_cnt[e]++;
}

/* 建连失败处理: 归因 + 自适应降速 */
static void on_connect_fail(int e)
{
    g_st.failed++;
    count_errno(e);

    if (e == EMFILE || e == ENFILE) {
        fprintf(stderr,
            "\n[!] connect 失败: %s —— 进程/系统 fd 耗尽, 这就是本机的并发连接上限\n",
            strerror(e));
        g_fatal = 1;
        g_stop_reason = STOP_FD_EXHAUSTED;
        return;
    }
    if (e == EADDRNOTAVAIL || e == EADDRINUSE) {
        /* 本地端口/四元组耗尽: 若还有其它目标端口可用则继续, 否则判定上限 */
        if (cfg.nports > 1) {
            g_consec_fail++;
        } else {
            fprintf(stderr,
                "\n[!] connect 失败: %s —— 本地端口(四元组)耗尽\n"
                "    解决: 加 -p 扩大目标端口范围, 或加 -b 绑定更多源 IP\n",
                strerror(e));
            g_fatal = 1;
            g_stop_reason = STOP_PORTS_EXHAUSTED;
        }
        return;
    }
    if (e == ECONNREFUSED) {
        fprintf(stderr, "\n[!] 目标拒绝连接(ECONNREFUSED), 请确认服务端已启动\n");
        g_fatal = 1;
        g_stop_reason = STOP_FATAL;
        return;
    }
    if (e == ETIMEDOUT || e == EHOSTUNREACH || e == ENETUNREACH) {
        g_consec_fail++;
    } else {
        g_consec_fail++;
    }

    /* 自适应: 连续失败 5 次就把速率减半, 把"限流/抖动"与"硬上限"区分开。
       CPS 阶梯模式下必须关闭 —— 阶梯加压本来就是要看"这一档会不会失败",
       自适应降速会直接覆盖档位的目标速率, 导致高档位永远发不出去
       (实测: 8000/s 档被连续降到 500/s, 达成率只剩 13%)。 */
    if (cfg.adaptive && !g_cps_mode && g_consec_fail >= 5) {
        g_consec_fail = 0;
        double nr = g_cur_rate / 2.0;
        if (nr < 1.0) nr = 1.0;
        if (nr < g_cur_rate) {
            fprintf(stderr, "[自适应] 连续失败, 建连速率 %.1f -> %.1f 条/秒\n", g_cur_rate, nr);
            g_cur_rate = nr;
            g_interval_us = (uint64_t)(1000000.0 / g_cur_rate);
        }
    }
}

/* 停滞检测的"净增长"判定。
   只看"有没有建连成功"是不够的: 链路会话表满时, 中间设备会踢掉旧连接给新连接让位,
   此时 established 一路在涨、failed 一直是 0, 但存活数原地不动 —— 程序会永远跑下去。
   所以必须看净增长(存活数有没有真的创新高), 容忍阈值用于吸收存活数的常态抖动。 */
static void stall_note_growth(void)
{
    int threshold = g_stall_ref_est / 200;
    if (threshold < 3) threshold = 3;
    if (g_cur_est >= g_stall_ref_est + threshold) {
        g_stall_ref_est = g_cur_est;
        g_last_growth_us = now_us();
    }
}

/* 停滞检测的前提: 必须已经出现过异常信号(建连失败 / 被对端关闭 / 心跳失效)。
   否则说明链路一切正常, 只是在慢慢建连, 不该自动判定触顶。 */
static int stall_armed(void)
{
    return (g_st.failed > 0 || g_st.peer_closed > 0 || g_st.hb_dead > 0);
}

/* 建连成功 */
static void on_connect_ok(conn_t *c)
{
    g_inflight--;
    pend_remove(c);
    g_consec_fail = 0;
    g_st.established++;

    double ms = (double)(now_us() - c->t0_us) / 1000.0;
    lat_push(ms);

    /* CPS 模式: 连上立即关闭, 测的是"新建速率"而不是"能保持多少"。
       刻意不把 state 改成 ESTABLISHED —— 这样 remove_conn 不会去减 g_cur_est,
       也不需要额外判断, 并发计数天然保持为 0。 */
    if (g_cps_mode) {
        remove_conn(c);
        return;
    }

    c->state = ST_ESTABLISHED;
    g_cur_est++;
    if (g_cur_est > g_peak_est) g_peak_est = g_cur_est;
    stall_note_growth();

    /* 建连后只关注可读, 用于感知对端 FIN/RST */
    nb_ev_mod(&g_loop, c->fd, EV_READ);

    if (cfg.payload > 0) {
        char *buf = calloc(1, (size_t)cfg.payload);
        if (buf) {
            ssize_t n = send(c->fd, buf, (size_t)cfg.payload, 0);
            if (n > 0) g_st.hb_sent += 0;   /* payload 不计入心跳统计 */
            free(buf);
        }
    }
}

static int start_conn(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        on_connect_fail(errno);
        return -1;
    }
    if (set_nonblock(fd) < 0) { close(fd); on_connect_fail(errno); return -1; }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    if (cfg.keepalive) {
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
    /* CPS 模式是短连接, 调缓冲没有意义(连上就关), 省掉两次系统调用以提升新建速率 */
    if (!g_cps_mode) {
        if (cfg.rcvbuf > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));
        if (cfg.sndbuf > 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cfg.sndbuf, sizeof(cfg.sndbuf));
    }

    /* CPS 模式必须开: 短连接高速率时, 主动关闭方会堆积海量 TIME_WAIT(默认60s),
       几万条就把本地端口占光, 后续全部 EADDRNOTAVAIL。
       SO_LINGER{l_onoff=1,l_linger=0} 让 close() 直接发 RST, 不进 TIME_WAIT。 */
    if (cfg.linger_rst > 0) {
        struct linger lg;
        lg.l_onoff  = 1;
        lg.l_linger = 0;
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }

    /* 绑定源 IP(可选): 扩展四元组空间, 突破单 IP 的 6 万端口上限 */
    if (cfg.nsrcs > 0) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr = cfg.srcs[g_src_idx++ % cfg.nsrcs];
        sa.sin_port = 0;                 /* 端口交给内核自动分配 */
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            int e = errno;
            close(fd);
            on_connect_fail(e);
            return -1;
        }
    }

    int di = g_dst_idx++ % cfg.nports;
    struct sockaddr_in dst = g_dst[di];

    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) { close(fd); return -1; }
    c->fd = fd;
    c->state = ST_CONNECTING;
    c->t0_us = now_us();
    c->dport = ntohs(dst.sin_port);
    c->dst_idx = di;

    int r = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (r == 0) {
        /* 本机/回环可能立即完成 */
        if (conns_reserve() < 0 || fd2conn_ensure(fd) < 0) { close(fd); free(c); return -1; }
        g_conns[g_nconns++] = c;
        g_fd2conn[fd] = c;
        nb_ev_mod(&g_loop, fd, EV_READ);
        g_inflight++;
        g_st.attempted++;
        on_connect_ok(c);
        return 0;
    }
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
        int e = errno;
        close(fd);
        free(c);
        on_connect_fail(e);
        return -1;
    }

    if (conns_reserve() < 0 || fd2conn_ensure(fd) < 0) { close(fd); free(c); return -1; }
    g_conns[g_nconns++] = c;
    g_fd2conn[fd] = c;
    /* 等待可写事件 = 三次握手完成(或失败) */
    if (nb_ev_mod(&g_loop, fd, EV_WRITE) < 0) {
        remove_conn(c);
        on_connect_fail(errno);
        return -1;
    }
    g_inflight++;
    g_st.attempted++;
    if (pend_push(c) < 0) { remove_conn(c); g_inflight--; return -1; }
    return 0;
}

/* 处理一个事件 */
static void handle_event(int fd, int mask)
{
    conn_t *c = (fd < g_fd2conn_cap) ? g_fd2conn[fd] : NULL;
    if (!c) return;

    if (c->state == ST_CONNECTING) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
        if (err == 0) {
            on_connect_ok(c);
        } else {
            g_inflight--;
            remove_conn(c);
            on_connect_fail(err);
        }
        return;
    }

    /* 已建立的连接 */
    if (mask & EV_READ) {
        char buf[4096];
        for (;;) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n > 0) { g_st.rx_bytes += (uint64_t)n; continue; }
            if (n == 0) {                    /* 对端关闭 */
                g_st.peer_closed++;
                remove_conn(c);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            g_st.peer_closed++;
            remove_conn(c);
            return;
        }
    }
    if (mask & EV_ERR) {
        g_st.peer_closed++;
        remove_conn(c);
    }
}

/* 心跳: 每轮遍历全量连接, 分摊到每秒执行, 用 1 字节探测连接是否还活着 */
static void heartbeat_tick(void)
{
    if (cfg.hb_interval <= 0 || g_nconns == 0) return;

    int budget = g_nconns / cfg.hb_interval + 1;
    if (budget > cfg.hb_batch) budget = cfg.hb_batch;

    static int cursor = 0;
    for (int i = 0; i < budget && g_nconns > 0; i++) {
        int idx = (cursor++) % g_nconns;
        conn_t *c = g_conns[idx];
        if (!c || c->state != ST_ESTABLISHED) continue;

        ssize_t n = send(c->fd, "P", 1, 0);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            /* 多数情况下是 ECONNRESET / EPIPE: 连接已被对端或中间设备悄悄干掉 */
            g_st.hb_dead++;
            count_errno(errno);
            remove_conn(c);
        } else {
            g_st.hb_sent++;
            c->hb_ok = 1;
        }
    }
}

/* 启动前先建一条探测连接, 确认链路与服务端可用, 避免白跑几十分钟 */
static int probe_once(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    set_nonblock(fd);
    if (connect(fd, (struct sockaddr *)&g_dst[0], sizeof(g_dst[0])) < 0 &&
        errno != EINPROGRESS) {
        fprintf(stderr, "探测连接失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int r = poll(&pfd, 1, 3000);
    if (r <= 0) {
        fprintf(stderr, "探测连接超时(3s), 请确认服务端可达\n");
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        fprintf(stderr, "探测连接失败: %s\n", strerror(err ? err : errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* 对 [from, g_lat_n) 区间的延迟样本排序取分位数 */
static void calc_lat(uint64_t from, double *p50, double *p99, double *p999)
{
    *p50 = *p99 = *p999 = 0;
    if (g_lat_n <= from) return;
    uint64_t n = g_lat_n - from;
    double *tmp = malloc((size_t)n * sizeof(double));
    if (!tmp) return;
    memcpy(tmp, g_lat + from, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), lat_cmp);
    *p50  = tmp[n / 2];
    *p99  = tmp[(uint64_t)((double)n * 0.99)  < n ? (uint64_t)((double)n * 0.99)  : n - 1];
    *p999 = tmp[(uint64_t)((double)n * 0.999) < n ? (uint64_t)((double)n * 0.999) : n - 1];
    free(tmp);
}

static void step_begin(int idx, uint64_t now)
{
    step_t *st = &cfg.steps[idx];
    st->base_attempted   = g_st.attempted;
    st->base_established = g_st.established;
    st->base_failed      = g_st.failed;
    st->base_timeout     = g_st.timeout;
    st->base_lat_n       = g_lat_n;
    memcpy(st->base_errno, g_st.errno_cnt, sizeof(st->base_errno));

    /* Little's law: 所需并发窗口 = 目标速率 × 握手延迟。
       链路一慢, 固定窗口会被在途连接占死, 客户端根本发不出目标速率。
       所以按上一档实测延迟自动放大窗口(留 2 倍余量)。 */
    if (g_cps_mode && idx > 0 && cfg.steps[idx - 1].p99 > 0) {
        uint64_t need = (uint64_t)(st->target * cfg.steps[idx - 1].p99 / 1000.0 * 2.0) + 64;
        if (need > (uint64_t)cfg.max_inflight && need <= 65536) {
            printf("  [自动] 握手窗口 -I %d -> %llu (目标 %.0f/s × 上档 p99 %.0fms 估算)\n",
                   cfg.max_inflight, (unsigned long long)need,
                   st->target, cfg.steps[idx - 1].p99);
            cfg.max_inflight = (int)need;
        }
    }

    g_step_idx = idx;
    g_step_start_us = now;
    g_step_samples = 0;
    g_step_full = 0;
    g_debt = 0;
    g_last_tick_us = now;
    g_cur_rate = st->target;
    g_interval_us = (uint64_t)(1000000.0 / g_cur_rate);
}

static const char *verdict_str(int v)
{
    if (v == VERDICT_PASS) return "PASS";
    if (v == VERDICT_WARN) return "临界";
    if (v == VERDICT_FAIL) return "FAIL";
    return "未达成";
}

/* 结算前把还在握手中的连接收干净。否则这些连接既不算成功也不算失败,
   会被误判成"失败", 让成功率凭空下降(实测出现过: 失败 0 次但成功率 82.9%)。 */
static void drain_pending(void)
{
    uint64_t limit = cfg.conn_timeout > 3 ? 3 : cfg.conn_timeout;
    uint64_t deadline = now_us() + (uint64_t)limit * 1000000ULL;

    while (g_inflight > 0) {
        uint64_t now = now_us();
        check_conn_timeout(now);
        if (g_inflight <= 0 || now >= deadline) break;
        int n = nb_ev_wait(&g_loop, 20);
        if (n > 0) {
            int fd, mask;
            while (nb_ev_next(&g_loop, &fd, &mask)) handle_event(fd, mask);
        }
    }
}

/* 结算当前档位并打印; 返回 0 表示全部档位跑完 */
static int finish_step(uint64_t now)
{
    step_t *st = &cfg.steps[g_step_idx];

    double hold = (double)(now - g_step_start_us) / 1e6;   /* 纯档位时长 */
    drain_pending();                  /* 先收干净在途握手, 再统计 */
    uint64_t end = now_us();
    double dur = (double)(end - g_step_start_us) / 1e6;    /* 含收尾时间 */

    st->attempts = g_st.attempted   - st->base_attempted;
    st->success  = g_st.established - st->base_established;
    st->failed   = g_st.failed      - st->base_failed;
    st->timeout  = g_st.timeout     - st->base_timeout;
    memcpy(st->end_errno, g_st.errno_cnt, sizeof(st->end_errno));
    /* 分两个速率看, 才能区分"发不出去"和"发出去了但被拒":
         attempt_rate = 发起速率 -> 衡量客户端自己发不发得出来
         actual_cps   = 成功速率 -> 衡量实际达成的新建能力
       达成率必须用发起速率, 否则 drain 收尾时间会把数字拉低而误判成未达成。 */
    st->attempt_rate  = hold > 0 ? (double)st->attempts / hold : 0;
    st->actual_cps    = dur  > 0 ? (double)st->success  / dur  : 0;
    st->rate_achieved = st->target > 0 ? st->attempt_rate / st->target : 0;
    st->window_full_ratio = g_step_samples > 0
                          ? (double)g_step_full / (double)g_step_samples : 0;

    /* 分母必须是"已有结果"的连接, 不能是发起数 —— 在途连接不算失败 */
    uint64_t done = st->success + st->failed;
    st->success_rate = done > 0 ? (double)st->success / (double)done : 0;
    calc_lat(st->base_lat_n, &st->p50, &st->p99, &st->p999);

    /* 速率都没发出来, 成功率再高也不能算通过 —— 那只是"发得少所以没失败" */
    double p99_ratio = (g_base_p99 > 0 && st->p99 > 0) ? st->p99 / g_base_p99 : 1.0;
    st->p99_ratio = p99_ratio;

    /* 判定优先级有讲究:
       "速率没发满"不一定等于"客户端发不出" —— 如果同时延迟已严重恶化,
       说明是对端开始吃紧、握手变慢, 导致客户端攒不出额度。
       这种情况下该档是有价值的拐点证据, 不能简单丢弃。 */
    if      (st->rate_achieved < 0.80 && p99_ratio <= 10.0) st->verdict = VERDICT_UNREACHED;
    else if (st->success_rate < 0.80)                       st->verdict = VERDICT_FAIL;
    else if (st->success_rate < 0.95)                       st->verdict = VERDICT_WARN;
    else if (p99_ratio > 10.0)                              st->verdict = VERDICT_WARN;
    else if (st->rate_achieved < 0.80)                      st->verdict = VERDICT_UNREACHED;
    else                                                    st->verdict = VERDICT_PASS;

    if (st->verdict == VERDICT_PASS && g_base_p99 == 0 && st->p99 > 0)
        g_base_p99 = st->p99;

    if (st->verdict == VERDICT_PASS) {
        g_cps_ceiling  = st->target;
        g_cps_critical = st->target;
    } else if (st->verdict == VERDICT_WARN) {
        /* 只有真正"发出去了但开始恶化"才算临界; 纯客户端发不出的档不算 */
        g_cps_critical = st->target;
        g_cps_critical_actual = st->attempt_rate;
    }

    printf("[档 %d/%d] 目标 %-7.0f/s 实测 %-7.0f/s (达成 %3.0f%%) 成功 %llu (%5.1f%%) "
           "超时 %-5llu p99=%-7.1f 窗口%-5.0f%% %s\n",
           g_step_idx + 1, cfg.nsteps, st->target, st->actual_cps,
           st->rate_achieved * 100.0,
           (unsigned long long)st->success, st->success_rate * 100.0,
           (unsigned long long)st->timeout, st->p99,
           st->window_full_ratio * 100.0, verdict_str(st->verdict));
    fflush(stdout);

    int next = g_step_idx + 1;
    if (next >= cfg.nsteps) return 0;
    /* 连续两档 FAIL 就没必要继续加压了, 已经远超上限 */
    if (next >= 2 && cfg.steps[next - 1].verdict == VERDICT_FAIL &&
                     cfg.steps[next - 2].verdict == VERDICT_FAIL) return 0;

    step_begin(next, now);
    return 1;
}

static void print_progress(uint64_t now)
{
    double el = (double)(now - g_start_us) / 1e6;
    static uint64_t last_est = 0;
    uint64_t delta = g_st.established - last_est;
    last_est = g_st.established;

    printf("[%8.1fs] 存活=%-9d 峰值=%-9d 握手=%-4d 累计成功=%-9llu 失败=%-6llu "
           "超时=%-6llu 对端关闭=%-6llu 速率=%.0f/s\n",
           el, g_cur_est, g_peak_est, g_inflight,
           (unsigned long long)g_st.established,
           (unsigned long long)g_st.failed,
           (unsigned long long)g_st.timeout,
           (unsigned long long)g_st.peer_closed,
           g_cur_rate);

    /* 握手槽位打满 + 实际成功率远低于设定速率 = SYN 发出去没有回应。
       没有这个提示, 用户只会看到"数字不动了", 完全不知道卡在哪。 */
    if (g_cps_mode) {
        static uint64_t last_iters = 0, last_batch = 0;
        uint64_t di = g_loop_iters - last_iters;
        uint64_t db = g_loop_batch_sum - last_batch;
        last_iters = g_loop_iters;
        last_batch = g_loop_batch_sum;
        printf("           [内部] 循环 %llu 次/秒, 平均批量 %.1f, 在途 %d/%d\n",
               (unsigned long long)di, di > 0 ? (double)db / (double)di : 0.0,
               g_inflight, cfg.max_inflight);
    }

    /* 停滞倒计时: 让用户知道程序在"确认是否真的到顶", 而不是卡死了 */
    if (!g_cps_mode && cfg.stall_sec > 0 && !g_goal_us && stall_armed()) {
        double idle = (double)(now - g_last_growth_us) / 1e6;
        if (idle > 5.0) {
            double left = (double)cfg.stall_sec - idle;
            if (left > 0)
                printf("           [停滞] 已 %.0f 秒未新增连接, %.0f 秒后判定为已达上限并自动停止\n",
                       idle, left);
        }
    }

    if (g_inflight >= cfg.max_inflight && (double)delta < cfg.rate * 0.5) {
        printf("           [!] 握手槽位已满(%d) 且本秒仅成功 %llu 条 —— SYN 疑似被上游静默丢弃\n",
               g_inflight, (unsigned long long)delta);
    }
    fflush(stdout);
}

static const char *stop_reason_str(int r)
{
    switch (r) {
    case STOP_GOAL:            return "达到目标连接数并完成观察";
    case STOP_FD_EXHAUSTED:    return "fd 耗尽(进程/系统文件描述符上限)";
    case STOP_PORTS_EXHAUSTED: return "本地端口/四元组耗尽";
    case STOP_TIMEOUT:         return "总时限到达";
    case STOP_STALLED:         return "连接数停止增长, 判定已达上限";
    case STOP_SIGNAL:          return "收到中断信号";
    case STOP_FATAL:           return "致命错误";
    default:                   return "未知";
    }
}

/* CPS 模式的瓶颈归因: 不同失败画像指向完全不同的位置 */
static void diagnose_cps(void)
{
    int fi = -1;
    for (int i = 0; i <= g_step_idx && i < cfg.nsteps; i++)
        if (cfg.steps[i].verdict == VERDICT_FAIL) { fi = i; break; }
    if (fi < 0)
        for (int i = 0; i <= g_step_idx && i < cfg.nsteps; i++)
            if (cfg.steps[i].verdict == VERDICT_WARN) { fi = i; break; }

    printf("\n【原因】\n");

    /* 客户端自己发不出这么快时, 后面所有归因都没有意义 */
    int unreached = 0, total_steps = 0;
    double min_achieved = 1e9;
    for (int i = 0; i <= g_step_idx && i < cfg.nsteps; i++) {
        total_steps++;
        if (cfg.steps[i].verdict == VERDICT_UNREACHED) unreached++;
        if (cfg.steps[i].rate_achieved < min_achieved) min_achieved = cfg.steps[i].rate_achieved;
    }
    if (unreached > 0) {
        int ui = -1;
        for (int i = 0; i <= g_step_idx && i < cfg.nsteps; i++)
            if (cfg.steps[i].verdict == VERDICT_UNREACHED) { ui = i; break; }
        step_t *u = &cfg.steps[ui];

        printf("  (有 %d 档速率未达成, 已在判定中排除)\n", unreached);
        printf("  未达成起点: %.0f 条/秒档, 实际只发出 %.0f 条/秒 (达成 %.0f%%)\n",
               u->target, u->attempt_rate, u->rate_achieved * 100.0);
        printf("  握手窗口  : %.0f%% 的时间处于满载 (-I=%d)\n",
               u->window_full_ratio * 100.0, cfg.max_inflight);

        /* 窗口打满与否, 是区分"客户端发不出"和"服务端接不住"的关键分水岭 */
        if (u->window_full_ratio > 0.5) {
            printf("  判定      : 握手窗口被打满 —— 不是客户端发不出,\n");
            printf("              而是已发出的握手迟迟不返回, 把窗口占死了\n");
            if (u->p99_ratio > 5.0)
                printf("              依据: p99 延迟较基线恶化 %.0f 倍 (达 %.1fms), 队列严重积压\n",
                       u->p99_ratio, u->p99);
            printf("              结论: 服务端/链路处理能力已到上限, 这基本就是 CPS 天花板\n");
            printf("              验证: 调大 -I 后若实测速率随之上升, 说明只是窗口不够;\n");
            printf("                    若调大后延迟继续线性恶化, 则确认服务端已到顶\n");
        } else {
            printf("  判定      : 客户端单线程发送能力不足 (定时器精度约 1ms)\n");
            printf("              处理: 调大 -M(单次批量, 当前 %d), 或降低 -Z 最高档,\n", cfg.max_batch);
            printf("                    仍不够则多进程并行加压\n");
        }

        if (unreached == total_steps) {
            printf("\n  提示: 所有档位都未达成, 建议降低 -Z 档位表, 先找到能稳定发出的速率\n");
            return;
        }
        printf("\n");
    }

    if (fi < 0) {
        if (unreached > 0) {
            printf("  已达成速率的档位全部通过, 但更高档位客户端没能发出去。\n");
            printf("  说明    : 受限于客户端单线程发送能力, 未能探到网络/服务端的真实上限\n");
            printf("  处理    : 调大 -M(单次批量, 当前 %d) 与 -I(并发窗口, 当前 %d),\n",
                   cfg.max_batch, cfg.max_inflight);
            printf("            或降低 -Z 最高档; 仍不够则需多进程并行加压\n");
        } else {
            printf("  所有档位均通过, 未探到 CPS 上限。\n");
            printf("  建议加大档位表最高值重测, 例: -Z ...,32000,64000\n");
        }
        return;
    }

    step_t *st = &cfg.steps[fi];
    uint64_t total = 0, tmo = 0, refused = 0, reset = 0, noaddr = 0, emfile = 0, other = 0;
    for (int e = 0; e < 256; e++) {
        uint64_t d = st->end_errno[e] - st->base_errno[e];
        if (!d) continue;
        total += d;
        if      (e == ETIMEDOUT)                        tmo += d;
        else if (e == ECONNREFUSED)                     refused += d;
        else if (e == ECONNRESET)                       reset += d;
        else if (e == EADDRNOTAVAIL || e == EADDRINUSE) noaddr += d;
        else if (e == EMFILE || e == ENFILE)            emfile += d;
        else                                            other += d;
    }

    if (st->success_rate >= 0.95)
        printf("  拐点档  : %.0f 条/秒 —— 成功率仍有 %.1f%%, 但延迟已严重恶化\n",
               st->target, st->success_rate * 100.0);
    else
        printf("  拐点档  : %.0f 条/秒 —— 成功率降至 %.1f%%, 失败 %llu 次\n",
               st->target, st->success_rate * 100.0, (unsigned long long)st->failed);

    if (fi > 0) {
        step_t *prev = &cfg.steps[fi - 1];
        if (prev->p99 > 0 && st->p99 > prev->p99 * 3)
            printf("  佐证    : p99 延迟 %.1fms -> %.1fms (抬升 %.0f 倍, 队列严重积压)\n",
                   prev->p99, st->p99, st->p99 / prev->p99);
    }
    if (st->window_full_ratio > 0.5)
        printf("  佐证    : 握手窗口 %.0f%% 时间满载, 握手迟迟不返回\n",
               st->window_full_ratio * 100.0);

    if (total == 0) {
        printf("  判定    : 失败未产生 errno, 多为内部超时, 即 SYN 无响应\n");
        return;
    }

    printf("  该档错误分布:\n");
#define P(v, name) do { if (v) printf("      %-32s %-10llu %5.1f%%\n", name, \
                        (unsigned long long)(v), (double)(v) / (double)total * 100.0); } while (0)
    P(tmo,     "超时 (SYN 无响应)");
    P(refused, "Connection refused");
    P(reset,   "Connection reset by peer");
    P(noaddr,  "本地端口/TIME_WAIT 耗尽");
    P(emfile,  "fd 耗尽");
    P(other,   "其它");
#undef P

    printf("  判定    : ");
    if (tmo * 2 >= total) {
        printf("SYN 被上游静默丢弃 —— 瓶颈在链路中间, 不是服务端容量\n");
        printf("            位置: 家宽路由器 NAT / 运营商 CGNAT / 云厂商安全组或网关限速\n");
        printf("            依据: 失败以超时为主。服务端若真接不住, 应表现为 refused/reset 而非超时\n");
        printf("            验证: 换同机房内网客户端对比, 内网正常即可确认是公网链路限速\n");
    } else if (refused * 2 >= total) {
        printf("服务端主动拒绝 —— accept 队列溢出\n");
        printf("            依据: 收到 RST 而非超时, 说明包到了服务端但被拒\n");
        printf("            处理: 服务端调大 somaxconn 与 listen backlog\n");
    } else if (reset * 2 >= total) {
        printf("连接被立即重置 —— 本机或服务端 somaxconn 过小\n");
        printf("            处理: sysctl -w net.core.somaxconn=65535 (macOS: kern.ipc.somaxconn)\n");
    } else if (noaddr * 2 >= total) {
        printf("本地端口耗尽 —— 短连接 TIME_WAIT 堆积\n");
        printf("            处理: 加 -L 强制 RST 关闭避免 TIME_WAIT, 或扩大 -p 目标端口范围\n");
    } else if (emfile * 2 >= total) {
        printf("本机 fd 耗尽\n");
        printf("            处理: ulimit -n 1048576 后重测\n");
    } else {
        printf("混合型失败, 按上方错误分布逐项排查\n");
    }
}

static void print_cps_report(void)
{
    printf("\n============ CPS(每秒新建连接数)测试报告 ============\n");
    printf("目标      : %s:%s\n", cfg.host, cfg.portspec ? cfg.portspec : "8000");
    printf("档位      : %d 档, 每档 %d 秒\n\n", cfg.nsteps, cfg.cps_hold);

    printf("%-5s %-9s %-9s %-8s %-15s %-8s %-9s %-9s %s\n",
           "档位", "目标/秒", "实测/秒", "达成率", "成功/尝试", "成功率", "p50(ms)", "p99(ms)", "判定");
    for (int i = 0; i <= g_step_idx && i < cfg.nsteps; i++) {
        step_t *st = &cfg.steps[i];
        printf("%-5d %-9.0f %-9.0f %-7.0f%% %llu/%-11llu %-7.1f%% %-9.2f %-9.1f %s\n",
               i + 1, st->target, st->actual_cps, st->rate_achieved * 100.0,
               (unsigned long long)st->success, (unsigned long long)st->attempts,
               st->success_rate * 100.0, st->p50, st->p99, verdict_str(st->verdict));
    }

    double peak = 0;
    for (int i = 0; i <= g_step_idx && i < cfg.nsteps; i++)
        if (cfg.steps[i].actual_cps > peak) peak = cfg.steps[i].actual_cps;

    printf("\n【结论】最大每秒新建连接数(CPS)\n");
    if (g_cps_ceiling > 0)
        printf("  稳定上限 : %.0f 条/秒  (成功率 >= 95%% 的最高档)\n", g_cps_ceiling);
    else
        printf("  稳定上限 : 低于 %.0f 条/秒  (连最低档都没达到 95%% 成功率)\n",
               cfg.steps[0].target);
    if (g_cps_critical > 0) {
        printf("  临界值   : 目标 %.0f 条/秒时开始恶化", g_cps_critical);
        if (g_cps_critical_actual > 0 &&
            g_cps_critical_actual < g_cps_critical * 0.9)
            printf(" (该档实际只跑到 %.0f 条/秒)", g_cps_critical_actual);
        printf("\n");
    }
    printf("  实测峰值 : %.0f 条/秒  (某一档实际达成的最大值)\n", peak);

    diagnose_cps();

    double safe = g_cps_ceiling > 0 ? g_cps_ceiling * 0.3 : 50;
    printf("\n【下一步】测最大并发连接数时, 建连速率取稳定上限的 30%% 最稳妥 ——\n");
    printf("          太快会撞 CPS 限流而误判成容量不足, 太慢则耗时过长:\n");
    printf("    ./client -s %s -p %s -c <目标连接数> -r %.0f -T %d\n",
           cfg.host, cfg.portspec ? cfg.portspec : "8000", safe, cfg.conn_timeout);
    printf("====================================================\n");
}

/* 报告缓冲区提为全局: 诊断段也要能写进 -o 指定的文件 */
static char   g_rep[16384];
static size_t g_rep_off = 0;

#define EMIT(...) do { \
        if (g_rep_off < sizeof(g_rep) - 1) \
            g_rep_off += (size_t)snprintf(g_rep + g_rep_off, \
                                         sizeof(g_rep) - g_rep_off, __VA_ARGS__); \
        if (g_rep_off > sizeof(g_rep) - 1) g_rep_off = sizeof(g_rep) - 1; \
    } while (0)

/* 并发容量模式的结论与归因 */
static void diagnose_conn(void)
{
    EMIT("\n【结论】最大并发连接数\n");
    if (g_stop_reason == STOP_GOAL) {
        EMIT("  已达成目标 %d 条, 观察期内保持稳定 —— 尚未触顶, 可加大 -c 继续压\n",
             cfg.max_conns);
    } else if (g_stop_reason == STOP_STALLED) {
        EMIT("  %d 条 —— 已触顶\n", g_peak_est);
        EMIT("  依据  : 持续 %.0f 秒存活数不再净增长, 期间失败 %llu 次\n",
             g_stall_elapsed, (unsigned long long)g_st.failed);
        if (g_st.peer_closed > 0) {
            double total_sec = (double)(now_us() - g_start_us) / 1e6;
            double close_rate = total_sec > 0 ? (double)g_st.peer_closed / total_sec : 0;
            EMIT("  形态  : 踢旧建新 —— 新连接一直建得上, 但旧连接被对端关闭(FIN/RST) %llu 条\n",
                 (unsigned long long)g_st.peer_closed);
            EMIT("          稳态水位 %d 条(历史峰值 %d), 平均每条连接活约 %.0f 秒后被关掉\n",
                 g_cur_est, g_peak_est, close_rate > 0 ? (double)g_cur_est / close_rate : 0.0);
            EMIT("  注意  : 本工具的 server 不会主动关闭健康的连接, 所以这些 FIN/RST 大概率来自\n");
            EMIT("          中间设备的会话回收, 或服务端的 accept 队列溢出 / 进程重启。\n");
            EMIT("          拿服务端进度行的「当前 / close累计」与客户端对拍即可定位(见 README)。\n");
        } else if (g_st.failed > 0) {
            EMIT("  形态  : 静默丢弃 —— 新连接建不上(SYN 无响应), 已建立的连接保持不动\n");
        }
        if (g_st.peer_closed == 0 && g_st.hb_dead == 0)
            EMIT("  稳定性: 已建立的连接全程零掉线 —— 连接本身是健康的, 只是新连接被挡住了\n");
    } else if (g_stop_reason == STOP_FD_EXHAUSTED) {
        EMIT("  %d 条 —— 撞到本机 fd 上限, 这是本机限制, 不是链路/服务端容量\n", g_peak_est);
    } else if (g_stop_reason == STOP_PORTS_EXHAUSTED) {
        EMIT("  %d 条 —— 撞到四元组/本地端口上限\n", g_peak_est);
    } else if (g_stop_reason == STOP_SIGNAL) {
        EMIT("  %d 条 (用户中断, 可能尚未触顶)\n", g_peak_est);
    } else {
        EMIT("  %d 条\n", g_peak_est);
    }

    EMIT("\n【原因】\n");
    if (g_st.failed == 0) {
        if (g_st.peer_closed > 0) {
            EMIT("  建连全程零失败 —— 瓶颈不在\"新建连接\", 而在\"保持住\"。\n");
            EMIT("  被对端关闭(FIN/RST) %llu 条, 存活数被压在 %d 上不去。\n",
                 (unsigned long long)g_st.peer_closed, g_cur_est);
            EMIT("  排查  : 拿服务端进度行的「当前 / close累计」与客户端对拍 ——\n");
            EMIT("          两边对得上, 说明关闭确实发生在服务端(accept 队列溢出/进程重启);\n");
            EMIT("          服务端计数明显大于客户端存活, 说明 FIN/RST 是链路中间设备伪造的。\n");
        } else if (g_st.hb_dead > 0) {
            EMIT("  建连零失败, 但心跳发现 %llu 条连接失效 —— NAT 空闲会话超时回收。\n",
                 (unsigned long long)g_st.hb_dead);
            EMIT("  处理  : 缩短心跳周期(-B 5), 或确认链路上的 NAT 空闲超时。\n");
        } else {
            EMIT("  全程零失败, 未触及瓶颈。\n");
        }
        return;
    }

    uint64_t tmo     = g_st.errno_cnt[ETIMEDOUT];
    uint64_t refused = g_st.errno_cnt[ECONNREFUSED];
    uint64_t reset   = g_st.errno_cnt[ECONNRESET];
    uint64_t noaddr  = g_st.errno_cnt[EADDRNOTAVAIL];
    uint64_t emfile  = g_st.errno_cnt[EMFILE] + g_st.errno_cnt[ENFILE];
    uint64_t tf      = g_st.failed;

    if (tmo * 2 >= tf) {
        EMIT("  失败以「超时」为主 (%llu/%llu) —— SYN 发出后没有任何响应\n", tmo, tf);
        if (g_st.peer_closed == 0) {
            EMIT("  判定  : 链路侧 NAT/CGNAT 会话配额已用满\n");
            EMIT("  依据  :\n");
            EMIT("    1) 失败是超时而非 refused/reset —— 包没到服务端就被丢了\n");
            EMIT("    2) 已建立的 %d 条全程零掉线 —— 服务端没主动断开任何一条\n", g_cur_est);
            EMIT("    3) %d 这个量级正落在家宽/CGNAT 的 2000~15000 区间\n", g_peak_est);
            EMIT("  建议  :\n");
            EMIT("    - 若目标是测服务端容量, 必须换同机房云主机做客户端, 否则永远压不到\n");
            EMIT("    - 确认是否 CGNAT: 对比本机内网 IP 与公网出口 IP(100.64.x.x 即 CGNAT)\n");
        } else {
            EMIT("  判定  : SYN 被静默丢弃, 且保持期有掉线, 链路整体已达上限\n");
        }
    } else if (refused * 2 >= tf) {
        EMIT("  失败以「拒绝」为主 —— 服务端 accept 队列溢出\n");
        EMIT("  处理  : 服务端调大 somaxconn 与 listen backlog\n");
    } else if (reset * 2 >= tf) {
        EMIT("  失败以「重置」为主 —— 本机或服务端 somaxconn 过小\n");
        EMIT("  处理  : sysctl -w net.core.somaxconn=65535\n");
    } else if (noaddr * 2 >= tf) {
        EMIT("  失败以「端口耗尽」为主 —— 四元组用尽\n");
        EMIT("  处理  : 加大 -p 端口范围, 或 -b 绑定更多源 IP\n");
    } else if (emfile * 2 >= tf) {
        EMIT("  失败以「fd 耗尽」为主 —— 本机 ulimit 不够\n");
        EMIT("  处理  : ulimit -n 1048576\n");
    } else {
        EMIT("  混合型失败, 详见上方错误分布\n");
    }

    if (g_st.peer_closed > 0 || g_st.hb_dead > 0)
        EMIT("  补充  : 保持期掉线 %llu 条(心跳发现 %llu 条), 需留意 NAT 空闲会话超时\n",
             (unsigned long long)g_st.peer_closed, (unsigned long long)g_st.hb_dead);
}

static void print_report(void)
{
    g_rep_off = 0;

    EMIT("\n================ 测试报告 ================\n");
    EMIT("目标主机        : %s:%s (%d 个目标端口)\n",
         cfg.host, cfg.portspec ? cfg.portspec : "8000", cfg.nports);
    EMIT("源 IP 数        : %d\n", cfg.nsrcs ? cfg.nsrcs : 1);
    EMIT("初始建连速率    : %.1f 条/秒, 结束时 %.1f 条/秒\n", cfg.rate, g_cur_rate);
    EMIT("目标连接数      : %d\n", cfg.max_conns);
    EMIT("------------------------------------------\n");
    EMIT("尝试建连        : %llu\n", (unsigned long long)g_st.attempted);
    EMIT("建连成功(累计)  : %llu\n", (unsigned long long)g_st.established);
    EMIT("建连失败        : %llu\n", (unsigned long long)g_st.failed);
    EMIT("历史峰值并发    : %d\n", g_peak_est);
    EMIT("结束时仍存活    : %d\n", g_cur_est);
    EMIT("对端主动关闭    : %llu\n", (unsigned long long)g_st.peer_closed);
    EMIT("建连超时        : %llu  (SYN 发出后无任何响应)\n", (unsigned long long)g_st.timeout);
    EMIT("心跳发送        : %llu, 心跳发现失效: %llu\n",
         (unsigned long long)g_st.hb_sent, (unsigned long long)g_st.hb_dead);

    if (g_lat_n > 0) {
        double *tmp = malloc((size_t)g_lat_n * sizeof(double));
        if (tmp) {
            memcpy(tmp, g_lat, (size_t)g_lat_n * sizeof(double));
            qsort(tmp, (size_t)g_lat_n, sizeof(double), lat_cmp);
            EMIT("建连耗时(ms)    : min=%.2f p50=%.2f p99=%.2f p999=%.2f max=%.2f\n",
                 tmp[0],
                 tmp[g_lat_n / 2],
                 tmp[(size_t)((double)g_lat_n * 0.99) < g_lat_n ? (size_t)((double)g_lat_n * 0.99) : g_lat_n - 1],
                 tmp[(size_t)((double)g_lat_n * 0.999) < g_lat_n ? (size_t)((double)g_lat_n * 0.999) : g_lat_n - 1],
                 tmp[g_lat_n - 1]);
            free(tmp);
        }
    }

    int any = 0;
    for (int e = 0; e < 256; e++) {
        if (!g_st.errno_cnt[e]) continue;
        if (!any) { EMIT("------------------------------------------\n错误分布        :\n"); any = 1; }
        EMIT("    %-38s %llu\n", strerror(e), (unsigned long long)g_st.errno_cnt[e]);
    }

    EMIT("------------------------------------------\n");
    EMIT("结束原因        : %s\n", stop_reason_str(g_stop_reason));

    diagnose_conn();
    EMIT("==========================================\n");

    fwrite(g_rep, 1, g_rep_off, stdout);
    fflush(stdout);

    if (cfg.report) {
        FILE *fp = fopen(cfg.report, "w");
        if (fp) {
            fwrite(g_rep, 1, g_rep_off, fp);
            fclose(fp);
            printf("报告已写入: %s\n", cfg.report);
        } else {
            perror("写报告文件失败");
        }
    }
}

/* 给出本机容量方面的提示 */
static void env_hints(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        printf("RLIMIT_NOFILE: soft=%llu hard=%llu\n",
               (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
        if (rl.rlim_cur != RLIM_INFINITY &&
            (unsigned long long)rl.rlim_cur < (unsigned long long)cfg.max_conns + 64)
            fprintf(stderr,
                "[警告] soft nofile=%llu 小于目标连接数 %d, 测试必然提前撞到 fd 上限。\n"
                "       先执行: ulimit -n %d\n",
                (unsigned long long)rl.rlim_cur, cfg.max_conns, cfg.max_conns + 1024);
    }

    /* 估算四元组容量 */
    long long quad = (long long)(cfg.nsrcs ? cfg.nsrcs : 1) * 60000LL * (long long)cfg.nports;
    printf("四元组理论容量  : 约 %lld (源IP %d × 本地端口 ~60000 × 目标端口 %d)\n",
           quad, cfg.nsrcs ? cfg.nsrcs : 1, cfg.nports);
    if (quad < cfg.max_conns)
        fprintf(stderr,
            "[警告] 四元组容量(%lld) < 目标连接数(%d), 会撞到本地端口耗尽。\n"
            "       解决: -p 扩大目标端口范围, 或 -b 绑定多个源 IP\n",
            quad, cfg.max_conns);

#ifdef __linux__
    FILE *fp = fopen("/proc/sys/net/ipv4/ip_local_port_range", "r");
    if (fp) {
        int lo, hi;
        if (fscanf(fp, "%d %d", &lo, &hi) == 2)
            printf("本地端口范围    : %d - %d (共 %d 个)\n", lo, hi, hi - lo + 1);
        fclose(fp);
    }
#endif

    double eta = (double)cfg.max_conns / cfg.rate;
    printf("预计建连耗时    : %.0f 秒 (%.1f 分钟), 速率 %.1f 条/秒\n",
           eta, eta / 60.0, cfg.rate);
    if (eta > 3600)
        fprintf(stderr, "[提示] 按当前速率需要 %.1f 小时；如需加快可调大 -r，"
                        "但要注意 CPS 限流风险\n", eta / 3600.0);
}

int main(int argc, char **argv)
{
    int opt;
    const char *portspec = "8000";

    while ((opt = getopt(argc, argv, "s:p:c:r:I:T:b:B:P:n:w:t:kK:R:S:Aqo:m:Z:Y:L:M:G:?")) != -1) {
        switch (opt) {
        case 's': cfg.host = optarg; break;
        case 'p': portspec = optarg; break;
        case 'c': cfg.max_conns = atoi(optarg); break;
        case 'r': cfg.rate = atof(optarg); break;
        case 'I': cfg.max_inflight = atoi(optarg); break;
        case 'T': cfg.conn_timeout = atoi(optarg); break;
        case 'b':
            cfg.nsrcs = parse_srcs(optarg, cfg.srcs, MAX_SRCS);
            if (cfg.nsrcs <= 0) { fprintf(stderr, "源 IP 解析失败: %s\n", optarg); return 1; }
            break;
        case 'B': cfg.hb_interval = atoi(optarg); break;
        case 'P': cfg.hb_batch = atoi(optarg); break;
        case 'n': cfg.payload = atoi(optarg); break;
        case 'w': cfg.hold_sec = atoi(optarg); break;
        case 't': cfg.time_limit = atoi(optarg); break;
        case 'k': cfg.keepalive = 1; break;
        case 'K': cfg.ka_idle = atoi(optarg); break;
        case 'R': cfg.rcvbuf = atoi(optarg); break;
        case 'S': cfg.sndbuf = atoi(optarg); break;
        case 'A': cfg.adaptive = 0; break;
        case 'q': cfg.quiet = 1; break;
        case 'o': cfg.report = optarg; break;
        case 'm':
            if (!strcmp(optarg, "cps"))       cfg.mode = MODE_CPS;
            else if (!strcmp(optarg, "conn")) cfg.mode = MODE_CONN;
            else { fprintf(stderr, "未知模式: %s (可选 conn / cps)\n", optarg); return 1; }
            break;
        case 'Z':
            cfg.nsteps = parse_steps(optarg, cfg.steps, MAX_STEPS);
            if (cfg.nsteps <= 0) { fprintf(stderr, "档位表解析失败: %s\n", optarg); return 1; }
            break;
        case 'Y': cfg.cps_hold = atoi(optarg); break;
        case 'L': cfg.linger_rst = 1; break;
        case 'M': cfg.max_batch = atoi(optarg); break;
        case 'G': cfg.stall_sec = atoi(optarg); break;
        case '?': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!cfg.host) { usage(argv[0]); return 1; }
    if (cfg.rate <= 0) cfg.rate = 1.0;
    if (cfg.max_inflight < 1) cfg.max_inflight = 1;

    cfg.portspec = portspec;
    cfg.nports = parse_ports(portspec, cfg.ports, MAX_PORTS);
    if (cfg.nports <= 0) { fprintf(stderr, "端口规格解析失败: %s\n", portspec); return 1; }

    struct in_addr haddr;
    if (resolve_host(cfg.host, &haddr) < 0) return 1;
    for (int i = 0; i < cfg.nports; i++) {
        memset(&g_dst[i], 0, sizeof(g_dst[i]));
        g_dst[i].sin_family = AF_INET;
        g_dst[i].sin_addr = haddr;
        g_dst[i].sin_port = htons((uint16_t)cfg.ports[i]);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 按模式补齐默认参数 */
    if (cfg.mode == MODE_CPS) {
        g_cps_mode = 1;
        if (cfg.nsteps == 0)
            cfg.nsteps = parse_steps(DEFAULT_CPS_STEPS, cfg.steps, MAX_STEPS);
        if (cfg.linger_rst < 0) cfg.linger_rst = 1;
        if (cfg.max_inflight < 2048) cfg.max_inflight = 2048;
        if (cfg.conn_timeout == 15)  cfg.conn_timeout = 5;
    } else if (cfg.linger_rst < 0) {
        cfg.linger_rst = 0;
    }

    if (g_cps_mode)
        printf("=== TCP 每秒新建连接数(CPS)测试 ===\n");
    else
        printf("=== TCP 最大并发连接测试(慢速建连模式) ===\n");
    char ipbuf[64];
    printf("目标: %s (%s), 端口 %d 个\n", cfg.host,
           inet_ntop(AF_INET, &haddr, ipbuf, sizeof(ipbuf)), cfg.nports);

    if (g_cps_mode) {
        printf("档位: ");
        for (int i = 0; i < cfg.nsteps; i++) printf("%.0f ", cfg.steps[i].target);
        printf("条/秒 (每档 %d 秒)\n", cfg.cps_hold);
        printf("短连接: 建连后立即关闭%s\n",
               cfg.linger_rst ? " (强制 RST, 避免 TIME_WAIT 堆积)" : "");
    } else {
        env_hints();
    }

    if (nb_ev_create(&g_loop, 8192) < 0) { perror("ev_create"); return 1; }

    printf("\n探测连通性...\n");
    if (probe_once() < 0) { nb_ev_destroy(&g_loop); return 1; }
    if (g_cps_mode)
        printf("探测成功, 开始阶梯加压 (Ctrl-C 可随时停止并输出报告)\n\n");
    else
        printf("探测成功, 开始按 %.1f 条/秒建连 (Ctrl-C 可随时停止并输出报告)\n\n", cfg.rate);

    g_cur_rate = cfg.rate;
    g_interval_us = (uint64_t)(1000000.0 / g_cur_rate);
    g_conn_timeout_us = cfg.conn_timeout > 0
                      ? (uint64_t)cfg.conn_timeout * 1000000ULL : 0;

    g_cap = cfg.max_conns + 16;
    g_conns = calloc((size_t)g_cap, sizeof(conn_t *));
    g_lat_cap = (uint64_t)cfg.max_conns + 16;
    g_lat = calloc((size_t)g_lat_cap, sizeof(double));
    if (!g_conns || !g_lat) { fprintf(stderr, "内存分配失败\n"); return 1; }

    g_start_us = now_us();
    g_last_growth_us = g_start_us;
    uint64_t last_print = g_start_us;
    uint64_t last_hb = g_start_us;

    if (g_cps_mode) step_begin(0, g_start_us);

    while (!g_fatal) {
        uint64_t now = now_us();
        check_conn_timeout(now);          /* 先回收挂死的握手, 再决定是否发起新连接 */

        if (g_sigint) { g_stop_reason = STOP_SIGNAL; break; }

        /* ---- 0) CPS 档位切换 ---- */
        if (g_cps_mode &&
            now - g_step_start_us >= (uint64_t)cfg.cps_hold * 1000000ULL) {
            if (!finish_step(now)) { g_cps_done = 1; break; }   /* 档位跑完或连续失败 */
            now = now_us();
        }

        /* ---- 1) 定速发起新连接(令牌桶) ----
           为什么不用"每条一个间隔": 系统定时器只能精确到 ~1ms, 那样最高只能发到
           约 1000 条/秒, 高速档永远达不到目标。
           为什么不用"overdue / interval"补发: 超时时间向上取整会带来系统性偏慢,
           实测 4000/s 档只能跑到 3100/s (78%)。
           正确做法是浮点欠账: 按真实流逝时间发放额度, 小数部分留到下次,
           既不会超速, 也不会因定时器粒度而系统性亏损。 */
        double elapsed = (double)(now - g_last_tick_us) / 1e6;
        g_last_tick_us = now;
        if (elapsed > 0) g_debt += elapsed * g_cur_rate;

        int batch = (int)g_debt;          /* 取整数部分 */
        if (batch > cfg.max_batch) {
            batch = cfg.max_batch;        /* 积压过多, 丢弃避免持续追赶造成突发 */
            g_debt = 0;
        } else {
            g_debt -= (double)batch;
        }

        int launched = 0;
        int quota = batch;
        while (quota-- > 0) {
            if (g_cps_mode) {
                if (g_inflight >= cfg.max_inflight) break;
            } else {
                if (g_nconns >= cfg.max_conns || g_inflight >= cfg.max_inflight) break;
            }
            start_conn();
            launched++;
            if (g_fatal) break;
        }
        g_loop_iters++;
        g_loop_batch_sum += (uint64_t)launched;
        if (g_fatal) break;
        now = now_us();

        /* 采样握手窗口饱和度: 用于区分"客户端发不出"和"握手迟迟不返回" */
        if (g_cps_mode) {
            g_step_samples++;
            if (g_inflight >= cfg.max_inflight) g_step_full++;
        }

        /* ---- 2) 计算等待超时 ---- */
        int timeout_ms = 1000;
        if (g_cps_mode || g_nconns < cfg.max_conns) {
            if (g_debt >= 1.0) {
                timeout_ms = 0;                  /* 还有额度, 立刻继续发 */
            } else {
                /* 等到攒够 1 条为止; 向下取整以便提前唤醒, 由欠账机制补偿精度 */
                double wait_us = (1.0 - g_debt) / g_cur_rate * 1e6;
                timeout_ms = (int)(wait_us / 1000);
                if (timeout_ms < 1)  timeout_ms = 1;      /* 定时器最小粒度 */
                if (timeout_ms > 1000) timeout_ms = 1000;
            }
        }

        int n = nb_ev_wait(&g_loop, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("ev_wait");
            g_stop_reason = STOP_FATAL;
            break;
        }
        int fd, mask;
        while (nb_ev_next(&g_loop, &fd, &mask)) handle_event(fd, mask);

        now = now_us();

        /* ---- 3) 心跳 ---- */
        if (!g_cps_mode && cfg.hb_interval > 0 && now - last_hb >= 1000000) {
            heartbeat_tick();
            last_hb = now;
        }

        /* ---- 4) 周期进度 ---- */
        if (!cfg.quiet && now - last_print >= 1000000) {
            print_progress(now);
            last_print = now;
        }

        /* ---- 5) 结束判定 ---- */
        if (!g_cps_mode) {
            if (g_nconns >= cfg.max_conns && g_inflight == 0) {
                if (!g_goal_us) {
                    g_goal_us = now;
                    printf("\n[*] 已达目标 %d 条连接, 保持观察 %d 秒以验证连接稳定性...\n",
                           cfg.max_conns, cfg.hold_sec);
                }
            } else if (g_goal_us) {
                g_goal_us = 0;    /* 有连接掉了, 继续补建 */
            }

            if (g_goal_us && (cfg.hold_sec <= 0 ||
                              now - g_goal_us >= (uint64_t)cfg.hold_sec * 1000000ULL)) {
                g_stop_reason = STOP_GOAL;
                break;
            }
        }
        if (cfg.time_limit > 0 &&
            now - g_start_us >= (uint64_t)cfg.time_limit * 1000000ULL) {
            g_stop_reason = STOP_TIMEOUT;
            break;
        }

        /* 停滞检测: 长时间一条都建不上, 说明已经顶到上限了。
           没有这个检测, 程序会以最低速率无限重试, 用户只能手动 Ctrl-C。 */
        if (!g_cps_mode && cfg.stall_sec > 0 && !g_goal_us && stall_armed() &&
            now - g_last_growth_us >= (uint64_t)cfg.stall_sec * 1000000ULL) {
            g_stall_elapsed = (double)(now - g_last_growth_us) / 1e6;
            g_stop_reason = STOP_STALLED;
            break;
        }
    }

    if (g_cps_mode) {
        if (!g_cps_done) finish_step(now_us());   /* 中途退出时结算当前档 */
        print_cps_report();
    } else {
        print_report();
    }

    /* 收尾: 关掉所有连接 */
    while (g_nconns > 0) remove_conn(g_conns[0]);
    nb_ev_destroy(&g_loop);
    free(g_conns);
    free(g_pend);
    free(g_fd2conn);
    free(g_lat);
    return 0;
}
