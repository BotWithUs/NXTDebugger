<#
.SYNOPSIS
    Assert the Debug Draw panel's mirrored caps still match the producer's.

.DESCRIPTION
    src/panels/DebugDrawPanel.cpp displays the drawing API's limits -- 512
    retained commands, 64 text slots, 64 polyline slots, 256 items per batch, a
    47-byte key, a 127-unit caption, and the two coordinate bounds -- because a
    cap the user cannot see is a cap they discover by hitting it.

    Displaying them means copying them, and a copied constant drifts silently:
    the panel would go on confidently telling the user "512" long after the
    producer moved, and the number looks authoritative precisely because it is
    rendered by a debugger.

    Neither existing gate covers this. tools\check_protocol_doc.ps1 compares
    METHOD NAMES only -- it cannot see a cap. src\wire\ProtocolDocPins.h pins
    absolute offsets with the compiler, but it can only pin what lives in
    wire\, and the draw store is deliberately NOT in wire\: it is not in shared
    memory, it does not move kProtocolVersion, and overlay\DrawTypes.h carries
    game internals that must not be published. So this is a third gate in the
    same family, conditional on a producer checkout exactly as the others are.

    Two derived values are checked as derivations rather than as numbers, since
    that is what would actually be wrong if either moved:

      kMaxKeyBytes  == kMaxKeyChars    - 1   (the producer counts the NUL)
      kMaxTextUnits == kTextSlotChars  - 1   (likewise)

    What this does NOT check: the auto-key formats, the reply field names, and
    the batch's success-carrying-a-cap-hit semantics. Those are prose contracts
    with no machine-readable producer-side form, and the panel mirrors them in
    comments that cite where they came from. They remain unguarded -- see the
    Phase 4 report.

.PARAMETER NxtLibraryRoot
    Path to the NXTLibrary checkout. Defaults to ../NXTLibrary relative to the
    repository root. Same default as tools/sync_wire.ps1.

.EXAMPLE
    .\tools\check_draw_caps.ps1
    .\tools\check_draw_caps.ps1 -NxtLibraryRoot D:\src\NXTLibrary
#>
[CmdletBinding()]
param(
    [string] $NxtLibraryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot  = Split-Path -Parent $PSScriptRoot
$panelPath = Join-Path $repoRoot 'src\panels\DebugDrawPanel.cpp'

if (-not $NxtLibraryRoot)
{
    $NxtLibraryRoot = Join-Path (Split-Path -Parent $repoRoot) 'NXTLibrary'
}
$drawTypesPath = Join-Path $NxtLibraryRoot 'src\overlay\DrawTypes.h'
$handlersPath  = Join-Path $NxtLibraryRoot 'src\rpc\Handlers.cpp'

if (-not (Test-Path -LiteralPath $panelPath))
{
    Write-Error "Required file not found: $panelPath"
}

if (-not (Test-Path -LiteralPath $drawTypesPath) -or
    -not (Test-Path -LiteralPath $handlersPath))
{
    # SKIPPED, and it must not read as PASSED. The caller prints this line
    # verbatim on the success path precisely so that "nothing was checked" and
    # "everything checked out" cannot produce the same entry in the CMake log.
    Write-Host "SKIPPED - nothing was checked: no producer checkout at $NxtLibraryRoot"
    exit 0
}

# Collect `constexpr <integer type> <name> = <expr>;` into name -> expression.
# Pointer and array constants are excluded by the type alternation, so the
# kind/font name tables never land here.
function Read-IntConstants
{
    param([string] $Path)

    $table = @{}
    $pattern = '^\s*constexpr\s+(?:u?int(?:8|16|32|64)_t|unsigned\s+\w+|unsigned|int|DWORD)\s+(\w+)\s*=\s*([^;]+);'
    foreach ($line in (Get-Content -LiteralPath $Path))
    {
        $m = [regex]::Match($line, $pattern)
        if ($m.Success)
        {
            $table[$m.Groups[1].Value] = $m.Groups[2].Value.Trim()
        }
    }
    return $table
}

# Evaluate the small expression grammar these constants actually use: decimal
# literals, references to other constants in the same table, `<<` and `*`.
# Anything else is an error rather than a guess -- a silently-zero evaluator
# would make every comparison below pass vacuously, which is the shape of bug
# this script exists to catch.
function Resolve-Constant
{
    param(
        [hashtable] $Table,
        [string]    $Name,
        [int]       $Depth = 0
    )

    if ($Depth -gt 8)
    {
        Write-Error "Constant '$Name' is recursive or too deeply nested to evaluate."
    }
    if (-not $Table.ContainsKey($Name))
    {
        return $null
    }

    $expr = $Table[$Name]
    # Strip a trailing comment and any cast-free parentheses.
    $expr = ($expr -replace '/\*.*?\*/', '').Trim()
    $expr = $expr.Trim('(', ')', ' ')

    $tokens = [regex]::Split($expr, '\s*(<<|\*)\s*')
    $values = @()
    $ops    = @()
    for ($i = 0; $i -lt $tokens.Count; $i++)
    {
        $t = $tokens[$i].Trim()
        if ($t -eq '<<' -or $t -eq '*')
        {
            $ops += $t
            continue
        }
        if ($t -match '^-?\d+$')
        {
            $values += [int64] $t
            continue
        }
        if ($t -match '^\w+$')
        {
            $nested = Resolve-Constant -Table $Table -Name $t -Depth ($Depth + 1)
            if ($null -eq $nested)
            {
                Write-Error "Constant '$Name': cannot resolve '$t' in expression '$expr'."
            }
            $values += $nested
            continue
        }
        Write-Error "Constant '$Name': unsupported expression '$expr' (token '$t')."
    }

    $acc = $values[0]
    for ($i = 0; $i -lt $ops.Count; $i++)
    {
        if ($ops[$i] -eq '<<') { $acc = $acc -shl [int] $values[$i + 1] }
        else                   { $acc = $acc * $values[$i + 1] }
    }
    return $acc
}

$panelConsts   = Read-IntConstants -Path $panelPath
$producerTypes = Read-IntConstants -Path $drawTypesPath
$producerRpc   = Read-IntConstants -Path $handlersPath

# A parser that matched nothing must fail, not pass. These floors are floors,
# not counts to maintain.
$parseFloors = @(
    @{ What = 'panel';               Table = $panelConsts;   Min = 11 },
    @{ What = 'producer DrawTypes.h'; Table = $producerTypes; Min = 10 },
    @{ What = 'producer Handlers.cpp'; Table = $producerRpc;  Min = 2  }
)
foreach ($floor in $parseFloors)
{
    if ($floor.Table.Count -lt $floor.Min)
    {
        Write-Host ""
        Write-Host ("draw caps: parsed only {0} constant(s) from {1} (expected at least {2})" -f `
            $floor.Table.Count, $floor.What, $floor.Min) -ForegroundColor Red
        Write-Host "  The parser is broken, not the data." -ForegroundColor Red
        exit 1
    }
}

# panel constant -> how the producer spells the same quantity.
# `Adjust` is the producer-side correction: the two length caps count the NUL
# terminator on the producer side and do not on the wire.
$mappings = @(
    @{ Panel = 'kMaxDrawCmds';     Producer = 'kMaxDrawCmds';       From = 'types'; Adjust =  0; Note = 'retained commands' },
    @{ Panel = 'kMaxTextSlots';    Producer = 'kMaxTextSlots';      From = 'types'; Adjust =  0; Note = 'text slots' },
    @{ Panel = 'kMaxPolySlots';    Producer = 'kMaxPolySlots';      From = 'types'; Adjust =  0; Note = 'polyline slots' },
    @{ Panel = 'kMaxKeyBytes';     Producer = 'kMaxKeyChars';       From = 'types'; Adjust = -1; Note = 'key length, minus the NUL' },
    @{ Panel = 'kMaxTextUnits';    Producer = 'kTextSlotChars';     From = 'types'; Adjust = -1; Note = 'caption length, minus the NUL' },
    @{ Panel = 'kMaxScreenCoord';  Producer = 'kMaxDrawCoord';      From = 'types'; Adjust =  0; Note = 'screen coordinate bound' },
    @{ Panel = 'kMaxScreenExtent'; Producer = 'kMaxDrawExtent';     From = 'types'; Adjust =  0; Note = 'screen extent bound' },
    @{ Panel = 'kMaxWorldCoord';   Producer = 'kMaxWorldCoord';     From = 'types'; Adjust =  0; Note = 'world coordinate bound' },
    @{ Panel = 'kMaxWorldTile';    Producer = 'kMaxWorldTile';      From = 'types'; Adjust =  0; Note = 'world tile bound' },
    @{ Panel = 'kMaxBatchItems';   Producer = 'kDrawBatchMax';      From = 'rpc';   Adjust =  0; Note = 'items per batch' },
    @{ Panel = 'kListPageMax';     Producer = 'kDrawListPageMax';   From = 'rpc';   Adjust =  0; Note = 'list page limit' }
)

$failures = @()
foreach ($map in $mappings)
{
    $panelValue = Resolve-Constant -Table $panelConsts -Name $map.Panel
    if ($null -eq $panelValue)
    {
        $failures += "$($map.Panel): not found in src\panels\DebugDrawPanel.cpp -- was it renamed?"
        continue
    }

    $producerTable = if ($map.From -eq 'types') { $producerTypes } else { $producerRpc }
    $producerValue = Resolve-Constant -Table $producerTable -Name $map.Producer
    if ($null -eq $producerValue)
    {
        $failures += "$($map.Producer): not found in the producer -- was it renamed?"
        continue
    }

    $expected = $producerValue + $map.Adjust
    if ($panelValue -ne $expected)
    {
        $failures += ("{0} ({1}): panel says {2}, producer's {3} says {4}{5}" -f `
            $map.Panel, $map.Note, $panelValue, $map.Producer, $expected,
            $(if ($map.Adjust -ne 0) { " (= $($map.Producer) $($map.Adjust))" } else { '' }))
    }
}

if ($failures.Count -gt 0)
{
    Write-Host ""
    Write-Host "The Debug Draw panel's caps have drifted from the producer:" -ForegroundColor Red
    foreach ($failure in $failures)
    {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "The panel DISPLAYS these numbers, so a stale one is a debugger" -ForegroundColor Yellow
    Write-Host "confidently reporting the wrong limit. The producer wins." -ForegroundColor Yellow
    exit 1
}

Write-Host "checked $($mappings.Count) cap(s) against the producer - all match"
exit 0
