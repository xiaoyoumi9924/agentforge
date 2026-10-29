# ---------------------------------------------------------------------------
#  L4 · verify.ps1 —— v1「能读」的端到端验证（不需要真 API Key）
#
#  验证思路与 L3 一致：**协议层 + 工具层的正确性不依赖密钥。**
#  假服务端真的发 tool_calls（参数切 4 段、finish_reason 同帧），
#  我们要证明的是：客户端①把意图完整取出来 ②真的执行了工具
#  ③把结果回喂给了模型 ④把 assistant.tool_calls 与 tool 结果都存回了历史。
#
#  五个用例：
#    C1  Read        → 模型主动读文件；工具结果回喂后模型用它作答
#    C2  WordCount   → C ABI 插件被加载、经权限门批准、真的执行
#    C3  非法参数     → 缺 path 的调用被参数校验拦下并回喂
#    C4  未知工具     → 未知工具被回喂"可用工具列表"
#    C5  未加载插件   → 同一个 WordCount 变"未知工具"（反证插件是唯一来源）
#    C6  权限被拒     → 用户拒绝后工具**不执行**，拒绝也作为 observation 回喂
#
#  用法：
#      .\verify.ps1            # 需要先 .\build.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Proj = Split-Path -Parent $Here
Set-Location $Here
$Exe  = Join-Path $Proj "build\agent.exe"
$Dll  = Join-Path $Proj "build\plugins\sample_plugin.dll"
$Mock = Join-Path $Here "mock_server.py"
$Port = 8789
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
        # ★ 有的工具会命中权限门的 Ask 分支 —— 必须显式回答，
        #   否则 stdin 为空 → confirm() 拿到 EOF → 返回 false → 拒绝执行。
        #   这不是 bug，是 fail-closed（没有明确批准就不执行）。
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
if (-not (Test-Path $Dll)) {
    Write-Output "[准备] 插件还没编 —— 先跑 .\plugins\build_plugin.ps1"
    & (Join-Path $Proj "plugins\build_plugin.ps1")
    if (-not (Test-Path $Dll)) { Write-Output "FATAL: 插件构建失败"; exit 1 }
}

Write-Output "================================================"
Write-Output "  L4 验证：v1 能读"
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

# ---- C1 模型主动读文件 -----------------------------------------------------
Write-Output ""
Write-Output "[用例] C1 Read：模型主动读文件"
$env:AF_PLUGIN = ""
$f1 = Join-Path $Here "_c1.txt"
$c1 = RunAgent "请读取 mock_target.txt 并告诉我里面有哪些函数" $f1
$t1 = Get-Content $f1 -Raw -Encoding UTF8
Check ($c1 -eq 0)                          "C1 退出码为 0"                  "exit=$c1"
Check (Has $t1 '[模型请求工具] Read')       "C1 模型发出了 Read 工具调用"
Check (Has $t1 '[工具返回]')                "C1 工具真的被执行了"
Check (Has $t1 'toolCalls=1')              "C1 恰好一次工具调用"
Check (Has $t1 'int add')                  "C1 文件内容被回喂、并体现在最终回答里"
Check (Has $t1 'history=4')                "C1 历史 4 条：user / assistant(tool_calls) / tool / assistant"

# ---- C2 插件（中文路径 + 经权限门批准）------------------------------------
Write-Output ""
Write-Output "[用例] C2 WordCount：C ABI 插件（路径含中文，需批准）"
$env:AF_PLUGIN = $Dll
$f2 = Join-Path $Here "_c2.txt"
# ★ 回答 "y" —— WordCount 没有命中任何权限规则，落到 fallback=Ask
$c2 = RunAgent "请用 WordCount 统计 mock_target.txt" $f2 "y"
$t2 = Get-Content $f2 -Raw -Encoding UTF8
Check ($c2 -eq 0)                            "C2 退出码为 0"                "exit=$c2"
Check (Has $t2 '[插件] 已加载')               "C2 插件被加载（中文路径 OK）"
Check (Has $t2 '[模型请求工具] WordCount')    "C2 插件工具出现在了工具表里"
Check (Has $t2 '允许?')                       "C2 权限门走了 Ask 分支（端口 ④ 在场）"
Check (Has $t2 '[工具返回]')                  "C2 批准后插件被真的调用"
Check (Has $t2 '行数')                        "C2 返回值来自插件实现（不是宿主伪造）"
Check (Has $t2 'toolCalls=1')                "C2 恰好一次工具调用"

# ---- C3 非法参数 -----------------------------------------------------------
Write-Output ""
Write-Output "[用例] C3 非法参数：缺 path 的 Read 调用"
$env:AF_PLUGIN = ""
$f3 = Join-Path $Here "_c3.txt"
$c3 = RunAgent "调用 Read 但故意缺少 path 参数" $f3
$t3 = Get-Content $f3 -Raw -Encoding UTF8
Check (Has $t3 '[工具失败]')                  "C3 失败被当成 observation 回喂（不是抛异常）"
Check (Has $t3 '缺少必需字段')                "C3 错误信息能指导修正"
Check (Has $t3 'toolCalls=1')                "C3 调用计数正确"
Check ($c3 -eq 0)                            "C3 循环没有崩，正常收敛"        "exit=$c3"

# ---- C4 未知工具 -----------------------------------------------------------
Write-Output ""
Write-Output "[用例] C4 未知工具：模型幻觉出一个不存在的工具"
$f4 = Join-Path $Here "_c4.txt"
$c4 = RunAgent "请调用一个不存在的工具 NoSuchTool 看看" $f4
$t4 = Get-Content $f4 -Raw -Encoding UTF8
Check (Has $t4 '[工具失败]')                  "C4 未知工具被判为失败而非崩溃"
Check (Has $t4 '未知工具')                    "C4 明确告知是未知工具"
Check (Has $t4 '可用工具')                    "C4 一并回喂可用工具列表（模型下一轮能改对）"

# ---- C5 未加载插件时 WordCount 不存在（反证）--------------------------------
Write-Output ""
Write-Output "[用例] C5 未加载插件：同一个 WordCount 变未知工具"
$env:AF_PLUGIN = ""
$f5 = Join-Path $Here "_c5.txt"
$c5 = RunAgent "请用 WordCount 统计 mock_target.txt" $f5
$t5 = Get-Content $f5 -Raw -Encoding UTF8
Check (Has $t5 '未知工具')                    "C5 未加插件时 WordCount 不可见"
Check (Has $t5 '可用工具: Read')              "C5 可用工具列表里只有 Read（反证插件是唯一来源）"

# ---- C6 权限被拒（fail-closed）---------------------------------------------
Write-Output ""
Write-Output "[用例] C6 权限被拒：用户回答 n，工具不得执行"
$env:AF_PLUGIN = $Dll
$f6 = Join-Path $Here "_c6.txt"
$c6 = RunAgent "请用 WordCount 统计 mock_target.txt" $f6 "n"
$t6 = Get-Content $f6 -Raw -Encoding UTF8
Check (Has $t6 '用户拒绝了这次操作')          "C6 拒绝被作为 observation 回喂"
Check ((-not (Has $t6 '行数')))               "C6 ★ 被拒后插件没有被执行（fail-closed）"
Check ($c6 -eq 0)                            "C6 拒绝不导致崩溃"              "exit=$c6"

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

Check ((-not (Has $joined 'BAD JSON')))            "服务端未收到坏 JSON"           "argv 编码正确"
Check (Has $joined 'model=mock-1')                 "请求里的 model 是真实模型名"    "没退化成厂商名"
Check (Has $joined 'stream=True')                  "请求走的是流式"
Check (Has $joined "tools=1 ['Read']")             "C1 下发了 1 个工具（Read）"     "不下发 tools 模型永远不会调工具"
Check (Has $joined "tools=2 ['Read', 'WordCount']") "C2 插件工具被追加、且顺序稳定" "Read 在前，WordCount 在后"
Check (Has $joined 'tool_calls=["Read"]')          "assistant.tool_calls 被原样存回历史并发回"
Check (Has $joined 'tool_call_id=call_read_1')     "tool 结果带 tool_call_id 配对回填"
Check (Has $joined '[MODE] final')                 "第二轮确实看到了工具结果"

Write-Output ""
Write-Output "================================================"
Write-Output ("  结果：{0} 通过 / {1} 失败" -f $script:pass, $script:fail)
Write-Output "================================================"

foreach ($f in @($f1, $f2, $f3, $f4, $f5, $f6)) {
    Write-Output ""
    Write-Output "----- $(Split-Path -Leaf $f) -----"
    Get-Content $f -Encoding UTF8 | Write-Output
}

if ($script:fail -gt 0) { exit 1 }
exit 0

