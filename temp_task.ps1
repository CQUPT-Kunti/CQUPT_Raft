$ErrorActionPreference = "Stop"

$OutDir = "tmp"
$LogFile = Join-Path $OutDir "t034-windows-metadata-read-admission.log"

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
    Write-Host "==== T034 Windows Metadata Read Admission Validation ===="
    Write-Host "time: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Write-Host "repo: $(Get-Location)"
    Write-Host "cmake: $CMakeExe"
    Write-Host "ctest: $CTestExe"
    Write-Host "log: $LogFile"

    Write-Host ""
    Write-Host "==== Configure ===="
    Run-Cmd $CMakeExe @("--preset", "windows")

    Write-Host ""
    Write-Host "==== Build T034 affected targets ===="
    Run-Cmd $CMakeExe @(
        "--build", "--preset", "windows-debug",
        "--target",
        "raft_metadata_client",
        "test_metadata_client_scenario",
        "test_metadata_failover",
        "test_metadata_state_machine"
    )

    Write-Host ""
    Write-Host "==== Run T034 related tests ===="
    $env:CTEST_PARALLEL_LEVEL = "1"
    Run-Cmd $CTestExe @(
        "--test-dir", "build/windows",
        "-C", "Debug",
        "--output-on-failure",
        "-R", "(MetadataClientScenarioTest|MetadataFailoverTest|MetadataStateMachineTest)"
    )

    Write-Host ""
    Write-Host "==== Result ===="
    Write-Host "T034 Windows metadata read admission validation PASS"
    Write-Host "Only T034 related targets/tests were run; full CTest was not run."
    Write-Host "log saved to: $LogFile"
}
finally {
    Stop-Transcript
}