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

# ---- 5. 编译 ----------------------------------------------------------------
$sources = Get-ChildItem $Here -Filter "*.cpp" -File |
           Where-Object { $_.Name -notlike "_*" } |
           ForEach-Object { $_.FullName }
if (-not $sources) { Write-Output "FATAL: 没找到 .cpp 文件"; exit 1 }

$buildDir = Join-Path $Here "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

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
$args += $sources

Set-Location $Here
Write-Output "=== build: $($sources.Count) 个 .cpp -> build\$Out ==="
& $cl @args 2>&1 | Out-String -Width 200 | Write-Output

if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $buildDir $Out))) {
  Write-Output "[FAIL] 构建失败"
  exit 1
}
Write-Output "[OK] build\$Out"
