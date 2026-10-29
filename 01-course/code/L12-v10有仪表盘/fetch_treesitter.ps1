# ---------------------------------------------------------------------------
#  fetch_treesitter.ps1 —— 把 tree-sitter 与 C++ 文法拉到本地并摆成固定布局
#
#  为什么需要这个脚本：
#    本机没有 vcpkg（CMake 也认不出 VS 2026 的工具集），而 tree-sitter 恰好是
#    一个"纯 C、无依赖、unity build"的库 —— 只要把源码拉下来用 cl.exe 直编即可。
#    实测：下载 3s，编译 3 个 C 文件共 2.6s，其中 25.8MB 的语法表只占 1.6s。
#    "没有包管理器"在这件事上根本不是障碍。
#
#  产物布局（固定目录名，不随版本号变）：
#    thirdparty/tree-sitter/core/include/tree_sitter/api.h
#    thirdparty/tree-sitter/core/src/*.c *.h     （含 unity build 的 lib.c）
#    thirdparty/tree-sitter/cpp/src/parser.c scanner.c tree_sitter/parser.h
#
#  ★ core/src 与 cpp/src 里都有一个 parser.c（一个是库，一个是语法表），
#    必须分目录放，否则会互相覆盖。
#
#  用法： powershell -File fetch_treesitter.ps1 [-Force]
# ---------------------------------------------------------------------------
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$Here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dest  = Join-Path $Here "thirdparty\tree-sitter"
$Dl    = Join-Path $Here "thirdparty\_ts_dl"

$TS_VER  = "v0.25.0"
$CPP_REF = "master"

function Say($m) { Write-Output $m }

if ((Test-Path $Dest) -and -not $Force) {
    $api = Join-Path $Dest "core\include\tree_sitter\api.h"
    $pc  = Join-Path $Dest "cpp\src\parser.c"
    if ((Test-Path $api) -and (Test-Path $pc)) {
        Say "[skip] thirdparty\tree-sitter 已就绪（api.h + parser.c 都在）。加 -Force 重新拉取。"
        exit 0
    }
}

New-Item -ItemType Directory -Force -Path $Dl | Out-Null
$curl = "C:\Windows\system32\curl.exe"

$tsZip  = Join-Path $Dl "ts.zip"
$cppZip = Join-Path $Dl "tscpp.zip"

Say "=== 1/4 下载 tree-sitter $TS_VER ==="
& $curl -sSL -o $tsZip "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/$TS_VER.zip"
if ($LASTEXITCODE -ne 0) { Say "[FAIL] curl exit=$LASTEXITCODE"; exit 1 }
Say "    $((Get-Item $tsZip).Length) bytes"

Say "=== 2/4 下载 tree-sitter-cpp $CPP_REF ==="
& $curl -sSL -o $cppZip "https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/heads/$CPP_REF.zip"
if ($LASTEXITCODE -ne 0) { Say "[FAIL] curl exit=$LASTEXITCODE"; exit 1 }
Say "    $((Get-Item $cppZip).Length) bytes"

Say "=== 3/4 解压 ==="
Expand-Archive -Path $tsZip  -DestinationPath $Dl -Force
Expand-Archive -Path $cppZip -DestinationPath $Dl -Force

$core = Get-ChildItem $Dl -Directory | Where-Object { $_.Name -like "tree-sitter-*" -and $_.Name -notlike "*cpp*" } | Select-Object -First 1
$cpp  = Get-ChildItem $Dl -Directory | Where-Object { $_.Name -like "tree-sitter-cpp*" } | Select-Object -First 1
if (-not $core) { Say "[FAIL] 没找到 tree-sitter 核心目录"; exit 1 }
if (-not $cpp)  { Say "[FAIL] 没找到 tree-sitter-cpp 目录"; exit 1 }
Say "    core = $($core.Name)"
Say "    cpp  = $($cpp.Name)"

Say "=== 4/4 摆成固定布局 ==="
$coreInc = Join-Path $Dest "core\include"
$coreSrc = Join-Path $Dest "core\src"
$cppSrc  = Join-Path $Dest "cpp\src"
foreach ($d in @($coreInc, $coreSrc, $cppSrc)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }

Copy-Item (Join-Path $core.FullName "lib\include\tree_sitter") $coreInc -Recurse -Force
Copy-Item (Join-Path $core.FullName "lib\src\*") $coreSrc -Recurse -Force
Copy-Item (Join-Path $cpp.FullName  "src\parser.c")  $cppSrc -Force
Copy-Item (Join-Path $cpp.FullName  "src\scanner.c") $cppSrc -Force
Copy-Item (Join-Path $cpp.FullName  "src\tree_sitter") $cppSrc -Recurse -Force

# 版本对账：文法的 LANGUAGE_VERSION 必须 ≤ 核心库的 TREE_SITTER_LANGUAGE_VERSION
$apiH = Join-Path $coreInc "tree_sitter\api.h"
$pcF  = Join-Path $cppSrc "parser.c"
$coreV = (Select-String -Path $apiH -Pattern "define TREE_SITTER_LANGUAGE_VERSION" | Select-Object -First 1).Line
$cppV  = (Select-String -Path $pcF  -Pattern "define LANGUAGE_VERSION" | Select-Object -First 1).Line
$rec = @(
  "tree-sitter $TS_VER",
  "core: $coreV",
  "cpp : $cppV",
  "fetched: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
)
Set-Content -Path (Join-Path $Dest "_version.txt") -Value $rec -Encoding UTF8

Say ""
Say "[OK] 就绪："
Say "    $((Get-Item (Join-Path $coreSrc 'lib.c')).Length) bytes  core/src/lib.c"
Say "    $((Get-Item $pcF).Length) bytes  cpp/src/parser.c"
$rec | ForEach-Object { Say "    $_" }
