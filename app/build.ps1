# ---------------------------------------------------------------------------
#  build.ps1 —— AgentForge 构建脚本（L1 起每一讲通用）
#
#  为什么不用 CMake 驱动构建：
#    - 本机 CMake 对 VS 工具集识别可能有问题（课程 09 讲踩过坑）；
#    - PowerShell 中无法继承 vcvarsall.bat 的环境，于是手动配置
#      INCLUDE / LIB / PATH 直接调 cl.exe —— 单层进程，最稳。
#    CMakeLists.txt 仍保留，供 IDE / 后续课程阶段使用。
#
#  用法：
#     .\build.ps1                      # 默认 /W3 /O2，输出 build\agentforge.exe
#     .\build.ps1 -Out agent.exe -W4    # 改输出名 / 更高告警
#     .\build.ps1 -NoOptimize          # 调试：/Od
# ---------------------------------------------------------------------------
param(
  [string]$Out  = "",
  [switch]$W4,
  [switch]$NoOptimize,
  [ValidateSet('agentforge','contract_check')]
  [string]$Target = 'agentforge'
)

if (-not $Out) {
  $Out = if ($Target -eq 'contract_check') { "contract_check.exe" } else { "agentforge.exe" }
}

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# ---- 1. 定位 MSVC（优先 vswhere 官方机制） -----------------------------------
$MsvcDir = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  # vswhere 出错时会把错误文本打到 stdout（例如 "Error 0x80070005"），
  # 这里只接受真正的路径，否则退化到下面的目录探测。
  $inst = & $vswhere -latest -products * -property installationPath 2>$null |
          Where-Object { $_ -match '^[A-Za-z]:\\' } | Select-Object -First 1
  if ($inst) {
    $vcTools = Join-Path $inst "VC\Tools\MSVC"
    if (Test-Path $vcTools) {
      $pick = Get-ChildItem $vcTools -Directory | Sort-Object Name -Descending | Select-Object -First 1
      if ($pick) { $MsvcDir = $pick.FullName }
    }
  }
}

if (-not $MsvcDir) {
  # 退化路径：VS 安装布局是 <root>\<version>\<edition>\VC\Tools\MSVC\<ver>
  # 把 root / <version> / <edition> 三层都当候选探测一遍。
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
  Write-Output "FATAL: 找不到 MSVC。"
  Write-Output "请先用管理员权限安装 Visual Studio 2022（或 2026）Build Tools，"
  Write-Output "安装时勾选「使用 C++ 的桌面开发」工作负载（含 MSVC x64 与 Windows SDK）。"
  exit 1
}
Write-Output "MSVC: $MsvcDir"

# ---- 2. 定位 Windows SDK ------------------------------------------------------
# 顺序：① 注册表 KitsRoot10（系统安装）；② thirdparty/winsdk（NuGet 免安装兜底）。
$SdkIncludeDirs = @()
$SdkLibDirs     = @()

$SdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (-not $SdkRoot) {
  $SdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
}
if ($SdkRoot) {
  $SdkRoot = $SdkRoot.TrimEnd('\')
  $ver = Get-ChildItem "$SdkRoot\Include" -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | Select-Object -First 1
  if ($ver -and (Test-Path "$SdkRoot\Lib\$($ver.Name)")) {
    $SdkIncludeDirs = @("$SdkRoot\Include\$($ver.Name)\ucrt", "$SdkRoot\Include\$($ver.Name)\um", "$SdkRoot\Include\$($ver.Name)\shared")
    $SdkLibDirs     = @("$SdkRoot\Lib\$($ver.Name)\ucrt\x64", "$SdkRoot\Lib\$($ver.Name)\um\x64")
    Write-Output "SDK: $SdkRoot ($($ver.Name))"
  }
}

if (-not $SdkIncludeDirs) {
  # NuGet 兜底：thirdparty\winsdk\microsoft.windows.sdk.cpp.<ver>（头文件）
  #            thirdparty\winsdk\microsoft.windows.sdk.cpp.x64.<ver>（x64 库）
  $winsdk = Join-Path $Here 'thirdparty\winsdk'
  if (Test-Path $winsdk) {
    $basePkg = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.*' -and $_.Name -notlike '*.x64.*' } | Select-Object -First 1
    $x64Pkg  = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.x64.*' } | Select-Object -First 1
    if ($basePkg -and $x64Pkg) {
      $incVer = Get-ChildItem (Join-Path $basePkg.FullName 'c\Include') -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
      if ($incVer) {
        $SdkIncludeDirs = @((Join-Path $incVer.FullName 'ucrt'), (Join-Path $incVer.FullName 'um'), (Join-Path $incVer.FullName 'shared'))
        $SdkLibDirs     = @((Join-Path $x64Pkg.FullName 'c\ucrt\x64'), (Join-Path $x64Pkg.FullName 'c\um\x64'))
        Write-Output "SDK: NuGet ($($basePkg.Name) / $($x64Pkg.Name))"
      }
    }
  }
}

if (-not $SdkIncludeDirs) {
  Write-Output "FATAL: 找不到 Windows SDK。可选：安装 Windows 10/11 SDK，"
  Write-Output "或把 Microsoft.Windows.SDK.CPP 与 .x64 两个 nupkg 解压到 thirdparty\winsdk\ 下。"
  exit 1
}

# ---- 3. 配置编译环境 ----------------------------------------------------------
$env:PATH    = "$MsvcDir\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = (@("$MsvcDir\include") + $SdkIncludeDirs) -join ';'
$env:LIB     = (@("$MsvcDir\lib\x64") + $SdkLibDirs) -join ';'

$cl = "$MsvcDir\bin\Hostx64\x64\cl.exe"
if (-not (Test-Path $cl)) { Write-Output "FATAL: cl.exe not found at $cl"; exit 1 }

# ---- 4. 头文件路径（含 thirdparty/nlohmann 向上探测） --------------------------
$Includes = @("$Here\src")
if (Test-Path "$Here\thirdparty\sqlite") { $Includes += "$Here\thirdparty\sqlite" }
$probe = $Here
for ($i = 0; $i -lt 5 -and $probe; $i++) {
  $tp = Join-Path $probe "thirdparty"
  if (Test-Path (Join-Path $tp "nlohmann\json.hpp")) { $Includes += $tp; break }
  $probe = Split-Path -Parent $probe
}

# ---- 5. 选择源文件 ------------------------------------------------------------
switch ($Target) {
  'contract_check' {
    $sources = @((Join-Path $Here 'tests\contract_check.cpp'))
  }
  default {
    $sources = Get-ChildItem "$Here\src" -Filter "*.cpp" -File -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -notlike "_*" } |
               ForEach-Object { $_.FullName }
    # SQLite amalgamation（官方单文件源码）
    $sqlite = Join-Path $Here 'thirdparty\sqlite\sqlite3.c'
    if (Test-Path $sqlite) { $sources += $sqlite }
  }
}
if (-not ($sources | Where-Object { Test-Path $_ })) { Write-Output "FATAL: 没找到源文件（Target=$Target）"; exit 1 }

$buildDir = Join-Path $Here "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

$warn = if ($W4) { "/W4" } else { "/W3" }
$opt  = if ($NoOptimize) { "/Od" } else { "/O2" }

$clArgs = @(
  '/nologo', '/std:c++20',
  # /utf-8 必加：源文件是无 BOM 的 UTF-8，不指定会被 MSVC 按 GBK 解析，
  # 报一堆莫名其妙的 C3872 / C1075（中文注释导致类体没闭合）。
  '/utf-8',
  '/EHsc', '/permissive-', $warn, $opt,
  '/DSQLITE_THREADSAFE=0', '/DSQLITE_OMIT_LOAD_EXTENSION',
  '/D_CRT_SECURE_NO_WARNINGS',
  "/Fo$buildDir\\", "/Fe$buildDir\$Out"
)
foreach ($inc in $Includes) { $clArgs += "/I$inc" }
$clArgs += $sources

Set-Location $Here
Write-Output "=== build: $($sources.Count) 个 .cpp -> build\$Out ==="
& $cl @clArgs 2>&1 | Out-String -Width 200 | Write-Output

if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $buildDir $Out))) {
  Write-Output "[FAIL] 构建失败"
  exit 1
}
Write-Output "[OK] build\$Out"


