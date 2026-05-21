$ErrorActionPreference = "Stop"

$OutDir = "tmp"
$LogFile = Join-Path $OutDir "t010-windows-full-cmake-ctest.log"

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$CMakeExe = (Get-Command cmake -ErrorAction Stop).Source
$CMakeDir = Split-Path $CMakeExe -Parent
$CTestExe = Join-Path $CMakeDir "ctest.exe"

if (-not (Test-Path $CTestExe)) {
    $CTestCmd = Get-Command ctest -ErrorAction SilentlyContinue
    if ($null -eq $CTestCmd) {
        throw "ctest.exe not found. cmake.exe path: $CMakeExe"
    }
    $CTestExe = $CTestCmd.Source
}

function Run-Cmd {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    Write-Host ""
    Write-Host ">>>> $Command $($Arguments -join ' ')"
    & $Command @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command $($Arguments -join ' ')"
    }
}

Start-Transcript -Path $LogFile -Force

try {
    Write-Host "==== T010 Windows Full CMake/CTest Validation ===="
    Write-Host "time: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Write-Host "repo: $(Get-Location)"
    Write-Host "cmake: $CMakeExe"
    Write-Host "ctest: $CTestExe"
    Write-Host "log: $LogFile"
    Write-Host ""

    Write-Host "==== Git status ===="
    Run-Cmd "git" @("status", "--short")

    Write-Host ""
    Write-Host "==== Configure ===="
    Run-Cmd $CMakeExe @("--preset", "windows")

    Write-Host ""
    Write-Host "==== Build all ===="
    Run-Cmd $CMakeExe @("--build", "--preset", "windows-debug")

    Write-Host ""
    Write-Host "==== Full CTest ===="
    $env:CTEST_PARALLEL_LEVEL = "1"
    Run-Cmd $CTestExe @("--preset", "windows-debug-managed-tests")

    Write-Host ""
    Write-Host "==== Result ===="
    Write-Host "Windows full CMake/CTest validation PASS"
    Write-Host "log saved to: $LogFile"
}
finally {
    Stop-Transcript
}