# ---------------------------------------------------------------------------
#  AgentForge 一键安装
#
#  用法（PowerShell）：
#     irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
#
#  它做四件事：查最新版 → 下载 → 解压到 LOCALAPPDATA\AgentForge → 启动浏览器版。
#  不写注册表、不改 PATH，卸载就是删文件夹。
#
#  ⚠ 这个文件必须存成 UTF-8 **无 BOM**。
#    README 用的是 `irm ... | iex`：整段脚本是当**字符串**送进 Invoke-Expression 的，
#    BOM 会变成字符串的第一个字符，于是 param(...) 不再是第一条语句、被当成命令，
#    报「赋值表达式无效 / InvalidLeftHandSide」。而用 -File 运行的其他脚本
#    （deploy\*.ps1）恰恰相反，必须带 BOM，否则中文被当 ANSI 读成乱码。
# ---------------------------------------------------------------------------
param(
    [string]$Version = 'latest',
    [string]$Dir = (Join-Path $env:LOCALAPPDATA 'AgentForge'),
    [string]$Proxy = $env:HTTPS_PROXY,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$Repo = 'xiaoyoumi9924/agentforge'
$Headers = @{ 'User-Agent' = 'AgentForge-Installer' }
$Curl = Join-Path $env:SystemRoot 'System32\curl.exe'

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }

# 有代理就走代理（国内直连 GitHub 经常超时）
$net = @{}
if ($Proxy) { $net['Proxy'] = $Proxy }

# 下载：先试 .NET，失败再用 curl（curl 走另一套 TLS 栈，能绕开一部分环境问题）
function Get-File($url, $out) {
    try {
        Invoke-WebRequest -Uri $url -OutFile $out -Headers $Headers -TimeoutSec 300 @net
        return $true
    } catch {
        if (-not (Test-Path $Curl)) { return $false }
    }
    $a = @('-fsSL', '--max-time', '300', '-o', $out, $url)
    if ($Proxy) { $a = @('-x', $Proxy) + $a }
    & $Curl @a
    return ($LASTEXITCODE -eq 0)
}

function Get-Json($url) {
    try {
        return Invoke-RestMethod -Uri $url -Headers $Headers -TimeoutSec 30 @net
    } catch {
        if (-not (Test-Path $Curl)) { throw }
    }
    $tmp = Join-Path $env:TEMP ("af_meta_" + [guid]::NewGuid().ToString('N').Substring(0, 6) + ".json")
    $a = @('-fsSL', '--max-time', '60', '-o', $tmp, $url)
    if ($Proxy) { $a = @('-x', $Proxy) + $a }
    & $Curl @a | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "下载版本信息失败" }
    $txt = Get-Content -LiteralPath $tmp -Raw -Encoding UTF8
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    return ($txt | ConvertFrom-Json)
}

Say ""
Say "  AgentForge 安装器" Cyan
Say "  ----------------------------------------" DarkGray
if ($Proxy) { Say "  代理: $Proxy" DarkGray }

# ---- 1. 查版本 ------------------------------------------------------------
$api = if ($Version -eq 'latest') {
    "https://api.github.com/repos/$Repo/releases/latest"
} else {
    "https://api.github.com/repos/$Repo/releases/tags/$Version"
}

try {
    $rel = Get-Json $api
} catch {
    Say "  [x] 查不到版本信息：$($_.Exception.Message)" Red
    Say "      手动下载：https://github.com/$Repo/releases" Yellow
    exit 1
}

$asset = @($rel.assets | Where-Object { $_.name -like 'AgentForge-Web-*.zip' })[0]
if (-not $asset) {
    Say "  [x] $($rel.tag_name) 里没有 AgentForge-Web-*.zip" Red
    exit 1
}

Say "  版本: $($rel.tag_name)   包: $($asset.name)   大小: $([math]::Round($asset.size/1MB,1)) MB"

# ---- 2. 下载 --------------------------------------------------------------
$zip = Join-Path $env:TEMP ("agentforge-" + [guid]::NewGuid().ToString('N').Substring(0, 8) + ".zip")
Say "  下载中…"
if (-not (Get-File $asset.browser_download_url $zip)) {
    Say "  [x] 下载失败" Red
    Say "      https://github.com/$Repo/releases" Yellow
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
