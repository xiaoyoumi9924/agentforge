# ---------------------------------------------------------------------------
#  L7-L8 · verify_L7.ps1 —— 仓库索引 + 检索 + 组装验证
#
#  ① --selftest（31 项，含 T9 索引测试）
#  ② --index-stats：索引统计 + 依赖边（含 src/agent_loop.cpp -> agent_loop.hpp）
#  ③ --index "AgentLoop"：检索命中 class AgentLoop
#  ④ Mock 冒烟：检索注入 L3 后循环仍收敛
#
#  用法：
#      .\tests\verify_L7.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Proj = Split-Path -Parent $Here
Set-Location $Here
$Exe  = Join-Path $Proj "build\agent.exe"
$Port = 8789
$Log  = Join-Path $Here "mock_requests.log"
$Py   = "C:\Program Files\Python313\python.exe"
if (-not (Test-Path $Py)) { $Py = (Get-Command python.exe).Source }

function Has { param($text, $needle) return (($null -ne $text) -and ($text -as [string]).Contains($needle)) }

$script:pass = 0
$script:fail = 0
function Check { param($ok, $name, $detail = '')
    if ($ok) { $script:pass++ } else { $script:fail++ }
    Write-Output ("  {0} {1}{2}" -f ($(if ($ok) {'[PASS]'} else {'[FAIL]'})), $name, $(if ($detail) {"  —— $detail"} else {''}))
}

if (-not (Test-Path $Exe)) { Write-Output "FATAL: 找不到 $Exe —— 先跑 .\build.ps1"; exit 1 }

Write-Output "================================================"
Write-Output "  L7-L8 验证：仓库索引与检索"
Write-Output "================================================"

# ---- ① selftest ----
Write-Output ""
Write-Output "[自检] --selftest"
$st = & $Exe --selftest *>&1 | Out-String
$stPass = ([regex]::Matches($st, '\[PASS\]')).Count
$stFail = ([regex]::Matches($st, '\[FAIL\]')).Count
Check (($stFail -eq 0) -and ($stPass -ge 25)) "自检：$stPass PASS / $stFail FAIL（≥25 项）"

# ---- ② 索引统计 ----
Write-Output ""
Write-Output "[索引] --index-stats"
Push-Location $Proj
$idx = & $Exe --index-stats *>&1 | Out-String
Check (Has $idx '索引：') "输出索引统计"
Check (Has $idx '依赖边') "输出依赖边统计"
Check (Has $idx 'src/agent_loop.cpp -> agent_loop.hpp') "依赖边正确（agent_loop.cpp -> agent_loop.hpp）"
Check (Has $idx 'src/main.cpp -> agent_loop.hpp') "依赖边正确（main.cpp -> agent_loop.hpp）"

# ---- ③ 检索 ----
Write-Output ""
Write-Output "[检索] --index AgentLoop"
$hit = & $Exe --index "AgentLoop" *>&1 | Out-String
Pop-Location
Check (Has $hit 'class AgentLoop') "检索命中 class AgentLoop"
Check (Has $hit 'src/agent_loop.hpp') "命中片段来自 agent_loop.hpp"

# ---- ④ Mock 冒烟（检索注入 L3 后循环仍收敛）----
Write-Output ""
Write-Output "[冒烟] 带检索的读文件任务"
$job = Start-Job -ScriptBlock {
    param($py, $script, $port, $logfile)
    & $py $script $port $logfile
} -ArgumentList $Py, (Join-Path $Here "mock_server.py"), $Port, $Log
Start-Sleep -Seconds 3

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"
$env:AF_PLUGIN       = ""

$f1 = Join-Path $Here "_l7_smoke.txt"
& $Exe "请读取 mock_target.txt 并告诉我里面有哪些函数" *>&1 | Out-File -FilePath $f1 -Encoding UTF8
$code = $LASTEXITCODE
$t1 = Get-Content $f1 -Raw -Encoding UTF8

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue

Check ($code -eq 0) "冒烟退出码为 0（Completed）" "exit=$code"
Check (Has $t1 'int add') "工具读到的内容进入最终回答"
Check (Has $t1 'completed') "LoopOutcome=Completed"

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

if ($script:fail -gt 0) { exit 1 }
exit 0

