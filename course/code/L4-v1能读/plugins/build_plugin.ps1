# ---------------------------------------------------------------------------
#  build_plugin.ps1 —— 把 sample_plugin.cpp 编成一个真正能被 LoadLibrary 的 .dll
#
#  为什么单独一个脚本、不复用上级的 build.ps1：
#    上级 build.ps1 是"把一组 .cpp 链接成一个 exe"，用 /Fe。
#    插件要的是"把一个 .cpp 编成一个共享库"，用 /LD。
#    两者只有环境准备这一半是共用的 —— 而那一半必须逐字复制过来，
#    因为 PowerShell 里不能调 cmd.exe，跑不了 vcvarsall.bat。
#
#  三条铁律里唯一被构建方式影响的：
#    铁律 1（extern "C"）—— 不体现为编译选项，体现为插件源码里的符号名；
#      验证方法是编完之后去 dumpbin /exports 看符号名是不是**原样**的
#      af_plugin_entry，而不是 ?af_plugin_entry@@YAPEBUAFPlugin@@XZ 这种。
#
#  用法：
#      .\plugins\build_plugin.ps1
#
#  产物：
#      build\plugins\sample_plugin.dll
# ---------------------------------------------------------------------------
param(
  [string]$Out = "sample_plugin.dll"
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $Here            # code\

# ---- 1. 定位 MSVC（与上级 build.ps1 同一套逻辑）----------------------------
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
  $probe = @()
  foreach ($root2 in @("C:\Program Files\Microsoft Visual Studio",
                       "C:\Program Files (x86)\Microsoft Visual Studio")) {
    if (-not (Test-Path $root2)) { continue }
    $probe += $root2
    $lv1 = Get-ChildItem $root2 -Directory -ErrorAction SilentlyContinue
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

if (-not $MsvcDir) { Write-Output "FATAL: 找不到 MSVC"; exit 1 }

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
if (-not (Test-Path $cl)) { Write-Output "FATAL: cl.exe not found"; exit 1 }

# ---- 4. 编译成 DLL -----------------------------------------------------------
$buildDir = Join-Path $Root "build\plugins"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

#  ★ 用到 dllexport 的符号名：/LD 让链接器产出一个 .dll（而不是 .exe）。
#  ★ 同样必须加 /utf-8：源码里的中文注释与中文字面量，
#    不加会被 MSVC 按 GBK 解析（症状见上级 build.ps1 的注释）。
$args = @(
  '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/permissive-',
  '/W3', '/O2',
  '/D_CRT_SECURE_NO_WARNINGS',
  '/LD',
  '/I' + $Here,
  "/Fo$buildDir\\", "/Fe$buildDir\$Out",
  (Join-Path $Here "sample_plugin.cpp")
)

Set-Location $Here
Write-Output "=== build plugin: sample_plugin.cpp -> build\plugins\$Out ==="
& $cl @args 2>&1 | Out-String -Width 200 | Write-Output

$dll = Join-Path $buildDir $Out
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) {
  Write-Output "[FAIL] 插件构建失败"
  exit 1
}
Write-Output "[OK] build\plugins\$Out"

# ---- 5. 验证铁律 1：导出符号名必须是原样的 C 符号 ---------------------------
#  如果插件忘了 extern "C"，这里会看到一个带 ?? 或 ?xxx@@ 的 C++ mangled 名，
#  宿主 dlsym("af_plugin_entry") 就会失败。**让脚本替你发现这件事**，
#  比在运行时看到 "未找到导出符号 af_plugin_entry" 再回头查要快得多。
$dumpbin = "$MsvcDir\bin\Hostx64\x64\dumpbin.exe"
if (Test-Path $dumpbin) {
  Write-Output ""
  Write-Output "=== 导出符号（应含原样的 af_plugin_entry）==="
  $exp = & $dumpbin /nologo /exports $dll 2>&1 | Out-String
  $hay = $exp
  if ($hay -match 'af_plugin_entry') {
    Write-Output "[OK] 导出符号含 af_plugin_entry（extern ""C"" 生效）"
  } else {
    Write-Output "[FAIL] 导出符号里找不到 af_plugin_entry —— 检查是否漏了 extern ""C"" / dllexport"
    Write-Output $exp
    exit 1
  }
  if ($hay -match '\?\?') {
    Write-Output "[WARN] 导出表里出现了 C++ mangled 名（??...），确认没有多余的 dllexport"
  }
}
