<#
.SYNOPSIS
Build ai-desktop from this checkout and install it for the current user, from the terminal.

.DESCRIPTION
The same entry point the other desktop apps have (scripts/build-install.ps1, scripts/uninstall.ps1),
for a machine where no setup may be started (SmartScreen, a managed workstation). The work is
scripts/setup.ps1's: it configures and builds with CMake (MSYS2 / MinGW-w64), installs into
%LOCALAPPDATA%\ai-desktop and smoke-tests the copy; here the launcher folder also goes on the user
PATH, so `ai-desktop` runs in a new terminal. $env:MSYS2_ROOT, when set, is the MSYS2 to build with.

Run it in a normal terminal: a Claude session (the Claude app redirects AppData and HKCU) may
build (-BuildOnly), installing there is refused.

.PARAMETER BuildOnly
Build build\ai-desktop.exe and stop (setup.ps1 -SkipInstall).

.PARAMETER NoPath
Install, but leave the user PATH alone.

.EXAMPLE
.\scripts\build-install.ps1
.\scripts\build-install.ps1 -BuildOnly
#>
[CmdletBinding()]
param(
    [switch] $BuildOnly,
    [switch] $NoPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $BuildOnly -and $env:CLAUDECODE) {
    throw 'build-install.ps1: install in a normal terminal, not in a Claude session (the app redirects AppData and the registry); -BuildOnly builds'
}

$setupArgs = @{}
if ($env:MSYS2_ROOT) { $setupArgs['Msys2Root'] = $env:MSYS2_ROOT }
if ($BuildOnly) { $setupArgs['SkipInstall'] = $true }
elseif (-not $NoPath) { $setupArgs['AddToPath'] = $true }

& (Join-Path $PSScriptRoot 'setup.ps1') @setupArgs
