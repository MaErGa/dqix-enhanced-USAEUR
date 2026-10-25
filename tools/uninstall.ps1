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

# The texts in this file are English; T gives the Spanish one when Windows is in Spanish.
$Lang = 'en'
try { $Lang = (Get-UICulture).TwoLetterISOLanguageName } catch { }
$Es = @{
    'Dragon Quest IX isn''t in {0} any more. Its entry in Apps is removed.' = 'Dragon Quest IX ya no est\u00e1 en {0}. Su entrada en Aplicaciones se ha quitado.'
    'Uninstall {0}' = 'Desinstalar {0}'
    'This removes Dragon Quest IX from this PC:\n  {0}\nand its shortcuts.\n' = 'Esto quita Dragon Quest IX de este PC:\n  {0}\ny sus accesos directos.\n'
    'Keep my save file and settings (moved to Documents\{0} saves)' = 'Conservar mi partida guardada y ajustes (se mueven a Documents\\{0} saves)'
    'Uninstall' = 'Desinstalar'
    'Cancel' = 'Cancelar'
    'Your save file will be deleted too. This cannot be undone. Continue?' = 'Tu partida guardada tambi\u00e9n se borrar\u00e1. No se puede deshacer. \u00bfContinuar?'
    'Close the game first, then press OK.' = 'Cierra primero el juego y luego pulsa Aceptar.'
    'Some files couldn''t be removed: {0}' = 'No se pudieron quitar algunos archivos: {0}'
    'Dragon Quest IX is uninstalled.' = 'Dragon Quest IX se ha desinstalado.'
    '\n\nYour saves are in:\n{0}\n(To use them again: put them in the new game folder.)' = '\n\nTus partidas guardadas est\u00e1n en:\n{0}\n(Para usarlas de nuevo: ponlas en la carpeta del juego nuevo.)'
    '\n\nA few files are still in use; delete the folder {0} yourself later.' = '\n\nUnos pocos archivos siguen en uso; borra t\u00fa la carpeta {0} m\u00e1s tarde.'
    '\n\nThe build tools (MSYS2) are still installed; remove them in Settings > Apps if you don''t need them.' = '\n\nLas herramientas de compilaci\u00f3n (MSYS2) siguen instaladas; qu\u00edtalas en Configuraci\u00f3n > Aplicaciones si no las necesitas.'
}
function T([string]$Text) {
    if ($Lang -ne 'es') { return $Text }
    $k = $Text.Replace("`r`n", '\n').Replace("`n", '\n')
    if ($Es.ContainsKey($k)) { return [regex]::Unescape($Es[$k]) }
    return $Text
}

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
    [void][System.Windows.Forms.MessageBox]::Show(((T 'Dragon Quest IX isn''t in {0} any more. Its entry in Apps is removed.') -f $Folder), $AppName)
    if (Test-Path $UninstallKey) { Remove-Item -Path $UninstallKey -Recurse -Force }
    exit 0
}

$form = New-Object System.Windows.Forms.Form
$form.Text = ((T 'Uninstall {0}') -f $AppName)
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false; $form.MinimizeBox = $false
$form.StartPosition = 'CenterScreen'; $form.AutoSize = $true; $form.AutoSizeMode = 'GrowAndShrink'
$form.Padding = New-Object System.Windows.Forms.Padding(16)
$flow = New-Object System.Windows.Forms.FlowLayoutPanel
$flow.FlowDirection = 'TopDown'; $flow.AutoSize = $true; $flow.WrapContents = $false
$form.Controls.Add($flow)
$msg = New-Object System.Windows.Forms.Label
$msg.AutoSize = $true; $msg.MaximumSize = New-Object System.Drawing.Size(460, 0)
$msg.Text = ((T "This removes Dragon Quest IX from this PC:`n  {0}`nand its shortcuts.`n") -f $Folder)
$keep = New-Object System.Windows.Forms.CheckBox
$keep.AutoSize = $true; $keep.Checked = $true
$keep.Text = ((T 'Keep my save file and settings (moved to Documents\{0} saves)') -f $AppName)
$buttons = New-Object System.Windows.Forms.FlowLayoutPanel
$buttons.AutoSize = $true; $buttons.Anchor = 'Right'; $buttons.Margin = New-Object System.Windows.Forms.Padding(0, 14, 0, 0)
$ok = New-Object System.Windows.Forms.Button; $ok.Text = (T 'Uninstall'); $ok.AutoSize = $true; $ok.DialogResult = 'OK'
$no = New-Object System.Windows.Forms.Button; $no.Text = (T 'Cancel'); $no.AutoSize = $true; $no.DialogResult = 'Cancel'
$buttons.Controls.AddRange(@($ok, $no))
$flow.Controls.AddRange(@($msg, $keep, $buttons))
$form.AcceptButton = $no; $form.CancelButton = $no

if ($form.ShowDialog() -ne 'OK') { exit 0 }
if (-not $keep.Checked) {
    $a = [System.Windows.Forms.MessageBox]::Show((T 'Your save file will be deleted too. This cannot be undone. Continue?'), $AppName, 'YesNo', 'Warning')
    if ($a -ne 'Yes') { exit 0 }
}
Get-Process dqix -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Folder, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object {
    [void][System.Windows.Forms.MessageBox]::Show((T 'Close the game first, then press OK.'), $AppName)
}
try { $kept = Remove-Install $Folder $keep.Checked } catch {
    [void][System.Windows.Forms.MessageBox]::Show(((T 'Some files couldn''t be removed: {0}') -f $_.Exception.Message), $AppName, 'OK', 'Warning'); exit 1
}
$done = (T 'Dragon Quest IX is uninstalled.')
if ($kept) { $done += ((T "`n`nYour saves are in:`n{0}`n(To use them again: put them in the new game folder.)") -f $kept) }
if (Test-Path -LiteralPath $Folder) { $done += ((T "`n`nA few files are still in use; delete the folder {0} yourself later.") -f $Folder) }
$done += (T "`n`nThe build tools (MSYS2) are still installed; remove them in Settings > Apps if you don't need them.")
[void][System.Windows.Forms.MessageBox]::Show($done, $AppName)
