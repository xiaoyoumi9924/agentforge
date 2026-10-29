#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  AgentForge 一键安装（bash 包装）
#
#  用法：
#     curl -fsSL https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.sh | bash
#
#  AgentForge 是 Windows 程序，这个脚本只负责在 Git Bash / MSYS / Cygwin 里
#  把 PowerShell 安装器拉下来跑一遍。其它系统会直接告诉你不行。
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="xiaoyoumi9924/agentforge"
RAW="https://raw.githubusercontent.com/${REPO}/main/install.ps1"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    if ! command -v powershell >/dev/null 2>&1; then
      echo "[x] 找不到 powershell，请在 PowerShell 里运行：" >&2
      echo "    irm ${RAW} | iex" >&2
      exit 1
    fi
    tmp="$(mktemp --suffix=.ps1 2>/dev/null || mktemp)"
    trap 'rm -f "$tmp"' EXIT
    echo "下载安装器…"
    curl -fsSL "$RAW" -o "$tmp"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$tmp"
    ;;
  *)
    cat >&2 <<'EOF'
[x] AgentForge 目前只支持 Windows。

    Windows 用户请在 PowerShell 里运行：
      irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex

    或者直接去 Releases 下载 zip：
      https://github.com/xiaoyoumi9924/agentforge/releases
EOF
    exit 1
    ;;
esac
