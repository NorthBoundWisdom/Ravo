[CmdletBinding()]
param(
    [ValidateSet("x64", "x86", "arm64", "x64_arm64", "x64_x86")]
    [string]$Architecture = "x64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
    throw "GITHUB_ENV is required"
}

$programFilesX86 = ${env:ProgramFiles(x86)}
if ([string]::IsNullOrWhiteSpace($programFilesX86)) {
    throw "ProgramFiles(x86) is not defined"
}

$vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe is missing: $vswhere"
}

$vcvarsArguments = @(
    "-latest",
    "-products", "*",
    "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "-find", "VC\Auxiliary\Build\vcvarsall.bat"
)
$vcvarsCandidates = @(& $vswhere @vcvarsArguments)
$vcvars = $vcvarsCandidates |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($vcvars) -or
    -not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
    throw "A Visual Studio C++ vcvarsall.bat installation was not found"
}

$command = 'call "{0}" {1} >nul && set' -f $vcvars, $Architecture
$configuredEnvironment = @(& $env:ComSpec /d /s /c $command)
if ($LASTEXITCODE -ne 0) {
    throw "vcvarsall.bat failed with exit code $LASTEXITCODE"
}

$pathVariables = @("PATH", "INCLUDE", "LIB", "LIBPATH")
$changedCount = 0
foreach ($line in $configuredEnvironment) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) {
        continue
    }
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    $oldValue = [Environment]::GetEnvironmentVariable($name, "Process")
    if ($oldValue -ceq $value) {
        continue
    }
    if ($pathVariables -contains $name) {
        $seen = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase
        )
        $parts = foreach ($part in $value.Split(';')) {
            if ($seen.Add($part)) {
                $part
            }
        }
        $value = $parts -join ';'
    }
    Add-Content -LiteralPath $env:GITHUB_ENV -Value ("{0}={1}" -f $name, $value) -Encoding utf8
    [Environment]::SetEnvironmentVariable($name, $value, "Process")
    $changedCount += 1
}

foreach ($required in @("VCINSTALLDIR", "VCToolsInstallDir", "VCToolsRedistDir")) {
    if ([string]::IsNullOrWhiteSpace(
            [Environment]::GetEnvironmentVariable($required, "Process")
        )) {
        throw "vcvarsall.bat did not define $required"
    }
}

Write-Host "Configured the MSVC $Architecture environment ($changedCount changed variables)"
