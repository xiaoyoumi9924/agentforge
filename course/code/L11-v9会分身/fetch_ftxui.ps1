# ---------------------------------------------------------------------------
#  fetch_ftxui.ps1 —— 把 FTXUI 拉到本地并摆成固定布局
#
#  为什么需要这个脚本（与 fetch_treesitter.ps1 是同一个套路）：
#    本机没有 vcpkg，CMake 也认不出 VS 2026 的工具集。而 FTXUI 是"纯 C++20、
#    无第三方依赖"的库 —— 源码拉下来用 cl.exe 直编即可。
#
#  ★ 但和 tree-sitter 有一个**关键差别**：
#      tree-sitter 编 3 个 C 文件只要 2.8s（每次全量重编无所谓）；
#      FTXUI 编 77 个 .cpp 要 **85.5s**（必须编一次存 .lib 缓存）。
#    所以这里**只负责下载摆目录**，编译由 build_ftxui.ps1 单独做（且只跑一次）。
#    "什么时候该缓存"是量出来的 —— 差 30 倍就是两个决策。（K-33）
#
#  产物布局（固定目录名，不随版本号变）：
#    thirdparty/ftxui/include/ftxui/...   公开头
#    thirdparty/ftxui/src/ftxui/...       .cpp + ★ 内部私有头（node_decorator.hpp 等）
#    thirdparty/ftxui/_version.txt
#
#  ★ 头文件是**两个**目录：只给 include/ 的话，77 个文件里有 34 个编不过
#    （它们 #include "ftxui/dom/node_decorator.hpp"，那个文件在 src/ 下）。
#
#  用法： powershell -File fetch_ftxui.ps1 [-Force]
# ---------------------------------------------------------------------------
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dest = Join-Path $Here "thirdparty\ftxui"
$Dl   = Join-Path $Here "thirdparty\_ftxui_dl"

$FTX_VER = "v7.0.3"

function Say($m) { Write-Output $m }

if ((Test-Path $Dest) -and -not $Force) {
    $h = Join-Path $Dest "include\ftxui\screen\screen.hpp"
    $s = Join-Path $Dest "src\ftxui\dom\node.cpp"
    if ((Test-Path $h) -and (Test-Path $s)) {
        Say "[skip] thirdparty\ftxui 已就绪（screen.hpp + node.cpp 都在）。加 -Force 重新拉取。"
        exit 0
    }
}

New-Item -ItemType Directory -Force -Path $Dl | Out-Null
$curl = "C:\Windows\system32\curl.exe"
$zip  = Join-Path $Dl "ftxui.zip"

# ---------------------------------------------------------------------------
#  ★ 下载：优先 codeload，失败再退回 github archive
#
#    为什么不能直接信 github.com/<repo>/archive/refs/tags/<tag>：
#      那个 URL 会 302 跳到 codeload，而**跳转在本次实测中被拦** ——
#      curl 拿到 **0 字节**并且 **exit=0**（静默失败）。
#      后果是后面 Expand-Archive 报错，而报错信息指向"压缩包坏了"，
#      你会去怀疑下载完整性，不会怀疑 URL。
#
#    ★ 所以真正有用的是那一行字节数检查：0 字节 = 失败，必须大声说出来。
# ---------------------------------------------------------------------------
function Download-Zip($url, $outPath) {
    & $curl -sSL -o $outPath $url
    $code = $LASTEXITCODE
    $len  = if (Test-Path $outPath) { (Get-Item $outPath).Length } else { 0 }
    if ($code -ne 0 -or $len -lt 100000) {
        return $false
    }
    return $true
}

Say "=== 1/4 下载 FTXUI $FTX_VER ==="
$ok = Download-Zip "https://codeload.github.com/ArthurSonzogni/FTXUI/zip/refs/tags/$FTX_VER" $zip
if (-not $ok) {
    Say "    codeload 失败（exit=$LASTEXITCODE），退回 github archive ..."
    $ok = Download-Zip "https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/$FTX_VER.zip" $zip
}
if (-not $ok) {
    $len = if (Test-Path $zip) { (Get-Item $zip).Length } else { 0 }
    Say "[FAIL] 下载 FTXUI 失败（$len bytes，exit=$LASTEXITCODE）。"
    Say "       检查网络，或手动下载后放到：$zip"
    exit 1
}
Say "    $((Get-Item $zip).Length) bytes"

Say "=== 2/4 解压 ==="
Expand-Archive -Path $zip -DestinationPath $Dl -Force
$root = Get-ChildItem $Dl -Directory | Where-Object { $_.Name -like "FTXUI-*" } | Select-Object -First 1
if (-not $root) { Say "[FAIL] 没找到 FTXUI-* 目录"; exit 1 }
Say "    $($root.Name)"

Say "=== 3/4 摆成固定布局 ==="
$incDst = Join-Path $Dest "include"
$srcDst = Join-Path $Dest "src"
foreach ($d in @($incDst, $srcDst)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
Copy-Item (Join-Path $root.FullName "include\*") $incDst -Recurse -Force
Copy-Item (Join-Path $root.FullName "src\*")     $srcDst -Recurse -Force

# ---- 统计：库源码 vs 测试源码（★ 这个数字要写进 _version.txt）----
$allCpp = Get-ChildItem -Path $srcDst -Recurse -Filter "*.cpp" -File
$libCpp = $allCpp | Where-Object { $_.Name -notmatch "_test\.cpp$|_fuzzer\.cpp$|^test_" }

Say "=== 4/4 版本对账 ==="
$rec = @(
  "FTXUI $FTX_VER",
  "require: C++20（本项目用的正是 /std:c++20）",
  "srcCpp    = $($libCpp.Count)   （排除 test/fuzzer 后的库源码）",
  "allCpp    = $($allCpp.Count)   （含 $($allCpp.Count - $libCpp.Count) 个 *_test.cpp / *_fuzzer.cpp）",
  "incDirs   = include/ + src/    ★ 两个都要给 /I",
  "fetched: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
)
Set-Content -Path (Join-Path $Dest "_version.txt") -Value $rec -Encoding UTF8

Say ""
Say "[OK] 就绪："
$rec | ForEach-Object { Say "    $_" }
Say ""
Say "    下一步： .\\build_ftxui.ps1   （★ 约 85s，只跑一次，产出 ftxui.lib 缓存）"
