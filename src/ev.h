/*
 * ev.h —— 跨平台事件循环薄封装
 *   Linux  : epoll (LT 水平触发)
 *   macOS/BSD: kqueue
 *
 * 只做四件事: create / add / mod / del / wait / next
 * 之所以不用 libevent/libuv: 本工具要求零依赖、单文件可编译, 且只需
 * 极小的事件语义(可读、可写、出错), 引入第三方库反而增加部署成本。
 */
#ifndef NB_EV_H
#define NB_EV_H

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#if defined(__linux__)
#  include <sys/epoll.h>
#  define NB_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
#  include <sys/types.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  define NB_KQUEUE 1
#else
#  error "unsupported platform: need epoll (Linux) or kqueue (BSD/macOS)"
#endif

/* 注意: 不能叫 EV_ERROR —— kqueue 的 <sys/event.h> 里已有同名的
   EV_ERROR(0x4000), 重定义会让 "flags & EV_ERROR" 的判断彻底失效。 */
#define EV_NONE  0
#define EV_READ  1
#define EV_WRITE 2
#define EV_ERR   4

#if defined(__GNUC__) || defined(__clang__)
#  define NB_UNUSED __attribute__((unused))
#else
#  define NB_UNUSED
#endif

typedef struct {
    int fd;
    int mask;
} nb_event;

typedef struct {
    int  fd;          /* epoll fd 或 kqueue fd */
    int  max_events;  /* 单次 wait 最多返回多少事件 */
    int  nfired;
    int  cur;
    int *masks;       /* 以 fd 为下标的"当前关注事件"表 */
    int  masks_cap;
    nb_event *fired;
#if NB_EPOLL
    struct epoll_event *native;
#else
    struct kevent *native;
#endif
} nb_loop;

static int nb_ev_create(nb_loop *l, int max_events)
{
    memset(l, 0, sizeof(*l));
    l->max_events = max_events > 0 ? max_events : 1024;

#if NB_EPOLL
    l->fd = epoll_create1(0);
#else
    l->fd = kqueue();
#endif
    if (l->fd < 0) return -1;

    l->native = (void *)calloc((size_t)l->max_events, sizeof(*l->native));
    l->fired  = (nb_event *)calloc((size_t)l->max_events, sizeof(nb_event));
    l->masks_cap = 4096;
    l->masks = (int *)calloc((size_t)l->masks_cap, sizeof(int));
    if (!l->native || !l->fired || !l->masks) return -1;
    return 0;
}

static void nb_ev_destroy(nb_loop *l)
{
    if (l->fd >= 0) close(l->fd);
    free(l->native);
    free(l->fired);
    free(l->masks);
    memset(l, 0, sizeof(*l));
}

/* fd 可能很大(几十万), 按需扩容掩码表 */
static int nb_ev_masks_ensure(nb_loop *l, int fd)
{
    if (fd < l->masks_cap) return 0;
    int ncap = l->masks_cap;
    while (ncap <= fd) ncap *= 2;
    int *p = (int *)realloc(l->masks, (size_t)ncap * sizeof(int));
    if (!p) return -1;
    memset(p + l->masks_cap, 0, (size_t)(ncap - l->masks_cap) * sizeof(int));
    l->masks = p;
    l->masks_cap = ncap;
    return 0;
}

/* 覆盖式修改某 fd 关注的事件 */
static int nb_ev_mod(nb_loop *l, int fd, int mask)
{
    if (nb_ev_masks_ensure(l, fd) < 0) return -1;
    int old = l->masks[fd];

#if NB_EPOLL
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events = 0;
    if (mask & EV_READ)  ee.events |= EPOLLIN;
    if (mask & EV_WRITE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    int op = old ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(l->fd, op, fd, &ee) < 0) return -1;
#else
    struct kevent ke[2];
    int n = 0;
    /* 先处理需要删除的 filter */
    struct kevent del[2];
    int dn = 0;
    if ((old & EV_READ)  && !(mask & EV_READ))
        EV_SET(&del[dn++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    if ((old & EV_WRITE) && !(mask & EV_WRITE))
        EV_SET(&del[dn++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (dn && kevent(l->fd, del, dn, NULL, 0, NULL) < 0 && errno != ENOENT) return -1;

    if ((mask & EV_READ)  && !(old & EV_READ))
        EV_SET(&ke[n++], fd, EVFILT_READ,  EV_ADD | EV_ENABLE, 0, 0, NULL);
    if ((mask & EV_WRITE) && !(old & EV_WRITE))
        EV_SET(&ke[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (n && kevent(l->fd, ke, n, NULL, 0, NULL) < 0) return -1;
#endif
    l->masks[fd] = mask;
    return 0;
}

static NB_UNUSED int nb_ev_add(nb_loop *l, int fd, int mask)
{
    int old = (fd < l->masks_cap) ? l->masks[fd] : 0;
    return nb_ev_mod(l, fd, old | mask);
}

static int nb_ev_del(nb_loop *l, int fd)
{
    int old = (fd < l->masks_cap) ? l->masks[fd] : 0;
    if (!old) return 0;
#if NB_EPOLL
    epoll_ctl(l->fd, EPOLL_CTL_DEL, fd, NULL);
#else
    struct kevent del[2];
    int dn = 0;
    if (old & EV_READ)  EV_SET(&del[dn++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    if (old & EV_WRITE) EV_SET(&del[dn++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (dn) kevent(l->fd, del, dn, NULL, 0, NULL);
#endif
    l->masks[fd] = 0;
    return 0;
}

/* timeout_ms < 0 表示无限等待; 返回就绪事件个数 */
static int nb_ev_wait(nb_loop *l, int timeout_ms)
{
    l->nfired = 0;
    l->cur = 0;

#if NB_EPOLL
    int n = epoll_wait(l->fd, l->native, l->max_events, timeout_ms);
    if (n <= 0) return n < 0 ? -1 : 0;
    for (int i = 0; i < n; i++) {
        uint32_t e = l->native[i].events;
        int mask = 0;
        if (e & EPOLLIN)  mask |= EV_READ;
        if (e & EPOLLOUT) mask |= EV_WRITE;
        if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) mask |= EV_ERR;
        l->fired[i].fd   = l->native[i].data.fd;
        l->fired[i].mask = mask;
    }
    l->nfired = n;
#else
    struct timespec ts, *pts = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    int n = kevent(l->fd, NULL, 0, l->native, l->max_events, pts);
    if (n <= 0) return n < 0 ? -1 : 0;
    for (int i = 0; i < n; i++) {
        struct kevent *k = &l->native[i];
        int mask = 0;
        if (k->filter == EVFILT_READ)  mask |= EV_READ;
        if (k->filter == EVFILT_WRITE) mask |= EV_WRITE;
        if (k->flags & EV_ERROR) { mask |= EV_ERR | EV_WRITE | EV_READ; }
        if (k->flags & EV_EOF)   { mask |= EV_READ; }
        l->fired[i].fd   = (int)k->ident;
        l->fired[i].mask = mask;
    }
    l->nfired = n;
#endif
    return l->nfired;
}

static int nb_ev_next(nb_loop *l, int *fd, int *mask)
{
    if (l->cur >= l->nfired) return 0;
    *fd   = l->fired[l->cur].fd;
    *mask = l->fired[l->cur].mask;
    l->cur++;
    return 1;
}

#endif /* NB_EV_H */
