# ---------------------------------------------------------------------------
#  L3 · verify.ps1 —— v0 的端到端验证（不需要真 API Key）
#
#  验证思路：**协议层的正确性不依赖密钥。**
#  起一个假服务端，故意把响应切碎、塞 null、同帧带 finish_reason，
#  然后看客户端能不能全部扛住。
#
#  四个用例：
#    C1  英文提问       → 正常流式；usage 事件恰好 1 次
#    C2  中文提问       → 验证 argv 编码（不处理的话这里会 BAD JSON）
#    C3  被截断的回答   → finish_reason=length 必须被显式报出来
#    C4  空回答         → 200 但无内容，必须被判为失败而不是成功
#
#  用法：
#      .\verify.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe  = Join-Path $Here "build\agent.exe"
$Mock = Join-Path $Here "mock_server.py"
$Port = 8788
$Log  = Join-Path $Here "mock_requests.log"
$Py   = "C:\Program Files\Python313\python.exe"
if (-not (Test-Path $Py)) { $Py = (Get-Command python.exe).Source }

$script:pass = 0
$script:fail = 0

function Check {
    param($ok, $name, $detail)
    if ($ok) { $script:pass++ } else { $script:fail++ }
    $tag = '[FAIL]'
    if ($ok) { $tag = '[PASS]' }
    $suffix = ''
    if ($detail) { $suffix = "  —— $detail" }
    Write-Output ("  {0} {1}{2}" -f $tag, $name, $suffix)
}

function RunAgent {
    param($task, $outFile)
    & $Exe $task *>&1 | Out-File -FilePath $outFile -Encoding UTF8
    return $LASTEXITCODE
}

if (-not (Test-Path $Exe)) {
    Write-Output "FATAL: 找不到 $Exe —— 先跑 .\build.ps1"
    exit 1
}

Write-Output "================================================"
Write-Output "  L3 验证：v0 能对话"
Write-Output "================================================"

Write-Output ""
Write-Output "[准备] 启动假服务端 127.0.0.1:$Port"
$job = Start-Job -ScriptBlock {
    param($py, $script, $port, $logfile)
    & $py $script $port $logfile
} -ArgumentList $Py, $Mock, $Port, $Log
Start-Sleep -Seconds 3

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"

# ---- C1 英文提问 ----------------------------------------------------------
Write-Output ""
Write-Output "[用例] C1 英文提问（正常流式）"
$f1  = Join-Path $Here "_c1.txt"
$c1  = RunAgent "what is SSE in one sentence" $f1
$t1  = Get-Content $f1 -Raw -Encoding UTF8
Check ($c1 -eq 0)             "C1 退出码为 0"                     "exit=$c1"
Check ($t1 -match "假服务端")  "C1 收到完整正文（跨块累积正确）"
Check ($t1 -match "usageEvents=1") "C1 usage 事件恰好 1 次"        "带 null 的帧未被误当成用量"
Check ($t1 -match "finish=0")  "C1 finish_reason=stop（0）"

# ---- C2 中文提问（argv 编码）-----------------------------------------------
Write-Output ""
Write-Output "[用例] C2 中文提问（argv 编码）"
$f2  = Join-Path $Here "_c2.txt"
$c2  = RunAgent "用一句话解释什么是流式输出" $f2
$t2  = Get-Content $f2 -Raw -Encoding UTF8
Check ($c2 -eq 0)             "C2 退出码为 0"                     "exit=$c2"
Check ($t2 -match "流式输出")  "C2 中文任务原样到达服务端并被回显"

# ---- C3 被截断 ------------------------------------------------------------
Write-Output ""
Write-Output "[用例] C3 被截断的回答"
$f3  = Join-Path $Here "_c3.txt"
$c3  = RunAgent "请给我一个会被截断的回答" $f3
$t3  = Get-Content $f3 -Raw -Encoding UTF8
Check ($t3 -match "被截断")    "C3 length 被显式标记为截断"        "没被当成正常完成"
Check ($t3 -match "finish=2")  "C3 finish_reason=length（2）"

# ---- C4 空回答 ------------------------------------------------------------
Write-Output ""
Write-Output "[用例] C4 空回答（200 但无内容）"
$f4  = Join-Path $Here "_c4.txt"
$c4  = RunAgent "给我一个空回答" $f4
$t4  = Get-Content $f4 -Raw -Encoding UTF8
Check ($c4 -eq 2)                  "C4 退出码为 2（判为失败）"    "exit=$c4"
Check ($t4 -match "没有收到任何内容") "C4 空响应被显式报出"          "不假装成功"

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# ---- 服务端侧断言 ---------------------------------------------------------
Write-Output ""
Write-Output "[服务端收到的请求]"
$mockLog = @()
if (Test-Path $Log) { $mockLog = @(Get-Content $Log -Encoding UTF8) }
$mockLog | Write-Output

$joined = $mockLog -join "`n"
$reqLines = @($mockLog | Where-Object { $_ -match '^\[REQ\] model=' })
Check ($joined -notmatch "BAD JSON")  "服务端未收到坏 JSON"          "argv 编码正确（无 type_error.316）"
Check ($joined -match "model=mock-1") "请求里的 model 是真实模型名"   "没有退化成厂商名 openai"
Check ($joined -match "stream=True")  "请求走的是流式"
Check ($joined -match "tools=0")      "v0 未下发工具（符合预期）"
Check ($reqLines.Count -ge 4)         "四个用例都打到了服务端"        "实际 $($reqLines.Count) 次"

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

foreach ($f in @($f1, $f2, $f3, $f4)) {
    Write-Output ""
    Write-Output "----- $(Split-Path -Leaf $f) -----"
    Get-Content $f -Encoding UTF8 | Write-Output
}

if ($script:fail -gt 0) { exit 1 }
exit 0
