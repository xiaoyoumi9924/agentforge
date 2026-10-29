# ---------------------------------------------------------------------------
#  L5 · verify_L5.ps1 —— AgentLoop + 上下文预算 + 消息守卫 验证
#
#  ① --selftest：进程内自检（预算/测量/守卫/收敛/重复检测/权限门），不联网
#  ② Mock 冒烟：假服务端驱动"读文件"任务，验证组装好的上下文能跑通新循环
#
#  用法：
#      .\tests\verify_L5.ps1
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

if (-not (Test-Path $Exe)) {
    Write-Output "FATAL: 找不到 $Exe —— 先跑 .\build.ps1"
    exit 1
}

Write-Output "================================================"
Write-Output "  L5 验证：AgentLoop + 上下文预算 + 消息守卫"
Write-Output "================================================"

# ---- ① 进程内自检 -----------------------------------------------------------
Write-Output ""
Write-Output "[自检] agent.exe --selftest"
$st = & $Exe --selftest *>&1 | Out-String
Write-Output $st
$stPass = ([regex]::Matches($st, '\[PASS\]')).Count
$stFail = ([regex]::Matches($st, '\[FAIL\]')).Count
$script:pass = 0
$script:fail = 0
if ($stFail -eq 0 -and $stPass -ge 8) { $script:pass++ } else { $script:fail++ }
Write-Output ("  {0} 自检：{1} PASS / {2} FAIL" -f (($(if ($stFail -eq 0) {'[PASS]'} else {'[FAIL]'})), $stPass, $stFail))

# ---- ② Mock 冒烟 ------------------------------------------------------------
Write-Output ""
Write-Output "[冒烟] 假服务端驱动读文件任务"
$job = Start-Job -ScriptBlock {
    param($py, $script, $port, $logfile)
    & $py $script $port $logfile
} -ArgumentList $Py, (Join-Path $Here "mock_server.py"), $Port, $Log
Start-Sleep -Seconds 3

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"
$env:AF_PLUGIN       = ""

$f1 = Join-Path $Here "_l5_smoke.txt"
& $Exe "请读取 mock_target.txt 并告诉我里面有哪些函数" *>&1 | Out-File -FilePath $f1 -Encoding UTF8
$code = $LASTEXITCODE
$t1 = Get-Content $f1 -Raw -Encoding UTF8

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue

function Has { param($text, $needle) return (($null -ne $text) -and $text.Contains($needle)) }

if ($code -eq 0) { $script:pass++ } else { $script:fail++ }
Write-Output ("  {0} 冒烟退出码为 0（Completed）—— exit={1}" -f ($(if ($code -eq 0) {'[PASS]'} else {'[FAIL]'})), $code)
if (Has $t1 'int add') { $script:pass++ } else { $script:fail++ }
Write-Output ("  {0} 工具读到的内容进入最终回答" -f ($(if (Has $t1 'int add') {'[PASS]'} else {'[FAIL]'})))
if (Has $t1 'completed') { $script:pass++ } else { $script:fail++ }
Write-Output ("  {0} LoopOutcome=Completed" -f ($(if (Has $t1 'completed') {'[PASS]'} else {'[FAIL]'})))

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

if ($script:fail -gt 0) { exit 1 }
exit 0
