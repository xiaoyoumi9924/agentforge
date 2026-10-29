# ---------------------------------------------------------------------------
#  web_probe.ps1 —— 不打开浏览器，也能验证 Web UI 后端
#
#  ★ 为什么要有它：
#    题目八的验收要"浏览器里点允许"，但"点"这个动作不该成为**唯一的**证据。
#    真正要证明的是三件事：
#      ① HTTP 能返回那一页（GET /）；
#      ② WebSocket 上能收到事件流（hello / replay / text / toolStart / ...）；
#      ③ 回一条 permAnswer，Agent 那边真的继续/真的拒绝。
#    这三件事全都可以用 ClientWebSocket 直接断言 —— 浏览器只是把它画出来。
#    （这与 L9 用 --ui-demo 把"工具执行的 1.2 秒空窗"写成脚本是同一个道理：
#      **把时序/交互写成输入，而不是交给运气**。）
#
#  用法：
#    .\web_probe.ps1                                  # 起 --ui-demo，断言事件流
#    .\web_probe.ps1 -AgentArgs '"权限：列目录"' -Answer allow   # 走一次真实权限往返
#    .\web_probe.ps1 -NoLaunch -SendTask '读一下 mock_target.txt'  # 连已在跑的 --web
# ---------------------------------------------------------------------------
param(
  [string]   $Exe       = "",
  [string[]] $AgentArgs = @("--ui-demo"),
  [int]      $Port      = 8080,
  [string]   $Answer    = "",        # "" | "allow" | "deny"
  [string]   $SendTask  = "",        # 连上后替人"输入任务"
  [switch]   $CloseOnPerm,           # 收到权限弹窗就**关掉页面**（验证"关页面=拒绝"）
  [int]      $TimeoutSec = 30,
  [switch]   $NoLaunch,
  [string]   $OutJson   = ""
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $Here "build\agent.exe" }

$env:AF_UI       = "web"
$env:AF_WEB_PORT = "$Port"

$proc    = $null
$logOut  = Join-Path $env:TEMP "af_web_out.txt"
$logErr  = Join-Path $env:TEMP "af_web_err.txt"
$msgs    = New-Object System.Collections.ArrayList
$reply   = $false
$fail    = 0

function Say($s) { Write-Output $s }

# ---- 1. 起进程（或用正在跑的那个）----------------------------------------
if (-not $NoLaunch) {
  Say "[probe] 启动: $Exe $($AgentArgs -join ' ')"
  $proc = Start-Process -FilePath $Exe -ArgumentList $AgentArgs -WorkingDirectory $Here `
            -PassThru -NoNewWindow -RedirectStandardOutput $logOut -RedirectStandardError $logErr
}

# ---- 2. HTTP：GET / 必须是一页 HTML --------------------------------------
$html = $null
for ($i = 0; $i -lt 100; $i++) {
  try {
    $html = (Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2).Content
    break
  } catch { Start-Sleep -Milliseconds 100 }
}
if (-not $html) { Say "[probe] FAIL  HTTP 首页拿不到（端口 $Port）"; exit 1 }
Say ("[probe] HTTP 首页 {0} 字符" -f $html.Length)
if ($html -notmatch "<title>Lumin") { Say "[probe] FAIL  首页不是我们的 HTML"; $fail++ }

# ---- 3. WebSocket 连接 ----------------------------------------------------
$ws = $null
for ($i = 0; $i -lt 50; $i++) {
  try {
    $ws = [System.Net.WebSockets.ClientWebSocket]::new()
    # ★ 像真浏览器一样带上 Origin：握手会校验它（跨站页面必须被拒，I-103）。
    #   探针从这里连，所以用同一个 $Port 拼来源，永远判定为"本机同端口"。
    $ws.Options.SetRequestHeader("Origin", "http://127.0.0.1:$Port")
    $ws.ConnectAsync([uri]"ws://127.0.0.1:$Port/ws", [Threading.CancellationToken]::None).Wait(2000) | Out-Null
    break
  } catch { $ws = $null; Start-Sleep -Milliseconds 100 }
}
if (-not $ws) { Say "[probe] FAIL  WebSocket 连不上"; exit 1 }
Say "[probe] WebSocket 已连接 ws://127.0.0.1:$Port/ws"

$buf = New-Object byte[] 65536

function Send-Text([string]$text) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($text)
  $seg   = [ArraySegment[byte]]::new($bytes)
  $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true,
                [Threading.CancellationToken]::None).Wait(3000) | Out-Null
}

if ($SendTask) {
  Start-Sleep -Milliseconds 200
  Send-Text (@{ t = "task"; s = $SendTask } | ConvertTo-Json -Compress)
  Say "[probe] 已提交任务: $SendTask"
}

# ---- 4. 收消息 ------------------------------------------------------------
#  ★ 为什么不用 CancellationTokenSource 做"单条超时"：
#    .NET 的 ReceiveAsync 一旦被取消，会**中止整条 WebSocket 连接**
#    （不是取消这一次读）。用它做超时，表现是"收到几条之后就莫名断线"。
#    所以这里改成"一个待决的 Task + 轮询 Wait"：超时只影响我们的循环，
#    不影响连接。
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$idle = 0
while ((Get-Date) -lt $deadline) {
  $seg  = [ArraySegment[byte]]::new($buf)
  $task = $ws.ReceiveAsync($seg, [Threading.CancellationToken]::None)

  while (-not $task.IsCompleted -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 30
  }
  if (-not $task.IsCompleted) { break }              # 总体超时

  try { $r = $task.GetAwaiter().GetResult() } catch { break }
  if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { break }

  $raw = [Text.Encoding]::UTF8.GetString($buf, 0, $r.Count)
  if (-not $r.EndOfMessage) { continue }             # 分片：这里不做拼接
  if (-not $raw) { continue }

  # replay 里是一整串事件：拆开发，后面的断言对每个事件都成立
  try { $obj = $raw | ConvertFrom-Json } catch { continue }
  if ($obj.t -eq "replay") {
    foreach ($e in $obj.e) { [void]$msgs.Add($e) }
  } else {
    [void]$msgs.Add($obj)
  }

  if ($obj.t -eq "perm" -and -not $reply) {
    if ($CloseOnPerm) {
      # ★ 模拟"用户直接把浏览器关了"：不回答，直接断开。
      #   Agent 必须当场按拒绝处理并继续跑完，而不是卡在那里等一个永远不会来的回答。
      Say "[probe] 收到权限弹窗 → 直接关掉页面（不回答）"
      $ws.Dispose()
      break
    }
    if ($Answer) {
      $allow = ($Answer -eq "allow")
      Send-Text (@{ t = "permAnswer"; id = $obj.id; allow = $allow } | ConvertTo-Json -Compress)
      $reply = $true
      Say ("[probe] 已回答权限 #{0} → {1}" -f $obj.id, $Answer)
    }
  }
}

$ws.Dispose()

# ---- 5. 汇总 --------------------------------------------------------------
$types = $msgs | ForEach-Object { $_.t } | Group-Object | Sort-Object Name
Say ""
Say "[probe] 收到 $($msgs.Count) 条事件："
$types | ForEach-Object { Say ("         {0,-10} x{1}" -f $_.Name, $_.Count) }

if ($OutJson) {
  $msgs | ConvertTo-Json -Depth 8 | Set-Content -Path $OutJson -Encoding UTF8
  Say "[probe] 事件已存 $OutJson"
}

# ---- 6. 断言 --------------------------------------------------------------
function Need([bool]$ok, [string]$why) {
  if ($ok) { Say "  PASS  $why" } else { Say "  FAIL  $why"; $script:fail++ }
}

$seen = @{}
foreach ($m in $msgs) { $seen[$m.t] = $true }

Need ([bool]$seen["hello"])                    "收到 hello（后端确实在跑）"
Need ([bool]$seen["text"])                     "收到 text（流式正文）"
Need ([bool]($seen["toolStart"] -and $seen["toolDone"])) "收到工具卡片事件（toolStart/toolDone）"
if ($Answer) {
  Need ([bool]$seen["perm"])                   "收到权限弹窗事件 perm"
  Need ([bool]$reply)                          "确实把 permAnswer 发了回去"
  Need ([bool]$seen["permEnd"])                "服务端回填了 permEnd（弹窗关闭）"
}

if ($proc) {
  if (-not $proc.HasExited) { $proc.WaitForExit(8000) | Out-Null }
  if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
  Say ""
  Say "[probe] agent 输出（前 25 行）："
  Get-Content $logOut -Encoding UTF8 -ErrorAction SilentlyContinue |
    Select-Object -First 25 | ForEach-Object { Say "         $_" }
}

Say ""
if ($fail -gt 0) { Say "[probe] 结论：$fail 项失败"; exit 1 }
Say "[probe] 结论：全部通过"
exit 0
