# Dragon Quest IX for PC - uninstall. Started from Settings > Apps, or the
# Start menu's "Uninstall Dragon Quest IX". Removes the game's folder, its
# shortcuts and its Apps entry. Your saves are kept unless you say otherwise
# (moved to Documents\Dragon Quest IX saves). The build tools (MSYS2) stay:
# remove them from Settings > Apps if nothing else uses them.
#
#   -Test  load the functions only (no window)
param([switch]$Test, [string]$Folder)

$ErrorActionPreference = 'Stop'
$AppName = 'Dragon Quest IX'
$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\DQIX-Enhanced'
if (-not $Folder) { $Folder = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$SaveFiles = @('dqix.sav', 'dqix.sav.bak', 'settings.ini', 'wireless.sav', 'reports')

function Get-SaveFolder { return (Join-Path ([Environment]::GetFolderPath('MyDocuments')) "$AppName saves") }

# Move the saves out of the game folder; returns where they went (or '' if there were none).
function Save-Saves([string]$Game) {
    $found = @($SaveFiles | ForEach-Object { Join-Path $Game $_ } | Where-Object { Test-Path -LiteralPath $_ })
    $found += @(Get-ChildItem -LiteralPath $Game -Filter '*.sav*' -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
    $found = @($found | Select-Object -Unique)
    if (-not $found.Count) { return '' }
    $dest = Get-SaveFolder
    if (Test-Path -LiteralPath $dest) { $dest = "$dest $(Get-Date -Format 'yyyy-MM-dd HHmm')" }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    foreach ($f in $found) { Move-Item -LiteralPath $f -Destination $dest -Force }
    return $dest
}

function Remove-Install([string]$Dir, [bool]$KeepSaves) {
    $kept = ''
    $game = Join-Path $Dir 'game'
    if ($KeepSaves -and (Test-Path -LiteralPath $game)) { $kept = Save-Saves $game }
    foreach ($lnk in @((Join-Path ([Environment]::GetFolderPath('Desktop')) "$AppName.lnk"))) {
        # only our own shortcut: the one that points into this folder
        if (Test-Path -LiteralPath $lnk) {
            $t = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath
            if ($t -and $t.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $lnk -Force }
        }
    }
    $sm = Join-Path ([Environment]::GetFolderPath('Programs')) $AppName
    if (Test-Path -LiteralPath $sm) { Remove-Item -LiteralPath $sm -Recurse -Force }
    if (Test-Path $UninstallKey) {
        $loc = (Get-ItemProperty -Path $UninstallKey -ErrorAction SilentlyContinue).InstallLocation
        if (-not $loc -or $loc -ieq $Dir) { Remove-Item -Path $UninstallKey -Recurse -Force }
    }
    Remove-Item -LiteralPath $Dir -Recurse -Force -ErrorAction SilentlyContinue
    return $kept
}

if ($Test) { return }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# Run from a copy in TEMP, so the folder being removed isn't in use by this script.
$self = $MyInvocation.MyCommand.Path
if (-not $self.StartsWith($env:TEMP, [StringComparison]::OrdinalIgnoreCase)) {
    $copy = Join-Path $env:TEMP 'dqix-uninstall.ps1'
    Copy-Item -LiteralPath $self -Destination $copy -Force
    Start-Process -FilePath (Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe') -WorkingDirectory $env:TEMP `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden', '-File', "`"$copy`"", '-Folder', "`"$Folder`"")
    exit 0
}

if (-not (Test-Path -LiteralPath (Join-Path $Folder 'BUILD.bat'))) {
    [void][System.Windows.Forms.MessageBox]::Show("Dragon Quest IX isn't in $Folder any more. Its entry in Apps is removed.", $AppName)
    if (Test-Path $UninstallKey) { Remove-Item -Path $UninstallKey -Recurse -Force }
    exit 0
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "Uninstall $AppName"
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false; $form.MinimizeBox = $false
$form.StartPosition = 'CenterScreen'; $form.AutoSize = $true; $form.AutoSizeMode = 'GrowAndShrink'
$form.Padding = New-Object System.Windows.Forms.Padding(16)
$flow = New-Object System.Windows.Forms.FlowLayoutPanel
$flow.FlowDirection = 'TopDown'; $flow.AutoSize = $true; $flow.WrapContents = $false
$form.Controls.Add($flow)
$msg = New-Object System.Windows.Forms.Label
$msg.AutoSize = $true; $msg.MaximumSize = New-Object System.Drawing.Size(460, 0)
$msg.Text = "This removes Dragon Quest IX from this PC:`n  $Folder`nand its shortcuts.`n"
$keep = New-Object System.Windows.Forms.CheckBox
$keep.AutoSize = $true; $keep.Checked = $true
$keep.Text = "Keep my save file and settings (moved to Documents\$AppName saves)"
$buttons = New-Object System.Windows.Forms.FlowLayoutPanel
$buttons.AutoSize = $true; $buttons.Anchor = 'Right'; $buttons.Margin = New-Object System.Windows.Forms.Padding(0, 14, 0, 0)
$ok = New-Object System.Windows.Forms.Button; $ok.Text = 'Uninstall'; $ok.AutoSize = $true; $ok.DialogResult = 'OK'
$no = New-Object System.Windows.Forms.Button; $no.Text = 'Cancel'; $no.AutoSize = $true; $no.DialogResult = 'Cancel'
$buttons.Controls.AddRange(@($ok, $no))
$flow.Controls.AddRange(@($msg, $keep, $buttons))
$form.AcceptButton = $no; $form.CancelButton = $no

if ($form.ShowDialog() -ne 'OK') { exit 0 }
if (-not $keep.Checked) {
    $a = [System.Windows.Forms.MessageBox]::Show('Your save file will be deleted too. This cannot be undone. Continue?', $AppName, 'YesNo', 'Warning')
    if ($a -ne 'Yes') { exit 0 }
}
Get-Process dqix -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Folder, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object {
    [void][System.Windows.Forms.MessageBox]::Show('Close the game first, then press OK.', $AppName)
}
try { $kept = Remove-Install $Folder $keep.Checked } catch {
    [void][System.Windows.Forms.MessageBox]::Show("Some files couldn't be removed: $($_.Exception.Message)", $AppName, 'OK', 'Warning'); exit 1
}
$done = 'Dragon Quest IX is uninstalled.'
if ($kept) { $done += "`n`nYour saves are in:`n$kept`n(To use them again: put them in the new game folder.)" }
if (Test-Path -LiteralPath $Folder) { $done += "`n`nA few files are still in use; delete the folder $Folder yourself later." }
$done += "`n`nThe build tools (MSYS2) are still installed; remove them in Settings > Apps if you don't need them."
[void][System.Windows.Forms.MessageBox]::Show($done, $AppName)
