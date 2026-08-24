[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Configure", "Build", "Run", "Test", "Install")]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter()]
        [string[]]$Arguments = @()
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command $($Arguments -join ' ')"
    }
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw "This MSVC entrypoint runs on Windows; use freecm_project.py on macOS or Linux."
}

if (-not $env:VSCMD_VER) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "Visual Studio Installer vswhere.exe was not found."
    }

    $installationPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $installationPath) {
        throw "A Visual Studio installation with the x64 C++ toolchain was not found."
    }

    $devShell = Join-Path $installationPath "Common7\Tools\Launch-VsDevShell.ps1"
    & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ravoRoot = Join-Path $repositoryRoot "Ravo"
$configurationName = $Configuration.ToLowerInvariant()
$preset = "ravo_win_msvc_$configurationName"
$buildDirectory = Join-Path $repositoryRoot "build\$preset"
$installDirectory = Join-Path $repositoryRoot "install\$preset"

Push-Location $ravoRoot
try {
    switch ($Action) {
        "Configure" {
            $testing = if ($Configuration -eq "Debug") { "ON" } else { "OFF" }
            Invoke-NativeCommand "cmake" @("--preset", $preset, "-DBUILD_TESTING=$testing")
        }
        "Build" {
            Invoke-NativeCommand "cmake" @("--build", "--preset", $preset, "--parallel")
        }
        "Run" {
            Invoke-NativeCommand "cmake" @("--build", "--preset", $preset, "--target", "ravo_studio", "--parallel")
            Invoke-NativeCommand (Join-Path $buildDirectory "desktop\ravo_studio.exe")
        }
        "Test" {
            Invoke-NativeCommand "cmake" @("--preset", $preset, "-DBUILD_TESTING=ON")
            Invoke-NativeCommand "cmake" @("--build", "--preset", $preset, "--parallel")
            Invoke-NativeCommand "ctest" @("--test-dir", $buildDirectory, "--output-on-failure")
        }
        "Install" {
            Invoke-NativeCommand "cmake" @("--build", "--preset", $preset, "--parallel")
            Invoke-NativeCommand "cmake" @("--install", $buildDirectory, "--prefix", $installDirectory)
        }
    }
}
finally {
    Pop-Location
}
