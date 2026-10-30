# ---------------------------------------------------------------------------
#  L6 · verify_L6.ps1 —— 六道闸门 + 沙箱 + 结构化日志 + 审计 验证
#
#  ① --selftest（27 项）：闸门/守卫/沙箱/围栏/权限门
#  ② --sandbox-check：沙箱能力自检输出（如实报告已强制/未强制）
#  ③ Mock 冒烟：假服务端驱动读文件任务 → Completed
#  ④ 结构化日志：agentforge.log 里出现 main start / done 事件
#
#  用法：
#      .\tests\verify_L6.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Proj = Split-Path -Parent $Here
Set-Location $Here
$Exe  = Join-Path $Proj "build\agent.exe"
$Port = 8789
$Log  = Join-Path $Here "mock_requests.log"
$AppLog = Join-Path $Here "agentforge.log"
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
Write-Output "  L6 验证：闸门 + 沙箱 + 日志/审计"
Write-Output "================================================"

# ---- ① selftest ----
Write-Output ""
Write-Output "[自检] agent.exe --selftest"
$st = & $Exe --selftest *>&1 | Out-String
Write-Output $st
$stPass = ([regex]::Matches($st, '\[PASS\]')).Count
$stFail = ([regex]::Matches($st, '\[FAIL\]')).Count
Check (($stFail -eq 0) -and ($stPass -ge 20)) "自检：$stPass PASS / $stFail FAIL（≥20 项）"

# ---- ② sandbox-check ----
Write-Output ""
Write-Output "[沙箱] agent.exe --sandbox-check"
$sb = & $Exe --sandbox-check *>&1 | Out-String
Write-Output $sb
Check (Has $sb 'Job Object') "自检如实报告实现方式"
Check (Has $sb '进程树强杀') "进程树强杀已生效"
Check (Has $sb '未强制') "如实报告能力边界（不吹牛）"

# ---- ③ Mock 冒烟 ----
Write-Output ""
Write-Output "[冒烟] 假服务端驱动读文件任务"
Remove-Item $AppLog -ErrorAction SilentlyContinue
$job = Start-Job -ScriptBlock {
    param($py, $script, $port, $logfile)
    & $py $script $port $logfile
} -ArgumentList $Py, (Join-Path $Here "mock_server.py"), $Port, $Log
Start-Sleep -Seconds 3

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"
$env:AF_PLUGIN       = ""

$f1 = Join-Path $Here "_l6_smoke.txt"
& $Exe "请读取 mock_target.txt 并告诉我里面有哪些函数" *>&1 | Out-File -FilePath $f1 -Encoding UTF8
$code = $LASTEXITCODE
$t1 = Get-Content $f1 -Raw -Encoding UTF8

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue

Check ($code -eq 0) "冒烟退出码为 0（Completed）" "exit=$code"
Check (Has $t1 'int add') "工具读到的内容进入最终回答"
Check (Has $t1 'completed') "LoopOutcome=Completed"

# ---- ④ 结构化日志 ----
Write-Output ""
Write-Output "[日志] agentforge.log"
if (Test-Path $AppLog) {
    $logText = Get-Content $AppLog -Raw -Encoding UTF8
    Check (Has $logText '"main"') "日志包含 main 模块事件"
    Check (Has $logText '"start"') "日志包含 start 事件"
    Check (Has $logText '"done"') "日志包含 done 事件"
} else {
    Check $false "日志文件存在"
}

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

if ($script:fail -gt 0) { exit 1 }
exit 0
