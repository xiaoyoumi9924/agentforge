# ---------------------------------------------------------------------------
#  L12 · evaluate.ps1 —— 回归评测（不联网：MockLLM 驱动）
#
#  评测任务（对应课程考核的功能展示维度）：
#    E1 读文件能力：模型主动调 Read → 内容回喂 → 最终回答包含事实
#    E2 插件 + 权限门：C ABI 插件被加载、Ask 分支批准后执行
#    E3 失败恢复：非法参数 → observation 回喂 → 循环不崩
#    E4 未知工具：幻觉工具 → 回喂可用工具列表
#
#  输出：scorecard（每个任务的 outcome / 工具调用数 / token / 历史条数）
#  用法：
#      .\tests\evaluate.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Proj = Split-Path -Parent $Here
Set-Location $Here
$Exe  = Join-Path $Proj "build\agent.exe"
$Dll  = Join-Path $Proj "build\plugins\sample_plugin.dll"
$Port = 8789
$Log  = Join-Path $Here "mock_requests.log"
$Py   = "C:\Program Files\Python313\python.exe"
if (-not (Test-Path $Py)) { $Py = (Get-Command python.exe).Source }

if (-not (Test-Path $Exe)) { Write-Output "FATAL: 先跑 .\build.ps1"; exit 1 }

# ---- 启动假服务端 ----
$job = Start-Job -ScriptBlock {
    param($py, $script, $port, $logfile)
    & $py $script $port $logfile
} -ArgumentList $Py, (Join-Path $Here "mock_server.py"), $Port, $Log
Start-Sleep -Seconds 3

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"
$env:AF_PLUGIN       = ""
$env:AF_SESSION      = "eval"

function RunTask {
    param($name, $task, $answer = $null, $plugin = $false)
    $env:AF_PLUGIN = if ($plugin) { $Dll } else { "" }
    $env:AF_SESSION = "eval_${RunId}_$name"
    $out = Join-Path $Here ("_eval_{0}.txt" -f $name)
    if ($null -ne $answer) { $answer | & $Exe $task *>&1 | Out-File -FilePath $out -Encoding UTF8 }
    else                   { & $Exe $task *>&1 | Out-File -FilePath $out -Encoding UTF8 }
    $code = $LASTEXITCODE
    $text = Get-Content $out -Raw -Encoding UTF8
    $spent = if ($text -match 'spent=(\d+)') { $matches[1] } else { '?' }
    $tools = if ($text -match 'toolCalls=(\d+)') { $matches[1] } else { '?' }
    $hist  = if ($text -match 'history=(\d+)') { $matches[1] } else { '?' }
    return [pscustomobject]@{
        Code = $code; Text = $text; Spent = $spent; Tools = $tools; Hist = $hist
    }
}

$RunId = Get-Date -Format "yyyyMMdd_HHmmss"

Write-Output "================================================"
Write-Output "  AgentForge 回归评测（MockLLM，零 API 费用）"
Write-Output "================================================"
Write-Output ""

# ---- E1 读文件 ----
$e1 = RunTask "E1" "请读取 mock_target.txt 并告诉我里面有哪些函数"
$e1ok = ($e1.Code -eq 0) -and $e1.Text.Contains('int add') -and $e1.Text.Contains('completed')

# ---- E2 插件 + 权限 ----
$e2 = RunTask "E2" "请用 WordCount 统计 mock_target.txt" "y" $true
$e2ok = ($e2.Code -eq 0) -and $e2.Text.Contains('已加载') -and $e2.Text.Contains('行数')

# ---- E3 非法参数恢复 ----
$e3 = RunTask "E3" "调用 Read 但故意缺少 path 参数"
$e3ok = ($e3.Code -eq 0) -and $e3.Text.Contains('缺少必需字段')

# ---- E4 未知工具 ----
$e4 = RunTask "E4" "请调用一个不存在的工具 NoSuchTool 看看"
$e4ok = ($e4.Code -eq 0) -and $e4.Text.Contains('未知工具')

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
$env:AF_PLUGIN = ""
$env:AF_SESSION = ""

# ---- Scorecard ----
Write-Output "| 用例 | 结果 | 退出码 | 工具调用 | 计费 token | 历史条数 |"
Write-Output "|---|---|---|---|---|---|"
foreach ($row in @(
    @("E1 读文件", $e1ok, $e1),
    @("E2 插件+权限门", $e2ok, $e2),
    @("E3 非法参数恢复", $e3ok, $e3),
    @("E4 未知工具", $e4ok, $e4)
)) {
    $ok = $row[1]; $r = $row[2]
    $mark = if ($ok) { "PASS" } else { "FAIL" }
    Write-Output ("| {0} | {1} | {2} | {3} | {4} | {5} |" -f $row[0], $mark, $r.Code, $r.Tools, $r.Spent, $r.Hist)
}

$pass = @($e1ok, $e2ok, $e3ok, $e4ok | Where-Object { $_ }).Count
Write-Output ""
Write-Output ("评测结果：{0}/4 通过" -f $pass)

if ($pass -lt 4) { exit 1 }
exit 0


