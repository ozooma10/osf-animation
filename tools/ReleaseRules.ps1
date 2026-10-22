# Shared release rules for the packagers, the smoke bench and the game scripts. No side effects on dot-source.

# Paths that must never ship: dev scripts, test harness mailboxes, smoke payloads, source maps, player state.
function Test-ReleaseForbiddenEntry([string]$Path) {
    $Path -match '(?i)(OSFTest\.(pex|psc)|TestHarness|smoke|\.map$|(^|/)(values|node_modules)/)'
}

# The private smoke build embeds the harness mailbox name; MSVC may store it narrow or wide.
function Test-InstrumentedDll([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    foreach ($encoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
        if ($encoding.GetString($bytes).Contains('OSFAnimationTestHarness.json')) { return $true }
    }
    $false
}
