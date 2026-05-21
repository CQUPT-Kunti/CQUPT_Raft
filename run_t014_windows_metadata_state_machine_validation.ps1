$ErrorActionPreference = "Stop"

$OutDir = "tmp"
$LogFile = Join-Path $OutDir "t017-windows-metadata-state-machine-test.log"

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
    Write-Host "==== T017 Windows MetadataStateMachine Validation ===="
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
    Write-Host "==== Build test_metadata_state_machine ===="
    Run-Cmd $CMakeExe @(
        "--build", "--preset", "windows-debug",
        "--target", "test_metadata_state_machine"
    )

    Write-Host ""
    Write-Host "==== Run MetadataStateMachineTest ===="
    $env:CTEST_PARALLEL_LEVEL = "1"
    Run-Cmd $CTestExe @(
        "--test-dir", "build/windows",
        "-C", "Debug",
        "--output-on-failure",
        "-R", "^MetadataStateMachineTest\."
    )

    Write-Host ""
    Write-Host "==== Result ===="
    Write-Host "T017 Windows MetadataStateMachine validation PASS"
    Write-Host "Only related target/test was run; full CTest was not run."
    Write-Host "log saved to: $LogFile"
}
finally {
    Stop-Transcript
}