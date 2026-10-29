# ---------------------------------------------------------------------------
#  L7 · verify.ps1 —— v5「有记忆」的端到端验证（不需要 API Key）
#
#  本讲的验证目标与 L6 不同：
#    L5 证明"环转起来了"；L6 逐道闸门证明"每种死法都被拦住了"；
#    L7 要证明"**长了之后还活着**"——
#      上下文被压下来、最初的需求与"读过哪些文件"没丢、换第二家厂商还认得。
#
#  所以用例分三组：
#    A 组  回归（C1..C14）：L6 的 15 个用例一个都不能坏
#          ★ 其中 C15 从"零改动"升级成了**改动清单核对**（见下）
#    B 组  压缩（C16..C17）：H5 端到端 + Anthropic
#    C 组  量（C18..C19）：L0 落盘的实际字节数 / 压缩开与关的 A-B 对比
#
#  用例表：
#    C1  回归：正经干活仍然能跑完（改名 → 验证 → 收工）
#    C2  观察者：LoopHooks 仍然在抛事件
#    C3  --selftest：消息合法性 + 组装三规矩 + **L7 压缩级联（32 条）**
#    C4  权限门：危险命令被 Deny
#    C5  闸门 5：重复调用被拦下并警告
#    C6  服务端侧断言：协议形状仍然正确
#    C7  闸门 1：步数上限
#    C8  闸门 4：连续错误上限
#    C9  闸门 6：取消
#    C10 沙箱：超时强杀
#    C11 沙箱：禁网策略
#    C12 沙箱自检：能力边界必须诚实
#    C13 闸门 3：墙钟上限
#    C14 闸门 2：token 预算上限
#    C16 ★ H5 端到端：连续读 50 个真文件，上下文不爆，且**摘要里还记着需求与路径**
#    C17 ★ 第二家厂商：Anthropic 形状（/messages、工具结果折进 user、角色严格交替）
#    C18 ★ L0 落盘：一条命令产出 ~73K 字符 / ~90K 字节 → 上下文里只留 ~1.5K（用字节数说话）
#    C19 ★ A/B：同样的 50 个文件，压缩关掉 → 上下文量级差一个数量级
#    C15 ★ 改动清单核对：本讲声明的"零改动/已改动/新增"三张清单与磁盘一致
#
#  用法：
#      .\verify.ps1            # 需要先 .\build.ps1
#
#  ★ 关于"服务端进程"：本环境禁止 Start-Job（脚本块后台作业），
#    所以这里用 Start-Process 起一个普通子进程并在结束时回收。
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

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
    if ($null -ne $detail -and "$detail" -ne '') { $suffix = "  —— $detail" }
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
            "AF_SANDBOX","AF_SANDBOX_NET","AF_PLUGIN","AF_PROVIDER","AF_MODEL",
            "AF_CONTEXT_WINDOW","AF_COMPACT","AF_COMPACT_KEEP_TOKENS",
            "AF_COMPACT_FOLD_CHARS","AF_COMPACT_SUMMARY_CHARS","AF_UI",
            "AF_SESSION_DIR","AF_SESSION_ID","AF_MAP","AF_MAP_TOKENS","AF_SYMBOLS",
            "AF_SUBAGENT_STEPS")
function ClearAfEnv {
    foreach ($k in $AfKeys) { Set-Item -Path "Env:$k" -Value "" -ErrorAction SilentlyContinue }
}

function RunAgent {
    param($task, $outFile)
    & $Exe $task *>&1 | Out-File -FilePath $outFile -Encoding UTF8
    return $LASTEXITCODE
}

# 多参数版本（--ui-demo 需要两个 argv，不能塞进一个字符串里）
function RunExe {
    param([string[]]$argv, $outFile)
    & $Exe @argv *>&1 | Out-File -FilePath $outFile -Encoding UTF8
    return $LASTEXITCODE
}

# ★ 数一个文件里有多少个 ESC(0x1B)。
#   这条不能用 -match '\x1b' —— 重定向到文件的字节流才是真相，
#   经过 PowerShell 解码再匹配，等于又引入一层编码假设。
function CountEsc {
    param($path)
    if (-not (Test-Path $path)) { return -1 }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $n = 0
    foreach ($b in $bytes) { if ($b -eq 27) { $n++ } }
    return $n
}

# ---- 服务端日志切片：把"这个用例新产生的行"单独拿出来 ----------------------
function LogMark { return @(Get-Content $Log -Encoding UTF8 -ErrorAction SilentlyContinue).Count }
function LogDelta {
    param($from)
    $all = @(Get-Content $Log -Encoding UTF8 -ErrorAction SilentlyContinue)
    if ($all.Count -le $from) { return "" }
    return (($all | Select-Object -Skip $from) -join "`n")
}

function WaitPort {
    param($port, $timeoutSec)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect("127.0.0.1", $port)
            $c.Close()
            return $true
        } catch { Start-Sleep -Milliseconds 200 }
    }
    return $false
}

if (-not (Test-Path $Exe)) {
    Write-Output "FATAL: 找不到 $Exe —— 先跑 .\build.ps1"
    exit 1
}

Set-Location $Here

# ---- 每次验证前把"会被改的文件"重置成原始内容 -----------------------------
# ★ K-26：PowerShell 的变量名**大小写不敏感**！
#   所以 `$Work`（路径）和 `$work`（文件内容）**是同一个变量** ——
#   C1 里那句 `$work = Get-Content $Work -Raw` 会把**路径覆盖成内容**，
#   于是后面 C17 的 `Copy-Item -Destination $Work` 报
#   "Cannot find path '// 这是 L4 用来验证…'"。
#
#   而它**看起来**完全不像脚本的问题：C17 的 Edit 报"old_str 未找到"，
#   你的第一反应是"模型改错了文件"或"Edit 工具有 bug"。
#   教训：**"路径"和"从路径读到的内容"必须用两个不可能混淆的名字** ——
#   不要靠大小写区分（更不要靠"我记得这里是大写"）。
$PristinePath = Join-Path $Here "mock_target.txt"
$WorkPath     = Join-Path $Here "work_target.txt"
Copy-Item -LiteralPath $PristinePath -Destination $WorkPath -Force
# 重置之后立刻验一次：把"静默失败"变成"大声失败"
if (-not (Get-Content $WorkPath -Raw -Encoding UTF8).Contains('int add(')) {
    Write-Output "FATAL: 夹具重置失败 —— $WorkPath 里没有 'int add('（后续用例会集体误判）"
    exit 1
}

# ---- L0 落盘的目录先清掉，好让 C18 的断言只看本次产生的文件 ---------------
$DumpDir = Join-Path $Here ".af-dump"
# 不用 Remove-Item -Recurse -Force：
#   ① 沙箱会把它判成"批量删除"直接拒绝；② 递归强删一旦路径变量算错就是不可逆的，
#   而这只是个测试产物目录，没资格冒这个险。改用"移到 .af-dump.old" ——
#   目录名一换，C18 就只看见本次产生的文件，效果等价且可回溯。
$DumpOld  = Join-Path $Here ".af-dump.old"
if (Test-Path $DumpOld) { Move-Item -LiteralPath $DumpOld -Destination (Join-Path $Here ".af-dump.bak") -Force -ErrorAction SilentlyContinue }
if (Test-Path $DumpDir)  { Move-Item -LiteralPath $DumpDir  -Destination $DumpOld -Force -ErrorAction SilentlyContinue }

Write-Output "================================================"
Write-Output "  L7 验证：v5 有记忆（压缩级联 + 落盘 + 第二家厂商）"
Write-Output "================================================"

Write-Output ""
Write-Output "[准备] 启动假服务端 127.0.0.1:$Port"
$mockOut = Join-Path $Here "_mock_stdout.txt"
$mockErr = Join-Path $Here "_mock_stderr.txt"
# ★ 参数一律用**相对名** + -WorkingDirectory：
#   本目录路径里有空格（"coding harness"），而 Start-Process 的 -ArgumentList
#   是按空格拼接的 —— 传绝对路径会被拆成两半，python 拿到 "C:\tools\coding"，
#   于是它在启动时 open(log) 就崩了，端口永远不监听。
#   症状会伪装成"agent 连不上服务端"，让你去查网络，而根因只是少了一对引号。
$mockProc = Start-Process -FilePath $Py `
    -ArgumentList @("mock_server.py", "$Port", "mock_requests.log") `
    -WorkingDirectory $Here `
    -PassThru -NoNewWindow `
    -RedirectStandardOutput $mockOut -RedirectStandardError $mockErr
if (-not (WaitPort $Port 15)) {
    Write-Output "FATAL: 假服务端 15 秒内没起来，看 $mockErr"
    Get-Content $mockErr -Encoding UTF8 -ErrorAction SilentlyContinue | Select-Object -First 20 | Write-Output
    if ($mockProc -and -not $mockProc.HasExited) { Stop-Process -Id $mockProc.Id -Force }
    exit 1
}
Write-Output "[准备] 服务端就绪（pid=$($mockProc.Id)）"

$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"
ClearAfEnv
$env:OPENAI_BASE_URL = "http://127.0.0.1:$Port/v1"
$env:OPENAI_API_KEY  = "mock-key-not-real"
$env:AF_MODEL        = "mock-1"

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
$workText = Get-Content $WorkPath -Raw -Encoding UTF8
Check (Has $workText 'int sum(int a, int b)')  "C1 ★ 文件里真的出现了 int sum("
Check ((-not (Has $workText 'int add(int a, int b)'))) "C1 ★ 原来的 int add( 已被替换"

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
#  C3  自检（G8 + 组装三规矩 + L7 压缩级联）
# ===========================================================================
Write-Output ""
Write-Output "[用例] C3 自检（--selftest：L6 6 + L7 26 + L8 7 + L9 11 + L10 16 + L11 8 = 74 条）"
$f3 = Join-Path $Here "_c3.txt"
& $Exe --selftest *>&1 | Out-File -FilePath $f3 -Encoding UTF8
$c3 = $LASTEXITCODE
$t3 = Get-Content $f3 -Raw -Encoding UTF8
Check ($c3 -eq 0)                            "C3 自检退出码为 0"                "exit=$c3"
Check (Has $t3 '0 失败')                      "C3 ★ 自检 0 失败"
# ★ 不写死条数：写死的话每加一条都要改这里，而改验证脚本会让人怀疑"是不是为了让它过"。
#   这里只断言"汇总行的形状"和"条数不比上一讲少"。
$n3 = -1
if ($t3 -match '(\d+) 通过 / (\d+) 失败') { $n3 = [int]$Matches[1] }
Check ($n3 -ge 74)                           "C3 ★ 自检条数 = $n3（≥ L10 的 66 + L11 新增 8）"
Check (Has $t3 'L8 自检：仓库索引')           "C3 ★ L8 的仓库索引自检确实在跑"
Check (Has $t3 'L9 自检：UI 事件队列')        "C3 ★ L9 的 UI 自检确实在跑（stripAnsi / 队列 / 双后端）"
Check (Has $t3 'L10 自检：会话持久化')        "C3 ★ L10 的会话自检确实在跑（JSONL / T3 / 检查点）"
Check (Has $t3 'L11 自检：子代理')            "C3 ★ L11 的子代理自检确实在跑（只读视图 / 摘要契约 / 隔离）"
Check (Has $t3 'I-52')                       "C3 ★ T3 那条断言在跑（replaced_upto 生效）"
Check (Has $t3 'I-36b')                      "C3 ★ PageRank 归一化断言在跑（rank 之和 ≈ 1）"
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
# ===========================================================================
Write-Output ""
Write-Output "[用例] C5 ★ 闸门 5：重复调用被拦下"
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
#  C6  服务端侧断言（看 C1..C5 期间的全部报文）
# ===========================================================================
Write-Output ""
Write-Output "[服务端收到的请求]"
$mockLog = @()
if (Test-Path $Log) { $mockLog = @(Get-Content $Log -Encoding UTF8) }
$mockLog | Select-Object -First 30 | Write-Output
$joined = $mockLog -join "`n"

Check ((-not (Has $joined 'BAD JSON')))                    "C6 服务端未收到坏 JSON"
Check (Has $joined 'model=mock-1')                         "C6 model 是真实模型名（没被 provider.id() 覆盖）"
Check (Has $joined 'stream=True')                          "C6 请求走的是流式"
# ★ 只钉**前缀**，不钉数量。
#   "顺序即缓存"要求的是"已有工具的相对顺序不变"，不是"永远只有 4 个工具"。
#   写死 `tools=4 [...]` 的话，每加一个工具都要改一次验证脚本 ——
#   那是把"稳定性"和"数量不变"混为一谈，而后者根本不是我们想保证的东西。
#   ★ L11 追加的 Task 在**末尾**，前缀一字未动 —— 这正是稳定性要求的形态。
Check (Has $joined "'Read', 'Write', 'Edit', 'Bash'") "C6 ★ 四个原语按注册顺序下发（顺序即缓存）"
Check (Has $joined "'Bash', 'Task'") "C6 ★★ L11 的 Task 追加在**末尾**（没插队 → 前缀缓存仍然命中）"
Check (Has $joined 'tool_call_id=call_read')                "C6 tool 结果带 tool_call_id 配对回填"
Check (Has $joined '[len=')                                 "C6 每条消息都记录了进上下文的长度（可量化"量"）"

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

# ===========================================================================
#  C16  ★ H5 端到端：连续读 50 个真文件，上下文不爆，记忆还在
#
#  ★ 配置的用意（每一处都不是随手写的）：
#    AF_CONTEXT_WINDOW=60000 → L5 的历史预算约 9.5K token（27% 那一份）
#    AF_COMPACT=llm          → 走**真实摘要路径**（一次非流式请求），
#                              顺带证明"客户端引入的第一条非流式请求"是通的
#    AF_COMPACT_FOLD_CHARS 调到极大 → **关掉 L2**。为什么要在用例里关掉它：
#      级联的顺序是"先用免费的手段"，L2（折叠老工具结果）会把 50 个文件读
#      压成"每条 300 字符"，通常**在 L3 之前就把问题解决了**。
#      本用例要验的恰恰是 L3（摘要）这条"保住语义"的路 ——
#      不关掉 L2 就永远测不到它，日志上只会看到 L1/L2，而你会以为
#      "摘要那条路也能跑"（其实它一次都没跑过）。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C16 ★ H5 端到端：连续读 50 个真文件（压缩开，逼出 L3 摘要）"
$env:AF_CONTEXT_WINDOW      = "60000"
$env:AF_COMPACT             = "llm"
$env:AF_COMPACT_KEEP_TOKENS = "2000"
$env:AF_COMPACT_FOLD_CHARS  = "100000000"     # 关掉 L2
$env:AF_MAX_STEPS           = "200"

$lm16 = LogMark
$f16 = Join-Path $Here "_c16.txt"
$task16 = "逐个读很多文件然后汇总它们的模块归属，最后回述最初的需求"
$c16 = RunAgent $task16 $f16
$t16 = Get-Content $f16 -Raw -Encoding UTF8
$d16 = LogDelta $lm16
$l5on = NumOf $t16 'L5'

Check ($c16 -eq 0)                            "C16 退出码为 0"                    "exit=$c16"
Check (Has $t16 'completed')                  "C16 收敛为 completed"
Check ((NumOf $t16 'toolCalls') -eq 50)        "C16 ★ 真读了 50 个文件（一次没重复）" "toolCalls=$(NumOf $t16 'toolCalls')"
Check (Has $t16 '[压缩] L3')                   "C16 ★ L3 摘要被真正执行了（不是只跑了便宜的级）"
Check ($l5on -gt 0 -and $l5on -lt 20000)      "C16 ★ 上下文被打回预算量级（不爆）" "最终 L5=$l5on token（预算≈9.5K）"
Check (Has $d16 '! 压缩摘要消息在场')          "C16 ★ 摘要消息真的进了发给模型的请求里"
Check ($t16 -match '★ 交接摘要里记着的原始需求：(?!\(未)') "C16 ★ 摘要里记着原始需求"
Check ($t16 -match '逐个读很多文件')           "C16 ★ 而且就是最初那句需求（没被压丢）"
Check ($t16 -match '★ 交接摘要里保住的路径：[^\s]*\.(hpp|cpp|py|ps1|txt)') "C16 ★ 摘要里保住了文件路径（交接口诀）"
# ★ 具体到"第一个被读的文件"：它是最早被压掉的那批消息里的，
#   如果摘要会丢路径，它一定是第一个丢的。它还在 → 路径没丢。
Check (Has $t16 '★ 交接摘要里保住的路径：compaction.hpp') "C16 ★ 连最初读的第 1 个文件都还记得（compaction.hpp）"
ClearAfEnv

# ===========================================================================
#  C17  ★ 第二家厂商：Anthropic
# ===========================================================================
Write-Output ""
Write-Output "[用例] C17 ★ Anthropic 形状（/messages、工具结果折进 user、角色严格交替）"
Copy-Item -LiteralPath $PristinePath -Destination $WorkPath -Force
if (-not (Get-Content $WorkPath -Raw -Encoding UTF8).Contains('int add(')) {
    Write-Output "  [警告] C17 的夹具重置没生效（work_target.txt 里没有 'int add('）"
}
$env:AF_PROVIDER          = "anthropic"
$env:ANTHROPIC_BASE_URL   = "http://127.0.0.1:$Port/v1"
$env:ANTHROPIC_API_KEY    = "mock-key-not-real"
$env:AF_MODEL             = "mock-claude"
$env:AF_COMPACT           = "off"

$lm17 = LogMark
$f17 = Join-Path $Here "_c17.txt"
$c17 = RunAgent "把 work_target.txt 里的 add 改名为 sum，然后跑测试确认" $f17
$t17 = Get-Content $f17 -Raw -Encoding UTF8
$d17 = LogDelta $lm17
$work17Text = Get-Content $WorkPath -Raw -Encoding UTF8

Check ($c17 -eq 0)                            "C17 退出码为 0"                    "exit=$c17"
Check (Has $t17 'completed')                  "C17 收敛为 completed"
Check ((NumOf $t17 'toolCalls') -eq 3)         "C17 ★ 三步脚本在第二协议下照样推进"  "toolCalls=$(NumOf $t17 'toolCalls')"
Check (Has $work17Text 'int sum(int a, int b)') "C17 ★ 文件真的被改对了"
Check (Has $d17 'shape=anthropic')            "C17 ★ 请求走的是 Anthropic 形状"
# ★ 同 C6：只钉前缀（换协议不改变工具表的顺序，这才是要证明的）
Check (Has $d17 "'Read', 'Write', 'Edit', 'Bash'") "C17 工具表在第二协议下同样按序下发"
Check (Has $d17 "'Bash', 'Task'") "C17 ★ 第二协议下 Task 同样追加在末尾（适配层没重排工具表）"
Check (Has $d17 'tool_result×')               "C17 ★ 工具结果被折进 user 消息（Anthropic 的结构差异）"
Check ((-not (Has $d17 '连续同角色')))         "C17 ★ 没有连续同角色（适配器把 user 合并了，否则会 400）"
Check ((-not (Has $d17 'BAD JSON')))          "C17 服务端未收到坏 JSON"
ClearAfEnv
$env:AF_MODEL = "mock-1"

# ===========================================================================
#  C18  ★ L0 落盘：一条命令产出 ~73K 字符 / ~90K 字节 → 上下文里只留 ~1.5K
#
#  用**字节数**说话：磁盘上的落盘文件 > 60K，而请求里那条 tool 消息 < 3K。
#  这两个数字放在一起，才是"L0 生效"的完整证据 ——
#  只看"日志里提到落盘了"是不够的（那可能落盘了但上下文照样塞满了）。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C18 ★ L0 落盘：~90K 字节的工具输出 → 上下文只留预览"
$env:AF_MAX_STEPS = "6"
$lm18 = LogMark
$f18 = Join-Path $Here "_c18.txt"
$c18 = RunAgent "跑一条命令输出一个超大文件的内容" $f18
$t18 = Get-Content $f18 -Raw -Encoding UTF8
$d18 = LogDelta $lm18

Check ($c18 -eq 0)                            "C18 退出码为 0"                    "exit=$c18"
Check (Has $t18 'completed')                  "C18 收敛为 completed"

# 磁盘上的落盘文件
$dumpFiles = @()
if (Test-Path $DumpDir) {
    $dumpFiles = @(Get-ChildItem -LiteralPath $DumpDir -File -ErrorAction SilentlyContinue)
}
$maxDump = 0
if ($dumpFiles.Count -gt 0) { $maxDump = ($dumpFiles | Measure-Object -Property Length -Maximum).Maximum }
Check ($maxDump -gt 60000)                    "C18 ★ 磁盘上真的落了一个 >60K 的文件"  "最大 $maxDump 字节 / $($dumpFiles.Count) 个文件"

# 上下文里那条 tool 消息的长度（服务端日志里有 [len=...]）
$toolLens = @()
foreach ($line in ($d18 -split "`n")) {
    if ($line -match '\[REQ\]\s+tool:.*\[len=(\d+)\]') { $toolLens += [int]$Matches[1] }
}
$maxToolLen = -1
if ($toolLens.Count -gt 0) { $maxToolLen = ($toolLens | Measure-Object -Maximum).Maximum }
Check ($maxToolLen -gt 0 -and $maxToolLen -lt 3000) "C18 ★ 上下文里那条 tool 消息 <3K（原本 ~73K 字符 / ~90K 字节）" "上下文里最大 $maxToolLen 字符"
ClearAfEnv

# ===========================================================================
#  C19  ★ A/B：同样的 50 个文件，压缩关掉
#
#  这一条不是"再测一遍压缩"，而是**对照组**：
#  同样读 50 个文件，只把压缩关掉，看上下文量级差多少。
#  没有对照组，"上下文不爆"只是一个孤立的数字；
#  有了对照组，它才是"压缩**起了作用**"。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C19 ★ A/B 对照：同样 50 个文件，压缩关掉（AF_COMPACT=off）"
$env:AF_CONTEXT_WINDOW = "60000"
$env:AF_COMPACT        = "off"
$env:AF_MAX_STEPS      = "200"

$lm19 = LogMark
$f19 = Join-Path $Here "_c19.txt"
$task19 = "很多文件请逐个读一遍并汇总各自的用途，做完告诉我读过哪些文件"
$c19 = RunAgent $task19 $f19
$t19 = Get-Content $f19 -Raw -Encoding UTF8
$d19 = LogDelta $lm19
$l5off = NumOf $t19 'L5'

Check ($c19 -eq 0)                            "C19 退出码为 0"                    "exit=$c19"
Check ((NumOf $t19 'toolCalls') -eq 50)        "C19 同样读了 50 个文件"             "toolCalls=$(NumOf $t19 'toolCalls')"
Check (Has $t19 'compact=0 level=none')        "C19 ★ 压缩确实关掉了（没有一次压缩事件）"
Check ((-not (Has $t19 '[压缩] L')))           "C19 没有任何压缩事件"
Check ((-not (Has $d19 '! 压缩摘要消息在场')))  "C19 请求里没有摘要消息"
Check (Has $t19 '★ 交接摘要里记着的原始需求：(未找到摘要)') "C19 ★ 关掉压缩后就没有这份交接文档了"
Check ($l5off -gt 40000)                       "C19 ★ 上下文量级远大于 C16（对照组成立）" ("L5=$l5off vs C16 的 $l5on")
Check ($l5off -gt ($l5on * 3))                "C19 ★ 至少差 3 倍（压缩确实在起作用）"   ("$l5off vs $l5on")
ClearAfEnv

# ===========================================================================
#  ★ L8 新增：C20..C26 —— 仓库地图（模块 ③）
#
#  这一组的验证哲学与前几讲一致：**不验证"功能存在"，验证"数字对得上"**。
#  所以每个用例都从程序输出里抠出具体数字再断言，而不是看有没有某行日志。
#
#    C20 基本盘：200 文件 → 地图 < 1500 token，且含"答案符号"
#    C21 ★ A/B：真解析器 vs 词法兜底的召回率（可量化，不是口头宣称）
#    C22 预算伸缩：256 / 1024 / 4096 三档，地图随预算长，且都不超
#    C23 活动文件偏置：改一个文件的 rank，地图头部跟着变
#    C24 tree-sitter 真的编进来了（编译期口径，不是运行时猜的）
#    C25 词法兜底仍然可用（tree-sitter 不在时地图照出）
#    C26 ★ 地图真的进了 HTTP 请求（L2_RepoMap 层不是摆设）
# ===========================================================================
Write-Output ""
Write-Output "[用例] C20 ★ 仓库地图基本盘（200 文件 → <1500 token 且含答案符号）"

$Repo = Join-Path $Here "_fixture\repo200"
if (-not (Test-Path (Join-Path $Repo "src"))) {
    & $Py (Join-Path $Here "make_fixture.py") "_fixture\repo200" *>&1 | Out-Null
}

$env:AF_MAP_TOKENS = "1024"
$f20 = Join-Path $Here "_c20.txt"
& $Exe --show-repo-map "_fixture\repo200" *>&1 | Out-File -FilePath $f20 -Encoding UTF8
$t20 = Get-Content $f20 -Raw -Encoding UTF8

$nFiles   = NumOf $t20 'files'
$nSyms    = NumOf $t20 'symbols'
$nTok     = NumOf $t20 'estTokens'
$nTooBig  = NumOf $t20 'tooBig'
$nIgnored = NumOf $t20 'ignored'

Check (Has $t20 'backend=treesitter')      "C20 ★ 后端真的是 tree-sitter"
Check ($nFiles -ge 200)                    "C20 ★ 索引到 200+ 个文件"            "files=$nFiles"
Check ($nSyms  -gt 1000)                   "C20 ★ 抽出 1000+ 个符号"             "symbols=$nSyms"
Check ($nTok   -le 1500)                   "C20 ★ 地图 < 1500 token（H6 验收线）"  "estTokens=$nTok"
Check ($nTok   -le 1024)                   "C20 地图不超预算 1024"                "estTokens=$nTok"
Check (Has $t20 'key_manager')             "C20 ★★ 200 个文件里，答案文件在地图中"
Check (Has $t20 'rotateMasterKey')         "C20 ★★ 答案符号 rotateMasterKey 在地图中"
Check ($nTooBig  -ge 1)                    "C20 超大文件被跳过（不会拖垮索引）"     "tooBig=$nTooBig"
Check ($nIgnored -ge 5)                    "C20 ★ .gitignore + 构建目录被跳过"     "ignored=$nIgnored"
Check ((-not (Has $t20 'vendor/')))        "C20 被忽略的 vendor/ 没进地图"
Check ((-not (Has $t20 'build/gen')))      "C20 被忽略的 build/ 没进地图"

# ===========================================================================
Write-Output ""
Write-Output "[用例] C21 ★ 符号 A/B：真解析器 vs 词法兜底"
$f21 = Join-Path $Here "_c21.txt"
& $Exe --symbol-ab "_fixture\repo200" *>&1 | Out-File -FilePath $f21 -Encoding UTF8
$t21 = Get-Content $f21 -Raw -Encoding UTF8

# 从表格里抠两行的 symbols 列
$abLines = @(Get-Content $f21 -Encoding UTF8 | Where-Object { $_ -match '^(treesitter|lexical)\s' })
$tsSym = -1; $lxSym = -1; $onlyTs = -1; $onlyLx = -1
if ($abLines.Count -ge 2) {
    $tsSym  = [int]($abLines[0] -split '\s+')[2]
    $lxSym  = [int]($abLines[1] -split '\s+')[2]
}
if ($t21 -match 'onlyTs=(\d+)\s+onlyLx=(\d+)') {
    $onlyTs = [int]$Matches[1]; $onlyLx = [int]$Matches[2]
}

Check ($tsSym -gt 0 -and $lxSym -gt 0)     "C21 两个后端都跑出了符号"            "ts=$tsSym lexical=$lxSym"
Check ($tsSym -gt $lxSym)                  "C21 ★ tree-sitter 抽到的符号更多"     "$tsSym vs $lxSym"
Check ($onlyTs -gt 0)                      "C21 ★★ 存在只有真解析器能抽到的符号"  "onlyTs=$onlyTs"
Check ($onlyLx -ge 0)                      "C21 词法端的独有条目已列出（含误报）"  "onlyLx=$onlyLx"
Check (Has $t21 '召回率')                  "C21 输出里给出了召回率（可量化）"
Check (Has $t21 '带限定名')                "C21 ★ 限定名差异被单独统计（口径没混）"

# ===========================================================================
Write-Output ""
Write-Output "[用例] C22 预算伸缩：地图随预算长，且从不超预算"
$env:AF_MAP_TOKENS = "256"
$t22a = (& $Exe --show-repo-map "_fixture\repo200" *>&1 | Out-String)
$env:AF_MAP_TOKENS = "4096"
$t22b = (& $Exe --show-repo-map "_fixture\repo200" *>&1 | Out-String)
$env:AF_MAP_TOKENS = "1024"
$tokA = NumOf $t22a 'estTokens'
$tokB = NumOf $t22b 'estTokens'
Check ($tokA -gt 0 -and $tokA -le 256)     "C22 预算 256 → 地图 ≤ 256 token"     "estTokens=$tokA"
Check ($tokB -gt 3000)                     "C22 预算 4096 → 地图真的变大了"       "estTokens=$tokB"
Check ($tokB -gt $tokA)                    "C22 ★ 预算越大地图越大（不是写死的）"  "$tokA → $tokB"

# ===========================================================================
Write-Output ""
Write-Output "[用例] C23 ★ 活动文件偏置：正在编辑的文件，其邻居优先进地图"
$t23 = (& $Exe --show-repo-map "_fixture\repo200" "src/metrics/metrics_comp00.cpp" *>&1 | Out-String)
$rBiased = -1.0; $rPlain = -1.0
if ($t23 -match 'rank=([0-9.]+)\s')      { $rBiased = [double]$Matches[1] }
if ($t23 -match 'rank=([0-9.]+)\)')      { $rPlain  = [double]$Matches[1] }
Check ($rBiased -gt 0)                   "C23 活动文件拿到了非零 rank（路径解析对了）" "rank=$rBiased"
Check ($rBiased -gt $rPlain)             "C23 ★★ 偏置后 rank 显著提高"          "$rPlain → $rBiased"
Check (Has $t23 'metrics_comp00')        "C23 偏置后该文件出现在地图正文里"

# ===========================================================================
Write-Output ""
Write-Output "[用例] C24 tree-sitter 的编译期口径"
Check (Has $t20 '编译期 tree-sitter: 有')  "C24 ★ tree-sitter 是**编译期**链进来的（不是运行时猜的）"

# ===========================================================================
Write-Output ""
Write-Output "[用例] C25 词法兜底仍然可用（可降级，不是硬依赖）"
$env:AF_SYMBOLS = "lexical"
$t25 = (& $Exe --show-repo-map "_fixture\repo200" *>&1 | Out-String)
$env:AF_SYMBOLS = ""
Check (Has $t25 'backend=lexical')       "C25 ★ 强制走词法后端时，输出明确说了是 lexical"
Check ((NumOf $t25 'estTokens') -le 1500) "C25 词法后端下地图仍不超预算"          "estTokens=$(NumOf $t25 'estTokens')"
Check ((NumOf $t25 'symbols') -gt 0)     "C25 词法后端仍能抽出符号（降级不是瘫痪）" "symbols=$(NumOf $t25 'symbols')"

# ===========================================================================
Write-Output ""
Write-Output "[用例] C26 ★★ 地图真的进了 HTTP 请求（L2_RepoMap 层不是摆设）"
#
#  ★ 这一条踩过的坑值得写下来：**日志看不见 ≠ 东西没发**。
#    mock_server.py 每条消息只打**前 60 字符**，而仓库地图是**追加在 system 末尾**的 ——
#    所以第一版断言"请求体里有没有 # 仓库地图"必然失败，可地图其实早就发出去了。
#    （同样的病在 C16 也犯过一次：判据写在了看不见的地方。）
#
#  ★ 修法分两层，都要：
#    ① 服务端补一行 system **全长**清单（长度 + 命中哪些段落标题）—— 见 mock_server.py；
#    ② 判据首选**长度差**而不是字符串。长度是服务端真实收到的字节数，
#       "客户端自己打印了一句'已发送'"那是自证，不算证据。
#    所以这里跑两次：关地图 vs 开地图，比服务端收到的 system 长度。
$env:AF_MAP = "0"
$lm26o = LogMark
$fo26 = Join-Path $Here "_c26_off.txt"
$co26 = RunAgent "看一下这个工程里有哪些文件" $fo26
$d26o = LogDelta $lm26o
$sysOff = NumOf $d26o 'full'

$env:AF_MAP = "1"
$env:AF_MAP_TOKENS = "1024"
$lm26 = LogMark
$f26 = Join-Path $Here "_c26.txt"
$c26 = RunAgent "看一下这个工程里有哪些文件" $f26
$t26 = Get-Content $f26 -Raw -Encoding UTF8
$d26 = LogDelta $lm26
$sysOn  = NumOf $d26  'full'
# 「地图 1024 token」中间是空格不是等号，NumOf 抓不到，单独抠
$mapTok = -1
if ($t26 -match '地图\s+(\d+)\s+token') { $mapTok = [int]$Matches[1] }
Check ($c26 -eq 0)                        "C26 agent 正常跑完"                  "exit=$c26"
Check ((Has $t26 '[地图]'))               "C26 ★ 启动时打印了地图实况（可观测）"
Check (Has $d26 '# 仓库地图')             "C26 ★★ 服务端收到的 system 里带「# 仓库地图」"
Check (($sysOff -gt 0) -and ($sysOn -gt 0)) "C26 两次都取到了服务端 system 长度"  "off=$sysOff on=$sysOn"
# 1024 token 的地图 ≈ 2600~4500 字符（estimateTokens 按 3.5 字符/token 估）
Check (($sysOn - $sysOff) -gt 2000)      "C26 ★★★ 开地图后服务端 system 真的变长了（字节上了线路）" ("$sysOff → $sysOn（+$($sysOn-$sysOff) 字符）")
Check (($sysOn - $sysOff) -lt 12000)     "C26 而且没长到离谱（地图受预算约束，不是整仓塞进去）" ("+$($sysOn-$sysOff) 字符，地图 $mapTok token")
$env:AF_MAP = ""
$env:AF_MAP_TOKENS = ""
ClearAfEnv

# ===========================================================================
#  C27  ★★ 非 TTY 输出零 ANSI
#
#  ★ 这条用例守护的是**前面 8 讲的全部 133 条断言**。
#    整套验证体系的方法论是"把 stdout 重定向到文件再 grep"；
#    只要有一条 ESC 漏进文件，某些 grep 就会失配，
#    而报错会以一种**看起来完全不相关**的样子出现在另一个用例里
#    （K-21 那一类：犯错的地方和报错的地方不是同一个）。
#
#    所以判据必须硬到"零个 ESC"，不能写成"大部分是纯文本"。
#
#  ★ 而且两个后端都要测：
#    plain 从不产生 ANSI；ftxui **产生** ANSI，靠 writeFrame() 在出口剥掉。
#    只测 plain 的话，这条用例等于没写 —— 会漏的那个路径根本没被走到。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C27 ★★ 非 TTY 输出零 ANSI（守护前 8 讲的 133 条断言）"
$f27p = Join-Path $Here "_c27_plain.txt"
$f27f = Join-Path $Here "_c27_ftxui.txt"
$env:AF_UI = "plain"
$c27p = RunAgent "看一下这个工程里有哪些文件" $f27p
$env:AF_UI = "ftxui"
$c27f = RunAgent "看一下这个工程里有哪些文件" $f27f
$env:AF_UI = ""
ClearAfEnv
Check ($c27p -eq 0)                 "C27 plain 后端正常跑完"                    "exit=$c27p"
Check ($c27f -eq 0)                 "C27 ftxui 后端正常跑完"                    "exit=$c27f"
$e27p = CountEsc $f27p
$e27f = CountEsc $f27f
Check ($e27p -eq 0)                 "C27 ★ plain 输出零 ESC"                    "esc=$e27p"
Check ($e27f -eq 0)                 "C27 ★★ ftxui 重定向时 ANSI 被剥净"         "esc=$e27f"
$t27f = Get-Content $f27f -Raw -Encoding UTF8
Check (Has $t27f 'AgentForge v7')  "C27 ftxui 真的走了画布路径（不是偷偷退回 plain）"

# ===========================================================================
#  C28  渲染线程不阻塞流式
#
#  判据：Agent 线程收到的 chunk 数，应当**全部**进了 UI 队列。
#  如果渲染回调里有 I/O 或有锁，事件就会被丢/被合并，events 会小于 chunks。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C28 渲染线程不阻塞流式（每个 chunk 都进了队列）"
$f28 = Join-Path $Here "_c28.txt"
$c28 = RunAgent "把 work_target.txt 里的 add 改名为 sum，然后跑命令验证" $f28
$t28 = Get-Content $f28 -Raw -Encoding UTF8
$chunks28 = NumOf $t28 'chunks'
$events28 = NumOf $t28 'events'
Check ($c28 -eq 0)                       "C28 agent 正常跑完"                  "exit=$c28"
Check ($chunks28 -gt 0)                  "C28 确实收到了流式 chunk"            "chunks=$chunks28"
Check ($events28 -ge $chunks28)          "C28 ★★ 每个 chunk 都进了 UI 队列（一个都没被渲染吞掉）" ("chunks=$chunks28 events=$events28")
Check (Has $t28 '[UI] 后端=')            "C28 UI 实况可观测（后端 / 事件数 / 帧数都打出来了）"
ClearAfEnv

# ===========================================================================
#  C29 ★ 工具执行中有 spinner（H7 的耗时条同源）
#
#  ★ 为什么用 --ui-demo 而不是一次真实对话：
#    "工具刚好跑够 1 秒"是**碰运气**，碰运气的用例不是可回归的断言。
#    把时序写成脚本，它就变成每次都成立的判据。
#
#  ★ 这段 1.2 秒里 Agent 线程**一个事件都不推** ——
#    画面上的「执行中」只能是渲染线程自己画出来的。这就是"线程分离"的实证。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C29 ★ 工具执行期间画面仍在更新（spinner / 耗时条）"
$f29 = Join-Path $Here "_c29.txt"
$c29 = RunExe @("--ui-demo","plain") $f29
$t29 = Get-Content $f29 -Raw -Encoding UTF8
$fr29 = NumOf $t29 'frames'
Check ($c29 -eq 0)                       "C29 --ui-demo plain 正常跑完"        "exit=$c29"
Check (Has $t29 '[执行中]')              "C29 ★★ 工具执行期间出现了「执行中」中间态（不是只有开始和结束）"
Check ($fr29 -ge 5)                      "C29 ★ 1.2 秒的空窗期里画面仍在出帧"  "frames=$fr29"
Check (Has $t29 '(1200ms)')              "C29 ★ H7：工具调用旁显示了耗时"

# ===========================================================================
#  C30 ★ 权限弹窗不阻塞渲染
#
#  ★ 判据不是"agent 没卡住"（那要真人按键），而是：
#    **弹窗挂在那儿的时候，渲染循环还在出帧**。
#    --ui-demo 只推 PermAsk、**故意不回答**，就是为了把这一刻冻住给人看。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C30 ★ 权限弹窗不阻塞渲染（弹窗挂着时画面仍在出帧）"
$f30 = Join-Path $Here "_c30.txt"
$c30 = RunExe @("--ui-demo","ftxui") $f30
$t30 = Get-Content $f30 -Raw -Encoding UTF8
$fr30 = NumOf $t30 'frames'
Check ($c30 -eq 0)                       "C30 --ui-demo ftxui 正常跑完"        "exit=$c30"
Check (Has $t30 '[权限]')                "C30 权限弹窗出现在画面里"
Check (Has $t30 'Bash: rm -rf build')    "C30 弹窗内容正确（询问的是那次具体调用）"
Check ($fr30 -ge 5)                      "C30 ★★ 弹窗未回答期间渲染循环仍在出帧"  "frames=$fr30"

# ===========================================================================
#  C31  双实现可切换：同一个进程里两个后端都能产出内容
# ===========================================================================
Write-Output ""
Write-Output "[用例] C31 双实现可切换（plain / ftxui 都能跑）"
$f31p = Join-Path $Here "_c31_plain.txt"
$f31f = Join-Path $Here "_c31_ftxui.txt"
$c31p = RunExe @("--ui-demo","plain") $f31p
$c31f = RunExe @("--ui-demo","ftxui") $f31f
$t31p = Get-Content $f31p -Raw -Encoding UTF8
$t31f = Get-Content $f31f -Raw -Encoding UTF8
Check ($c31p -eq 0)                      "C31 plain 后端能跑完"                "exit=$c31p"
Check ($c31f -eq 0)                      "C31 ftxui 后端能跑完"                "exit=$c31f"
Check (Has $t31p '实际后端=plain')       "C31 请求 plain 就拿到 plain"
Check (Has $t31f '实际后端=ftxui')       "C31 请求 ftxui 就拿到 ftxui（库在场时）"
$sz31p = (Get-Item $f31p).Length
$sz31f = (Get-Item $f31f).Length
Check (($sz31p -gt 100) -and ($sz31f -gt 100)) "C31 两个后端都产出了内容（不是空跑）" ("plain=$($sz31p)B ftxui=$($sz31f)B")

# ===========================================================================
#  C32 ★ ftxui.lib 不在场时可降级（本讲的"L8 的 C25"）
#
#  ★ 做法：把库挪走 → 用 build.ps1 编一份 agent_plain.exe →
#    **故意请求 ftxui** → 断言实际拿到 plain 且程序照常跑完 → 再把库挪回来。
#
#  ★ 为什么必须真的编一次：
#    "库不在就不编那个文件"是编译期的行为，只有真的跑一遍构建才能证明。
#    靠读代码推断出来的"应该能编译"和跑过了之间，平均藏着 2-3 个 bug
#    （这句话在 L8 的 PageRank 上刚应验过一次）。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C32 ★ 缺库降级：ftxui.lib 不在场时仍能编出能跑的 exe"
$libPath = Join-Path $Here "thirdparty\ftxui\build\ftxui.lib"
$libBak  = Join-Path $Here "thirdparty\ftxui\build\ftxui.lib.bak"
$plainExe = Join-Path $Here "build\agent_plain.exe"
$hadLib = Test-Path $libPath
if ($hadLib) { Move-Item -LiteralPath $libPath -Destination $libBak -Force }
$f32b = Join-Path $Here "_c32_build.txt"
try {
    & (Join-Path $Here "build.ps1") -Out "agent_plain.exe" *>&1 |
        Out-File -FilePath $f32b -Encoding UTF8
    $b32 = $LASTEXITCODE
    Check (Test-Path $plainExe)          "C32 ★★ 库不在场时 build.ps1 仍能编出 exe"   "buildExit=$b32"
    $tb32 = Get-Content $f32b -Raw -Encoding UTF8
    Check (Has $tb32 '退回纯文本 UI')    "C32 构建脚本明确报告了降级（不是悄悄编过去）"

    if (Test-Path $plainExe) {
        $f32 = Join-Path $Here "_c32.txt"
        # ★ 故意请求 ftxui：库不在场，它必须退回 plain 而不是崩
        $c32 = & $plainExe --ui-demo ftxui *>&1 | Out-File -FilePath $f32 -Encoding UTF8
        $t32 = Get-Content $f32 -Raw -Encoding UTF8
        Check (Has $t32 '实际后端=plain') "C32 ★★ 缺库时请求 ftxui 退回 plain（降级不是瘫痪）"
        Check (Has $t32 '[执行中]')       "C32 降级后的 UI 仍能工作（spinner 照常）"
    }
} finally {
    # ★ 无论上面成败，库必须归位 —— 否则后面每一次构建都会静默降级
    if (Test-Path $libBak) { Move-Item -LiteralPath $libBak -Destination $libPath -Force }
}
Check (Test-Path $libPath)               "C32 收尾：ftxui.lib 已归位"            "hadLib=$hadLib"
# 把带 FTXUI 的主 exe 重新编回来（上面那次构建把它覆盖了）
$f32r = Join-Path $Here "_c32_rebuild.txt"
& (Join-Path $Here "build.ps1") *>&1 | Out-File -FilePath $f32r -Encoding UTF8
Check (Test-Path $Exe)                   "C32 收尾：主 exe 已重建"               "buildExit=$LASTEXITCODE"

# ===========================================================================
#  C33 ★★ 会话落盘
# ===========================================================================
Write-Output ""
Write-Output "[用例] C33 ★★ 会话落盘：JSONL 真的在盘上"

$Scratch = Join-Path $Here "_scratch"
if (-not (Test-Path $Scratch)) { New-Item -ItemType Directory -Path $Scratch -Force | Out-Null }
$sessDir = Join-Path $Here "_sess"
if (Test-Path $sessDir) {
    Move-Item -LiteralPath $sessDir -Destination (Join-Path $Scratch ("_sess_" + (Get-Date).ToString("HHmmss"))) `
        -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $sessDir -Force | Out-Null

ClearAfEnv
$env:AF_SESSION_DIR = $sessDir
$env:AF_SESSION_ID  = "c33"
$f33 = Join-Path $Here "_c33.txt"
$c33 = RunAgent "读一下 main.cpp 的前 20 行" $f33
$t33 = Get-Content $f33 -Raw -Encoding UTF8
$jsonl33 = Join-Path $sessDir "c33.jsonl"
Check ($c33 -eq 0)              "C33 agent 正常跑完"                     "exit=$c33"
Check (Has $t33 'resumed=0')    "C33 ★ 第一次是**新**会话（resumed=0，不是假装恢复了）"
Check (Has $t33 '[落盘]')       "C33 ★ turn 结束时有落盘动作（可观测）"
Check (Test-Path $jsonl33)      "C33 ★★ JSONL 真的落到盘上了（不是只在内存里）"
$lines1 = @([System.IO.File]::ReadAllLines($jsonl33, [System.Text.Encoding]::UTF8)).Count
Check ($lines1 -ge 2)           "C33 日志有 meta 行 + 消息行"            "行数=$lines1"

# ===========================================================================
#  C34 ★★ 半行 = 被杀的进程
#
#  ★ 为什么"被杀"用**手工追加半行**来模拟，而不是真的 kill：
#    真的 kill 只能证明"进程死了"；这一条要证明的是
#    "**日志处于半行状态时恢复仍然成立**" —— 后者才是设计要扛的东西。
#    而且真 kill 是随机的（可能刚好停在行之间），半行是**最坏情况**。
#    ★ 先测最坏情况，再谈运气。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C34 ★★ 半行（进程被杀的真实形态）：只丢自己，且报出来"
[System.IO.File]::AppendAllText($jsonl33, '{"seq":999,"t":"user","content":"写到一半进程被杀')
$f34 = Join-Path $Here "_c34.txt"
$c34 = RunExe @("--resume") $f34
$t34 = Get-Content $f34 -Raw -Encoding UTF8
$bad34 = -1; $msg34 = -1
if ($t34 -match '坏行\s+(\d+)') { $bad34 = [int]$Matches[1] }
if ($t34 -match '消息\s+(\d+)') { $msg34 = [int]$Matches[1] }
Check ($c34 -eq 0)              "C34 --resume 退出码为 0（半行不能让程序崩）"  "exit=$c34"
Check (Has $t34 'resumed=1')    "C34 ★★ 真的接着旧会话（resumed=1）"
Check ($bad34 -eq 1)            "C34 ★★ 半行被**报出来**是 1 条坏行（不静默跳过）" "坏行=$bad34"
Check ($msg34 -ge 1)            "C34 ★★ 其余消息照常恢复（半行只丢自己）"      "消息=$msg34"

# ===========================================================================
#  C35 ★★ 接着上次干：日志只追加，历史不重复写
# ===========================================================================
Write-Output ""
Write-Output "[用例] C35 ★★ 接着上次干：日志追加 + 历史不被重写一遍"
$f35 = Join-Path $Here "_c35.txt"
$c35 = RunExe @("--resume", "接上：把刚才读过的文件再读一遍") $f35
$t35 = Get-Content $f35 -Raw -Encoding UTF8
Check ($c35 -eq 0)              "C35 第二轮跑完"                          "exit=$c35"
Check (Has $t35 'resumed=1')    "C35 ★ 第二轮仍是同一个会话（id 没被偷偷改名）"
$lines2 = @([System.IO.File]::ReadAllLines($jsonl33, [System.Text.Encoding]::UTF8)).Count
Check ($lines2 -gt $lines1)     "C35 ★ 日志是**追加**的（旧行没被覆写）"   "$lines1 → $lines2"
$f35b = Join-Path $Here "_c35b.txt"
RunExe @("--resume") $f35b | Out-Null
$t35b = Get-Content $f35b -Raw -Encoding UTF8
$msg35 = -1
if ($t35b -match '消息\s+(\d+)') { $msg35 = [int]$Matches[1] }
$growth = $msg35 - $msg34
Check (($msg35 -ge $msg34) -and ($growth -le 8)) "C35 ★★ 恢复后不重复追加（增量=本轮新增，不是翻倍）" "$msg34 → $msg35（+$growth）"

# ===========================================================================
#  C36 ★ T3：压缩到一半的日志，恢复出来仍是正确上下文
#
#  ★ T3 是 L2 就挂在那里的开放问题："上下文压缩了一半，进程崩了，恢复的是什么？"
#    本讲给的答案分三层（讲义 §3）：
#      ① 顺序 —— 先落盘新消息，再落盘"老的可以丢了"（反过来就空了）
#      ② 消歧 —— compaction 事件带显式 replaced_upto（内容判据，不是行号）
#      ③ 兜底 —— 摘要为空 / 截断后为空时**回退不截断**（宁可长，不能空）
#    这里用夹具把三种形态都跑一遍；判据是"恢复出来的消息 **条数与内容**"。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C36 ★★ T3：压缩了一半的日志，恢复出来仍然对"
& $Py (Join-Path $Here "make_session_fixture.py") "_sessfix" *>&1 | Out-Null
$f36c = Join-Path $Here "_c36_compacted.txt"
RunExe @("--session-debug", "_sessfix\b_compacted.jsonl") $f36c | Out-Null
$t36c = Get-Content $f36c -Raw -Encoding UTF8
Check (Has $t36c '用到摘要=1')     "C36 ★★ 完整压缩：恢复时**用上了**交接摘要"
Check ((Has $t36c '[0] 交接摘要')) "C36 ★★ 被摘要取代的旧消息没进视图（replaced_upto 生效）"
Check (-not (Has $t36c '旧消息 1')) "C36 旧消息确实被取代了（不是新旧一起塞）"

$f36p = Join-Path $Here "_c36_partial.txt"
RunExe @("--session-debug", "_sessfix\c_partial.jsonl") $f36p | Out-Null
$t36p = Get-Content $f36p -Raw -Encoding UTF8
Check (Has $t36p '忽略半截压缩=1') "C36 ★★ 摘要为空的 compaction 被忽略（T3 的兜底层）"
Check (Has $t36p '旧消息 1')       "C36 ★ 回退到原文（宁可长，不能空）"

$f36a = Join-Path $Here "_c36_half.txt"
RunExe @("--session-debug", "_sessfix\a_halflane.jsonl") $f36a | Out-Null
$t36a = Get-Content $f36a -Raw -Encoding UTF8
$bad36 = -1
if ($t36a -match '坏行\s+(\d+)') { $bad36 = [int]$Matches[1] }
Check ($bad36 -ge 1)               "C36 ★ 半行在**真实文件**上也被报出来"     "坏行=$bad36"

# ===========================================================================
#  C37 ★ 会话列表 + 默认不落盘
# ===========================================================================
Write-Output ""
Write-Output "[用例] C37 会话列表可查；默认**不**落盘"
$f37 = Join-Path $Here "_c37.txt"
RunExe @("--sessions") $f37 | Out-Null
$t37 = Get-Content $f37 -Raw -Encoding UTF8
Check (Has $t37 'c33')             "C37 ★ --sessions 列出了刚才那个会话"

# ★ 默认关闭：既有 162 条用例是在"无副作用"前提下写的，一开落盘就会互相污染
$env:AF_SESSION_DIR = ""
$env:AF_SESSION_ID  = ""
$f37b = Join-Path $Here "_c37b.txt"
RunAgent "你好，自我介绍一下" $f37b | Out-Null
$t37b = Get-Content $f37b -Raw -Encoding UTF8
Check (-not (Has $t37b '[会话]'))  "C37 ★★ 不设 AF_SESSION_DIR 时**不落盘**（保护既有 162 条）"

# ===========================================================================
#  C38 ★ CLAUDE.md 进 L1 层（三层记忆之"项目知识"）
#
#  ★ 落点是 L2 就预留的 setProjectKnowledge()（头文件注释写着"L10 加载"）。
#  ★ 判据要落在**服务端收到的请求**上，不能只看客户端打印的一行字 ——
#    后者是自证。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C38 ★ CLAUDE.md 进 L1 层（项目知识，不是旧对话）"
$wd38 = Join-Path $Here "_cwd38"
if (Test-Path $wd38) {
    Move-Item -LiteralPath $wd38 -Destination (Join-Path $Scratch ("_cwd38_" + (Get-Date).ToString("HHmmss"))) `
        -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $wd38 -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $wd38 "CLAUDE.md"),
    "# 项目规则`n本项目规定：所有回答必须用中文。", [System.Text.Encoding]::UTF8)
$lm38 = LogMark
$f38 = Join-Path $Here "_c38.txt"
Push-Location $wd38
$c38 = RunAgent "你好" $f38
Pop-Location
$t38 = Get-Content $f38 -Raw -Encoding UTF8
$d38 = LogDelta $lm38
Check ($c38 -eq 0)                 "C38 agent 正常跑完"                     "exit=$c38"
Check (Has $t38 '[记忆] 项目知识') "C38 ★★ CLAUDE.md 被加载进 L1 层（可观测）"
Check (Has $d38 '所有回答必须用中文') "C38 ★★★ 它真的进了 HTTP 请求（服务端收到的才是证据）"

ClearAfEnv

# ===========================================================================
#  C39–C45  ★ L11：子代理（环里套环）
#
#  ★ 本组用例的关键在**夹具的设计**：20 个文件里放两层标记
#      MARK-nn    = 文件名/结论层 —— 摘要里会出现它
#      PAYLOAD-nn = 正文/过程层  —— **只有真读过**才会进上下文
#    少了这层区分，"读过全文"和"知道有这个文件"在日志上长得一模一样，
#    断言会**同时放过两种情况** —— 那这条用例就白写了。
#
#  ★ 判据取自服务端新打的 `[EVID]` 行（kind / bodyBytes / payHits / hasTask）。
#    为什么不从 agent 的 stdout 断言：agent 说"我隔离了"是**自证**，
#    服务端收到的字节才是**证据**。（与 C38 同一条规矩。）
# ===========================================================================
Write-Output ""
Write-Output "[准备] 生成 20 个标记文件（L11 夹具）"
& $Py (Join-Path $Here "make_subagent_fixture.py") "_subfix" *>&1 | Out-File -FilePath (Join-Path $Here "_c39_fix.txt") -Encoding UTF8
Check ((Get-ChildItem (Join-Path $Here "_subfix\mark") -File -ErrorAction SilentlyContinue).Count -eq 20) `
      "C39 夹具：20 个标记文件已生成"

# ---- 解析 [EVID] 行的小工具 ----
function ParseEvid {
    param($deltaText)
    $rows = @()
    foreach ($line in ($deltaText -split "`n")) {
        if ($line -notmatch '\[EVID\]') { continue }
        $o = [ordered]@{}
        foreach ($kv in ($line -split '\s+')) {
            if ($kv -match '^(\w+)=(\S+)$') { $o[$Matches[1]] = $Matches[2] }
        }
        if ($o.Count -gt 0) { $rows += [pscustomobject]$o }
    }
    return @($rows)
}

Write-Output ""
Write-Output "[用例] C39 ★★★ 上下文隔离：探索过程**没有**进主上下文"
ClearAfEnv
$env:AF_MAP = "0"          # 关掉地图，免得它的体积混进 bodyBytes 的对比
$lm39 = LogMark
$f39 = Join-Path $Here "_c39.txt"
$c39 = RunAgent "派子代理去查一下 _subfix 下的标记文件" $f39
$t39 = Get-Content $f39 -Raw -Encoding UTF8
$d39 = LogDelta $lm39
$ev  = ParseEvid $d39

Check ($c39 -eq 0)                        "C39 agent 正常跑完（环里套环没把主流程搞崩）" "exit=$c39"
Check ($ev.Count -ge 22)                  "C39 ★ 服务端收到了主 + 子**两串**请求"      ("evid 行数=" + $ev.Count)

$mainRows = @($ev | Where-Object { $_.kind -eq "main" })
$subRows  = @($ev | Where-Object { $_.kind -eq "sub" })
Check ($mainRows.Count -ge 2)             "C39 主模型的请求 ≥2（派活 + 收摘要）"      ("main=" + $mainRows.Count)
Check ($subRows.Count -ge 20)             "C39 ★ 子代理自己跑了一串请求（环里套环）"  ("sub=" + $subRows.Count)

# ★★ 核心断言：过程层内容（PAYLOAD-）**一次都没出现在主请求里**
$mainPay = @($mainRows | Where-Object { [int]$_.payHits -gt 0 })
Check ($mainPay.Count -eq 0)              "C39 ★★★ 主上下文里 PAYLOAD 命中 0 次（过程没泄漏）" `
                                          ("main 里有 PAYLOAD 的请求数=" + $mainPay.Count)
$subMaxPay = ($subRows | ForEach-Object { [int]$_.payHits } | Measure-Object -Maximum).Maximum
Check ($subMaxPay -ge 20)                 "C39 ★★ 子代理上下文里 PAYLOAD 命中 ≥20 次（它真的读了）" `
                                          ("sub 最大 payHits=" + $subMaxPay)

Write-Output ""
Write-Output "[用例] C40 ★ 主上下文只胖了「一份摘要」"
$mainMaxBody = ($mainRows | ForEach-Object { [int]$_.bodyBytes } | Measure-Object -Maximum).Maximum
$subMaxBody  = ($subRows  | ForEach-Object { [int]$_.bodyBytes } | Measure-Object -Maximum).Maximum
Check ($subMaxBody -gt $mainMaxBody)      "C40 ★ 子代理的上下文比主的**胖**（胖子在它那边）" `
                                          ("main最大=$mainMaxBody sub最大=$subMaxBody")
Check ($mainMaxBody -lt ($subMaxBody / 2)) "C40 ★★ 主上下文不到子代理的一半（省下的就是隔离的价值）" `
                                          ("main=$mainMaxBody < sub/2=" + [int]($subMaxBody / 2))
Check (Has $t39 '[子代理]')               "C40 ★ 子代理的运行实况在 stdout 可见（可观测）"
$subTok = -1
if ($t39 -match 'subagent=(\d+)\s+run/(\d+)') { $subTok = [int]$Matches[2] }
Check ($subTok -gt 0)                     "C40 汇总行里能抠出子代理 token"           "subTok=$subTok"
$mainLoopTok = NumOf $t39 'tokens'         # 汇总行形如 "tokens=228 (in 192 / out 36)"
Check ($mainLoopTok -gt 0)                "C40 汇总行里能抠出主循环 token"           "mainTok=$mainLoopTok"

Write-Output ""
Write-Output "[用例] C41 ★ 子代理真的读了 20 个文件，并把绝对路径带回主上下文"
Check (Has $t39 '读了 20 个文件')         "C41 ★ 子代理读了 20 个文件"
Check (Has $t39 '【子代理摘要】')         "C41 ★ 摘要真的回传了（不是空的）"
# ★ 摘要契约第 3 条：绝对路径。I-72 在源码里断言契约文本含"绝对路径"；
#   这里断言**实际产出**里也有 —— 契约写在那儿没人看，两端都测才叫守住。
Check (Has $d39 'MARK-01.txt')            "C41 ★★ 摘要里的绝对路径真的回到了主请求（服务端收到）"

Write-Output ""
Write-Output "[用例] C42 ★★ K-40：子代理的 tools 里没有 Task（不给递归能力）"
$subHasTask = @($subRows | Where-Object { $_.hasTask -eq "1" })
Check ($subHasTask.Count -eq 0)           "C42 ★★ 子代理的 tools 里**没有** Task（递归深度恒为 1）" `
                                          ("带 Task 的子请求数=" + $subHasTask.Count)
$mainHasTask = @($mainRows | Where-Object { $_.hasTask -eq "1" })
Check ($mainHasTask.Count -ge 1)          "C42 ★ 主模型的 tools 里**有** Task（否则上一条没意义）" `
                                          ("带 Task 的主请求数=" + $mainHasTask.Count)

Write-Output ""
Write-Output "[用例] C43 ★ 子代理只有只读工具"
$subToolCounts = @($subRows | ForEach-Object { [int]$_.tools } | Sort-Object -Unique)
Check (($subToolCounts | Where-Object { $_ -gt 1 }).Count -eq 0) `
                                          "C43 ★ 子代理的 tools 数 ≤1（Write/Edit/Bash 都被过滤掉）" `
                                          ("子请求里出现过的 tools 数=" + ($subToolCounts -join ","))
Check (($mainRows | ForEach-Object { [int]$_.tools } | Measure-Object -Minimum).Minimum -ge 4) `
                                          "C43 ★ 主模型的 tools 明显更多（对比才有意义）"

Write-Output ""
Write-Output "[用例] C44 ★★ 成本记账：token 不进上下文，但钱要记到主会话"
Check (Has $t39 'subagent=')              "C44 ★ 汇总行里单列了子代理成本（K-42）"
# ★ 子代理的 token 花在它自己的 loop 上，主循环的 spentTokens 一个数都看不到。
#   不单独记账 → /cost 报出来的数字是**假的**。
Check ($subTok -gt $mainLoopTok)          "C44 ★★ 子代理 token 远大于主循环（不记账就会漏掉大头）" `
                                          ("subTok=$subTok > mainTok=$mainLoopTok")

Write-Output ""
Write-Output "[用例] C45 ★ 子代理超步是**返回值**，不是崩溃"
ClearAfEnv
$env:AF_MAP = "0"
$env:AF_SUBAGENT_STEPS = "3"      # 只读 20 个文件的任务只给 3 步 → 必然超步
$lm45 = LogMark
$f45 = Join-Path $Here "_c45.txt"
$c45 = RunAgent "派子代理去查一下 _subfix 下的标记文件" $f45
$t45 = Get-Content $f45 -Raw -Encoding UTF8
Check ($c45 -eq 0)                        "C45 ★★ 子代理超步时主流程仍然正常结束（CR-2）" "exit=$c45"
Check (Has $t45 '未完成')                 "C45 ★ 失败被**报出来**了（不是静默截断）"
Check (Has $t45 '已获得的部分信息')       "C45 ★★ 已有的部分信息仍然回传（不浪费花掉的钱）"
Check (Has $t45 'completed')              "C45 ★ 主任务的 outcome 仍是 completed（子代理失败 ≠ 主任务失败）"
$env:AF_SUBAGENT_STEPS = ""

ClearAfEnv

Stop-Process -Id $mockProc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# ===========================================================================
#  C15  ★ 改动清单核对
#
#  L6 的 C15 是"零改动：本讲声明的没动的文件真的没动"。
#  L7 **必须**动代码（压缩是新模块），所以那个用例不能照抄 ——
#  照抄的话只有两种结果：要么它失败（因为确实动了），
#  要么你把类型删掉让 bug 回来，只为了让它通过。
#
#  L7 把它升级成**三张清单**（都在下面写死，一张都不能悄悄改）：
#    ① 零改动：一个字节都不许变
#    ② 已改动：必须**真的**变了（如果它没变，说明你以为改了却没改）
#    ③ 新增：上一讲里必须**不存在**（否则不是新增，是搬过来的）
#
#  ★ 第 ② 条比 L6 的第 ① 条更有用：它抓的是"改动清单是照抄上一讲、
#    而不是这一讲真实发生的"这类错误 —— 而这类错误在验收时最难发现，
#    因为它长得像"一切都对"。
#
#  ★★ L9 的清单必须**自己重算**，不能照抄 L8 的。
#    照抄的话它照样会通过（因为 L8 那几个文件在 L9 里也确实没动 / 确实存在），
#    但那样它就变成了"上一讲的清单"，而这一类错误正是 C15-② 存在的理由：
#    **长得像"一切都对"的东西，最需要核对**。
#
#  ★ L10 的清单本身也是一条结论：**接一个模块（⑫ 会话存储）进来，生产代码只动了 1 个文件**。
#    main.cpp（三个命令 + 接线 + 检查点 + 项目知识）。
#    **build.ps1 本讲回到零改动** —— 它 glob 收 .cpp，新模块自动进构建，
#    这正是"构建脚本不要写死文件清单"的价值（L10 一次都没为它操心）。
#    types.hpp 与其余 30 个文件一个字节都没动。
#
#  ★ 四讲的对照：
#      L8  接 tree-sitter → 生产代码动 3 个（main.cpp / build.ps1 / mock_server.py）
#      L9  接 FTXUI      → 生产代码动 3 个（main.cpp / build.ps1 / mock_server.py 归位）
#      L10 接会话存储    → 生产代码动 **1 个**（只有 main.cpp）
#      L11 接子代理      → 生产代码动 **3 个**（main.cpp / tool_registry.{hpp,cpp}）
#    L11 比 L10 多，是因为它**兑现了一个四讲前预留的落点**：
#    `readOnlyView()` 从 L4 起就作为一行注释挂在 tool_registry.hpp 里。
#    ★ 这正好说明"预留落点"不是偷懒：它把"将来要改哪儿"变成了已知，
#      于是真到那一天，改动范围是可以**预测**的 —— 这也是 C15 能提前写死清单的原因。
#
#  ★★ 另外：L9 收尾时我犯过一次错 —— **照抄了 L8 的清单，结果 162 条照样全绿**。
#    因为"L8 改过的文件在 L9 里也确实没动"，抄来的清单依然成立。
#    所以此后每讲第一件事就是把基线改成**上一讲**：`$L8`→`$L9`→`$L10`。
# ===========================================================================
Write-Output ""
Write-Output "[用例] C15 ★ 改动清单核对（零改动 / 已改动 / 新增 三张清单，基线 = L10）"
$Prev = Join-Path (Split-Path (Split-Path $Here -Parent) -Parent) "L10-v8能存档\code"

$zeroChange = @(
    "http_transport.hpp","sse_parser.hpp","sse_parser.cpp","http_winhttp.cpp",
    "sandbox.hpp","sandbox_win.cpp","model_provider.hpp","plugin.hpp","plugin.cpp",
    "agent_loop.hpp","agent_loop.cpp",
    "compaction.hpp","compaction.cpp","context_budget.hpp","context_budget.cpp",
    "providers.cpp","types.hpp","mock_target.txt",
    "repo_index.hpp","repo_index.cpp","symbol_extract.hpp","symbol_extract.cpp",
    "session_store.hpp","session_store.cpp",
    "build.ps1",
    "ui\tui.hpp","ui\tui_ftxui.cpp","ui\tui_plain.cpp",
    "fetch_ftxui.ps1","build_ftxui.ps1","fetch_treesitter.ps1",
    "make_fixture.py","make_session_fixture.py"
)
# ★ 本讲改了 4 个：main.cpp（接线/自检）+ tool_registry.{hpp,cpp}（兑现 readOnlyView）
#   + verify.ps1。mock_server.py **也动了**（加了 subagent 模式与 [EVID] 证据行）——
#   它必须如实写在这里：C15 的价值就在于和现实对得上。
$changed = @("main.cpp","verify.ps1","tool_registry.hpp","tool_registry.cpp","mock_server.py")
$added = @("subagent.hpp","subagent.cpp","make_subagent_fixture.py")

$diffZero = @()
foreach ($fn in $zeroChange) {
    $a = Join-Path $Prev $fn
    $b = Join-Path $Here $fn
    if (-not (Test-Path $a)) { $diffZero += "$fn(上一讲缺)"; continue }
    $ha = (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash
    $hb = (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
    if ($ha -ne $hb) { $diffZero += $fn }
}
Check ($diffZero.Count -eq 0) "C15-① $($zeroChange.Count) 个「零改动」文件真的零改动" ("差异: " + ($diffZero -join ", "))

# ★ 单独拎出来：types.hpp 是契约层。它已在 L8、L9、L10、L11 **连续四讲**零改动 ——
#   这才是 CR-3 被真正守住的证据（一次不动可能是巧合，四次不动是规矩生效了）。
$ta = Join-Path $Prev "types.hpp"; $tb = Join-Path $Here "types.hpp"
$th = (Get-FileHash -LiteralPath $ta -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $tb -Algorithm SHA256).Hash
Check $th "C15-①b ★ 契约层 types.hpp 连续四讲零改动（CR-3）"

# ★★ C15-② 是这三张清单里**最有用**的一张：
#    它抓的是"本讲声称改了、其实没改"。
#    ★ 光靠哈希是不够的：如果有人照抄上一讲的清单，而那些文件本讲确实也没动，
#      这一条照样通过（L9 就栽过）。所以它必须和"本讲新增的用例"配合 ——
#      C42（子代理 tools 里没有 Task）只能在 tool_registry.cpp 真改了之后才会通过。
$diffChanged = @()
foreach ($fn in $changed) {
    $a = Join-Path $Prev $fn
    $b = Join-Path $Here $fn
    if (-not (Test-Path $a)) { continue }
    $ha = (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash
    $hb = (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
    if ($ha -eq $hb) { $diffChanged += $fn }
}
Check ($diffChanged.Count -eq 0) "C15-② $($changed.Count) 个「已改动」文件确实都变了" ("没变: " + ($diffChanged -join ", "))

$badAdded = @()
foreach ($fn in $added) {
    $b = Join-Path $Here $fn
    $a = Join-Path $Prev $fn
    if (-not (Test-Path $b)) { $badAdded += "$fn(本讲缺)"; continue }
    if (Test-Path $a) { $badAdded += "$fn(上一讲已存在)" }
}
Check ($badAdded.Count -eq 0) "C15-③ $($added.Count) 个「新增」文件确实是新的" ("问题: " + ($badAdded -join ", "))

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

foreach ($f in @($f1,$f3,$f4,$f5,$f7,$f8,$f9,$f10,$f11,$f12,$f13,$f14,$f20,$f21,$f26)) {
    Write-Output ""
    Write-Output "----- $(Split-Path -Leaf $f) -----"
    Get-Content $f -Encoding UTF8 | Select-Object -First 40 | Write-Output
}

# ★ 压缩相关的三个用例只打**摘要行**和压缩事件 —— 全文有几百行噪音
foreach ($f in @($f16,$f17,$f18,$f19)) {
    Write-Output ""
    Write-Output "----- $(Split-Path -Leaf $f)（摘要行 + 压缩事件）-----"
    $lines = Get-Content $f -Encoding UTF8
    $lines | Where-Object { $_ -match '^(=====|\[压缩\]|compact=)' } | Select-Object -First 25 | Write-Output
    $lines | Where-Object { $_ -match '★ 交接摘要里记着|★ 交接摘要里保住|汇总完成：' } | Select-Object -First 5 | Write-Output
}

if ($script:fail -gt 0) { exit 1 }
exit 0
