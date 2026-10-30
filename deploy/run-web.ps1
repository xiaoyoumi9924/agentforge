# ---------------------------------------------------------------------------
#  run-web.ps1 —— 启动 AgentForge 的浏览器版
#
#  它做三件事：
#    1. 找到 API Key（环境变量 > 同目录 key.dat > 当场问一次）
#    2. 把工作目录固定到 workspace\web（Agent 的所有产出都落在这里）
#    3. 起 agent.exe --web，浏览器会自动打开 http://127.0.0.1:8080
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$Exe  = Join-Path $Here 'agent.exe'

if (-not (Test-Path $Exe)) {
    Write-Host "[x] 找不到 agent.exe，安装包可能不完整。" -ForegroundColor Red
    exit 1
}

# ---- 1. API Key -----------------------------------------------------------
function Save-Key([securestring]$sec) {
    ConvertFrom-SecureString $sec | Set-Content -LiteralPath (Join-Path $Here 'key.dat') -Encoding ASCII
}

if (-not $env:OPENAI_API_KEY) {
    $keyFile = Join-Path $Here 'key.dat'
    if (Test-Path $keyFile) {
        $sec = ConvertTo-SecureString (Get-Content -LiteralPath $keyFile -Raw)
    } else {
        Write-Host ""
        Write-Host "首次启动需要模型 API Key（DeepSeek / OpenAI 兼容端点都可以）。" -ForegroundColor Cyan
        Write-Host "密钥会用 Windows DPAPI 加密后存在本目录，只有当前用户可以解开。" -ForegroundColor DarkGray
        $sec = Read-Host "粘贴 API Key（回车跳过，之后会自动再问）" -AsSecureString
        if ($sec.Length -gt 0) { Save-Key $sec }
    }
    if ($sec.Length -gt 0) {
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
        try { $env:OPENAI_API_KEY = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }
}

if (-not $env:OPENAI_API_KEY) {
    Write-Host "[!] 没有 Key，先起服务也可以，但发消息会被拒绝。" -ForegroundColor Yellow
}

# ---- 2. 默认接 DeepSeek ---------------------------------------------------
if (-not $env:OPENAI_BASE_URL) { $env:OPENAI_BASE_URL = 'https://api.deepseek.com/v1' }
if (-not $env:AF_MODEL)        { $env:AF_MODEL        = 'deepseek-chat' }

# ---- 3. 工作目录固定 ------------------------------------------------------
$WorkDir = Join-Path $Here 'workspace\web'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Set-Location $WorkDir

Write-Host ""
Write-Host "工作目录: $WorkDir" -ForegroundColor DarkGray
Write-Host "模型:     $env:AF_MODEL  ($env:OPENAI_BASE_URL)" -ForegroundColor DarkGray
Write-Host ""

# ---- 4. 启动 --------------------------------------------------------------
#  先在后台排一个「3 秒后打开浏览器」的任务，再启动服务本体。
#  为什么不在启动后打开：& $Exe --web 是阻塞调用，它返回时服务已经停了。
$Port = 8080
if ($env:AF_WEB_PORT -match '^\d+$') { $Port = [int]$env:AF_WEB_PORT }
$Url  = "http://127.0.0.1:$Port"

Write-Host "启动中… 浏览器将自动打开 $Url" -ForegroundColor DarkGray
$opener = Start-Job -ScriptBlock {
    param($u)
    Start-Sleep -Seconds 3
    Start-Process $u
} -ArgumentList $Url

try {
    & $Exe --web
} finally {
    Stop-Job $opener -ErrorAction SilentlyContinue
    Remove-Job $opener -Force -ErrorAction SilentlyContinue
}
