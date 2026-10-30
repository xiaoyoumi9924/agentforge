# ---------------------------------------------------------------------------
#  L9-L10 · verify_L9_L10.ps1 —— 配置 + DPAPI 密钥 + SQLite 会话持久化
#
#  ① --selftest（42 项，含 T10 持久化 / T11 DPAPI / T12 配置）
#  ② --config：有效配置输出（密钥打码、sandbox/workspace 字段）
#  ③ --set-key / --clear-key：DPAPI 加密落盘（文件无明文）
#  ④ --sessions：列出会话；--resume 恢复历史
#  ⑤ Mock 冒烟：检索+会话持久化后循环仍收敛，消息被保存
#
#  用法：
#      .\tests\verify_L9_L10.ps1
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
$Session = "verify_l10"

function Has { param($text, $needle) return (($null -ne $text) -and ($text -as [string]).Contains($needle)) }

$script:pass = 0
$script:fail = 0
function Check { param($ok, $name, $detail = '')
    if ($ok) { $script:pass++ } else { $script:fail++ }
    Write-Output ("  {0} {1}{2}" -f ($(if ($ok) {'[PASS]'} else {'[FAIL]'})), $name, $(if ($detail) {"  —— $detail"} else {''}))
}

if (-not (Test-Path $Exe)) { Write-Output "FATAL: 找不到 $Exe —— 先跑 .\build.ps1"; exit 1 }

Write-Output "================================================"
Write-Output "  L9-L10 验证：配置 / DPAPI 密钥 / SQLite 持久化"
Write-Output "================================================"

# ---- ① selftest ----
Write-Output ""
Write-Output "[自检] --selftest"
$st = & $Exe --selftest *>&1 | Out-String
$stPass = ([regex]::Matches($st, '\[PASS\]')).Count
$stFail = ([regex]::Matches($st, '\[FAIL\]')).Count
Check (($stFail -eq 0) -and ($stPass -ge 35)) "自检：$stPass PASS / $stFail FAIL（≥35 项）"

# ---- ② --config ----
Write-Output ""
Write-Output "[配置] --config"
$cfg = & $Exe --config *>&1 | Out-String
Check (Has $cfg '"provider"') "输出 provider 段"
Check (Has $cfg '"sandbox"') "输出 sandbox 段"
Check (Has $cfg 'contextWindow') "输出 budget 段"
Check ((Has $cfg '未设置') -or (Has $cfg '已打码') -or (Has $cfg '已设置')) "密钥不以明文出现在配置输出（打码/未设置）"
Check (-not (Has $cfg 'sk-')) "配置输出里没有明文密钥痕迹"

# ---- ③ DPAPI 密钥 ----
Write-Output ""
Write-Output "[密钥] --set-key / --clear-key"
$secretPath = Join-Path $Here ".af_secret"
Remove-Item $secretPath -ErrorAction SilentlyContinue
$setOut = & $Exe --set-key "sk-verify-abc-123" *>&1 | Out-String
Check (Has $setOut 'DPAPI') "加密保存提示"
Check (Test-Path $secretPath) "密钥文件已生成"
if (Test-Path $secretPath) {
    $sec = Get-Content $secretPath -Raw
    Check (-not $sec.Contains('sk-verify-abc-123')) "密钥文件没有明文"
    $cfg2 = & $Exe --config *>&1 | Out-String
    Check (Has $cfg2 '已设置') "配置能读出已设置状态"
}
& $Exe --clear-key *>&1 | Out-Null
Check (-not (Test-Path $secretPath)) "clear-key 删除了密钥文件"

# ---- ④ 会话持久化 + 恢复 ----
Write-Output ""
Write-Output "[会话] 首次运行 + --sessions + --resume"
$job = Start-Job -ScriptBlock {
    param($py, $script, $port, $logfile)
    & $py $script $port $logfile
} -ArgumentList $Py, (Join-Path $Here "mock_server.py"), $Port, $Log
Start-Sleep -Seconds 3

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"
$env:AF_PLUGIN       = ""
$env:AF_SESSION      = $Session
Remove-Item (Join-Path $Here ".af-sessions") -Recurse -Force -ErrorAction SilentlyContinue

$f1 = Join-Path $Here "_l9_run1.txt"
& $Exe "请读取 mock_target.txt 并告诉我里面有哪些函数" *>&1 | Out-File -FilePath $f1 -Encoding UTF8
$code1 = $LASTEXITCODE
$t1 = Get-Content $f1 -Raw -Encoding UTF8
Check ($code1 -eq 0) "第一次运行 Completed" "exit=$code1"
Check (Has $t1 '已保存 4 条消息到会话库') "本轮消息被保存（4 条）"

$sessions = & $Exe --sessions *>&1 | Out-String
Check (Has $sessions $Session) "--sessions 列出了会话"

$f2 = Join-Path $Here "_l9_run2.txt"
& $Exe --resume $Session "请读取 mock_target.txt 并告诉我里面有哪些函数" *>&1 | Out-File -FilePath $f2 -Encoding UTF8
$code2 = $LASTEXITCODE
$t2 = Get-Content $f2 -Raw -Encoding UTF8
Check ($code2 -eq 0) "恢复运行 Completed" "exit=$code2"
Check (Has $t2 '恢复') "第二次运行恢复了历史（[恢复] 会话）"

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue

# ---- ⑤ 清理 ----
$env:AF_SESSION = $null
$env:OPENAI_BASE_URL = $null
$env:OPENAI_API_KEY = $null
$env:AF_MODEL = $null
$env:AF_PLUGIN = $null

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

if ($script:fail -gt 0) { exit 1 }
exit 0

