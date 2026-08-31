#!/usr/bin/env bash
#
# tune.sh —— 最大并发连接测试前的系统参数调优
#
#   Linux : sudo ./tune.sh
#   macOS : sudo ./tune.sh
#
# 只改与"海量并发连接"直接相关的内核参数, 不触碰无关项。
# 所有改动均为运行时生效, 重启后失效; 需要持久化请自行写入
# /etc/sysctl.conf (Linux) 或 /etc/sysctl.conf + launchd (macOS)。
#
set -u

OS="$(uname -s)"
say() { printf '\033[1;36m[ tune ]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[ warn ]\033[0m %s\n' "$*"; }

if [ "$(id -u)" -ne 0 ]; then
    warn "未以 root 运行, 内核参数可能写不进去。建议: sudo $0"
fi

# 目标并发量, 用于计算 fd 上限; 可通过环境变量覆盖, 例: TARGET=1000000 sudo ./tune.sh
TARGET="${TARGET:-1100000}"

# ---------------------------------------------------------------- Linux
if [ "$OS" = "Linux" ]; then

    say "当前 shell 的 ulimit -n 提升到 $TARGET (仅对当前 shell 有效)"
    ulimit -n "$TARGET" 2>/dev/null || warn "ulimit 提升失败, 请在测试命令前手动执行 ulimit -n $TARGET"

    say "fs.file-max / fs.nr_open"
    sysctl -w fs.file-max=16777216            >/dev/null
    sysctl -w fs.nr_open=16777216             >/dev/null

    say "放开本地端口范围 (默认通常只有 28232 个)"
    sysctl -w net.ipv4.ip_local_port_range="1024 65535" >/dev/null

    say "放大 accept / SYN 队列"
    sysctl -w net.core.somaxconn=65535        >/dev/null
    sysctl -w net.ipv4.tcp_max_syn_backlog=65535 >/dev/null
    sysctl -w net.core.netdev_max_backlog=250000 >/dev/null

    say "压缩每连接收发缓冲 (百万连接时这是内存大头)"
    sysctl -w net.ipv4.tcp_rmem="4096 4096 16777216" >/dev/null
    sysctl -w net.ipv4.tcp_wmem="4096 4096 16777216" >/dev/null
    sysctl -w net.core.rmem_default=212992     >/dev/null
    sysctl -w net.core.wmem_default=212992     >/dev/null
    sysctl -w net.core.rmem_max=16777216       >/dev/null
    sysctl -w net.core.wmem_max=16777216       >/dev/null
    sysctl -w net.ipv4.tcp_adv_win_scale=1     >/dev/null

    # tcp_mem 单位是"页", 按物理内存的 25% 设 high, 既不早触发压力模式也不至于 OOM
    if [ -r /proc/meminfo ]; then
        TOTAL_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
        PAGES=$((TOTAL_KB / 4))
        HIGH=$((PAGES * 25 / 100))
        PRESSURE=$((HIGH * 3 / 4))
        LOW=$((HIGH / 2))
        say "tcp_mem = $LOW $PRESSURE $HIGH (页, 按物理内存 25% 计算)"
        sysctl -w net.ipv4.tcp_mem="$LOW $PRESSURE $HIGH" >/dev/null
    fi

    say "TIME_WAIT 侧优化"
    sysctl -w net.ipv4.tcp_fin_timeout=15     >/dev/null
    sysctl -w net.ipv4.tcp_tw_reuse=1         >/dev/null
    sysctl -w net.ipv4.tcp_max_tw_buckets=2000000 >/dev/null

    say "关闭 syncookies —— 测试并发容量时它会掩盖真实的队列溢出"
    sysctl -w net.ipv4.tcp_syncookies=0       >/dev/null

    # conntrack: 只要加载了模块, 它就会成为并发连接的隐形天花板
    if [ -d /proc/sys/net/netfilter ]; then
        say "检测到 conntrack, 放大表容量"
        sysctl -w net.netfilter.nf_conntrack_max=4000000  >/dev/null 2>&1
        sysctl -w net.netfilter.nf_conntrack_buckets=1000000 >/dev/null 2>&1
        sysctl -w net.netfilter.nf_conntrack_tcp_timeout_established=3600 >/dev/null 2>&1
        warn "conntrack 本身也是瓶颈; 纯内网测试建议直接卸载模块或加 iptables NOTRACK"
    fi

    say "孤儿 socket 上限"
    sysctl -w net.ipv4.tcp_max_orphans=1048576 >/dev/null

# ---------------------------------------------------------------- macOS
elif [ "$OS" = "Darwin" ]; then

    say "提升 kern.maxfiles / maxfilesperproc"
    sysctl -w kern.maxfiles=1048576            >/dev/null
    sysctl -w kern.maxfilesperproc=1048576     >/dev/null

    say "提升 accept 队列 (默认仅 128, 是大并发测试最常见的坑)"
    sysctl -w kern.ipc.somaxconn=65535         >/dev/null

    say "放开本地端口范围"
    sysctl -w net.inet.ip.portrange.first=1024 >/dev/null
    sysctl -w net.inet.ip.portrange.last=65535 >/dev/null

    say "降低 keepalive 探测周期, 便于快速发现死连接"
    sysctl -w net.inet.tcp.keepidle=600000     >/dev/null
    sysctl -w net.inet.tcp.keepintvl=30000     >/dev/null

    say "放大 socket 缓冲上限"
    sysctl -w kern.ipc.maxsockbuf=16777216     >/dev/null

    say "当前 shell ulimit -n 提升到 $TARGET"
    ulimit -n "$TARGET" 2>/dev/null || warn "如失败, 请手动执行: ulimit -n $TARGET"
    warn "macOS 单进程 fd 上限受 kern.maxfilesperproc 约束, 已一并调大"

else
    warn "未知系统 $OS, 未做调整"
    exit 1
fi

cat <<'EOF'

调优完成。建议按如下方式再确认一遍:

    ulimit -n                                  # 应为你期望的连接数 + 余量
    sysctl net.core.somaxconn                  # Linux:  应为 65535
    sysctl kern.ipc.somaxconn                  # macOS:  应为 65535
    cat /proc/sys/net/ipv4/ip_local_port_range # Linux:  应为 1024 65535

注意: ulimit 只对"当前 shell 及其子进程"生效, 所以请把 ulimit 和测试命令
写在同一条命令里, 例如:

    bash -c 'ulimit -n 1048576 && ./client -s 10.0.0.2 -p 8000-8031 -c 1000000 -r 200'

EOF
