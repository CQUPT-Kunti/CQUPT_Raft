$ErrorActionPreference = "Stop"

$OutDir = "tmp"
$LogFile = Join-Path $OutDir "t011-windows-proto-boundary-build.log"

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$CMakeExe = (Get-Command cmake -ErrorAction Stop).Source

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
    Write-Host "==== T011 Windows Proto Boundary Validation ===="
    Write-Host "time: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Write-Host "repo: $(Get-Location)"
    Write-Host "cmake: $CMakeExe"
    Write-Host "log: $LogFile"

    Write-Host ""
    Write-Host "==== Git status ===="
    Run-Cmd "git" @("status", "--short")

    Write-Host ""
    Write-Host "==== Configure ===="
    Run-Cmd $CMakeExe @("--preset", "windows")

    Write-Host ""
    Write-Host "==== Build affected T011 targets ===="
    Run-Cmd $CMakeExe @(
        "--build", "--preset", "windows-debug",
        "--target",
        "common_proto",
        "raft_proto",
        "metadata_proto",
        "kv_proto",
        "raft_demo",
        "raft_kv_client",
        "raft_metadata_client",
        "test_kv_service",
        "test_metadata_client_scenario",
        "test_metadata_failover"
    )

    Write-Host ""
    Write-Host "==== Result ===="
    Write-Host "T011 Windows proto/service/client boundary validation PASS"
    Write-Host "Only configure + affected target build was run."
    Write-Host "Full CTest was not run for this task."
    Write-Host "log saved to: $LogFile"
}
finally {
    Stop-Transcript
}