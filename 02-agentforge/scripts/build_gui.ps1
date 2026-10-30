# ---------------------------------------------------------------------------
#  build_gui.ps1 —— 构建 Qt 6 图形界面 AgentForgeGUI（CMake + Ninja + MSVC）
#
#  前置（本机已就绪）：
#    · Qt 6.8.3 msvc2022_64   → C:\Qt\6.8.3\msvc2022_64
#    · CMake                   → C:\Qt\Tools\CMake_64\bin\cmake.exe
#    · Ninja                   → python -m ninja 安装（pip）
#    · MSVC + Windows SDK（与 build.ps1 同一套定位逻辑）
#
#  用法：
#      .\build_gui.ps1            # 配置 + 构建 + windeployqt
#      .\build_gui.ps1 -Smoke     # 构建后跑无头冒烟（QT_QPA_PLATFORM=offscreen）
# ---------------------------------------------------------------------------
param(
  [switch]$Smoke
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Here = Split-Path -Parent $Here   # 脚本在 scripts/，上一级才是工程根

# ---- 1. 定位 MSVC ----
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

# ---- 2. 定位 Windows SDK（与 build.ps1 一致）----
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
  }
}
if (-not $SdkIncludeDirs) {
  $winsdk = Join-Path $Here 'thirdparty\winsdk'
  if (Test-Path $winsdk) {
    $basePkg = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.*' -and $_.Name -notlike '*.x64.*' } | Select-Object -First 1
    $x64Pkg  = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like 'microsoft.windows.sdk.cpp.x64.*' } | Select-Object -First 1
    if ($basePkg -and $x64Pkg) {
      $incVer = Get-ChildItem (Join-Path $basePkg.FullName 'c\Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
      if ($incVer) {
        $SdkIncludeDirs = @((Join-Path $incVer.FullName 'ucrt'), (Join-Path $incVer.FullName 'um'), (Join-Path $incVer.FullName 'shared'))
        $SdkLibDirs     = @((Join-Path $x64Pkg.FullName 'c\ucrt\x64'), (Join-Path $x64Pkg.FullName 'c\um\x64'))
      }
    }
  }
}
if (-not $SdkIncludeDirs) { Write-Output "FATAL: 找不到 Windows SDK"; exit 1 }

$env:INCLUDE = (@("$MsvcDir\include") + $SdkIncludeDirs) -join ';'
$env:LIB     = (@("$MsvcDir\lib\x64") + $SdkLibDirs) -join ';'
$env:PATH    = "$MsvcDir\bin\Hostx64\x64;$env:PATH"

# ★ rc.exe / mt.exe 也必须在 PATH（VS 会调它们生成 manifest；本机系统 SDK 缺 bin 工具）
$sdkBinExtra = $null
$winsdk = Join-Path $Here "thirdparty\winsdk"
if (Test-Path $winsdk) {
  $basePkg = Get-ChildItem $winsdk -Directory | Where-Object { $_.Name -like "microsoft.windows.sdk.cpp.*" -and $_.Name -notlike "*.x64.*" } | Select-Object -First 1
  if ($basePkg) {
    $rcDir = Join-Path $basePkg.FullName "c\bin\10.0.26100.0\x64"
    if (Test-Path (Join-Path $rcDir "rc.exe")) { $sdkBinExtra = $rcDir }
  }
}
if ($sdkBinExtra) {
  $env:PATH = "$sdkBinExtra;$env:PATH"
  Write-Output "SDK bin（rc/mt）: $sdkBinExtra"
}

# ---- 3. 定位 CMake / Ninja / Qt ----
$cmake = "C:\Qt\Tools\CMake_64\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $cmake) { Write-Output "FATAL: 找不到 cmake"; exit 1 }

$ninjaDir = & python -c "import ninja; print(ninja.BIN_DIR)" 2>$null
$ninja = Join-Path $ninjaDir "ninja.exe"
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if (-not $ninja) { Write-Output "FATAL: 找不到 ninja"; exit 1 }

$qtRoot = "C:\Qt\6.8.3\msvc2022_64"
if (-not (Test-Path "$qtRoot\lib\cmake\Qt6")) { Write-Output "FATAL: 找不到 Qt6（$qtRoot）"; exit 1 }

$buildDir = Join-Path $Here "build_gui"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

Write-Output "MSVC: $MsvcDir"
Write-Output "Qt  : $qtRoot"
Write-Output "CMake: $cmake"
Write-Output "Ninja: $ninja"

# ---- 4. 配置 ----
$confArgs = @(
  '-S', (Join-Path $Here 'gui'),
  '-B', $buildDir,
  '-G', 'Ninja',
  '-DCMAKE_BUILD_TYPE=Release',
  "-DCMAKE_MAKE_PROGRAM=$ninja",
  "-DCMAKE_PREFIX_PATH=$qtRoot"
)
& $cmake @confArgs 2>&1 | Out-String -Width 200 | Write-Output
if ($LASTEXITCODE -ne 0) { Write-Output "[FAIL] cmake 配置失败"; exit 1 }

# ---- 5. 构建 ----
& $cmake --build $buildDir 2>&1 | Out-String -Width 200 | Write-Output
if ($LASTEXITCODE -ne 0) { Write-Output "[FAIL] 构建失败"; exit 1 }

# ---- 6. 部署 Qt 运行库 ----
$exe = Join-Path $buildDir "AgentForgeGUI.exe"
$wq  = Join-Path $qtRoot "bin\windeployqt.exe"
if (Test-Path $wq) {
  & $wq $exe --no-translations 2>&1 | Out-String -Width 200 | Write-Output
}

# ★ 无头冒烟需要 offscreen/minimal 平台插件（windeployqt 默认不带）
$platDir = Join-Path $buildDir "platforms"
if (-not (Test-Path $platDir)) { New-Item -ItemType Directory -Path $platDir | Out-Null }
foreach ($plug in @("qoffscreen.dll", "qminimal.dll")) {
  $srcPlug = Join-Path $qtRoot "plugins\platforms\$plug"
  if (Test-Path $srcPlug) { Copy-Item $srcPlug (Join-Path $platDir $plug) -Force }
}
Write-Output "[OK] $exe"



