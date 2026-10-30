# ---------------------------------------------------------------------------
#  package.ps1 —— 生成提交压缩包（设计文档 + 源代码）
#
#  包含：docs / src / gui / tests / plugins / configs / 脚本 / README
#        thirdparty/nlohmann（JSON 单头）+ thirdparty/sqlite（SQLite 源码）
#  排除：构建产物、SDK 副本、日志、会话库、临时输出
#
#  用法：
#      .\scripts\package.ps1                # 输出到 ..\AgentForge-提交版-v1.0.zip
#      .\scripts\package.ps1 -Out D:\x.zip
# ---------------------------------------------------------------------------
param(
  [string]$Out = ""
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Proj = Split-Path -Parent $Here
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Proj) "AgentForge-提交版-v1.0.zip" }

$staging = Join-Path $env:TEMP ("af_pkg_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$root = Join-Path $staging "AgentForge"
New-Item -ItemType Directory -Force -Path $root | Out-Null

# ---- 1. 复制目录 ----
foreach ($d in @('docs', 'src', 'gui', 'tests', 'plugins', 'configs')) {
    $src = Join-Path $Proj $d
    if (Test-Path $src) { Copy-Item $src (Join-Path $root $d) -Recurse -Force }
}

# ---- 2. 复制根文件 ----
foreach ($f in @('README.md', 'build.ps1', 'build_gui.ps1', 'CMakeLists.txt', '.gitignore')) {
    $p = Join-Path $Proj $f
    if (Test-Path $p) { Copy-Item $p (Join-Path $root $f) -Force }
}

# ---- 3. 第三方（自包含构建所需）----
New-Item -ItemType Directory -Force -Path (Join-Path $root 'thirdparty\nlohmann') | Out-Null
Copy-Item (Join-Path $Proj 'thirdparty\nlohmann\json.hpp') (Join-Path $root 'thirdparty\nlohmann\') -Force

New-Item -ItemType Directory -Force -Path (Join-Path $root 'thirdparty\sqlite') | Out-Null
Copy-Item (Join-Path $Proj 'thirdparty\sqlite\sqlite3.c') (Join-Path $root 'thirdparty\sqlite\') -Force
Copy-Item (Join-Path $Proj 'thirdparty\sqlite\sqlite3.h') (Join-Path $root 'thirdparty\sqlite\') -Force

# ---- 4. 清理临时/构建/日志文件 ----
Get-ChildItem $root -Recurse -File | Where-Object {
    $_.Name -match '^_' -or
    $_.Extension -in '.log', '.db', '.zip' -or
    $_.Name -in 'agentforge.log', 'agentforge_gui.log'
} | Remove-Item -Force

Get-ChildItem $root -Recurse -Directory | Where-Object {
    $_.Name -in 'build', 'build_gui', '.af-sessions', '__pycache__'
} | Remove-Item -Recurse -Force

# ---- 5. 打包（zip）----
$zipExe = 'C:\Program Files\7-Zip\7z.exe'
if (-not (Test-Path $zipExe)) { $zipExe = (Get-Command 7z -ErrorAction SilentlyContinue).Source }
if (-not $zipExe) { Write-Output "FATAL: 需要 7-Zip"; exit 1 }

Remove-Item $Out -ErrorAction SilentlyContinue
& $zipExe a -tzip -y $Out (Join-Path $staging 'AgentForge') 2>&1 | Out-Null
if (-not (Test-Path $Out)) { Write-Output "[FAIL] 打包失败"; exit 1 }

Remove-Item -Recurse -Force $staging
$size = [math]::Round((Get-Item $Out).Length / 1MB, 1)
Write-Output "[OK] $Out ($size MB)"
