# ---------------------------------------------------------------------------
#  build_plugin.ps1 —— 把 sample_plugin.cpp 编成可被 LoadLibrary 的 .dll
#
#  与上级 build.ps1 的区别只有两处：
#    ① /LD（产出 .dll 而不是 .exe）
#    ② 编完用 dumpbin /exports 验证铁律 1：导出符号必须是原样的
#       af_plugin_entry（extern "C" 生效），而不是 C++ mangled 名。
#
#  用法：
#      .\plugins\build_plugin.ps1
#  产物：
#      build\plugins\sample_plugin.dll
# ---------------------------------------------------------------------------
param(
  [string]$Out = "sample_plugin.dll"
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $Here            # 工程根

# ---- 1. 定位 MSVC（与上级 build.ps1 同一套逻辑）----------------------------
$MsvcDir = $null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  $inst = & $vswhere -latest -products * -property installationPath 2>$null
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
Write-Output "MSVC: $MsvcDir"

# ---- 2. 定位 Windows SDK（与上级 build.ps1 同一套逻辑）---------------------
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
  $winsdk = Join-Path $Root 'thirdparty\winsdk'
  if (Test-Path $winsdk) {
    $basePkg = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.*' -and $_.Name -notlike '*.x64.*' } | Select-Object -First 1
    $x64Pkg  = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.x64.*' } | Select-Object -First 1
    if ($basePkg -and $x64Pkg) {
      $incVer = Get-ChildItem (Join-Path $basePkg.FullName 'c\Include') -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
      if ($incVer) {
        $SdkIncludeDirs = @((Join-Path $incVer.FullName 'ucrt'), (Join-Path $incVer.FullName 'um'), (Join-Path $incVer.FullName 'shared'))
        $SdkLibDirs     = @((Join-Path $x64Pkg.FullName 'c\ucrt\x64'), (Join-Path $x64Pkg.FullName 'c\um\x64'))
        Write-Output "SDK: NuGet"
      }
    }
  }
}

if (-not $SdkIncludeDirs) { Write-Output "FATAL: 找不到 Windows SDK"; exit 1 }

# ---- 3. 配环境 ---------------------------------------------------------------
$env:PATH    = "$MsvcDir\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = (@("$MsvcDir\include") + $SdkIncludeDirs) -join ';'
$env:LIB     = (@("$MsvcDir\lib\x64") + $SdkLibDirs) -join ';'

$cl = "$MsvcDir\bin\Hostx64\x64\cl.exe"
if (-not (Test-Path $cl)) { Write-Output "FATAL: cl.exe not found"; exit 1 }

# ---- 4. 编译成 DLL（/LD）----------------------------------------------------
$buildDir = Join-Path $Root "build\plugins"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

$clArgs = @(
  '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/permissive-',
  '/W3', '/O2',
  '/D_CRT_SECURE_NO_WARNINGS',
  '/LD',
  "/I$Here", "/I$(Join-Path $Root 'src')",
  "/Fo$buildDir\\", "/Fe$buildDir\$Out",
  (Join-Path $Here "sample_plugin.cpp")
)

Set-Location $Here
Write-Output "=== build plugin: sample_plugin.cpp -> build\plugins\$Out ==="
& $cl @clArgs 2>&1 | Out-String -Width 200 | Write-Output

$dll = Join-Path $buildDir $Out
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) {
  Write-Output "[FAIL] 插件构建失败"
  exit 1
}
Write-Output "[OK] build\plugins\$Out"

# ---- 5. 验证铁律 1：导出符号名必须是原样的 C 符号 ---------------------------
$dumpbin = "$MsvcDir\bin\Hostx64\x64\dumpbin.exe"
if (Test-Path $dumpbin) {
  Write-Output ""
  Write-Output "=== 导出符号（应含原样的 af_plugin_entry）==="
  $exp = & $dumpbin /nologo /exports $dll 2>&1 | Out-String
  if ($exp -match 'af_plugin_entry') {
    Write-Output "[OK] 导出符号含 af_plugin_entry（extern ""C"" 生效）"
  } else {
    Write-Output "[FAIL] 导出符号里找不到 af_plugin_entry —— 检查是否漏了 extern ""C"" / dllexport"
    Write-Output $exp
    exit 1
  }
}

