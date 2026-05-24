$ErrorActionPreference = "Stop"

$OutDir = "tmp"
$ReportDir = "specs\006-remove-kv-metadata-state-machine\task-reports"

$ConfigureLog = Join-Path $OutDir "t052-windows-configure.log"
$BuildLog = Join-Path $OutDir "t052-windows-build.log"
$CTestLog = Join-Path $OutDir "t052-windows-full-ctest-single-worker.log"
$FailedFile = Join-Path $OutDir "t052-windows-failed-tests.md"
$ReportFile = Join-Path $ReportDir "T052-windows-final-validation.md"

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

if (-not (Test-Path $ReportDir)) {
    New-Item -ItemType Directory -Path $ReportDir | Out-Null
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

function Run-Step {
    param(
        [string]$Name,
        [string]$LogFile,
        [string]$Command,
        [string[]]$Arguments
    )

    Write-Host ""
    Write-Host "==== $Name ===="
    Write-Host ">>>> $Command $($Arguments -join ' ')"

    $StdOutFile = "$LogFile.stdout.tmp"
    $StdErrFile = "$LogFile.stderr.tmp"

    if (Test-Path $StdOutFile) { Remove-Item $StdOutFile -Force }
    if (Test-Path $StdErrFile) { Remove-Item $StdErrFile -Force }

    $Process = Start-Process `
        -FilePath $Command `
        -ArgumentList $Arguments `
        -NoNewWindow `
        -Wait `
        -PassThru `
        -RedirectStandardOutput $StdOutFile `
        -RedirectStandardError $StdErrFile

    $Combined = @()

    if (Test-Path $StdOutFile) {
        $Combined += Get-Content $StdOutFile
    }

    if (Test-Path $StdErrFile) {
        $Combined += Get-Content $StdErrFile
    }

    $Combined | Tee-Object -FilePath $LogFile

    if (Test-Path $StdOutFile) { Remove-Item $StdOutFile -Force }
    if (Test-Path $StdErrFile) { Remove-Item $StdErrFile -Force }

    return $Process.ExitCode
}

$ConfigureExit = Run-Step `
    -Name "Configure" `
    -LogFile $ConfigureLog `
    -Command $CMakeExe `
    -Arguments @("--preset", "windows")

$BuildExit = Run-Step `
    -Name "Build" `
    -LogFile $BuildLog `
    -Command $CMakeExe `
    -Arguments @("--build", "--preset", "windows-debug")

$env:CTEST_PARALLEL_LEVEL = "1"

$CTestExit = Run-Step `
    -Name "CTest Full Single Worker" `
    -LogFile $CTestLog `
    -Command $CTestExe `
    -Arguments @(
        "--test-dir", "build/windows",
        "-C", "Debug",
        "--output-on-failure",
        "--progress",
        "-j", "1"
    )

$FailedTests = @()

if (Test-Path $CTestLog) {
    $Lines = Get-Content $CTestLog
    $InFailedBlock = $false

    foreach ($Line in $Lines) {
        if ($Line -match "The following tests FAILED:") {
            $InFailedBlock = $true
            continue
        }

        if ($InFailedBlock -and $Line -match "^\s*[0-9]+\s+-\s+") {
            $FailedTests += $Line.Trim()
            continue
        }

        if ($InFailedBlock -and $Line -notmatch "^\s*[0-9]+\s+-\s+") {
            $InFailedBlock = $false
        }
    }
}

$ConfigureStatus = if ($ConfigureExit -eq 0) { "PASS" } else { "FAIL" }
$BuildStatus = if ($BuildExit -eq 0) { "PASS" } else { "FAIL" }
$CTestStatus = if ($CTestExit -eq 0) { "PASS" } else { "FAIL" }

$FailedContent = @()
$FailedContent += "# T052 Windows Failed Tests"
$FailedContent += ""
$FailedContent += "## Result"
$FailedContent += ""
$FailedContent += "- CTest: $CTestStatus"
$FailedContent += "- Exit code: $CTestExit"
$FailedContent += ""
$FailedContent += "## Failed tests"
$FailedContent += ""

if ($FailedTests.Count -eq 0) {
    $FailedContent += "- No failed tests"
} else {
    foreach ($Test in $FailedTests) {
        $FailedContent += "- $Test"
    }
}

$FailedContent += ""
$FailedContent += "## Full CTest log"
$FailedContent += ""
$FailedContent += "- $CTestLog"

$FailedContent | Set-Content -Path $FailedFile -Encoding UTF8

$ReportContent = @()
$ReportContent += "# T052 Windows Final Validation"
$ReportContent += ""
$ReportContent += "## Result summary"
$ReportContent += ""
$ReportContent += "- Configure: $ConfigureStatus"
$ReportContent += "- Build: $BuildStatus"
$ReportContent += "- CTest: $CTestStatus"
$ReportContent += ""
$ReportContent += "## Execution mode"
$ReportContent += ""
$ReportContent += "- CTEST_PARALLEL_LEVEL=1"
$ReportContent += "- ctest -j 1"
$ReportContent += "- Tests run one by one"
$ReportContent += ""
$ReportContent += "## Failed tests"
$ReportContent += ""

if ($FailedTests.Count -eq 0) {
    $ReportContent += "- No failed tests"
} else {
    foreach ($Test in $FailedTests) {
        $ReportContent += "- $Test"
    }
}

$ReportContent += ""
$ReportContent += "## Logs"
$ReportContent += ""
$ReportContent += "- Configure: $ConfigureLog"
$ReportContent += "- Build: $BuildLog"
$ReportContent += "- CTest: $CTestLog"
$ReportContent += "- Failed tests summary: $FailedFile"

$ReportContent | Set-Content -Path $ReportFile -Encoding UTF8

Write-Host ""
Write-Host "==== Summary ===="
Write-Host "Configure: $ConfigureStatus"
Write-Host "Build: $BuildStatus"
Write-Host "CTest: $CTestStatus"
Write-Host ""
Write-Host "CTest log: $CTestLog"
Write-Host "Failed tests file: $FailedFile"
Write-Host "T052 report: $ReportFile"

if ($ConfigureExit -ne 0 -or $BuildExit -ne 0 -or $CTestExit -ne 0) {
    exit 1
}

exit 0