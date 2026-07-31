<#
.SYNOPSIS
    Refresh NXTDebugger/wire/ from a sibling NXTLibrary checkout.

.DESCRIPTION
    wire/ holds the published wire surface — the schema this debugger and any
    third-party consumer bind against. NXTLibrary is the source of truth for
    those files; this script copies them across byte-for-byte.

    The copies must stay byte-identical: cmake.toml diffs them at configure
    time whenever a sibling NXTLibrary checkout is present, and fails the build
    on drift. Run this after any producer-side wire change, then rebuild.

    A public clone has no sibling checkout. There, wire/ is simply the schema,
    the configure-time check is skipped, and this script is not needed.

.PARAMETER NxtLibraryRoot
    Path to the NXTLibrary checkout. Defaults to ../NXTLibrary relative to the
    repository root.

.PARAMETER Check
    Report drift and exit non-zero without writing anything.

.EXAMPLE
    .\tools\sync_wire.ps1
    .\tools\sync_wire.ps1 -Check
    .\tools\sync_wire.ps1 -NxtLibraryRoot D:\src\NXTLibrary
#>
[CmdletBinding()]
param(
    [string] $NxtLibraryRoot,
    [switch] $Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Keep this list in sync with the WIRE_FILES list in cmake.toml — the configure
# time drift check walks the same set.
$wireFiles = @(
    'ipc/SharedLayout.h',
    'ipc/Events.h',
    'ipc/WireCategory.h',
    'rpc/MsgPack.h',
    'rpc/MsgPack.cpp'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NxtLibraryRoot)
{
    $NxtLibraryRoot = Join-Path (Split-Path -Parent $repoRoot) 'NXTLibrary'
}

$sourceRoot = Join-Path $NxtLibraryRoot 'src'
if (-not (Test-Path -LiteralPath $sourceRoot))
{
    Write-Error "NXTLibrary source not found at '$sourceRoot'. Pass -NxtLibraryRoot to point at the checkout."
}

$destRoot = Join-Path $repoRoot 'wire'
$changed = @()
$missing = @()

foreach ($relative in $wireFiles)
{
    $source = Join-Path $sourceRoot ($relative -replace '/', '\')
    $dest   = Join-Path $destRoot   ($relative -replace '/', '\')

    if (-not (Test-Path -LiteralPath $source))
    {
        $missing += $relative
        continue
    }

    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $destHash   = $null
    if (Test-Path -LiteralPath $dest)
    {
        $destHash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    }

    if ($sourceHash -eq $destHash)
    {
        Write-Host "  ok      $relative"
        continue
    }

    $changed += $relative
    if ($Check)
    {
        Write-Host "  DRIFT   $relative" -ForegroundColor Yellow
        continue
    }

    $destDir = Split-Path -Parent $dest
    if (-not (Test-Path -LiteralPath $destDir))
    {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $source -Destination $dest -Force
    Write-Host "  updated $relative" -ForegroundColor Green
}

if ($missing.Count -gt 0)
{
    Write-Error ("Missing in NXTLibrary: " + ($missing -join ', '))
}

if ($changed.Count -eq 0)
{
    Write-Host "wire/ is up to date."
    exit 0
}

if ($Check)
{
    Write-Host ""
    Write-Host "$($changed.Count) file(s) drifted. Run tools\sync_wire.ps1 to refresh." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "$($changed.Count) file(s) refreshed. Rebuild, and re-check the kProtocolVersion pin in src/attach/Session.h."
exit 0
