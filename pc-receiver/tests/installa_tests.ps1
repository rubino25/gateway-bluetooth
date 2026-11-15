# Prove delle funzioni di installa.ps1 che non dipendono da cosa è installato sul PC.
# Le lancia test_installa.py; da sole: powershell -File tests\installa_tests.ps1
$ErrorActionPreference = 'Stop'
$env:XEG_SETUP_NO_MAIN = '1'
. (Join-Path $PSScriptRoot '..\installa.ps1')
$env:XEG_SETUP_NO_MAIN = ''
$fails = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "PASS $name" } else { Write-Host "FAIL $name"; $script:fails++ }
}

# Test-PathListContains
$list = "C:\a;%USERPROFILE%\x;C:\Program Files\USBip\;"
Check 'path: barra finale' (Test-PathListContains $list 'C:\Program Files\USBip')
Check 'path: maiuscole' (Test-PathListContains $list 'c:\program files\usbip\')
Check 'path: variabile espansa' (Test-PathListContains $list "$env:USERPROFILE\x")
Check 'path: assente' (-not (Test-PathListContains $list 'C:\b'))
Check 'path: lista vuota' (-not (Test-PathListContains '' 'C:\b'))

# Test-VersionAtLeast
Check 'ver: uguale' (Test-VersionAtLeast '0.9.8.1' '0.9.8.1')
Check 'ver: più vecchia' (-not (Test-VersionAtLeast '0.9.7.8' '0.9.8.1'))
Check 'ver: più nuova' (Test-VersionAtLeast '1.0' '0.9.0')
Check 'ver: non valida' (-not (Test-VersionAtLeast 'abc' '0.9.0'))
Check 'ver: vuota' (-not (Test-VersionAtLeast '' '0.9.0'))

# Add-DirToPathValue su una chiave temporanea (non tocca il PATH vero)
$sub = 'Software\XegSetupTest-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($sub)
try {
    Check 'reg: aggiunta a valore assente' ((Add-DirToPathValue $k 'C:\U') -eq 'added')
    Check 'reg: tipo ExpandString di default' ($k.GetValueKind('Path') -eq 'ExpandString' -and $k.GetValue('Path') -eq 'C:\U')
    $k.SetValue('Path', '%USERPROFILE%\bin;C:\x;', [Microsoft.Win32.RegistryValueKind]::ExpandString)
    Check 'reg: solo controllo' ((Add-DirToPathValue $k 'C:\Program Files\USBip' -CheckOnly) -eq 'missing')
    Check 'reg: solo controllo non cambia' ($k.GetValue('Path', '', 'DoNotExpandEnvironmentNames') -eq '%USERPROFILE%\bin;C:\x;')
    Check 'reg: aggiunta' ((Add-DirToPathValue $k 'C:\Program Files\USBip') -eq 'added')
    Check 'reg: voci %VAR% non espanse' ($k.GetValue('Path', '', 'DoNotExpandEnvironmentNames') -eq '%USERPROFILE%\bin;C:\x;C:\Program Files\USBip')
    Check 'reg: tipo mantenuto' ($k.GetValueKind('Path') -eq 'ExpandString')
    Check 'reg: già presente' ((Add-DirToPathValue $k 'C:\Program Files\USBip\') -eq 'present')
    $k.SetValue('Path', 'C:\x', [Microsoft.Win32.RegistryValueKind]::String)
    [void](Add-DirToPathValue $k 'C:\y')
    Check 'reg: tipo String mantenuto' ($k.GetValueKind('Path') -eq 'String' -and $k.GetValue('Path') -eq 'C:\x;C:\y')
} finally {
    $k.Close()
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($sub)
}

# Parte da amministratore lanciata senza elevazione: un installer con SHA-256 sbagliato
# viene rifiutato e cancellato, e gli argomenti con spazi arrivano interi.
$dir = Join-Path $env:TEMP ('xeg prova ' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $dir | Out-Null
try {
    $fake = Join-Path $dir 'USBip finto.exe'
    Set-Content -LiteralPath $fake -Value 'non un installer'
    function Start-Process {
        param($FilePath, $Verb, $WindowStyle, [switch]$Wait, [switch]$PassThru, $ArgumentList)
        $script:capturedArgs = $ArgumentList
        Microsoft.PowerShell.Management\Start-Process -FilePath $FilePath -ArgumentList $ArgumentList `
            -Wait -PassThru -WindowStyle Hidden
    }
    $code = Start-AdminStage @{ UsbipInstaller = $fake; RemoveBlockRules = $false } $dir
    Remove-Item Function:\Start-Process
    Check "admin: codice di uscita $code" ($code -eq 1)
    Check 'admin: installer rifiutato e cancellato' (-not (Test-Path -LiteralPath $fake))
    Check 'admin: argomenti' ((($capturedArgs -join ' ') -match '-UsbipInstaller "[^"]+USBip finto\.exe"') -and ($capturedArgs -notcontains '-RemoveBlockRules'))
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}

# Install-Viiper: l'installer di VIIPER non deve partire dalla cartella del ricevitore,
# altrimenti legge il nostro config.toml e si ferma.
$dir = Join-Path $env:TEMP ('xeg viiper ' + [guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    $ViiperDir = Join-Path $dir 'VIIPER'
    $tmp = Join-Path $dir 'tmp'
    New-Item -ItemType Directory $tmp | Out-Null
    function Get-File { param($Url, $Dest, $Sha256) $script:downloaded = $Url }
    function Start-Process {
        param($FilePath, $ArgumentList, $WorkingDirectory, [switch]$NoNewWindow, [switch]$Wait, [switch]$PassThru)
        $script:viiperCall = @{ Exe = $FilePath; Args = $ArgumentList; Cwd = $WorkingDirectory }
        [pscustomobject]@{ ExitCode = 0 }
    }
    Push-Location -LiteralPath (Join-Path $PSScriptRoot '..')   # come installa.bat
    try { $reboot = Install-Viiper 'AMD64' $tmp $null } finally { Pop-Location }
    Remove-Item Function:\Start-Process, Function:\Get-File
    Check "viiper: url $downloaded" ($downloaded -eq 'https://github.com/Alia5/VIIPER/releases/download/v0.9.0/viiper-windows-amd64.exe')
    Check "viiper: cartella di lavoro $($viiperCall.Cwd)" ($viiperCall.Cwd -eq $ViiperDir -and (Test-Path -LiteralPath $ViiperDir))
    Check 'viiper: niente config.toml nella cartella di lavoro' (-not (Test-Path -LiteralPath (Join-Path $viiperCall.Cwd 'config.toml')))
    Check "viiper: argomenti $($viiperCall.Args -join ' ')" (($viiperCall.Args -join ' ') -eq 'install --quiet --no-update')
    Check 'viiper: nessun riavvio' ($reboot -eq $false)
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "failures: $fails"
exit $fails
