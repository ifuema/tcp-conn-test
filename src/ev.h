/*
 * ev.h —— 跨平台事件循环薄封装
 *   Linux    : epoll (LT 水平触发)
 *   macOS/BSD: kqueue
 *   Windows  : select（上限由 NB_WIN_FD_SETSIZE 决定，默认 8192）
 *
 * 只做四件事: create / add / mod / del / wait / next
 * 之所以不用 libevent/libuv: 本工具要求零依赖、单文件可编译, 且只需
 * 极小的事件语义(可读、可写、出错), 引入第三方库反而增加部署成本。
 *
 * Windows 说明: Winsock 的 fd_set 是"fd 列表"而不是"位图", FD_SET 直接把
 * SOCKET 追加进数组, 所以不受"fd 数值必须小于 FD_SETSIZE"的限制 ——
 * FD_SETSIZE 限制的只是单次 select 能监听的**条数**。本后端也因此用
 * (fd, mask) 数组维护关注列表, 不按 fd 数值建索引。
 */
#ifndef NB_EV_H
#define NB_EV_H

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#if defined(__linux__)
#  include <sys/epoll.h>
#  include <unistd.h>
#  define NB_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
#  include <sys/types.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <unistd.h>
#  define NB_KQUEUE 1
#elif defined(_WIN32) || defined(NB_FORCE_SELECT)
#  if defined(_WIN32)
#    include "wincompat.h"
#  else
#    include <sys/select.h>
#    include <unistd.h>
#    include <sys/time.h>
     /* NB_FORCE_SELECT: 在 Linux/macOS 上强制走 select 后端, 用于验证这段逻辑。
        注意 POSIX 的 fd_set 是位图, 连接数受 FD_SETSIZE(通常 1024) 限制。 */
#  endif
#  ifndef _WIN32
     /* 只有 POSIX 的 select 后端才需要把 SOCKET 兜底成 int。
        不能对 Windows 也这么干: Winsock 的 SOCKET 是 typedef(UINT_PTR) 而非宏,
        `#ifndef SOCKET` 会成立, 于是这里把 SOCKET 宏定义成 int, 覆盖掉真正的
        64 位类型 —— 结果是 FD_SET 拿 int 去和 fd_array 的 SOCKET 元素比较
        (-Wsign-compare), 且后续所有 SOCKET 声明都会被悄悄改成 int。 */
#    ifndef SOCKET
#      define SOCKET int
#    endif
#  endif
#  define NB_SELECT 1
#else
#  error "unsupported platform: need epoll (Linux), kqueue (BSD/macOS) or select (Windows)"
#endif

/* 注意: 不能叫 EV_ERROR —— kqueue 的 <sys/event.h> 里已有同名的
   EV_ERROR(0x4000), 重定义会让 "flags & EV_ERROR" 的判断彻底失效。 */
#define EV_NONE  0
#define EV_READ  1
#define EV_WRITE 2
/* Windows 的 winbase.h 里有个同名的 EV_ERR(0x80), 本工具用不到它, 先取消掉 */
#ifdef EV_ERR
#  undef EV_ERR
#endif
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
    int  fd;          /* epoll fd 或 kqueue fd (select 后端为 -1) */
    int  max_events;  /* 单次 wait 最多返回多少事件 */
    int  nfired;
    int  cur;
    int *masks;       /* 以 fd 为下标的"当前关注事件"表 (epoll/kqueue) */
    int  masks_cap;
    nb_event *fired;
#if NB_SELECT
    fd_set *rset, *wset, *eset;
    struct { int fd; int mask; } *ents;   /* 关注的 (fd, mask) 列表 */
    int  nents, ents_cap;
    int  fired_cap;
#else
#  if NB_EPOLL
    struct epoll_event *native;
#  else
    struct kevent *native;
#  endif
#endif
} nb_loop;

static int nb_ev_create(nb_loop *l, int max_events)
{
    memset(l, 0, sizeof(*l));
    l->max_events = max_events > 0 ? max_events : 1024;

#if NB_SELECT
    l->fd = -1;
    l->rset = (fd_set *)calloc(1, sizeof(fd_set));
    l->wset = (fd_set *)calloc(1, sizeof(fd_set));
    l->eset = (fd_set *)calloc(1, sizeof(fd_set));
    /* fired 至少要能装下一次 select 的全部结果 */
    l->fired_cap = l->max_events > FD_SETSIZE ? l->max_events : FD_SETSIZE;
    l->fired = (nb_event *)calloc((size_t)l->fired_cap, sizeof(nb_event));
    if (!l->rset || !l->wset || !l->eset || !l->fired) return -1;
    return 0;
#else
#  if NB_EPOLL
    l->fd = epoll_create1(0);
#  else
    l->fd = kqueue();
#  endif
    if (l->fd < 0) return -1;

    l->native = (void *)calloc((size_t)l->max_events, sizeof(*l->native));
    l->fired  = (nb_event *)calloc((size_t)l->max_events, sizeof(nb_event));
    l->masks_cap = 4096;
    l->masks = (int *)calloc((size_t)l->masks_cap, sizeof(int));
    if (!l->native || !l->fired || !l->masks) return -1;
    return 0;
#endif
}

static void nb_ev_destroy(nb_loop *l)
{
#if NB_SELECT
    free(l->rset);
    free(l->wset);
    free(l->eset);
    free(l->ents);
    free(l->fired);
#else
    if (l->fd >= 0) close(l->fd);
    free(l->native);
    free(l->fired);
    free(l->masks);
#endif
    memset(l, 0, sizeof(*l));
}

#if !NB_SELECT
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
#endif /* !NB_SELECT */

#if NB_SELECT
/* select 后端: 用 (fd, mask) 数组维护关注列表, 不依赖 fd 的数值大小 */
static int nb_sel_find(nb_loop *l, int fd)
{
    for (int i = 0; i < l->nents; i++)
        if (l->ents[i].fd == fd) return i;
    return -1;
}

static int nb_sel_push(nb_loop *l, int fd)
{
    if (l->nents >= l->ents_cap) {
        int ncap = l->ents_cap ? l->ents_cap * 2 : 1024;
        void *p = realloc(l->ents, (size_t)ncap * sizeof(*l->ents));
        if (!p) return -1;
        l->ents = (void *)p;
        l->ents_cap = ncap;
    }
    l->ents[l->nents].fd = fd;
    l->ents[l->nents].mask = 0;
    l->nents++;
    return 0;
}

static void nb_sel_erase(nb_loop *l, int idx)
{
    l->ents[idx] = l->ents[l->nents - 1];
    l->nents--;
}
#endif

/* 覆盖式修改某 fd 关注的事件 */
static int nb_ev_mod(nb_loop *l, int fd, int mask)
{
#if NB_SELECT
    int i = nb_sel_find(l, fd);
    if (mask == 0) {                       /* 取消关注 */
        if (i >= 0) nb_sel_erase(l, i);
        return 0;
    }
    if (i < 0) {
        if (nb_sel_push(l, fd) < 0) return -1;
        i = l->nents - 1;
    }
    l->ents[i].mask = mask;
    return 0;
#else
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
#endif /* !NB_SELECT */
}

static NB_UNUSED int nb_ev_add(nb_loop *l, int fd, int mask)
{
#if NB_SELECT
    int i = nb_sel_find(l, fd);
    int old = i >= 0 ? l->ents[i].mask : 0;
#else
    int old = (fd < l->masks_cap) ? l->masks[fd] : 0;
#endif
    return nb_ev_mod(l, fd, old | mask);
}

static int nb_ev_del(nb_loop *l, int fd)
{
#if NB_SELECT
    int i = nb_sel_find(l, fd);
    if (i < 0) return 0;
    nb_sel_erase(l, i);
    return 0;
#else
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
#endif /* !NB_SELECT */
}

/* timeout_ms < 0 表示无限等待; 返回就绪事件个数 */
static int nb_ev_wait(nb_loop *l, int timeout_ms)
{
    l->nfired = 0;
    l->cur = 0;

#if NB_SELECT
    FD_ZERO(l->rset);
    FD_ZERO(l->wset);
    FD_ZERO(l->eset);
    int maxfd = -1;
    for (int i = 0; i < l->nents; i++) {
        if (l->ents[i].mask & EV_READ)  FD_SET((SOCKET)l->ents[i].fd, l->rset);
        if (l->ents[i].mask & EV_WRITE) FD_SET((SOCKET)l->ents[i].fd, l->wset);
        FD_SET((SOCKET)l->ents[i].fd, l->eset);      /* 异常: 连接出错 */
        if (l->ents[i].fd > maxfd) maxfd = l->ents[i].fd;
    }

    struct timeval tv, *ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
        ptv = &tv;
    }
    /* Windows 的 select 忽略第一个参数; POSIX 需要 maxfd+1, 传两者的交集即可 */
    int n = select(maxfd + 1, l->rset, l->wset, l->eset, ptv);
    if (n <= 0) return n < 0 ? -1 : 0;

    int cnt = 0;
    for (int i = 0; i < l->nents && cnt < l->fired_cap; i++) {
        int fd = l->ents[i].fd;
        int mask = 0;
        if (FD_ISSET((SOCKET)fd, l->rset)) mask |= EV_READ;
        if (FD_ISSET((SOCKET)fd, l->wset)) mask |= EV_WRITE;
        if (FD_ISSET((SOCKET)fd, l->eset)) mask |= EV_ERR | EV_READ | EV_WRITE;
        if (mask) {
            l->fired[cnt].fd   = fd;
            l->fired[cnt].mask = mask;
            cnt++;
        }
    }
    l->nfired = cnt;
#elif NB_EPOLL
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
