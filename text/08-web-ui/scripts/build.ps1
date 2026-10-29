# ---------------------------------------------------------------------------
#  build.ps1 —— 通用构建脚本（每一讲都用这一份，直接复制过去即可）
#
#  为什么不用 CMake：
#    PowerShell 里不能调 cmd.exe，所以跑不了 vcvarsall.bat；
#    本机 CMake 对 VS 2026 工具集识别也有问题（见 09-核心代码/README.md）。
#    于是手动配好 INCLUDE / LIB / PATH，直接调 cl.exe —— 单层进程，最稳。
#
#  用法：
#     .\build.ps1
#     .\build.ps1 -Out myserver.exe -W4
#
#  输出名按讲次调整：L1 = agentforge.exe，L2 = contract_check.exe，L3+ = agent.exe
# ---------------------------------------------------------------------------
param(
  [string]$Out  = "agent.exe",
  [switch]$W4,                     # 打开 /W4（默认 /W3）
  [switch]$NoOptimize              # 默认 /O2，调试时关掉
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# ---- 1. 定位 MSVC ------------------------------------------------------------
# 优先用 vswhere（官方机制，能正确处理 version\edition 目录层级和 Build Tools）；
# 找不到 vswhere 再退化为遍历目录。
$MsvcDir = $null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  # 本机补丁：vswhere 会把 "Error 0x80070005: 拒绝访问。" 也当成一行输出返回，必须先滤掉
  $inst = @(& $vswhere -latest -products * -property installationPath 2>$null) |
          Where-Object { $_ -and (Test-Path $_) } |
          ForEach-Object { if (Test-Path (Join-Path $_ 'VC\Tools\MSVC')) { $_ } } |
          Select-Object -First 1
  if ($inst) {
    $vcTools = Join-Path $inst "VC\Tools\MSVC"
    if (Test-Path $vcTools) {
      $pick = Get-ChildItem $vcTools -Directory | Sort-Object Name -Descending | Select-Object -First 1
      if ($pick) { $MsvcDir = $pick.FullName }
    }
  }
}

if (-not $MsvcDir) {
  # 退化路径：VS 的安装布局是 <root>\<version>\<edition>\VC\Tools\MSVC\<ver>
  # 所以我们把 root、version、edition 三层都当作候选探测一遍。
  $probe = @()
  foreach ($root in @("C:\Program Files\Microsoft Visual Studio",
                      "C:\Program Files (x86)\Microsoft Visual Studio")) {
    if (-not (Test-Path $root)) { continue }
    $probe += $root
    $lv1 = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue
    $probe += ($lv1 | ForEach-Object { $_.FullName })
    $probe += ($lv1 | ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
               ForEach-Object { $_.FullName })
  }
  $pick = $probe | ForEach-Object { Join-Path $_ "VC\Tools\MSVC" } |
          Where-Object { Test-Path $_ } |
          ForEach-Object { Get-ChildItem $_ -Directory } |
          Sort-Object Name -Descending | Select-Object -First 1
  if ($pick) { $MsvcDir = $pick.FullName }
}

if (-not $MsvcDir) {
  Write-Output "FATAL: 找不到 MSVC。请安装 Visual Studio 并勾选「使用 C++ 的桌面开发」工作负载。"
  exit 1
}
Write-Output "MSVC: $MsvcDir"

# ---- 2. 定位 Windows SDK -----------------------------------------------------
# 本机补丁：注册表 -> 常见安装位置 -> NuGet 免安装 SDK（AgentForge\thirdparty\winsdk）
$SdkIncludeDirs = @()
$SdkLibDirs     = @()

$sdkRoots = @()
$kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if ($kitsRoot) { $sdkRoots += $kitsRoot.TrimEnd('\') }
$sdkRoots += @("C:\Program Files (x86)\Windows Kits\10", "C:\Program Files\Windows Kits\10", "D:\Windows Kits\10")

foreach ($sdkRoot in $sdkRoots) {
  $incRoot = Join-Path $sdkRoot 'Include'
  if (-not (Test-Path $incRoot)) { continue }
  $sdkVer = Get-ChildItem $incRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
  if (-not $sdkVer) { continue }
  if (-not (Test-Path (Join-Path $sdkRoot "Lib\$($sdkVer.Name)\um\x64"))) { continue }
  $SdkIncludeDirs = @("$sdkRoot\Include\$($sdkVer.Name)\ucrt", "$sdkRoot\Include\$($sdkVer.Name)\um", "$sdkRoot\Include\$($sdkVer.Name)\shared")
  $SdkLibDirs     = @("$sdkRoot\Lib\$($sdkVer.Name)\ucrt\x64", "$sdkRoot\Lib\$($sdkVer.Name)\um\x64")
  Write-Output "SDK: $sdkRoot ($($sdkVer.Name))"
  break
}

if (-not $SdkIncludeDirs) {
  $cand = @()
  if ($env:AF_WINSDK) { $cand += $env:AF_WINSDK }
  $dir  = $Here
  for ($i = 0; $i -lt 7; $i++) {
    if (-not $dir) { break }
    $cand += (Join-Path $dir 'thirdparty\winsdk')
    $cand += (Join-Path $dir 'AgentForge\thirdparty\winsdk')
    $cand += (Join-Path $dir '03-AgentForge\thirdparty\winsdk')   # 工作区按 01~99 重整后的位置
    $parent = Split-Path -Parent $dir
    if ($parent -eq $dir) { break }
    $dir = $parent
  }
  foreach ($winsdk in ($cand | Select-Object -Unique)) {
    if (-not (Test-Path $winsdk)) { continue }
    $basePkg = Get-ChildItem $winsdk -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.*' -and $_.Name -notlike '*.x64.*' } |
               Select-Object -First 1
    $x64Pkg  = Get-ChildItem $winsdk -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.x64.*' } |
               Select-Object -First 1
    if (-not ($basePkg -and $x64Pkg)) { continue }
    $incVer = Get-ChildItem (Join-Path $basePkg.FullName 'c\Include') -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if (-not $incVer) { continue }
    $SdkIncludeDirs = @((Join-Path $incVer.FullName 'ucrt'), (Join-Path $incVer.FullName 'um'), (Join-Path $incVer.FullName 'shared'))
    $SdkLibDirs     = @((Join-Path $x64Pkg.FullName 'c\ucrt\x64'), (Join-Path $x64Pkg.FullName 'c\um\x64'))
    Write-Output "SDK: NuGet ($($basePkg.Name))"
    break
  }
}

if (-not $SdkIncludeDirs) {
  Write-Output "FATAL: 找不到 Windows SDK（系统安装与 NuGet 兜底都没找到）"
  exit 1
}

# ---- 3. 配环境 ---------------------------------------------------------------
$env:PATH    = "$MsvcDir\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = (@("$MsvcDir\include") + $SdkIncludeDirs) -join ';'
$env:LIB = (@("$MsvcDir\lib\x64") + $SdkLibDirs) -join ';'

$cl = "$MsvcDir\bin\Hostx64\x64\cl.exe"
if (-not (Test-Path $cl)) { Write-Output "FATAL: cl.exe not found at $cl"; exit 1 }

# ---- 4. 找 nlohmann/json -----------------------------------------------------
# 从当前目录向上找最多 4 层，只要有 thirdparty\nlohmann\json.hpp 就用它。
# （L1 用不到 JSON，找不到也正常继续。）
$Includes = @($Here)
$probe = $Here
for ($i = 0; $i -lt 5 -and $probe; $i++) {
  $tp = Join-Path $probe "thirdparty"
  if (Test-Path (Join-Path $tp "nlohmann\json.hpp")) { $Includes += $tp; break }
  $probe = Split-Path -Parent $probe
}

# ---- 4b. 找 tree-sitter（可选依赖）------------------------------------------
# 没有包管理器也能接：fetch_treesitter.ps1 把源码摆到 thirdparty\tree-sitter，
# 这里直接用 cl.exe 编那三个 C 文件（core/src/lib.c + cpp/src/{scanner,parser}.c）。
# 源码不在场 → 不定义 AF_HAVE_TREESITTER → symbol_extract.cpp 自动走词法兜底。
$TsObjs = @()
$TsInc  = @()
$TsDef  = @()

$TsRoot  = Join-Path $Here "thirdparty\tree-sitter"
$TsApi   = Join-Path $TsRoot "core\include\tree_sitter\api.h"
$TsLibC  = Join-Path $TsRoot "core\src\lib.c"
$TsCppP  = Join-Path $TsRoot "cpp\src\parser.c"

if ((Test-Path $TsApi) -and (Test-Path $TsLibC) -and (Test-Path $TsCppP)) {
  $coreInc = Join-Path $TsRoot "core\include"
  $coreSrc = Join-Path $TsRoot "core\src"
  $cppSrc  = Join-Path $TsRoot "cpp\src"
  $TsInc   = @($coreInc, $coreSrc, $cppSrc)
  $TsDef   = @('/DAF_HAVE_TREESITTER')
  Write-Output "tree-sitter: 已就位（cpp/src/parser.c = $((Get-Item $TsCppP).Length) bytes）"
} else {
  Write-Output "tree-sitter: 未就位 -> 退回词法抽取（跑 .\fetch_treesitter.ps1 可启用）"
}

# ---- 4c. 找 FTXUI（可选依赖）-----------------------------------------------
#  ★ 与 tree-sitter 的处理**不一样**：
#    tree-sitter 只 3 个 C 文件（2.8s）→ 每次全量重编无所谓；
#    FTXUI 有 77 个 .cpp（85.5s）→ 必须"编一次、存成 .lib、之后只链接"（K-33）。
#    所以这里**不编译**，只找 build_ftxui.ps1 缓存好的 ftxui.lib。
#    库不在场 → 不定义 AF_HAVE_FTXUI → tui_ftxui.cpp 整个不参与编译
#    → makeRenderer() 永远返回 plain → 降级不是瘫痪（用例 C32）。
$FtxInc  = @()
$FtxDef  = @()
$FtxLib  = @()
$FtxRoot = Join-Path $Here "thirdparty\ftxui"
$FtxLibPath = Join-Path $FtxRoot "build\ftxui.lib"

if (Test-Path $FtxLibPath) {
  $FtxInc = @(Join-Path $FtxRoot "include")
  $FtxDef = @('/DAF_HAVE_FTXUI')
  $FtxLib = @($FtxLibPath)
  Write-Output "FTXUI: 已就位（$((Get-Item $FtxLibPath).Length) bytes）"
} else {
  Write-Output "FTXUI: 库不在场 -> 退回纯文本 UI（跑 .\fetch_ftxui.ps1 + .\build_ftxui.ps1 可启用）"
}

# ---- 5. 编译 ----------------------------------------------------------------
$sources = Get-ChildItem $Here -Filter "*.cpp" -File |
           Where-Object { $_.Name -notlike "_*" } |
           ForEach-Object { $_.FullName }

# ui/ 是本讲的三个 UI 文件。★ tui_ftxui.cpp 只有在库在场时才编 ——
#   它是 FTXUI 头文件的**唯一入口**，不编它就没有任何编译期依赖。
$uiDir = Join-Path $Here "ui"
if (Test-Path $uiDir) {
  $sources += Get-ChildItem $uiDir -Filter "*.cpp" -File |
              Where-Object { $_.Name -notlike "_*" } |
              Where-Object { if ($_.Name -eq "tui_ftxui.cpp") { $FtxDef.Count -gt 0 } else { $true } } |
              ForEach-Object { $_.FullName }
}
# ★ 演示产物防污染：agent 在 code\ 里写示例程序时会留下带 main() 的 .cpp
#   （kmp.cpp / kmp_demo.cpp …）。全量 glob 会跟 main.cpp 撞成 LNK2005，把编译整个搞挂。
#   项目自己的源码只有 main.cpp 有 main，所以"带 main 又不是 main.cpp"一律不编。
$sources = @($sources | Where-Object {
    if ([IO.Path]::GetFileName($_) -eq 'main.cpp') { return $true }
    -not (Select-String -LiteralPath $_ -Pattern '^\s*int\s+main\s*\(' -Quiet)
})
if (-not $sources) { Write-Output "FATAL: 没找到 .cpp 文件"; exit 1 }

$buildDir = Join-Path $Here "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

# 5a. 先把 tree-sitter 的三个 C 文件编成 .obj
#
#     ★ 实测（cpp/src/parser.c，25,857,209 字节）：
#         /O1            1.77s   obj 5,703,414
#         /O2            1.54s   obj 5,704,492
#         /O2 + /bigobj  1.67s   obj 5,704,496
#         /O2 不加 bigobj 1.55s   obj 5,697,916  ← 也能编过
#
#     ★ 结论一：**25.8MB 的生成代码对 MSVC 根本不是问题**。
#       开工前最担心的是"这么大的语法表会不会编译很久 / 爆内存"，量完发现是
#       两秒以内的事。"先量再怕"—— 没量之前的担心全是臆断，本讲把它记下来，
#       因为下一讲你会用同样的语气担心别的东西。
#     ★ 结论二：/bigobj 在这里**不是必需的**（不加也编过）。留着它是因为
#       别的文法/别的编译器组合可能需要，而它零代价。
if ($TsDef.Count -gt 0) {
  $tsSrc = @($TsLibC,
             (Join-Path $TsRoot "cpp\src\scanner.c"),
             (Join-Path $TsRoot "cpp\src\parser.c"))
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $tsArgs = @('/nologo', '/c', '/O2', '/bigobj', '/D_CRT_SECURE_NO_WARNINGS', '/D_WIN32', '/utf-8')
  foreach ($i in $TsInc) { $tsArgs += "/I$i" }
  $tsArgs += "/Fo$buildDir\\"
  $tsArgs += $tsSrc
  & $cl @tsArgs 2>&1 | Out-String -Width 200 | Write-Output
  $sw.Stop()
  if ($LASTEXITCODE -ne 0) { Write-Output "[FAIL] tree-sitter 编译失败"; exit 1 }
  $TsObjs = @("$buildDir\lib.obj", "$buildDir\scanner.obj", "$buildDir\parser.obj")
  Write-Output "[OK] tree-sitter 3 个 C 文件编译完成，用时 $([math]::Round($sw.Elapsed.TotalSeconds,1))s"
}

$warn = if ($W4) { "/W4" } else { "/W3" }
$opt  = if ($NoOptimize) { "/Od" } else { "/O2" }

$args = @(
  '/nologo', '/std:c++20',
  # /utf-8 必加：源文件是无 BOM 的 UTF-8，不指定会被 MSVC 按 GBK 解析，
  # 报一堆莫名其妙的 C3872 / C1075（中文注释导致类体没闭合）
  '/utf-8',
  '/EHsc', '/permissive-', $warn, $opt,
  '/D_CRT_SECURE_NO_WARNINGS',
  "/Fo$buildDir\\", "/Fe$buildDir\$Out"
)
foreach ($inc in $Includes) { $args += "/I$inc" }
foreach ($inc in $TsInc)   { $args += "/I$inc" }
foreach ($inc in $FtxInc)  { $args += "/I$inc" }
$args += $TsDef
$args += $FtxDef
$args += $sources
$args += $TsObjs
$args += $FtxLib

Set-Location $Here
Write-Output "=== build: $($sources.Count) 个 .cpp $(if($TsObjs){'+ tree-sitter 3 个 .c'})$(if($FtxLib){' + ftxui.lib'}) -> build\$Out ==="
& $cl @args 2>&1 | Out-String -Width 200 | Write-Output

if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $buildDir $Out))) {
  Write-Output "[FAIL] 构建失败"
  exit 1
}
Write-Output "[OK] build\$Out"
