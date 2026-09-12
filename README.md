# TCP 最大并发连接数测试工具（慢速建连版）

一套零依赖的 C 工具，用来测 **一台机器 / 一条链路到底能同时 Hold 住多少条 TCP 连接**。

核心设计：**定速、缓慢地建立连接**，把「并发容量」和「每秒新建连接数（CPS）」这两个
被混为一谈的指标彻底分开。很多系统、云厂商安全组、LVS/SLB、`nf_conntrack`、网卡队列
都会对 CPS 单独限流 —— 如果一上来就疯狂建连，你撞到的往往是 **CPS 天花板**，
而不是真正的 **并发连接数上限**，测出来的数字会严重偏小，且无法指导容量规划。

## 特性

- **单进程单线程 + 非阻塞 IO**（Linux epoll / macOS kqueue / Windows select），
  无第三方依赖，Linux/macOS 下可撑百万连接
- **两种模式**：`conn` 测并发容量、`cps` 测每秒新建速率，各自输出结论与归因
- **自动触顶判定**：存活数长时间不净增长就自动停止并出报告，不用守着按 Ctrl-C
- **能识别三种触顶形态**：SYN 静默丢弃 / 会话表满后踢旧建新 / NAT 空闲超时悄悄掉线
- **失败精确归因**：fd 耗尽、端口耗尽、超时、拒绝、重置，各自给出修复动作
- **服务端对拍**：服务端进度行给出 `当前 / 峰值 / accept累计 / close累计`，
  用来验证「客户端以为连上了」是否等于「服务端真的收到了」

---

## 目录结构

```
tcp-conn-test/
├── Makefile            # make 即可，零依赖
├── src/
│   ├── ev.h            # 跨平台事件循环（Linux epoll / macOS-BSD kqueue / Windows select）
│   ├── wincompat.h     # Windows 兼容层（WSA 错误翻译、非阻塞、keepalive、getopt）
│   ├── server.c        # 受端：多端口监听、discard 模式、低内存占用
│   └── client.c        # 压测端：定速建连、连接保持、心跳探测、失败归因
├── tune.sh             # 测试前的系统参数调优（Linux / macOS）
├── tune.ps1            # 同上，Windows 版（管理员 PowerShell）
└── README.md
```

编译：

```bash
make            # 产出 ./server 和 ./client
make clean
```

仅用 C 标准库，无第三方依赖。Linux、macOS、Windows 三平台均支持；
Windows 需要 MinGW/Clang 工具链，或在别的系统上 `make windows` 交叉编译，
详见「在 Windows 上运行」。

---

## 快速开始

**服务端**（被压测机）：

```bash
sudo ./tune.sh
bash -c 'ulimit -n 1048576 && ./server -p 8000-8031'
```

**客户端**（压测机，必须是另一台机器，否则内存/CPU 会互相干扰）：

```bash
sudo ./tune.sh
bash -c 'ulimit -n 1048576 && ./client -s 10.0.0.2 -p 8000-8031 -c 1000000 -r 200'
```

`-r 200` 表示每秒只新建 200 条连接 —— 慢，但只有这样测出来的才是并发容量。
程序会每秒打印一次进度，触顶后自动停止并输出完整报告；`Ctrl-C` 也能随时中断并照常出报告。

---

## 在 Windows 上运行

三条路线，按推荐顺序：

| 路线                        | 适合场景                    | 连接数上限                    | 说明                       |
| ------------------------- | ----------------------- | ------------------------ | ------------------------ |
| **WSL2**（推荐）               | 一切场景，尤其是上万连接            | 同 Linux（几十万）             | 走原生 epoll，功能与 Linux 完全一致 |
| **原生编译**（.exe）            | 只要一个 exe、目标在万级以内        | `NB_WIN_FD_SETSIZE`（默认 8192） | select 后端，见下方限制      |
| **Docker**                | 已有 Docker Desktop        | 同 Linux                  | 注意端口映射会再套一层 NAT          |

### 五分钟跑通

不管用哪条路线，跑完一次测试都是下面这几步。以原生 `.exe` 为例（WSL2 把
`.\server.exe` 换成 `./server`、跳过 `tune.ps1` 改用 `sudo ./tune.sh` 即可）：

```powershell
# 1) 被压测机：管理员 PowerShell 先调优（Windows 必做，默认动态端口只有 16384 个）
.\tune.ps1

# 2) 被压测机：放行入站，否则客户端只会看到超时
New-NetFirewallRule -DisplayName "tcp-conn-test" -Direction Inbound `
  -Protocol TCP -LocalPort 8000-8031 -Action Allow

# 3) 被压测机：起服务端（多端口能扩大四元组空间，建议至少 16 个）
.\server.exe -p 8000-8031

# 4) 压测机（必须是另一台机器）：先小规模试通
.\client.exe -s <服务端IP> -p 8000-8031 -c 2000 -r 200

# 5) 压测机：正式测 —— 目标值给到预期上限的 2 倍，让它自然撞墙后自动出报告
.\client.exe -s <服务端IP> -p 8000-8031 -c 2000000 -r 200
```

看到 `结束原因` / `【结论】` / `【原因】` 三段就是跑完了；中途按 `Ctrl-C` 也会照常输出报告。
参数含义与 Linux 版完全一致，见「参数手册」。

### 路线一：WSL2（推荐）

```powershell
wsl --install -d Ubuntu
```

进入 WSL 后按 Linux 的方式编译运行：

```bash
sudo apt update && sudo apt install -y build-essential
git clone <你的仓库地址> && cd tcp-conn-test
make
bash -c 'ulimit -n 1048576 && ./client -s 10.0.0.2 -p 8000-8031 -c 100000 -r 200'
```

注意两点：

- **不要把源码放在 `/mnt/c` 下编译**，跨文件系统 IO 很慢；放到 WSL 自己的家目录里。
- **WSL2 自带一层 NAT**（Hyper-V 虚拟交换机）。如果目标是测物理网卡那条链路的上限，
  WSL2 的 NAT 会话表也会成为瓶颈，判定瓶颈位置时要把它算进去；
  如果只是用 Windows 当压测客户端去测服务端容量，则完全没问题。

### 路线二：原生 Windows 版（.exe）

**方式 A：在 Windows 上直接编译（MSYS2）**

```powershell
winget install MSYS2.MSYS2
```

打开 **MSYS2 UCRT64** 终端：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
cd /c/path/to/tcp-conn-test
make                      # 产出 server.exe / client.exe
```

**方式 B：在 Linux/macOS 上交叉编译，把 exe 拷过去**

```bash
# 用 zig（自带 mingw 头文件与导入库，不用另装工具链）
make windows WINCC="zig cc -target x86_64-windows-gnu"

# 或已装 MinGW-w64 的情况（macOS: brew install mingw-w64）
make windows WINCC="x86_64-w64-mingw32-gcc"
```

产出 `build-windows/server.exe` 与 `build-windows/client.exe`（PE32+ x86-64，
拷到 Windows 直接双击或用 PowerShell 运行即可，无需任何运行时依赖）。
该目录已在 `.gitignore` 中 —— 二进制不随仓库分发，请自行编译。

> 本项目的 Windows 版就是用 `zig cc -target x86_64-windows-gnu`（zig 0.14.1）
> 交叉编译验证的：**编译零警告**，产物为 PE32+ x86-64。

**运行**（PowerShell 或 CMD，参数与 Linux 版完全一致）：

```powershell
.\server.exe -p 8000-8031
.\client.exe -s 10.0.0.2 -p 8000-8031 -c 5000 -r 200
```

**Windows 系统调优**（管理员 PowerShell，对应 `tune.sh`）：

```powershell
.\tune.ps1               # 动态端口扩到 1025-65534, TIME_WAIT 降到 30 秒
.\tune.ps1 -Revert       # 恢复 Windows 默认值
```

Windows 默认的动态端口范围只有 `49152-65535`（16384 个），客户端在一万多条就会
撞到端口耗尽，所以这一步在 Windows 上比 Linux 更必要。

**Windows 防火墙**：服务端需要放行入站，否则客户端只会看到超时：

```powershell
New-NetFirewallRule -DisplayName "tcp-conn-test" -Direction Inbound `
  -Protocol TCP -LocalPort 8000-8031 -Action Allow
```

### Windows 原生版的差异与限制

| 项目           | Linux / macOS            | Windows 原生版                                        |
| ------------- | ------------------------ | -------------------------------------------------- |
| 事件循环         | epoll / kqueue           | **select**（Windows 没有 epoll/kqueue）                 |
| 连接数上限        | 内存与 fd 决定（百万级）           | **`NB_WIN_FD_SETSIZE`，默认 8192**                     |
| fd 上限概念      | `ulimit -n`              | 无，启动时打印 select 后端的上限                                |
| TCP keepalive | `TCP_KEEPIDLE`           | `SIO_KEEPALIVE_VALS`（`-k` / `-K` 同样有效）               |
| listen backlog | 受 `somaxconn` 限制并告警      | Win8+ 由系统动态管理，不再告警                                  |
| 端口复用         | `SO_REUSEADDR`           | 服务端改用 `SO_EXCLUSIVEADDRUSE`（Windows 的 SO_REUSEADDR 允许抢占他人端口） |
| 中断信号         | `SIGINT` / `SIGTERM`     | 控制台 Ctrl Handler（Ctrl-C / 关闭窗口都会正常出报告）             |
| 编译器          | cc / gcc / clang         | MinGW-w64、Clang；**MSVC (cl.exe) 未适配**              |

> 为什么不用 IOCP？IOCP 是完成端口模型（proactor），与本项目 reactor 式的事件循环
> 结构差异很大，接进来等于重写一遍事件层。为了保持**一份可读的跨平台代码**，
> Windows 侧选了语义最接近的 select。需要 Windows 上跑十万级连接时，用 WSL2 更划算。

**select 后端意味着什么：** `select()` 每次调用都要线性扫描整个 fd 集合，是 O(n)。
在 8192 这一档，客户端自身 CPU 会先于链路成为瓶颈，**测出来的数字不代表真实容量**。
所以：

- 目标在几千到一两万、只是想快速验证：原生版够用
- 要测真实上限：**用 WSL2**，或把 `NB_WIN_FD_SETSIZE` 调大重新编译
  （同时记得 select 的 O(n) 开销也会同步放大）：

```bash
make windows WINCC="zig cc -target x86_64-windows-gnu" \
     CFLAGS="-O2 -Wall -Wextra -std=c11 -DNB_WIN_FD_SETSIZE=32768"
```

> 说明：Windows 的 `fd_set` 是 fd 列表而非位图，`FD_SET` 直接把 SOCKET 追加进数组，
> 因此不受「fd 数值必须小于 FD_SETSIZE」的限制，`FD_SETSIZE` 限制的是**条数**。
> 本项目的 select 后端也据此用 `(fd, mask)` 数组维护关注列表，不按 fd 数值建索引。

**移植踩坑（改 Windows 相关代码前请先看）**：

1. **`SOCKET` 是 typedef，不是宏**。别用 `#ifndef SOCKET` 做平台判断 —— Winsock 的
   `SOCKET` 定义为 `UINT_PTR`，`#ifndef` 判断会成立，于是 `#define SOCKET int`
   把真正的 64 位类型覆盖掉，后续所有 `SOCKET` 声明都被悄悄改成 int
   （表现为 `FD_SET` 报 `-Wsign-compare`）。
2. **`getsockopt(SO_ERROR)` 返回的是 WSA 错误码**（如 10061），不是 errno。
   不翻译的话失败归因会全部落空，报告里只有一堆 `Unknown error`。
   本项目统一走 `nb_so_error()` 翻译。
3. **别用 `WSAPoll` 探测连接失败**：微软已记录该函数在连接失败时不回报 `POLLERR`，
   会把「服务端没起」误报成「超时」。用 `select` 的写集合 + 异常集合代替。
4. **`closesocket` 而非 `close`、`ioctlsocket(FIONBIO)` 而非 `fcntl`、
   `SIO_KEEPALIVE_VALS` 而非 `TCP_KEEPIDLE`、服务端用 `SO_EXCLUSIVEADDRUSE`** ——
   这些都在 `wincompat.h` 里包好了，新代码请复用，不要再手写 `#ifdef`。
5. **别在 Windows 上假设 6 万个本地端口**：默认动态端口只有 16384 个。

### 路线三：Docker

```powershell
docker run --rm -it --network host -v ${PWD}:/src -w /src gcc:13 bash -c "make && ./client ..."
```

`--network host` 在 Docker Desktop 上走的是虚拟机的网络栈，同样会多一层 NAT；
对结果要求严格时优先用 WSL2。

### Windows 排查速查

**命令对照** —— README 后面各节的排查命令都能在 Windows 上找到等价物：

| 用途         | Linux / macOS                        | Windows                                                  |
| ---------- | ------------------------------------ | -------------------------------------------------------- |
| 本机内网 IP    | `ipconfig getifaddr en0`             | `ipconfig`                                               |
| 公网出口 IP    | `curl -s ifconfig.me`                | `curl.exe -s ifconfig.me`                                |
| 看第 2 跳是否 CGNAT | `traceroute -m 4 223.5.5.5`    | `tracert -h 4 -w 1000 223.5.5.5`                         |
| 当前 ESTABLISHED 数 | `ss -s`                        | `(netstat -an \| Select-String ESTABLISHED).Count`        |
| TIME_WAIT 数量 | `ss -s`                           | `(netstat -an \| Select-String TIME_WAIT).Count`          |
| 动态端口范围     | `sysctl net.ipv4.ip_local_port_range` | `netsh int ipv4 show dynamicport tcp`                    |
| 连接数上限      | `ulimit -n`                          | 无 —— 原生版受 `NB_WIN_FD_SETSIZE` 限制                          |

**常见报错**：

| 现象                              | 原因                        | 处理                                                  |
| ------------------------------- | ------------------------- | --------------------------------------------------- |
| `探测连接失败: Connection refused`     | 服务端没起，或防火墙拦了              | 先确认 `.\server.exe` 在跑，再查入站规则                        |
| 客户端一直 `超时`、`存活` 不涨              | 服务端防火墙未放行（Windows 默认拦入站）   | 执行上面的 `New-NetFirewallRule`                          |
| `Can't assign requested address` 持续增长 | 动态端口耗尽（Windows 默认只有 16384 个） | 管理员执行 `.\tune.ps1`                                   |
| 存活卡在 8192 附近不动                  | select 后端上限               | 加大 `NB_WIN_FD_SETSIZE` 重编译，或改用 WSL2                    |
| 报告里出现 `Unknown error`           | 旧版未翻译 Winsock 错误码          | 用当前版本（`SO_ERROR` 已翻译成 errno）                          |
| 控制台中文乱码                         | 代码页不是 UTF-8               | 先执行 `chcp 65001`                                     |
| 双击 exe 一闪而过                     | 缺参数，程序直接退出                | 在 PowerShell / CMD 里带参数运行                            |
| 下载的 exe 被拦或提示来源不明               | SmartScreen / 杀软对未签名程序     | 右键属性勾「解除锁定」，或自行从源码编译                                 |

---

## 为什么必须慢速建连

|      | 并发连接数（本工具测的）              | 每秒新建连接数 CPS             |
| ---- | -------------------------- | ----------------------- |
| 含义   | 同时**保持**多少条连接             | 每秒**新建**多少条连接           |
| 决定因素 | 内存、fd 上限、四元组空间            | CPU、网卡队列、backlog、安全组规则  |
| 典型瓶颈 | `ulimit -n`、内存、conntrack 表 | SYN 队列、`syncookies`、网关限流 |
| 数量级  | 十万 ~ 百万                   | 几千 ~ 几万/秒               |

**如果建连太快会发生什么：** 短时间内几十万个 SYN 打过去，SYN 队列溢出触发
`syncookies`，或网关/conntrack 开始丢 SYN。此时你看到的是「建连失败」，
但这**不代表**服务端撑不住这么多并发 —— 换成 200 条/秒慢慢建，
同一台机器可能稳稳接住 100 万条连接。两者的差距常常是 10 倍以上。

**如何用本工具区分二者：** 固定一个目标连接数（比如 10 万），逐步提高 `-r`
（50 → 200 → 1000 → 5000），观察这几个指标的拐点：

- `建连失败` 从 0 开始变正
- `建连耗时 p99` 明显抬升
- 服务端 `accept 失败` 计数变正

**出现拐点时的速率就是 CPS 上限**；而**低速率下能达到的最大并发才是真正的并发容量**。

> 本机回环（127.0.0.1）测试看不到 CPS 瓶颈，因为没有中间设备、也没有真实的
> 队列压力。实测数据见文末。

---

## 参数手册

### client（压测端）

| 参数           | 默认值     | 说明                                         |
| ------------ | ------- | ------------------------------------------ |
| `-s <host>`  | 必填      | 目标主机（IP 或域名）                               |
| `-p <ports>` | 8000    | 目标端口，支持 `8000` / `8000,8001` / `8000-8031` |
| `-c <n>`     | 10000   | 目标并发连接数                                    |
| `-r <rate>`  | **50**  | **新建连接速率（条/秒）—— 本工具的关键参数**                 |
| `-I <n>`     | 64      | 最大未完成握手数，防止瞬间打出 SYN 洪峰                     |
| `-T <sec>`   | **15**  | **单次 connect 超时（秒）**，0 = 交给内核（约 127s）     |
| `-G <sec>`   | **60**  | **存活数多少秒不「净增长」就判定触顶并自动停止**，0 = 不自动停        |
| `-m <mode>`  | conn    | `conn` 并发容量 / `cps` 新建速率                   |
| `-Z <rates>` | 见下      | CPS 档位表，逗号分隔                               |
| `-Y <sec>`   | 10      | CPS 每档持续秒数                                 |
| `-L`         | cps 默认开 | 强制 RST 关闭，避免 TIME_WAIT 占满端口                |
| `-M <n>`     | 512     | 单次循环最大补发条数（令牌桶）                            |
| `-b <ips>`   | 无       | 绑定源 IP 列表，逗号分隔，用于扩展四元组                     |
| `-B <sec>`   | 10      | 心跳周期（秒），0 = 关闭                             |
| `-P <n>`     | 200000  | 心跳每秒最多探测条数                                 |
| `-n <bytes>` | 0       | 建连成功后立即发送的字节数                              |
| `-w <sec>`   | 30      | 达标后继续观察多少秒（验证连接不会被悄悄断开）                    |
| `-t <sec>`   | 0       | 总时限，0 = 不限                                 |
| `-k` / `-K`  | 关 / 600 | 开启 TCP keepalive / idle 秒数                 |
| `-R` / `-S`  | 4096    | SO_RCVBUF / SO_SNDBUF                      |
| `-A`         | —       | 关闭自适应降速                                    |
| `-q`         | —       | 静默，只输出最终报告                                 |
| `-o <file>`  | —       | 报告写入文件                                     |

`-G` 的判定条件是**净增长**（存活数有没有创新高），不是「有没有建连成功」。
因为链路会话表满时会出现「踢旧建新」：新连接一直建得上、`失败` 一直是 0，
但存活数原地不动，只看失败数会永远跑下去。

### server（受端）

| 参数           | 默认值     | 说明                     |
| ------------ | ------- | ---------------------- |
| `-p <ports>` | 8000    | 监听端口，支持范围写法            |
| `-b <ip>`    | 0.0.0.0 | 监听网卡 IP                |
| `-e`         | 关       | 回显模式（默认丢弃，避免带宽成为瓶颈）    |
| `-B <n>`     | 65535   | listen backlog         |
| `-R` / `-S`  | 4096    | 每连接收发缓冲，**百万连接时的内存大头** |
| `-k` / `-K`  | 开 / 600 | TCP keepalive          |
| `-q`         | —       | 静默                     |

---

## 进度行怎么读

```
[    63.3s] 存活=2366  峰值=2461  握手=18  累计成功=5340  失败=0  超时=0  对端关闭=2974  速率=100/s
```

| 字段    | 含义                                        |
| ----- | ----------------------------------------- |
| 存活    | 当前处于 ESTABLISHED 的连接数                     |
| 峰值    | 存活数的历史最大值 —— **这就是「最大并发连接数」**            |
| 握手    | 已发出 SYN 但握手未完成的连接数；顶到 `-I` 上限说明 SYN 没有回应 |
| 累计成功  | 全程建连成功总数（含已被关闭的）                          |
| 失败    | 建连失败总数，含下面两类                              |
| 超时    | 其中因 `-T` 超时而失败的：SYN 发出后**没有任何响应**         |
| 对端关闭  | 已建立的连接被对端 FIN/RST 关闭的条数                   |
| 速率    | 当前建连速率，连续失败时会自动降速（`-A` 关闭）                |

几个一眼诊断的组合：

- `握手` 顶满 + `超时` 持续增长 → SYN 被上游静默丢弃
- `失败=0` + `对端关闭` 跟着 `累计成功` 一起涨 → 踢旧建新（见下节形态 B）
- `累计成功` 涨但 `存活` 往下掉 → NAT 空闲超时在悄悄回收（形态 C）

---

## 两种模式：并发容量 vs 新建速率

| 模式               | 命令        | 测什么           | 关键指标     |
| ---------------- | --------- | ------------- | -------- |
| **conn**（默认）     | `-m conn` | 能同时 Hold 住多少条 | 历史峰值并发   |
| **cps**          | `-m cps`  | 每秒能新建多少条      | 稳定上限 / 临界值 |

### CPS 模式：自动阶梯加压 + 自动归因

```bash
./client -s 10.0.0.2 -p 8000-8031 -m cps -Z 200,500,1000,2000,4000,8000 -Y 10
```

按档位逐级加压，每档结束打印一行，最后输出结论与原因：

```
档位 目标/秒 实测/秒 达成率 成功/尝试   成功率 p50(ms)   p99(ms)   判定
1     1000      1000      100    % 3999/3999        100.0  % 0.13      1.4       PASS
2     4000      2753      77     % 12497/12507       99.9   % 388.76    1014.5    未达成

【结论】最大每秒新建连接数(CPS)
  稳定上限 : 1000 条/秒  (成功率 >= 95% 的最高档)
  临界值   : 目标 4000 条/秒时开始恶化 (该档实际只跑到 3090 条/秒)

【原因】
  拐点档  : 4000 条/秒 —— 成功率仍有 99.9%, 但延迟已严重恶化
  佐证    : p99 延迟 2.3ms -> 1220.7ms (抬升 537 倍, 队列严重积压)
  该档错误分布:
      Connection reset by peer         12         100.0%
  判定    : 连接被立即重置 —— 本机或服务端 somaxconn 过小
            处理: sysctl -w net.core.somaxconn=65535 (macOS: kern.ipc.somaxconn)
```

**CPS 模式的判定维度有四个**，不只是看成功率：

1. **达成率** = 实际发起速率 ÷ 目标速率 —— 低说明客户端自己发不出，该档作废
2. **成功率** = 成功 ÷ (成功+失败)，分母不含还在握手的连接
3. **p99 延迟恶化倍数** —— 成功率 100% 但延迟涨 10 倍以上，同样算拐点
4. **握手窗口饱和度** —— 区分「客户端发不出」和「对端接不住导致握手迟迟不返回」

### 归因对照表

| 现象                 | 判定                       | 处理                                          |
| ------------------ | ------------------------ | ------------------------------------------- |
| 失败以超时为主            | SYN 被上游静默丢弃（CGNAT/网关限速）  | 换链路对比验证                                     |
| `Connection refused` 为主 | 服务端 accept 队列溢出          | 调大 backlog                                   |
| `Connection reset` 为主 | 本机或服务端 `somaxconn` 过小    | `sysctl -w net.core.somaxconn=65535`         |
| `Can't assign` 为主  | 短连接 TIME_WAIT 占满端口       | 加 `-L` 强制 RST 关闭                             |
| 成功率正常但 p99 飙升      | 队列积压，已接近上限               | 取其为目标速率上限                                    |
| 达成率低且延迟正常          | 客户端单线程发不出                | 加大 `-M`/`-I` 或多进程                            |

> 客户端单线程在本机环境约能发到 3000 条/秒。若被测链路的 CPS 明显高于此，
> 工具会提示「速率未达成」，此时需要多进程并行加压，否则测到的是客户端自己的天花板。

---

## 三种触顶形态与判读

同样的「数字上不去」，背后可能是三种完全不同的机制。判读顺序：**先看「失败」和「对端关闭」哪个在涨**。

### 形态 A：SYN 静默丢弃（链路会话配额用满）

```
[   138.2s] 存活=2004  峰值=2004  握手=6  累计成功=2004  失败=164  超时=164  对端关闭=0  速率=1/s
```

特征：`存活` 卡死不动、`失败`≈`超时` 持续增长、`对端关闭=0`、`握手` 顶到 `-I`。
含义：包根本没到服务端就被丢了，但**已建立的连接全程不掉线** —— 服务端没主动断开任何一条。
定位：见「链路瓶颈定位」。

### 形态 B：踢旧建新（会话表满，中间设备踢掉旧连接让位）

```
[    63.3s] 存活=2366  峰值=2461  握手=0  累计成功=5340  失败=0  超时=0  对端关闭=2974  速率=100/s
```

特征：`失败=0`、`超时=0`，新连接一路建得上，但 `对端关闭` 跟着 `累计成功` 同步上涨，
存活数被压在一个水位上不去。平均连接寿命 ≈ `存活 ÷ 关闭速率`（本例约 24 秒）。

**关键点：本工具的 server 从不主动关闭健康的连接**（只在收到对端 FIN 或读写出错时才 close），
所以这些 FIN/RST 不是你的服务端程序发的。报告里会直接提示这一点：

```
  形态  : 踢旧建新 —— 新连接一直建得上, 但旧连接被对端关闭(FIN/RST) 362 条
          稳态水位 41 条(历史峰值 104), 平均每条连接活约 2 秒后被关掉
  注意  : 本工具的 server 不会主动关闭健康的连接, 所以这些 FIN/RST 大概率来自
          中间设备的会话回收, 或服务端的 accept 队列溢出 / 进程重启。
```

> 注意：形态 B 下 `失败` 恒为 0，早期版本只看失败数，**永远判定不出触顶、程序会一直跑**。
> 现在 `-G` 改用净增长判定，可以正常停止。

### 形态 C：悄悄掉线（NAT 空闲会话超时）

特征：`累计成功` 在涨，但 `存活` 缓慢下降，`心跳发现失效` 计数变正。
含义：NAT/CGNAT 满了的表现是**静默回收**，而不是 `connect` 失败 —— 连接还在，
但已经不通了，只能靠心跳发现。缩短心跳周期（`-B 5`）并加长观察时间（`-w 120`）可确认。

**这样也能区分「触发了限流的临时抖动」和「真正的容量上限」：**
降速后如果又能继续建连，说明刚才是 CPS 限制；降速后仍然失败，才是真上限。

---

## 链路瓶颈定位

如果目标是测**服务端（云主机）的并发容量**，却从**家宽**发起压测，那么在家宽出口
2000 条就撞墙，服务端可能连 1% 的能力都没用到。**必须用另一台云主机（同机房/内网）
做客户端**，才能测出服务端真实容量。反过来，如果就是要测这条家宽链路的上限，
那卡住的数值本身就是结论。

### 是否 CGNAT（决定天花板量级）

对比**本机内网 IP** 与 **公网出口 IP**，更重要的是看 **traceroute 第 2 跳**：

```bash
# Linux / macOS
ipconfig getifaddr en0            # 本机内网 IP（Linux: ip addr | grep inet）
curl -s --max-time 8 ifconfig.me  # 公网出口（先 env | grep -i proxy 确认没走代理）
traceroute -m 4 -w 1 -q 1 223.5.5.5
```

```powershell
# Windows
ipconfig                          # 本机内网 IP
curl.exe -s ifconfig.me           # 公网出口
tracert -h 4 -w 1000 223.5.5.5    # 看前 4 跳
```

第 2 跳落在 `100.64.0.0/10`（RFC 6598 运营商级 NAT 专用段）就是 CGNAT。

> `curl` 看到的公网 IP 落在移动/电信的公网段**不能排除 CGNAT** —— 那只是 CGNAT 设备
> 的公网侧，是很多人共用的。实测中 `111.27.69.229` 看起来是公网 IP，
> 但 traceroute 第 2 跳是 `100.127.0.1`，即处于 CGNAT 之后。

### 路由器瓶颈 vs 运营商限制：三步区分

从家宽压测时，卡住的数值要分清是**本地路由器 NAT 表满**还是**运营商 CGNAT 配额**，
两者现象几乎一样（都是静默丢 SYN）。按成本从低到高：

| 步骤                    | 操作                          | 判定                                          |
| --------------------- | --------------------------- | ------------------------------------------- |
| ① 路由器管理页看 NAT 会话数     | 测试进行时登录网关，实时观察会话数/CPU       | 顶在 2048/4096 这类整数档、管理页卡顿 → **路由器瓶颈**        |
| ② IPv6 直连重测           | `-s` 换成云主机的 IPv6 地址         | IPv6 无 NAT，同时绕开路由器和 CGNAT：轻松破 2000 → **瓶颈在 IPv4 NAT 层**；仍卡 → 非 NAT，是线路级配额或服务端 |
| ③ 光猫改桥接 + 本机 PPPoE 直拨 | 系统设置 → 网络 → 添加 PPPoE，输入宽带账号 | 仍卡在同一数值 → **运营商实锤**；上限大涨 → **路由器实锤**        |

补充手段：用同一路由器下的**第二台设备同时压测**，若两台合计仍是同一个总数 → 全局表限制；
若各自都能到上限 → 限制按每内网 IP 计。

解决路径：向运营商申请公网 IPv4（家宽一般可免费或低价开通），拿到后 CGNAT 层消失，
上限就只由路由器决定；换路由器或刷 OpenWrt 调大 conntrack 可继续往上走。

### 服务端对拍：识别伪造的 FIN/RST

「客户端建连成功」不等于「服务端真的收到」；同理，「客户端收到 FIN/RST」也不等于
「服务端真的发了」。用服务端进度行的 `当前 / close累计` 与客户端对拍：

- **服务端 `close累计` ≈ 客户端 `对端关闭`、`当前` ≈ 客户端 `存活`**
  → 关闭确实发生在服务端：查 accept 队列溢出（Linux：`netstat -s | grep -i listen`；
  Windows：`netstat -s -p tcp` 看 `Failed Connection Attempts` / `Reset Connections`）、
  `tcp_abort_on_overflow`、进程是否被 OOM 杀掉（`dmesg | grep -i kill`）
- **服务端 `当前` 明显大于客户端 `存活`**（服务端以为连接还活着）
  → FIN/RST 是**链路中间设备伪造的** → CGNAT 会话回收或云厂商防护实锤

服务端侧顺手查：

```bash
sudo sysctl net.netfilter.nf_conntrack_count net.netfilter.nf_conntrack_max
sudo dmesg | tail -50                  # conntrack table full / oom-kill
netstat -s | grep -iE "overflow|reset"  # accept 队列溢出
```

---

## 容量天花板与排查

真正的上限由下面几项里**最小**的那个决定：

| 天花板       | 计算 / 位置                   | 典型值                |
| --------- | ------------------------- | ------------------ |
| 进程 fd     | `ulimit -n`               | 默认 1024，需调到 100 万+ |
| 系统 fd     | `fs.file-max`             | 通常几百万              |
| **四元组空间** | 源IP数 × 源端口数 × 目的IP × 目的端口 | 单 IP 单端口 ≈ 6 万     |
| **内存**    | 连接数 × (收发缓冲 + 内核 sock 结构) | 每条约 8~16 KB        |
| conntrack | `nf_conntrack_max`        | 默认常为 26 万，最容易被忽略   |
| accept 队列 | `somaxconn`               | **常被忽略**，见下        |

### 两个最容易踩的坑

**1. backlog 被静默截断。** `listen(fd, 65535)` 会被内核截断到 `somaxconn`，
而 macOS 默认只有 **128**、部分 Linux 发行版只有 128。本工具的服务端启动时会
读取并告警：

```
[警告] backlog 被内核截断为 128 (你请求了 65535)。大并发建连时 accept 队列会溢出,
       表现为客户端报「建连成功」但服务端并未收到。修复:
         Linux: sysctl -w net.core.somaxconn=65535
         macOS: sudo sysctl -w kern.ipc.somaxconn=65535
```

**2. 客户端「建连成功」不等于服务端真的收到了。** 握手完成只代表客户端这一侧
认为连上了；如果服务端 accept 队列溢出，这条连接随后会被丢弃。所以
**最终结果必须以服务端的 `累计 accept` 为准**，两边数字对不上就说明有连接被丢了。

### 内存估算

```
所需内存 ≈ 连接数 × (tcp_rmem + tcp_wmem + struct sock ≈ 1.5KB)
```

默认缓冲下每条约 8~16 KB，**100 万连接需要 8~16 GB 内存**。
本工具已把每连接 `SO_RCVBUF`/`SO_SNDBUF` 压到 4KB，`tune.sh` 也调了
`tcp_rmem`/`tcp_wmem`，可以显著降低这个数字。

### 突破 6 万端口限制

客户端单 IP 最多约 6 万个本地端口，这是硬限制。要测更高并发：

```bash
# 方案一：服务端开多个端口（最简单）
./server -p 8000-8031                                    # 32 个端口
./client -s 10.0.0.2 -p 8000-8031 -c 2000000 -r 200      # 容量 ≈ 6万 × 32 = 192 万

# 方案二：客户端绑多个源 IP（需先在本机网卡上配置好这些 IP）
sudo ifconfig eth0:1 10.0.0.5 netmask 255.255.255.0 up
./client -s 10.0.0.2 -p 8000-8031 -b 10.0.0.5,10.0.0.6 -c 4000000 -r 200
```

---

## 已知限制

- `-G` 的停滞判定要求已出现过异常信号（失败 / 对端关闭 / 心跳失效）。全程零异常、
  只是单纯在慢速建连的场景不会触发自动停止，可用 `-t` 设总时限兜底。
- 客户端单线程在本机约能发到 3000 条/秒，更高的 CPS 需多进程并行。
- 存活数存在常态抖动，`-G` 用「净增长 + 容忍阈值」判定，极端抖动场景下可能多等一个窗口。
- 心跳是全量轮询分摊（`-P` 限制每秒探测条数），超百万连接时单轮心跳周期会被拉长，
  形态 C 的发现延迟会变大。
- **Windows 原生版用 select 后端，是 O(n) 扫描**，默认上限 8192 条（`NB_WIN_FD_SETSIZE`），
  上万条时客户端自身 CPU 会先吃紧。要测真实上限请用 WSL2。
- Windows 原生版的验证程度：**交叉编译通过、零警告**，且事件循环（select 后端）
  已在 macOS 上以 `-DNB_FORCE_SELECT` 强制走同一套代码路径跑通真实测试
  （建连、心跳、CPS 模式、踢旧建新判定均正常）；
  但 Winsock 相关部分（WSA 错误翻译、`SIO_KEEPALIVE_VALS`、控制台 Ctrl Handler、
  `SO_EXCLUSIVEADDRUSE`）**尚未在真实 Windows 上跑过** —— 首次在 Windows 上运行时
  请留意启动阶段的输出是否正常。
- 原生 Windows 版的容量估算按 16384 个动态端口计算（Windows 默认值），
  跑过 `tune.ps1` 之后端口范围已扩大，但程序不做二次探测，估算值会偏保守。
- MSVC (`cl.exe`) 未适配（缺 `getopt` 等 POSIX 接口），请用 MinGW-w64 / Clang 或 WSL2。

---

## 注意事项

- **只对自有机器做测试。** 对公网或他人服务发起大量连接属于 DoS 行为。
- 客户端与服务端**尽量分开两台机器**，同机测试时内存和 CPU 会互相抢占，
  测出来的数字没有参考价值。
- `ulimit` 只对当前 shell 及其子进程生效，务必和启动命令写在一起：
  `bash -c 'ulimit -n 1048576 && ./client ...'`
- `tune.sh` 的改动重启后失效，需要持久化请自行写入 `/etc/sysctl.conf`；
  Windows 的 `tune.ps1` 相反 —— **改的是注册表，重启后保留**，恢复请执行 `.\tune.ps1 -Revert`。
- Windows 没有 `ulimit`，原生版的连接数上限由 `NB_WIN_FD_SETSIZE` 决定（默认 8192），
  程序启动时会打印实际值。
- 测试结束后会留下大量 `TIME_WAIT`，属正常现象，可通过 `ss -s`（Linux）、
  `netstat -an | wc -l`（macOS）或 `(netstat -an | Select-String TIME_WAIT).Count`（Windows）
  观察回收情况。
- `syncookies` 建议测试期间关闭：它会掩盖真实的队列溢出问题，
  让「丢连接」看起来像「建连成功」。`tune.sh` 已处理。
