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
    echo "启动安装器…"
    #  ★ 不能落盘再 `powershell -File`：install.ps1 存成 UTF-8 无 BOM（README 的
    #    `irm ... | iex` 要求如此），-File 会让 PowerShell 5.1 按 ANSI 读中文而解析失败。
    #    走 -Command + iex，字符串由 PowerShell 自己下载解析，编码才是对的。
    powershell -NoProfile -ExecutionPolicy Bypass -Command "iex (irm '$RAW')"
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
