# ---------------------------------------------------------------------------
#  make-release.ps1 —— 把编译好的 agent.exe 打成可发布的 zip
#
#  用法：
#     .\deploy\make-release.ps1 -Exe <agent.exe 路径> -Version v1.0.0
#
#  产物：dist\AgentForge-Web-<version>.zip
#        zip 根目录直接是 agent.exe / run-web.* / workspace\，安装器解压即可用。
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Version = 'v1.0.0',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$Repo = Split-Path $Here -Parent
if (-not $OutDir) { $OutDir = Join-Path $Repo 'dist' }

if (-not (Test-Path $Exe)) { throw "找不到 agent.exe: $Exe" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stage = Join-Path $env:TEMP ("af_rel_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'workspace\web') | Out-Null

Copy-Item $Exe (Join-Path $stage 'agent.exe')
Copy-Item (Join-Path $Here 'run-web.ps1') $stage
Copy-Item (Join-Path $Here 'run-web.bat') $stage
Copy-Item (Join-Path $Here 'README.txt') $stage
Copy-Item (Join-Path $Here 'workspace\README.md') (Join-Path $stage 'workspace')

$zip = Join-Path $OutDir ("AgentForge-Web-{0}.zip" -f $Version)
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force

$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Output "[ok] $zip  ($mb MB)"
