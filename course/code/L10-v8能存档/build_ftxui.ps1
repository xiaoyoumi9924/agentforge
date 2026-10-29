# ---------------------------------------------------------------------------
#  build_ftxui.ps1 —— 把 FTXUI 编成静态库并**缓存**
#
#  ★★ 为什么这个脚本要单独存在（而不是并进 build.ps1）：
#     实测编译 77 个 .cpp 需要 **85.5s**。
#     对比 L8 的 tree-sitter：3 个 C 文件 **2.8s** —— 那个量级可以每次全量重编，
#     差 30 倍就变成另一个决策：
#
#       >30s 的东西必须"编一次、存起来、之后只链接"。
#
#     "什么时候该缓存"是量出来的，不是拍脑袋定的。（K-33）
#
#  产物：thirdparty/ftxui/build/ftxui.lib（约 19.5MB）
#  幂等：库已存在就跳过；加 -Force 才重编。
#
#  ★ 本脚本踩过的 4 个坑（都是"量了才知道"，已写进讲义 §3.3 / §3.4）：
#     ① 不加 /c → cl 编完就想链接成 exe → LNK1561 必须定义入口点
#     ② 头文件要**两个**目录：include/（公开）+ src/（内部私有头）
#     ③ src/ 里混着 53 个 *_test.cpp / *_fuzzer.cpp（要 gtest）→ 必须排除
#     ④ 同名文件撞 obj（component/util.cpp 与 dom/util.cpp）→ obj 名用相对路径拼
#     ⑤ ★ PowerShell 变量大小写不敏感（K-26）：目录叫 ...Dir，文本叫 ...Text
#
#  用法： powershell -File build_ftxui.ps1 [-Force]
# ---------------------------------------------------------------------------
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Ftx  = Join-Path $Here "thirdparty\ftxui"

$IncDir = Join-Path $Ftx "include"
$SrcDir = Join-Path $Ftx "src"
$ObjDir = Join-Path $Ftx "build\obj"
$LibPath = Join-Path $Ftx "build\ftxui.lib"

function Say($m) { Write-Output $m }

if (-not (Test-Path (Join-Path $IncDir "ftxui"))) {
    Say "[FAIL] 源码不在场。先跑 .\\fetch_ftxui.ps1"
    Say "       （没有 FTXUI 也能正常构建：build.ps1 会自动退回纯文本 UI）"
    exit 1
}

if ((Test-Path $LibPath) -and -not $Force) {
    Say "[skip] ftxui.lib 已存在（$((Get-Item $LibPath).Length) bytes）。加 -Force 重新编译。"
    exit 0
}

# ---- 1. 定位 MSVC / SDK（与 build.ps1 同一套逻辑）----
$MsvcDir = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $inst = & $vswhere -latest -products * -property installationPath 2>$null
    if ($inst) {
        $vct = Join-Path $inst "VC\Tools\MSVC"
        if (Test-Path $vct) {
            $MsvcDir = (Get-ChildItem $vct -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
        }
    }
}
if (-not $MsvcDir) { Say "[FAIL] 找不到 MSVC"; exit 1 }

$SdkRoot = "C:\Program Files (x86)\Windows Kits\10"
$SdkVer  = (Get-ChildItem "$SdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name

$env:PATH    = "$MsvcDir\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = @("$MsvcDir\include",
                 "$SdkRoot\Include\$SdkVer\ucrt",
                 "$SdkRoot\Include\$SdkVer\um",
                 "$SdkRoot\Include\$SdkVer\shared") -join ';'
$env:LIB = @("$MsvcDir\lib\x64",
             "$SdkRoot\Lib\$SdkVer\ucrt\x64",
             "$SdkRoot\Lib\$SdkVer\um\x64") -join ';'

$cl  = "$MsvcDir\bin\Hostx64\x64\cl.exe"
$lib = "$MsvcDir\bin\Hostx64\x64\lib.exe"
if (-not (Test-Path $cl)) { Say "[FAIL] cl.exe not found: $cl"; exit 1 }

Say "MSVC = $MsvcDir"
Say "SDK  = $SdkVer"

# ---- 2. 收集库源码（★ 排除 test / fuzzer）----
$allCpp = Get-ChildItem -Path $SrcDir -Recurse -Filter "*.cpp" -File
$libCpp = $allCpp | Where-Object { $_.Name -notmatch "_test\.cpp$|_fuzzer\.cpp$|^test_" }
Say "库源码 $($libCpp.Count) 个（全部 $($allCpp.Count) 个，排除 $($allCpp.Count - $libCpp.Count) 个 test/fuzzer）"

$null = New-Item -ItemType Directory -Force -Path $ObjDir

# ---- 3. 逐个 /c，obj 名用相对路径拼（★ 否则同名文件互相覆盖）----
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$objs = @()
$failed = @()
foreach ($f in $libCpp) {
    $rel     = $f.FullName.Substring($SrcDir.Length + 1)          # ftxui\dom\node.cpp
    $objName = ($rel -replace '[\\/]', '_') -replace '\.cpp$', '.obj'
    $objPath = Join-Path $ObjDir $objName
    # ★ 变量名：...Dir / ...Path 是路径，...Text 是文本。
    #   不要一个叫 $Out 一个叫 $out —— PowerShell 大小写不敏感（K-26）。
    $clText = & $cl /nologo /c /std:c++20 /utf-8 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS `
                    "/I$IncDir" "/I$SrcDir" "/Fo$objPath" $f.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        $failed += "$rel :: " + (($clText | Where-Object { $_ -match 'error' } | Select-Object -First 1))
    } elseif (Test-Path $objPath) {
        $objs += $objPath
    }
}
$sw.Stop()

if ($failed.Count -gt 0) {
    Say "[FAIL] $($failed.Count) 个文件编译失败："
    $failed | Select-Object -First 10 | ForEach-Object { Say "    $_" }
    exit 1
}

$objBytes = (Get-ChildItem $ObjDir -Filter "*.obj" | Measure-Object Length -Sum).Sum
Say "[OK] $($objs.Count) 个 obj（$objBytes bytes），用时 $([math]::Round($sw.Elapsed.TotalSeconds,1))s"

# ---- 4. 打成静态库 ----
$null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LibPath)
if (Test-Path $LibPath) { Remove-Item -LiteralPath $LibPath -Force }
$libText = & $lib /nologo "/OUT:$LibPath" $objs 2>&1
if ($LASTEXITCODE -ne 0) {
    Say "[FAIL] lib.exe 失败："
    $libText | Select-Object -First 8 | ForEach-Object { Say "    $_" }
    exit 1
}

Say "[OK] $LibPath（$((Get-Item $LibPath).Length) bytes）"
Say ""
Say "    库已缓存。之后跑 .\\build.ps1 只会链接它，不会再编这 77 个文件。"
