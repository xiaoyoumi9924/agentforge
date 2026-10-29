# ---------------------------------------------------------------------------
#  L5 · verify.ps1 —— v2/v3「能改 + 能自主循环」的端到端验证（不需要 API Key）
#
#  这一讲要证明的是"环成立了"，所以验证的重点从"协议形状对不对"
#  转成"**一串动作有没有正确串起来**"：
#
#      读 → 改 → 跑命令验证 → 收工
#
#  以及两件只有环才会暴露的事：
#      · 观察者（LoopHooks）在每一步都拿到了事件
#      · **没有闸门时它会一直转**（C5 故意跑到上限）—— 这是 L6 的引子
#
#  六个用例：
#    C1  多步自主循环   → Read → Edit → Bash（真的改了文件、真的跑了命令）
#    C2  观察者        → LoopHooks 的四个回调都被触发
#    C3  消息合法性自检  → agent.exe --selftest（G8 的三类非法历史 + 组装三规矩）
#    C4  危险命令被拒   → Bash(rm -rf /) 被权限门 Deny
#    C5  ★ 撞上限      → 重复调用会一直转到 maxSteps，如实返回 step_limit
#    C6  服务端侧断言   → 工具表、历史回传、tool_call_id 配对
#
#  用法：
#      .\verify.ps1            # 需要先 .\build.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe  = Join-Path $Here "build\agent.exe"
$Mock = Join-Path $Here "mock_server.py"
$Port = 8790
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

# 字面量包含判断（不用 -match：方括号会被当成正则字符类）
function Has { param($text, $needle) return (($null -ne $text) -and $text.Contains($needle)) }

function RunAgent {
    param($task, $outFile, $answer = $null)
    if ($null -ne $answer) {
        $answer | & $Exe $task *>&1 | Out-File -FilePath $outFile -Encoding UTF8
    } else {
        & $Exe $task *>&1 | Out-File -FilePath $outFile -Encoding UTF8
    }
    return $LASTEXITCODE
}

if (-not (Test-Path $Exe)) {
    Write-Output "FATAL: 找不到 $Exe —— 先跑 .\build.ps1"
    exit 1
}

Set-Location $Here

# ---- 每次验证前把"会被改的文件"重置成原始内容 -----------------------------
$Pristine = Join-Path $Here "mock_target.txt"
$Work     = Join-Path $Here "work_target.txt"
Copy-Item -LiteralPath $Pristine -Destination $Work -Force

Write-Output "================================================"
Write-Output "  L5 验证：v2 能改 / v3 能自主循环"
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
$env:AF_PLUGIN       = ""
$env:AF_MAX_STEPS    = ""

# ---- C1 多步自主循环 -------------------------------------------------------
Write-Output ""
Write-Output "[用例] C1 多步自主循环：读 → 改 → 跑命令验证 → 收工"
$f1 = Join-Path $Here "_c1.txt"
$c1 = RunAgent "把 work_target.txt 里的 add 改名为 sum，然后跑测试确认" $f1
$t1 = Get-Content $f1 -Raw -Encoding UTF8
Check ($c1 -eq 0)                          "C1 退出码为 0"                    "exit=$c1"
Check (Has $t1 'completed')                "C1 收敛为 completed"
Check (Has $t1 '[模型请求工具] Read')       "C1 第 1 步：模型自己决定读文件"
Check (Has $t1 '[模型请求工具] Edit')       "C1 第 2 步：模型自己决定改文件"
Check (Has $t1 '[模型请求工具] Bash')       "C1 第 3 步：模型自己决定跑命令验证"
Check (Has $t1 'toolCalls=3')              "C1 三步工具调用都落地了"
Check (Has $t1 '第 3 步')                   "C1 环真的转了 3 圈"

$work = Get-Content $Work -Raw -Encoding UTF8
Check (Has $work 'int sum(int a, int b)')  "C1 ★ 文件里真的出现了 int sum("
Check ((-not (Has $work 'int add(int a, int b)'))) "C1 ★ 原来的 int add( 已被替换"
Check (Has $t1 '已把 add 改名为 sum')        "C1 模型看到了验证结果后宣布完成"

# ---- C2 观察者 -------------------------------------------------------------
Write-Output ""
Write-Output "[用例] C2 观察者：LoopHooks 的事件都抛出来了"
Check (Has $t1 '[事件] onToolStart')        "C2 onToolStart 被触发"
Check (Has $t1 '[事件] onToolDone')         "C2 onToolDone 被触发"
Check (Has $t1 '[事件] onNotice')           "C2 onNotice 被触发（进度通知）"
Check (Has $t1 'chunks=')                   "C2 onChunk 被触发（流式增量计数）"

# ---- C3 消息合法性自检（G8）------------------------------------------------
Write-Output ""
Write-Output "[用例] C3 消息合法性自检（--selftest，单元级）"
$f3 = Join-Path $Here "_c3.txt"
& $Exe --selftest *>&1 | Out-File -FilePath $f3 -Encoding UTF8
$c3 = $LASTEXITCODE
$t3 = Get-Content $f3 -Raw -Encoding UTF8
Check ($c3 -eq 0)                            "C3 自检退出码为 0"                "exit=$c3"
Check (Has $t3 '6 通过 / 0 失败')             "C3 六条自检全 PASS"
Check ((-not (Has $t3 'FAIL')))               "C3 自检输出里没有 FAIL"
Check (Has $t3 'I-2 并行 tool_call 的两条结果都保留') "C3 ★ 并行工具调用的结果没有被守卫吃掉"

# ---- C4 危险命令被拒 -------------------------------------------------------
Write-Output ""
Write-Output "[用例] C4 危险命令被权限门拒绝"
$f4 = Join-Path $Here "_c4.txt"
$c4 = RunAgent "执行一个危险的删库命令" $f4
$t4 = Get-Content $f4 -Raw -Encoding UTF8
Check (Has $t4 '[模型请求工具] Bash')        "C4 模型确实请求了 Bash"
Check (Has $t4 '[工具失败]')                 "C4 被拒是 observation，不是异常"
Check (Has $t4 '被策略拒绝')                 "C4 明确说明是策略拒绝（不是命令报错）"
Check ($c4 -eq 0)                            "C4 拒绝后循环仍能正常收尾"          "exit=$c4"

# ---- C5 ★ 撞上限（L5 暴露的问题）-------------------------------------------
Write-Output ""
Write-Output "[用例] C5 重复调用：没有闸门时会一直转到上限"
$env:AF_MAX_STEPS = "6"
$f5 = Join-Path $Here "_c5.txt"
$c5 = RunAgent "请重复调用同一个工具直到我说停" $f5
$t5 = Get-Content $f5 -Raw -Encoding UTF8
Check ($c5 -eq 2)                            "C5 退出码为 2（未收敛）"           "exit=$c5"
Check (Has $t5 'step_limit')                 "C5 ★ 如实报告 step_limit，没有假装完成"
Check (Has $t5 'toolCalls=6')                "C5 ★ 它真的转满了 6 步（每步都发一次请求）"
Check (Has $t5 '第 6 步')                     "C5 最后一步的进度通知也到了"
$env:AF_MAX_STEPS = ""

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# ---- C6 服务端侧断言 -------------------------------------------------------
Write-Output ""
Write-Output "[服务端收到的请求]"
$mockLog = @()
if (Test-Path $Log) { $mockLog = @(Get-Content $Log -Encoding UTF8) }
$mockLog | Write-Output
$joined = $mockLog -join "`n"

Check ((-not (Has $joined 'BAD JSON')))                    "C6 服务端未收到坏 JSON"
Check (Has $joined 'model=mock-1')                         "C6 model 是真实模型名（没被 provider.id() 覆盖）"
Check (Has $joined 'stream=True')                          "C6 请求走的是流式"
Check (Has $joined "tools=4 ['Read', 'Write', 'Edit', 'Bash']") "C6 四个原语按注册顺序下发（顺序即缓存）"
Check (Has $joined 'tool_calls=["Read"]')                  "C6 assistant.tool_calls 被存回历史并回传"
Check (Has $joined 'tool_call_id=call_read')                "C6 tool 结果带 tool_call_id 配对回填"
Check (Has $joined 'msgs=4')                                "C6 第 2 圈请求带着 system+user+assistant+tool"

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

foreach ($f in @($f1, $f3, $f4, $f5)) {
    Write-Output ""
    Write-Output "----- $(Split-Path -Leaf $f) -----"
    Get-Content $f -Encoding UTF8 | Write-Output
}

if ($script:fail -gt 0) { exit 1 }
exit 0
