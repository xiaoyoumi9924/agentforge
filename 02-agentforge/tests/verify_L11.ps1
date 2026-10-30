# ---------------------------------------------------------------------------
#  L11 · verify_L11.ps1 —— Qt GUI 验证
#
#  ① 依赖检查：AgentForgeGUI.exe + Qt6Widgets.dll + platforms 插件
#  ② 无头冒烟：QT_QPA_PLATFORM=offscreen + AF_GUI_SMOKE=1 → "GUI SMOKE OK"
#  ③ CLI 回归：--selftest 仍全部 PASS（GUI 与 CLI 共用核心，防回归）
#
#  用法：
#      .\tests\verify_L11.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Proj = Split-Path -Parent $Here
$Gui  = Join-Path $Proj "build_gui"
$Exe  = Join-Path $Gui "AgentForgeGUI.exe"
$Cli  = Join-Path $Proj "build\agent.exe"

function Has { param($text, $needle) return (($null -ne $text) -and ($text -as [string]).Contains($needle)) }

$script:pass = 0
$script:fail = 0
function Check { param($ok, $name, $detail = '')
    if ($ok) { $script:pass++ } else { $script:fail++ }
    Write-Output ("  {0} {1}{2}" -f ($(if ($ok) {'[PASS]'} else {'[FAIL]'})), $name, $(if ($detail) {"  —— $detail"} else {''}))
}

Write-Output "================================================"
Write-Output "  L11 验证：Qt GUI"
Write-Output "================================================"

# ---- ① 依赖检查 ----
Write-Output ""
Write-Output "[GUI] 依赖检查"
Check (Test-Path $Exe) "AgentForgeGUI.exe 存在"
Check (Test-Path (Join-Path $Gui "Qt6Widgets.dll")) "Qt6Widgets.dll 已部署"
Check (Test-Path (Join-Path $Gui "platforms\qwindows.dll")) "qwindows 平台插件已部署"
Check (Test-Path (Join-Path $Gui "platforms\qoffscreen.dll")) "qoffscreen 平台插件已部署"

if (-not (Test-Path $Exe)) { Write-Output "FATAL: 先跑 .\build_gui.ps1"; exit 1 }

# ---- ② 无头冒烟 ----
Write-Output ""
Write-Output "[GUI] 无头冒烟"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:AF_GUI_SMOKE = '1'
$outFile = Join-Path $Here "_l11_out.txt"
$errFile = Join-Path $Here "_l11_err.txt"
$p = Start-Process -FilePath $Exe -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
$exited = $p.WaitForExit(8000)
$code = $p.ExitCode
$out = Get-Content $outFile -Raw -ErrorAction SilentlyContinue

Check $exited "进程按时退出" "exit=$code"
Check ($code -eq 0) "退出码为 0" "exit=$code"
Check (Has $out 'GUI SMOKE OK') "输出 GUI SMOKE OK" ($out -as [string]).Trim()

$env:QT_QPA_PLATFORM = $null
$env:AF_GUI_SMOKE = $null

# ---- ③ CLI 回归 ----
Write-Output ""
Write-Output "[回归] CLI --selftest"
$st = & $Cli --selftest *>&1 | Out-String
$stPass = ([regex]::Matches($st, '\[PASS\]')).Count
$stFail = ([regex]::Matches($st, '\[FAIL\]')).Count
Check (($stFail -eq 0) -and ($stPass -ge 35)) "CLI selftest：$stPass PASS / $stFail FAIL"

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

if ($script:fail -gt 0) { exit 1 }
exit 0
