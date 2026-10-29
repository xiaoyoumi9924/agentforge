# ---------------------------------------------------------------------------
#  AgentForge 一键安装
#
#  用法（PowerShell）：
#     irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
#
#  它做四件事：查最新版 → 下载 → 解压到 LOCALAPPDATA\AgentForge → 启动浏览器版。
#  不写注册表、不改 PATH，卸载就是删文件夹。
# ---------------------------------------------------------------------------
param(
    [string]$Version = 'latest',
    [string]$Dir = (Join-Path $env:LOCALAPPDATA 'AgentForge'),
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$Repo = 'xiaoyoumi9924/agentforge'
$Headers = @{ 'User-Agent' = 'AgentForge-Installer' }

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }

Say ""
Say "  AgentForge 安装器" Cyan
Say "  ----------------------------------------" DarkGray

# ---- 1. 查版本 ------------------------------------------------------------
$api = if ($Version -eq 'latest') {
    "https://api.github.com/repos/$Repo/releases/latest"
} else {
    "https://api.github.com/repos/$Repo/releases/tags/$Version"
}

try {
    $rel = Invoke-RestMethod -Uri $api -Headers $Headers -TimeoutSec 30
} catch {
    Say "  [x] 查不到版本信息：$($_.Exception.Message)" Red
    Say "      如果是网络问题，先开代理，或手动到 Releases 页面下载 zip。" Yellow
    exit 1
}

$asset = $rel.assets | Where-Object { $_.name -like 'AgentForge-Web-*.zip' } | Select-Object -First 1
if (-not $asset) {
    Say "  [x] $($rel.tag_name) 里没有找到 AgentForge-Web-*.zip" Red
    exit 1
}

Say "  版本: $($rel.tag_name)   包: $($asset.name)   大小: $([math]::Round($asset.size/1MB,1)) MB"

# ---- 2. 下载 --------------------------------------------------------------
$zip = Join-Path $env:TEMP ("agentforge-" + [guid]::NewGuid().ToString('N').Substring(0, 8) + ".zip")
Say "  下载中…"
try {
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers $Headers -TimeoutSec 300
} catch {
    Say "  [x] 下载失败：$($_.Exception.Message)" Red
    Say "      Releases 页面: https://github.com/$Repo/releases" Yellow
    exit 1
}

# ---- 3. 解压 --------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
Say "  解压到 $Dir"
Expand-Archive -Path $zip -DestinationPath $Dir -Force
Remove-Item $zip -Force -ErrorAction SilentlyContinue

if (-not (Test-Path (Join-Path $Dir 'agent.exe'))) {
    Say "  [x] 解压后没有 agent.exe，包可能损坏。" Red
    exit 1
}

Say "  [ok] 安装完成" Green
Say ""

# ---- 4. 启动 --------------------------------------------------------------
if ($NoStart) {
    Say "  已跳过启动。下次双击：$Dir\run-web.bat"
    exit 0
}

Say "  正在启动浏览器版…（首次会让你填 API Key）" Cyan
& (Join-Path $Dir 'run-web.ps1')
