#Requires -RunAsAdministrator
<#
.SYNOPSIS
    TCP 压测前的 Windows 系统参数调优（tune.sh 的 Windows 版）

.DESCRIPTION
    做三件事:
      1) 把 TCP 动态端口范围扩到最大 (默认只有 49152-65535 共 16384 个,
         客户端建连时会在一万多条就撞到端口耗尽)
      2) 把 TIME_WAIT 回收时间从默认 120 秒降到 30 秒 (短连接压测必需)
      3) 打印几条与压测相关的当前配置, 便于对照

    用 -Revert 可以恢复到 Windows 默认值。
    动态端口范围即时生效; TcpTimedWaitDelay 在部分 Windows 版本上需要重启才生效
    (脚本结束时会提示)。
#>
param(
    [switch]$Revert
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "    $msg" -ForegroundColor Yellow }

# ---------- 1. 动态端口范围 ----------
Write-Step "配置 TCP 动态端口范围"
if ($Revert) {
    netsh int ipv4 set dynamicport tcp start=49152 num=16384 | Out-Null
    netsh int ipv6 set dynamicport tcp start=49152 num=16384 | Out-Null
    Write-Ok "已恢复默认: 49152-65535 (16384 个)"
} else {
    # 1025 起步: 避开 0-1023 的知名端口
    netsh int ipv4 set dynamicport tcp start=1025 num=64510 | Out-Null
    netsh int ipv6 set dynamicport tcp start=1025 num=64510 | Out-Null
    Write-Ok "已设置: 1025-65534 (64510 个)"
}
netsh int ipv4 show dynamicport tcp

# ---------- 2. TIME_WAIT 回收 ----------
$tcpipParams = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters"
Write-Step "配置 TIME_WAIT 回收时间 (TcpTimedWaitDelay)"
if ($Revert) {
    Remove-ItemProperty -Path $tcpipParams -Name "TcpTimedWaitDelay" -ErrorAction SilentlyContinue
    Write-Ok "已恢复默认 (120 秒)"
} else {
    Set-ItemProperty -Path $tcpipParams -Name "TcpTimedWaitDelay" -Value 30 -Type DWord
    Write-Ok "已设置为 30 秒 (默认 120 秒)"
}

# ---------- 3. 相关配置一览 ----------
Write-Step "当前相关配置"
$delay = (Get-ItemProperty -Path $tcpipParams -Name "TcpTimedWaitDelay" -ErrorAction SilentlyContinue).TcpTimedWaitDelay
if ($null -eq $delay) { $delay = "120 (系统默认)" }
Write-Host "    TcpTimedWaitDelay : $delay 秒"

$maxSynRetrans = (Get-ItemProperty -Path $tcpipParams -Name "TcpMaxConnectRetransmissions" -ErrorAction SilentlyContinue).TcpMaxConnectRetransmissions
if ($null -eq $maxSynRetrans) { $maxSynRetrans = "未设置" }
Write-Host "    SYN 重传次数      : $maxSynRetrans"

Write-Host ""
Write-Step "提示"
Write-Warn "以上参数重启后保留, 需要恢复请执行: .\tune.ps1 -Revert"
Write-Warn "动态端口范围即时生效; TcpTimedWaitDelay 在部分 Windows 版本上需重启才生效,"
Write-Warn "如果短连接压测仍报端口耗尽, 重启一次再试。"
Write-Warn "服务端(server.exe)需要放行防火墙入站, 例如:"
Write-Host '    New-NetFirewallRule -DisplayName "tcp-conn-test" -Direction Inbound ' +
          '-Protocol TCP -LocalPort 8000-8031 -Action Allow' -ForegroundColor Gray
Write-Warn "Windows 原生版用 select 后端, 连接上限由 NB_WIN_FD_SETSIZE 决定 (默认 8192);"
Write-Warn "更大规模请用 WSL2, 或加大 NB_WIN_FD_SETSIZE 重新编译。"
