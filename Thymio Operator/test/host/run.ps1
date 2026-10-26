# Tests de la machine d'etats (Windows + Visual Studio C++)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path $project '.pio/host-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Installation Visual Studio C++ introuvable.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$batch = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /I"$PSScriptRoot/mocks" /I"$project/src" "$PSScriptRoot/test_sequences.cpp" "$project/src/arm_sequence.cpp" /Fe:sequence_tests.exe
if errorlevel 1 exit /b 1
sequence_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /I"$PSScriptRoot/mocks" /I"$project/src" "$PSScriptRoot/test_runtime.cpp" "$project/src/arm_sequence.cpp" "$project/src/motor_runtime.cpp" /Fe:runtime_tests.exe
if errorlevel 1 exit /b 1
runtime_tests.exe
"@
$runner = Join-Path $build 'run.cmd'
Set-Content -LiteralPath $runner -Encoding ascii -Value $batch
Push-Location $build
try {
  & $env:ComSpec /d /c run.cmd
  if ($LASTEXITCODE -ne 0) { throw 'Echec des tests de sequence.' }
} finally { Pop-Location }
