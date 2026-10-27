# Dragon Quest IX for PC: a ROM inside a .zip. Used by BUILD.bat and the setup window.
#
#   rom-unzip.ps1 -Zip <file.zip> -OutDir <folder> -Result <file>
#
# Takes the .nds out of the zip (the one at its top level if there are several,
# a Dragon Quest IX one first) into <folder>, and writes its path to <file>.
# Exits 1 with a message if the zip has no .nds in it.
#
# Runs on the PowerShell that comes with Windows 10 and 11 (5.1).
param([string]$Zip, [string]$OutDir, [string]$Result, [switch]$Load)   # -Load: only define the functions (setup-wizard.ps1)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# The 4-letter game code in a ROM's header (YDQE = Dragon Quest IX, USA).
function Get-RomCode([IO.Stream]$s) {
    $buf = New-Object byte[] 16
    $n = 0
    while ($n -lt 16) { $k = $s.Read($buf, $n, 16 - $n); if ($k -le 0) { break }; $n += $k }
    if ($n -lt 16) { return '' }
    return ([Text.Encoding]::ASCII.GetString($buf, 12, 4) -replace '[^A-Za-z0-9]', '')
}

# The .nds entry to use: @{ Name; Code; Size }, or $null.
function Find-ZipRom([string]$ZipPath) {
    $z = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $best = $null; $bestScore = -1
        foreach ($e in $z.Entries) {
            if (-not $e.FullName.EndsWith('.nds', [StringComparison]::OrdinalIgnoreCase)) { continue }
            $s = $e.Open(); try { $code = Get-RomCode $s } finally { $s.Close() }
            $score = 0
            if ($code -like 'YDQ*') { $score += 4 }
            if ($code -eq 'YDQE') { $score += 2 }
            if ($e.FullName -notmatch '[/\\]') { $score += 1 }      # at the top of the zip
            if ($score -gt $bestScore) { $best = @{ Name = $e.FullName; Code = $code; Size = $e.Length }; $bestScore = $score }
        }
        return $best
    } finally { $z.Dispose() }
}

# Unpack that entry to $Dir; returns the new file's path.
function Expand-ZipRom([string]$ZipPath, [string]$Dir) {
    $r = Find-ZipRom $ZipPath
    if (-not $r) { return $null }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $dest = Join-Path $Dir ([IO.Path]::GetFileName($r.Name))
    $z = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $e = $z.GetEntry($r.Name)
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
    } finally { $z.Dispose() }
    return $dest
}

if ($Load) { return }

try {
    $out = Expand-ZipRom $Zip $OutDir
} catch {
    [Console]::Out.WriteLine("Couldn't open the zip ($Zip): $($_.Exception.Message)")
    exit 1
}
if (-not $out) {
    [Console]::Out.WriteLine("There is no ROM (.nds file) in $Zip.")
    exit 1
}
[Console]::Out.WriteLine("Took the ROM out of the zip: $out")
if ($Result) { [IO.File]::WriteAllText($Result, $out) }
exit 0
