<#
.SYNOPSIS
    Assert wire/PROTOCOL.md still describes the real wire surface.

.DESCRIPTION
    PROTOCOL.md is the normative, language-agnostic description of this wire --
    what a third-party consumer binds against. Unlike the five files in wire/,
    it has no producer-side original to hash against: it is hand-maintained
    here, so the configure-time hash check in cmake.toml cannot see it drift.
    It duly did, twice, while that check stayed green.

    This closes the names half of that gap. It compares, in BOTH directions:

      1. Event discriminators  -- wire/ipc/Events.h  vs PROTOCOL.md section 3.3
      2. Event body structs    -- wire/ipc/Events.h  vs PROTOCOL.md section 3.4
      3. RPC method names      -- the producer's g_methods[] vs PROTOCOL.md section 4.4
      4. Retired methods       -- names section 4.4 calls removed must NOT be registered

    Checks 1, 2 and 4 read only files in this repository, so they run in a
    public clone. Check 3 needs the producer's src/rpc/Handlers.cpp, which is
    private and deliberately NOT copied into wire/ (it carries game internals);
    it is skipped when no producer checkout sits beside us, exactly as the
    cmake.toml hash check is.

    A set difference in either direction is a failure. "Documented but no longer
    registered" is as wrong as "registered but undocumented": the first sends a
    consumer after a method that now errors, and has happened here before.

    What this does NOT check: prose. section 4.4's narrative notes (which methods are
    stubs, what a field means) are not machine-comparable, and a stale one is
    how `change_login_state` came to be described as a stub months after it
    stopped being one. Absolute offsets are covered separately and by the
    compiler -- see src/wire/ProtocolDocPins.h.

.PARAMETER NxtLibraryRoot
    Path to the NXTLibrary checkout. Defaults to ../NXTLibrary relative to the
    repository root. Same default as tools/sync_wire.ps1.

.EXAMPLE
    .\tools\check_protocol_doc.ps1
    .\tools\check_protocol_doc.ps1 -NxtLibraryRoot D:\src\NXTLibrary
#>
[CmdletBinding()]
param(
    [string] $NxtLibraryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$docPath  = Join-Path $repoRoot 'wire\PROTOCOL.md'
$eventsH  = Join-Path $repoRoot 'wire\ipc\Events.h'

if (-not $NxtLibraryRoot)
{
    $NxtLibraryRoot = Join-Path (Split-Path -Parent $repoRoot) 'NXTLibrary'
}
$handlersPath = Join-Path $NxtLibraryRoot 'src\rpc\Handlers.cpp'

foreach ($required in @($docPath, $eventsH))
{
    if (-not (Test-Path -LiteralPath $required))
    {
        Write-Error "Required file not found: $required"
    }
}

$docLines = Get-Content -LiteralPath $docPath
$failures = @()

# Return the lines of PROTOCOL.md between two headings, exclusive of both.
function Get-DocSection
{
    param([string] $StartHeading, [string] $EndHeading)

    $start = -1
    $end   = -1
    for ($i = 0; $i -lt $docLines.Count; $i++)
    {
        if ($start -lt 0 -and $docLines[$i].StartsWith($StartHeading))
        {
            $start = $i + 1
            continue
        }
        if ($start -ge 0 -and $docLines[$i].StartsWith($EndHeading))
        {
            $end = $i
            break
        }
    }
    if ($start -lt 0) { Write-Error "PROTOCOL.md: heading not found: '$StartHeading'" }
    if ($end   -lt 0) { Write-Error "PROTOCOL.md: heading not found: '$EndHeading'" }
    return $docLines[$start..($end - 1)]
}

# Compare two name sets and record a failure per direction. A set that parsed
# empty is itself the failure: a regex that silently matches nothing would make
# every comparison below pass vacuously, which is the exact shape of bug this
# script exists to catch. $MinExpected is a floor, not a count to maintain.
function Compare-Sets
{
    param(
        [string]   $What,
        [string[]] $Producer,
        [string]   $ProducerLabel,
        [string[]] $Doc,
        [string]   $DocLabel,
        [int]      $MinExpected
    )

    if ($Producer.Count -lt $MinExpected)
    {
        $script:failures += "$What`: parsed only $($Producer.Count) name(s) from $ProducerLabel (expected at least $MinExpected) -- the parser is broken, not the data."
        return
    }
    if ($Doc.Count -lt $MinExpected)
    {
        $script:failures += "$What`: parsed only $($Doc.Count) name(s) from $DocLabel (expected at least $MinExpected) -- the parser is broken, not the data."
        return
    }

    $onlyProducer = @($Producer | Where-Object { $Doc -notcontains $_ } | Sort-Object)
    $onlyDoc      = @($Doc | Where-Object { $Producer -notcontains $_ } | Sort-Object)

    if ($onlyProducer.Count -gt 0)
    {
        $script:failures += "$What`: in $ProducerLabel but NOT documented in $DocLabel`:`n      $($onlyProducer -join "`n      ")"
    }
    if ($onlyDoc.Count -gt 0)
    {
        $script:failures += "$What`: documented in $DocLabel but NOT present in $ProducerLabel`:`n      $($onlyDoc -join "`n      ")"
    }
    if ($onlyProducer.Count -eq 0 -and $onlyDoc.Count -eq 0)
    {
        Write-Host "  ok      $What ($($Producer.Count) names)"
    }
}

# --- 1. Event discriminators: Events.h enum vs section 3.3 -------------------------
#
# The numbers are the contract; the doc's labels are prose and deliberately
# read better than the enum names ("varp change" for kEventVarChange), so only
# the discriminator values are compared.
#
# kEventNone = 0 is excluded: the header marks it "never published; reserved
# sentinel", so it is correctly absent from a table of types a consumer can
# receive. Any other producer-only value is drift.
$eventsText = Get-Content -LiteralPath $eventsH -Raw
$enumBlock  = [regex]::Match($eventsText, '(?s)enum\s+EventType\s*:\s*uint32_t\s*\{(.+?)\n\};')
if (-not $enumBlock.Success)
{
    Write-Error "Could not locate 'enum EventType' in $eventsH"
}
$excludedEvents = @('kEventNone')
$producerEvents = @(
    [regex]::Matches($enumBlock.Groups[1].Value, '(?m)^\s*(kEvent\w+)\s*=\s*(\d+)\s*,') |
        Where-Object { $excludedEvents -notcontains $_.Groups[1].Value } |
        ForEach-Object { $_.Groups[2].Value }
)

$sec33   = Get-DocSection '### 3.3 Event types' '### 3.4'
$docEvents = @(
    $sec33 | Where-Object { $_ -match '^\|\s*\d+\s*\|' } |
        ForEach-Object { ([regex]::Match($_, '^\|\s*(\d+)\s*\|')).Groups[1].Value }
)

Compare-Sets -What 'event discriminators' `
             -Producer $producerEvents -ProducerLabel 'wire/ipc/Events.h' `
             -Doc $docEvents -DocLabel 'PROTOCOL.md section 3.3' -MinExpected 15

# --- 2. Event body structs: Events.h vs section 3.4 --------------------------------
$producerBodies = @(
    [regex]::Matches($eventsText, '(?m)^struct\s+(\w+Body)\s*\{') |
        ForEach-Object { $_.Groups[1].Value }
)

$sec34     = Get-DocSection '### 3.4 Body layouts' '## 4.'
$docBodies = @(
    $sec34 | Where-Object { $_.StartsWith('|') } |
        ForEach-Object { [regex]::Matches($_, '`(\w+Body)`') } |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
)

Compare-Sets -What 'event body structs' `
             -Producer $producerBodies -ProducerLabel 'wire/ipc/Events.h' `
             -Doc $docBodies -DocLabel 'PROTOCOL.md section 3.4' -MinExpected 12

# --- 3 + 4. RPC method names: producer g_methods[] vs section 4.4 ------------------
#
# Only table rows are read, never the surrounding prose: section 4.4's narrative names
# retired methods in backticks on purpose, and they must not be mistaken for
# catalog entries.
$sec44      = Get-DocSection '### 4.4 Method catalog' '### 4.5'
$docMethods = @(
    $sec44 | Where-Object { $_.StartsWith('|') } |
        ForEach-Object { [regex]::Matches($_, '`([a-z_][a-z0-9_.]*)`') } |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
)

# The nine inert walker placeholders removed on 2026-08-09. section 4.4 tells consumers
# these now return "method not found"; if one is ever re-registered that promise
# silently breaks, so assert their absence rather than trusting the prose.
$retiredMethods = @(
    'walk_to', 'walk_world_path', 'walk_cancel', 'walk_status', 'is_reachable',
    'find_path', 'find_world_path', 'region_cache_info', 'region_cache_clear'
)

if (Test-Path -LiteralPath $handlersPath)
{
    $handlersText = Get-Content -LiteralPath $handlersPath -Raw
    $methodBlock  = [regex]::Match($handlersText, '(?s)const\s+Method\s+g_methods\[\]\s*=\s*\{(.+?)\n\};')
    if (-not $methodBlock.Success)
    {
        Write-Error "Could not locate 'const Method g_methods[]' in $handlersPath"
    }
    $producerMethods = @(
        [regex]::Matches($methodBlock.Groups[1].Value, '(?m)^\s*\{\s*"([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value }
    )

    Compare-Sets -What 'RPC methods' `
                 -Producer $producerMethods -ProducerLabel 'NXTLibrary g_methods[]' `
                 -Doc $docMethods -DocLabel 'PROTOCOL.md section 4.4' -MinExpected 40

    $resurrected = @($retiredMethods | Where-Object { $producerMethods -contains $_ } | Sort-Object)
    if ($resurrected.Count -gt 0)
    {
        $failures += "retired methods: section 4.4 documents these as removed, but they are registered again:`n      $($resurrected -join "`n      ")"
    }
    else
    {
        Write-Host "  ok      retired methods still absent ($($retiredMethods.Count) names)"
    }
}
else
{
    Write-Host "  skip    RPC methods -- no producer checkout at $handlersPath"
}

# A retired method must also stay out of the catalog table, which needs no
# producer checkout to verify.
$retiredInDoc = @($retiredMethods | Where-Object { $docMethods -contains $_ } | Sort-Object)
if ($retiredInDoc.Count -gt 0)
{
    $failures += "retired methods: listed in the section 4.4 catalog table despite being removed:`n      $($retiredInDoc -join "`n      ")"
}

if ($failures.Count -gt 0)
{
    Write-Host ""
    Write-Host "PROTOCOL.md has drifted from the wire surface:" -ForegroundColor Red
    foreach ($failure in $failures)
    {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "Update wire/PROTOCOL.md to match, then re-run. It is the normative" -ForegroundColor Yellow
    Write-Host "description a third-party consumer binds against -- the producer wins." -ForegroundColor Yellow
    exit 1
}

Write-Host "PROTOCOL.md matches the wire surface."
exit 0
