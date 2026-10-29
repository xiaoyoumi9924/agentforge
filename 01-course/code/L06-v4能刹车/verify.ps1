# ---------------------------------------------------------------------------
#  L6 · verify.ps1 —— v4「能刹车」的端到端验证（不需要 API Key）
#
#  本讲的验证目标与 L5 不同：
#    L5 证明"环转起来了"；L6 要**逐道闸门**证明"每种死法都被拦住了"。
#
#  所以用例是按**闸门**组织的，不是按功能：
#
#    C1  回归：正经干活仍然能跑完（改名 → 验证 → 收工）
#    C2  观察者：LoopHooks 仍然在抛事件
#    C3  --selftest：消息合法性（G8）+ 组装三规矩
#    C4  权限门：危险命令被 Deny（且模型知道要换方法）
#    C5  ★ 闸门 5：重复调用被**拦下并警告**（L5 的 C5 期望值在本讲改变！）
#    C6  服务端侧断言：协议形状仍然正确
#    C7  闸门 1：步数上限（参数每次不同，绕开闸门 5 才能测到它）
#    C8  闸门 4：连续错误上限
#    C9  闸门 6：取消（Ctrl-C 的自动化替身，走同一个 stop_source）
#    C10 沙箱：超时强杀（含子进程）
#    C11 沙箱：禁网策略（权限门放行、沙箱拦住 —— 纵深防御）
#    C12 沙箱自检：把"没做到的部分"也打印出来（不假装跑通）
#    C13 闸门 3：墙钟上限
#    C14 闸门 2：token 预算上限
#    C15 ★ 零改动：本讲声明的"没动的文件"真的一个字节都没动（哈希比对）
#
#  用法：
#      .\verify.ps1            # 需要先 .\build.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe  = Join-Path $Here "build\agent.exe"
$Mock = Join-Path $Here "mock_server.py"
$Port = 8791
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

# 从汇总行里抠一个整数
function NumOf { param($text, $key)
    if ($text -match ("$key=(\d+)")) { return [int]$Matches[1] }
    return -1
}

$AfKeys = @("AF_MAX_STEPS","AF_MAX_TOKENS","AF_MAX_ERRORS","AF_MAX_REPEATS",
            "AF_MAX_WALL_MS","AF_CMD_TIMEOUT_MS","AF_CANCEL_AFTER_MS",
            "AF_SANDBOX","AF_SANDBOX_NET","AF_PLUGIN")
function ClearAfEnv {
    foreach ($k in $AfKeys) { Set-Item -Path "Env:$k" -Value "" -ErrorAction SilentlyContinue }
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

Set-Location $Here

# ---- 每次验证前把"会被改的文件"重置成原始内容 -----------------------------
$Pristine = Join-Path $Here "mock_target.txt"
$Work     = Join-Path $Here "work_target.txt"
Copy-Item -LiteralPath $Pristine -Destination $Work -Force

Write-Output "================================================"
Write-Output "  L6 验证：v4 能刹车（六道闸门 + 沙箱）"
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
ClearAfEnv

# ===========================================================================
#  C1  回归：正经干活仍然能跑完
# ===========================================================================
Write-Output ""
Write-Output "[用例] C1 回归：读 → 改 → 跑命令验证 → 收工（闸门不该误伤正常流程）"
$f1 = Join-Path $Here "_c1.txt"
$c1 = RunAgent "把 work_target.txt 里的 add 改名为 sum，然后跑测试确认" $f1
$t1 = Get-Content $f1 -Raw -Encoding UTF8
Check ($c1 -eq 0)                          "C1 退出码为 0"                    "exit=$c1"
Check (Has $t1 'completed')                "C1 收敛为 completed"
Check (Has $t1 '[模型请求工具] Read')       "C1 第 1 步：模型自己决定读文件"
Check (Has $t1 '[模型请求工具] Edit')       "C1 第 2 步：模型自己决定改文件"
Check (Has $t1 '[模型请求工具] Bash')       "C1 第 3 步：模型自己决定跑命令验证"
Check ((NumOf $t1 'toolCalls') -eq 3)      "C1 三步工具调用都落地了"
$work = Get-Content $Work -Raw -Encoding UTF8
Check (Has $work 'int sum(int a, int b)')  "C1 ★ 文件里真的出现了 int sum("
Check ((-not (Has $work 'int add(int a, int b)'))) "C1 ★ 原来的 int add( 已被替换"

# ===========================================================================
#  C2  观察者
# ===========================================================================
Write-Output ""
Write-Output "[用例] C2 观察者：LoopHooks 的事件都抛出来了"
Check (Has $t1 '[事件] onToolStart')        "C2 onToolStart 被触发"
Check (Has $t1 '[事件] onToolDone')         "C2 onToolDone 被触发"
Check (Has $t1 '[事件] onNotice')           "C2 onNotice 被触发（进度通知）"
Check (Has $t1 'chunks=')                   "C2 onChunk 被触发（流式增量计数）"

# ===========================================================================
#  C3  消息合法性自检（G8）
# ===========================================================================
Write-Output ""
Write-Output "[用例] C3 消息合法性自检（--selftest，单元级）"
$f3 = Join-Path $Here "_c3.txt"
& $Exe --selftest *>&1 | Out-File -FilePath $f3 -Encoding UTF8
$c3 = $LASTEXITCODE
$t3 = Get-Content $f3 -Raw -Encoding UTF8
Check ($c3 -eq 0)                            "C3 自检退出码为 0"                "exit=$c3"
Check (Has $t3 '6 通过 / 0 失败')             "C3 六条自检全 PASS"
Check ((-not (Has $t3 'FAIL')))               "C3 自检输出里没有 FAIL"

# ===========================================================================
#  C4  权限门：危险命令被 Deny
# ===========================================================================
Write-Output ""
Write-Output "[用例] C4 权限门：危险命令被拒绝（deny 永远赢）"
$f4 = Join-Path $Here "_c4.txt"
$c4 = RunAgent "执行一个危险的删库命令" $f4
$t4 = Get-Content $f4 -Raw -Encoding UTF8
Check (Has $t4 '[模型请求工具] Bash')        "C4 模型确实请求了 Bash"
Check (Has $t4 '[工具失败]')                 "C4 被拒是 observation，不是异常"
Check (Has $t4 '被策略拒绝')                 "C4 明确说明是策略拒绝（不是命令报错）"
Check ($c4 -eq 0)                            "C4 拒绝后循环仍能正常收尾"          "exit=$c4"

# ===========================================================================
#  C5  ★ 闸门 5：重复调用被拦下并警告
#      —— L5 的期望值是"跑到 step_limit"，本讲必须改成"被闸门 5 拦下"
# ===========================================================================
Write-Output ""
Write-Output "[用例] C5 ★ 闸门 5：重复调用被拦下（L5 的期望值在本讲改变）"
$env:AF_MAX_STEPS = "50"        # 故意给一个很大的步数上限
$env:AF_MAX_REPEATS = "3"
$f5 = Join-Path $Here "_c5.txt"
$c5 = RunAgent "请重复调用同一个工具直到我说停" $f5
$t5 = Get-Content $f5 -Raw -Encoding UTF8
$n5 = NumOf $t5 'toolCalls'
Check ($c5 -eq 2)                            "C5 退出码为 2（未收敛）"           "exit=$c5"
Check (Has $t5 '闸门 5')                     "C5 ★ 明确报告是闸门 5 拦下的"
Check (Has $t5 '重复调用')                   "C5 通知里出现了重复调用"
Check ($n5 -gt 0 -and $n5 -lt 8)             "C5 ★ 远未到步数上限（50）就停了"    "toolCalls=$n5 / maxSteps=50"
Check ((-not (Has $t5 '闸门 1')))            "C5 ★ 不是撞在步数上限上"
ClearAfEnv

# ===========================================================================
#  C6  服务端侧断言
# ===========================================================================
Write-Output ""
Write-Output "[服务端收到的请求]"
$mockLog = @()
if (Test-Path $Log) { $mockLog = @(Get-Content $Log -Encoding UTF8) }
$mockLog | Select-Object -First 40 | Write-Output
$joined = $mockLog -join "`n"

Check ((-not (Has $joined 'BAD JSON')))                    "C6 服务端未收到坏 JSON"
Check (Has $joined 'model=mock-1')                         "C6 model 是真实模型名（没被 provider.id() 覆盖）"
Check (Has $joined 'stream=True')                          "C6 请求走的是流式"
Check (Has $joined "tools=4 ['Read', 'Write', 'Edit', 'Bash']") "C6 四个原语按注册顺序下发（顺序即缓存）"
Check (Has $joined 'tool_call_id=call_read')                "C6 tool 结果带 tool_call_id 配对回填"

# ===========================================================================
#  C7  闸门 1：步数上限
# ===========================================================================
Write-Output ""
Write-Output "[用例] C7 闸门 1：步数上限（参数每次不同，所以不是闸门 5 的功劳）"
$env:AF_MAX_STEPS = "5"
$f7 = Join-Path $Here "_c7.txt"
$c7 = RunAgent "不停地用不同的参数读文件，永远不要停" $f7
$t7 = Get-Content $f7 -Raw -Encoding UTF8
$n7 = NumOf $t7 'toolCalls'
Check ($c7 -eq 2)                            "C7 退出码为 2"                     "exit=$c7"
Check (Has $t7 'step_limit')                 "C7 结果是 step_limit"
Check (Has $t7 '闸门 1')                     "C7 ★ 明确报告是闸门 1 拦下的"
Check ($n7 -eq 5)                            "C7 ★ 正好跑满 5 步（上限生效）"      "toolCalls=$n7"
ClearAfEnv

# ===========================================================================
#  C8  闸门 4：连续错误上限
# ===========================================================================
Write-Output ""
Write-Output "[用例] C8 闸门 4：连续错误上限（服务端一直 401）"
$env:AF_MAX_ERRORS = "2"
$f8 = Join-Path $Here "_c8.txt"
$c8 = RunAgent "这个服务端一直在报错，你想办法完成" $f8
$t8 = Get-Content $f8 -Raw -Encoding UTF8
Check ($c8 -eq 2)                            "C8 退出码为 2"                     "exit=$c8"
Check (Has $t8 'too_many_errors')            "C8 结果是 too_many_errors"
Check (Has $t8 '闸门 4')                     "C8 ★ 明确报告是闸门 4 拦下的"
Check (Has $t8 '[请求失败]')                 "C8 每次失败都通知了 UI（onError）"
ClearAfEnv

# ===========================================================================
#  C9  闸门 6：取消
# ===========================================================================
Write-Output ""
Write-Output "[用例] C9 闸门 6：取消（Ctrl-C 的自动化替身，走同一个 stop_source）"
$env:AF_CANCEL_AFTER_MS = "1500"
$f9 = Join-Path $Here "_c9.txt"
$sw9 = Get-Date
$c9 = RunAgent "写一篇很长的回答，慢慢写，不要停" $f9
$el9 = ((Get-Date) - $sw9).TotalSeconds
$t9 = Get-Content $f9 -Raw -Encoding UTF8
Check ($c9 -eq 2)                            "C9 退出码为 2"                     "exit=$c9"
Check (Has $t9 'user_aborted')               "C9 结果是 user_aborted"
Check (Has $t9 '用户中断')                   "C9 ★ 明确报告是用户中断"
Check ($el9 -lt 20)                          "C9 ★ 真的提前停了，没等模型吐完"    ("耗时 {0:N1}s" -f $el9)
ClearAfEnv

# ===========================================================================
#  C10  沙箱：超时强杀
# ===========================================================================
Write-Output ""
Write-Output "[用例] C10 沙箱：超时强杀（命令本身要跑 30 秒）"
$env:AF_CMD_TIMEOUT_MS = "1500"
$f10 = Join-Path $Here "_c10.txt"
$sw10 = Get-Date
$c10 = RunAgent "运行一条要跑很久的命令" $f10
$el10 = ((Get-Date) - $sw10).TotalSeconds
$t10 = Get-Content $f10 -Raw -Encoding UTF8
Check ($c10 -eq 0)                           "C10 循环正常收尾"                  "exit=$c10"
Check (Has $t10 '被沙箱拦截')                 "C10 ★ 报告被沙箱拦截"
Check (Has $t10 '超时')                       "C10 原因是超时"
Check (Has $t10 '强杀')                       "C10 ★ 明确说强杀（含子进程）"
Check ($el10 -lt 20)                         "C10 ★ 没等满 30 秒就杀了"           ("耗时 {0:N1}s" -f $el10)
ClearAfEnv

# ===========================================================================
#  C11  沙箱：禁网（权限门放行、沙箱拦住 = 纵深防御）
# ===========================================================================
Write-Output ""
Write-Output "[用例] C11 沙箱：禁网策略（权限门放行 → 沙箱拦住）"
$f11 = Join-Path $Here "_c11.txt"
$c11 = RunAgent "帮我联网下载一个文件" $f11
$t11 = Get-Content $f11 -Raw -Encoding UTF8
Check ($c11 -eq 0)                           "C11 循环正常收尾"                  "exit=$c11"
Check (Has $t11 '[模型请求工具] Bash')        "C11 权限门放行了这次 Bash"
Check (Has $t11 '被沙箱拦截')                 "C11 ★ 沙箱把它拦住了（纵深防御）"
Check (Has $t11 '网络已禁用')                 "C11 原因是网络已禁用"
Check (Has $t11 '重试不会改变结果')           "C11 告诉模型「重试无用」（防反复重试）"

# ===========================================================================
#  C12  沙箱自检：把"没做到的部分"也打印出来
# ===========================================================================
Write-Output ""
Write-Output "[用例] C12 沙箱自检：能力边界必须诚实（不假装跑通）"
$f12 = Join-Path $Here "_c12.txt"
& $Exe --sandbox-info *>&1 | Out-File -FilePath $f12 -Encoding UTF8
$c12 = $LASTEXITCODE
$t12 = Get-Content $f12 -Raw -Encoding UTF8
Check ($c12 -eq 0)                           "C12 自检命令退出码为 0"            "exit=$c12"
Check (Has $t12 'JobObject')                 "C12 报告了 Job Object"
Check (Has $t12 '[已强制')                    "C12 ★ 列出了「内核强制」的能力"
Check (Has $t12 '[未实现]')                   "C12 ★ 也列出了「未实现」的部分（诚实边界）"
ClearAfEnv

# ===========================================================================
#  C13  闸门 3：墙钟上限
# ===========================================================================
Write-Output ""
Write-Output "[用例] C13 闸门 3：墙钟上限"
$env:AF_MAX_WALL_MS = "1500"
$f13 = Join-Path $Here "_c13.txt"
$c13 = RunAgent "用慢速的方式读文件" $f13
$t13 = Get-Content $f13 -Raw -Encoding UTF8
Check ($c13 -eq 2)                           "C13 退出码为 2"                    "exit=$c13"
Check (Has $t13 'wall_clock_exceeded')       "C13 结果是 wall_clock_exceeded"
Check (Has $t13 '闸门 3')                     "C13 ★ 明确报告是闸门 3 拦下的"
ClearAfEnv

# ===========================================================================
#  C14  闸门 2：token 预算上限
# ===========================================================================
Write-Output ""
Write-Output "[用例] C14 闸门 2：token 预算上限"
$env:AF_MAX_STEPS = "50"
$env:AF_MAX_TOKENS = "1"
$f14 = Join-Path $Here "_c14.txt"
$c14 = RunAgent "不停地用不同的参数读文件，永远不要停" $f14
$t14 = Get-Content $f14 -Raw -Encoding UTF8
Check ($c14 -eq 2)                           "C14 退出码为 2"                    "exit=$c14"
Check (Has $t14 'budget_exceeded')           "C14 结果是 budget_exceeded"
Check (Has $t14 '闸门 2')                     "C14 ★ 明确报告是闸门 2 拦下的"
ClearAfEnv

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# ===========================================================================
#  C15  ★ 零改动：本讲声明的"没动的文件"真的一个字节都没动
# ===========================================================================
Write-Output ""
Write-Output "[用例] C15 ★ 零改动：逐文件哈希比对（L5 的版本 vs L6 的版本）"
$L5 = Join-Path (Split-Path (Split-Path $Here -Parent) -Parent) "L5-v2v3能改能自主循环\code"
$unchanged = @(
    "types.hpp","agent_loop.hpp","model_provider.hpp","http_transport.hpp",
    "sse_parser.hpp","sse_parser.cpp","http_winhttp.cpp","sandbox.hpp",
    "tool_registry.hpp","tool_registry.cpp","plugin.hpp","plugin.cpp",
    "providers.cpp","context_budget.hpp","context_budget.cpp"
)
$diff = @()
foreach ($fn in $unchanged) {
    $a = Join-Path $L5 $fn
    $b = Join-Path $Here $fn
    if (-not (Test-Path $a)) { $diff += "$fn(L5缺)"; continue }
    $ha = (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash
    $hb = (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
    if ($ha -ne $hb) { $diff += $fn }
}
Check ($diff.Count -eq 0) "C15 ★ $($unchanged.Count) 个声明的「零改动」文件真的零改动" ("差异: " + ($diff -join ", "))

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

foreach ($f in @($f1,$f3,$f4,$f5,$f7,$f8,$f9,$f10,$f11,$f12,$f13,$f14)) {
    Write-Output ""
    Write-Output "----- $(Split-Path -Leaf $f) -----"
    Get-Content $f -Encoding UTF8 | Select-Object -First 60 | Write-Output
}

if ($script:fail -gt 0) { exit 1 }
exit 0
