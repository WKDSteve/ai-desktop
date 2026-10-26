<#
.SYNOPSIS
Remove ai-desktop for the current user on Windows, without administrator rights.

.DESCRIPTION
Takes away both ways it may be installed: the copy of scripts/build-install.ps1 / setup.ps1
(%LOCALAPPDATA%\ai-desktop, its launcher and its PATH entry; setup.ps1 -Uninstall) and the one of
the setup or the install line of a release (Inno Setup, run silently with its own uninstaller).
Your API keys and settings are not touched. A second run finds nothing to do and says so; it is
not an error.

.EXAMPLE
.\scripts\uninstall.ps1
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:CLAUDECODE) {
    throw 'uninstall.ps1: run this in a normal terminal, not in a Claude session (the app redirects AppData and the registry)'
}

$found = $false

# The copy of setup.ps1 (its default prefix).
if (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'ai-desktop\app\ai-desktop.exe')) {
    $found = $true
    & (Join-Path $PSScriptRoot 'setup.ps1') -Uninstall
}

# The copy of the setup: AppId of installer/ai-desktop.iss, PrivilegesRequired=lowest (HKCU).
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{5EA66451-99A0-4CBF-8A48-261C812EA959}_is1'
if (Test-Path -LiteralPath $key) {
    $found = $true
    $uninstaller = ((Get-ItemProperty -LiteralPath $key).UninstallString).Trim('"')
    Write-Output "running the uninstaller of the setup silently ($uninstaller)"
    $proc = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    if ($proc.ExitCode -ne 0) { throw "uninstall.ps1: the uninstaller failed with exit code $($proc.ExitCode)" }
    Write-Output 'removed the installation of the setup'
}

if (-not $found) { Write-Output 'ai-desktop is not installed for this user, nothing to do' }
