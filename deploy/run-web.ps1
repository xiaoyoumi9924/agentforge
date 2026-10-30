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
    #  ★ 没 Key 也要把服务起起来：网页右上角「设置」里能填 Key，保存后下一条消息生效。
    #    早期版本在这里只警告一句就往下走，而 agent.exe 拿不到 Key 会**直接退出**，
    #    用户看到的现象是「终端一开就没了 / 网页打不开」。用一个占位 Key 先把服务撑住。
    $env:OPENAI_API_KEY = 'sk-waiting-for-browser'
    Write-Host "[!] 暂时没有 Key：先把网页开起来。" -ForegroundColor Yellow
    Write-Host "    打开后点右上角「设置」填 API Key，保存后下一条消息生效。" -ForegroundColor Yellow
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
#  先在后台排一个「等端口通了就打开浏览器」的任务，再启动服务本体。
#  为什么不在启动后打开：& $Exe --web 是阻塞调用，它返回时服务已经停了。
$Port = 8080
if ($env:AF_WEB_PORT -match '^\d+$') { $Port = [int]$env:AF_WEB_PORT }
$Url  = "http://127.0.0.1:$Port"

Write-Host "启动中… 浏览器将自动打开 $Url" -ForegroundColor DarkGray
$opener = Start-Job -ScriptBlock {
    param($u, $p)
    #  ★ 等端口真的在听，再开浏览器。
    #    写死"3 秒后打开"在慢机器上（ARM 模拟层、冷启动）会抢在服务前面，
    #    浏览器只会显示一个"无法访问此页面"—— 看上去就像装坏了。
    for ($i = 0; $i -lt 80; $i++) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $p)
            $c.Close()
            break
        } catch { Start-Sleep -Milliseconds 250 }
    }
    try { Start-Process $u }
    catch { Write-Host "  打不开浏览器，请手动访问 $u" -ForegroundColor Yellow }
} -ArgumentList $Url, $Port

try {
    & $Exe --web
} finally {
    Stop-Job $opener -ErrorAction SilentlyContinue
    Remove-Job $opener -Force -ErrorAction SilentlyContinue
}
