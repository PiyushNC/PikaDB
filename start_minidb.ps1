$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

$qtCompiler = "C:\Qt\Tools\mingw1310_64\bin\g++.exe"
$qtPrefix = "C:\Qt\6.9.1\mingw_64"
$qtDeploy = Join-Path $qtPrefix "bin\windeployqt.exe"
$buildDirectory = Join-Path $projectRoot "build-qt-kit"
$guiExecutable = Join-Path $buildDirectory "minidb_gui.exe"

if ((Test-Path $qtCompiler) -and (Test-Path $qtPrefix)) {
    Write-Host "Configuring MiniDB with the Qt toolchain..."
    & cmake -S . -B $buildDirectory -G Ninja `
        -DCMAKE_BUILD_TYPE=Debug `
        "-DCMAKE_CXX_COMPILER=$qtCompiler" `
        "-DCMAKE_PREFIX_PATH=$qtPrefix"
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    Write-Host "Building MiniDB..."
    & cmake --build $buildDirectory --parallel 2
    if ($LASTEXITCODE -ne 0) { throw "MiniDB build failed." }

    if (Test-Path $qtDeploy) {
        Write-Host "Preparing Qt runtime files..."
        & $qtDeploy --no-translations $guiExecutable
        if ($LASTEXITCODE -ne 0) { throw "Qt runtime deployment failed." }
    }

    Write-Host "Starting MiniDB GUI..."
    & $guiExecutable
    exit $LASTEXITCODE
}

$coreBuildDirectory = Join-Path $projectRoot "build"
Write-Host "Qt was not found. Building the core SQL shell instead..."
& cmake -S . -B $coreBuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { throw "Core CMake configuration failed." }

& cmake --build $coreBuildDirectory --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Core MiniDB build failed." }

Write-Host "Starting MiniDB SQL shell..."
& (Join-Path $coreBuildDirectory "minidb_shell.exe") (Join-Path $projectRoot "data")
exit $LASTEXITCODE
