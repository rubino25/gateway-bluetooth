<#
  Prepara un PC Windows per il ricevitore del gateway. Va eseguito una volta sola,
  con doppio clic su installa.bat; dopo si usa avvia.bat.

  Cosa installa o imposta (ogni passo viene saltato se è già a posto):
    1. Python 3.11 o successivo (per l'utente corrente, senza amministratore);
    2. usbip-win2 (driver, chiede l'amministratore);
    3. regola del firewall: Python riceve i pacchetti UDP del dongle dalla rete locale
       (chiede l'amministratore);
    4. VIIPER, con il suo installer ufficiale (per l'utente corrente);
    5. la cartella di usbip-win2 nel PATH dell'utente;
    6. su richiesta, l'avvio automatico del ricevitore all'accesso a Windows
       (con avvio-automatico.ps1, lo stesso dei file avvio-automatico-*.bat).

  Le versioni sono quelle provate (vedi $Pins); i file scaricati da GitHub vengono
  controllati con lo SHA-256, quello di Python con la firma digitale.

  installa.bat -CheckOnly            controlla soltanto, senza scaricare né cambiare nulla.
  installa.bat -AvvioAutomatico      attiva l'avvio automatico senza chiedere.
  installa.bat -NoAvvioAutomatico    non chiede e lascia l'avvio automatico com'è.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$AvvioAutomatico,
    [switch]$NoAvvioAutomatico,
    # Parametri interni: li usa la parte che gira come amministratore.
    [switch]$AdminStage,
    [string]$UsbipInstaller = '',
    [string]$PythonExe = '',
    [int]$Port = 0,
    [switch]$RemoveBlockRules,
    [string]$LogFile = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest è molto più veloce senza barra

$Pins = @{
    PythonVersion = '3.12.10'
    PythonMin     = [version]'3.11'
    ViiperVersion = '0.9.0'
    ViiperSha256  = @{
        AMD64 = '142c144f6abee8b8a5f6a1abbc7018683444de71d2e45102cde6aaab9b2223e6'
        ARM64 = '683bb22a7f2bebb48e1125482c7d9a8bf7c5c1642abe04c75cae09fe6eda02a6'
    }
    UsbipVersion  = '0.9.8.1'
    UsbipSha256   = @{
        AMD64 = '38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a'
        ARM64 = 'cab7ff97f79275eeb5c5b8bb2eb3111ff6b6c4eead07d9bec9be5bd1e3a35800'
    }
}
$FirewallGroup = 'xbox-eth-gateway'
$UsbipUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{199505b0-b93d-4521-a8c7-897818e0205a}_is1'
$ViiperUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\VIIPER'
$RebootExitCode = 3010
$ViiperDir = Join-Path $env:LOCALAPPDATA 'VIIPER'   # dove lo mette "viiper install"

# ---------------------------------------------------------------- messaggi

function Say([string]$Level, [string]$Text) {
    $color = @{ STEP = 'Cyan'; OK = 'Green'; INFO = 'Gray'; WARN = 'Yellow'; FAIL = 'Red' }[$Level]
    $prefix = @{ STEP = '==> '; OK = '  ok  '; INFO = '      '; WARN = '  !!  '; FAIL = '  XX  ' }[$Level]
    Write-Host ($prefix + $Text) -ForegroundColor $color
    if ($LogFile) { Add-Content -LiteralPath $LogFile -Encoding UTF8 -Value "$Level|$Text" }
}

# ---------------------------------------------------------------- utilità

function Get-Arch {
    $arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($arch -notin 'AMD64', 'ARM64') { throw "Windows a 64 bit richiesto (trovato: $arch)." }
    $arch
}

function Test-VersionAtLeast([string]$Have, [string]$Want) {
    try { return ([version]$Have) -ge ([version]$Want) } catch { return $false }
}

function Get-RegistryValues([Microsoft.Win32.RegistryHive]$Hive, [string]$Path) {
    # Vista a 64 bit anche se PowerShell gira a 32 bit.
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, [Microsoft.Win32.RegistryView]::Registry64)
    $key = $base.OpenSubKey($Path)
    if (-not $key) { return $null }
    try {
        $values = @{}
        foreach ($name in $key.GetValueNames()) { $values[$name] = $key.GetValue($name) }
        return $values
    } finally { $key.Close() }
}

function Get-File([string]$Url, [string]$Dest, [string]$Sha256 = '') {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Say INFO "scarico $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Dest
    if ($Sha256) { Assert-Sha256 $Dest $Sha256 }
}

function Assert-Sha256([string]$Path, [string]$Sha256) {
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($hash -ne $Sha256) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        throw "Il file scaricato non corrisponde a quello atteso (SHA-256 $hash): $Path"
    }
}

function Update-ProcessPath {
    # Rilegge il PATH dal registro, così si vedono i programmi appena installati.
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = (@($machine, $user) | Where-Object { $_ }) -join ';'
}

function Test-PathListContains([string]$List, [string]$Dir) {
    $want = $Dir.Trim().TrimEnd('\')
    foreach ($entry in ($List -split ';')) {
        if (-not $entry.Trim()) { continue }
        $have = [Environment]::ExpandEnvironmentVariables($entry).Trim().TrimEnd('\')
        if ($have -ieq $want) { return $true }
    }
    $false
}

function Add-DirToPathValue([Microsoft.Win32.RegistryKey]$Key, [string]$Dir, [switch]$CheckOnly) {
    # Aggiunge $Dir in fondo al valore Path della chiave, senza espandere le voci
    # %VARIABILE% esistenti e mantenendo il tipo del valore. Restituisce
    # 'present', 'missing' (solo controllo) o 'added'.
    $raw = "$($Key.GetValue('Path', '', 'DoNotExpandEnvironmentNames'))"
    if (Test-PathListContains $raw $Dir) { return 'present' }
    if ($CheckOnly) { return 'missing' }
    $kind = if ($Key.GetValueNames() -contains 'Path') { $Key.GetValueKind('Path') } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }
    $new = if ($raw.Trim()) { $raw.TrimEnd(';') + ';' + $Dir } else { $Dir }
    $Key.SetValue('Path', $new, $kind)
    'added'
}

# ---------------------------------------------------------------- Python

function Find-Python {
    # Prima py -3 (quello che usa avvia.bat), poi python nel PATH (non l'alias dello
    # Store in WindowsApps), poi le cartelle standard dell'installer.
    $ErrorActionPreference = 'Continue'
    $candidates = New-Object System.Collections.Generic.List[object]
    if (Get-Command py.exe -ErrorAction SilentlyContinue) { $candidates.Add(@('py.exe', '-3')) }
    $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -notmatch '\\WindowsApps\\') { $candidates.Add(@($cmd.Source)) }
    foreach ($dir in @("$env:LOCALAPPDATA\Programs\Python", $env:ProgramFiles)) {
        if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { continue }
        Get-ChildItem -LiteralPath $dir -Directory -Filter 'Python3*' | Sort-Object Name -Descending | ForEach-Object {
            $exe = Join-Path $_.FullName 'python.exe'
            if (Test-Path -LiteralPath $exe) { $candidates.Add(@($exe)) }
        }
    }
    foreach ($c in $candidates) {
        $exe = $c[0]
        $rest = @($c | Select-Object -Skip 1)
        $out = @(& $exe @rest -c "import sys; print(sys.executable); print('%d.%d.%d' % sys.version_info[:3])" 2>$null)
        if ($LASTEXITCODE -ne 0 -or $out.Count -lt 2) { continue }
        if (Test-VersionAtLeast $out[1] $Pins.PythonMin) {
            return [pscustomobject]@{ Exe = $out[0].Trim(); Version = $out[1].Trim() }
        }
    }
    $null
}

function Install-Python([string]$Arch, [string]$TempDir) {
    $suffix = if ($Arch -eq 'ARM64') { 'arm64' } else { 'amd64' }
    $v = $Pins.PythonVersion
    $installer = Join-Path $TempDir "python-$v-$suffix.exe"
    Get-File "https://www.python.org/ftp/python/$v/python-$v-$suffix.exe" $installer
    $sig = Get-AuthenticodeSignature -LiteralPath $installer
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Python Software Foundation') {
        throw "Firma dell'installer di Python non valida ($($sig.Status))."
    }
    Say INFO "installo Python $v per l'utente corrente (un paio di minuti)"
    $p = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
        '/quiet', 'InstallAllUsers=0', 'PrependPath=1', 'Include_launcher=1',
        'InstallLauncherAllUsers=0', 'Include_test=0')
    if ($p.ExitCode -ne 0) { throw "Installer di Python terminato con codice $($p.ExitCode)." }
    Update-ProcessPath
}

function Get-ReceiverPort([string]$Python) {
    # Porta UDP letta da config.toml con lo stesso codice del ricevitore.
    $ErrorActionPreference = 'Continue'
    Push-Location -LiteralPath $PSScriptRoot
    try {
        $out = @(& $Python -c "import config; print(config.load_config().port)")
        if ($LASTEXITCODE -ne 0) { throw "config.toml non valido: correggerlo e rilanciare." }
        [int]$out[-1]
    } finally { Pop-Location }
}

# ---------------------------------------------------------------- usbip-win2 e VIIPER

function Get-UsbipInfo {
    $v = Get-RegistryValues LocalMachine $UsbipUninstallKey
    if (-not $v) { return $null }
    $dir = "$($v['InstallLocation'])".TrimEnd('\')
    if (-not $dir) { $dir = Join-Path $env:ProgramW6432 'USBip' }
    $exe = Join-Path $dir 'usbip.exe'
    [pscustomobject]@{ Version = "$($v['DisplayVersion'])"; Dir = $dir; Exe = $exe; ExeFound = (Test-Path -LiteralPath $exe) }
}

function Get-ViiperInfo {
    $v = Get-RegistryValues CurrentUser $ViiperUninstallKey
    if (-not $v) { return $null }
    $exe = Join-Path "$($v['InstallLocation'])" 'viiper.exe'
    if (-not (Test-Path -LiteralPath $exe)) { return $null }
    [pscustomobject]@{ Version = "$($v['DisplayVersion'])"; Exe = $exe }
}

function Install-Viiper([string]$Arch, [string]$TempDir, $Usbip) {
    $suffix = $Arch.ToLower()
    $exe = Join-Path $TempDir "viiper-windows-$suffix.exe"
    Get-File "https://github.com/Alia5/VIIPER/releases/download/v$($Pins.ViiperVersion)/viiper-windows-$suffix.exe" $exe $Pins.ViiperSha256[$Arch]
    Say INFO "installo VIIPER $($Pins.ViiperVersion) per l'utente corrente"
    # L'installer ufficiale copia viiper.exe in %LOCALAPPDATA%\VIIPER, imposta l'avvio
    # automatico e avvia il server. usbip-win2 è già installato, quindi lo salta.
    # --no-update: resta la versione provata invece dell'ultima uscita.
    if ($Usbip) { $env:Path = "$($Usbip.Dir);$env:Path" }
    # VIIPER legge config.toml/server.toml/... dalla cartella corrente: lanciato da qui
    # prenderebbe il config.toml del ricevitore e si fermerebbe ("unknown configuration
    # keys"). Parte quindi dalla propria cartella di installazione, come in launcher.py.
    New-Item -ItemType Directory -Force -Path $ViiperDir | Out-Null
    $p = Start-Process -FilePath $exe -ArgumentList 'install', '--quiet', '--no-update' `
        -WorkingDirectory $ViiperDir -NoNewWindow -Wait -PassThru
    $code = $p.ExitCode
    if ($code -ne 0 -and $code -ne $RebootExitCode) { throw "Installer di VIIPER terminato con codice $code." }
    $code -eq $RebootExitCode
}

# ---------------------------------------------------------------- firewall

function Test-FirewallRule([string]$Python, [int]$PortNumber) {
    foreach ($r in @(Get-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue)) {
        if ("$($r.Enabled)" -ne 'True' -or "$($r.Direction)" -ne 'Inbound' -or "$($r.Action)" -ne 'Allow') { continue }
        $app = $r | Get-NetFirewallApplicationFilter
        $ports = $r | Get-NetFirewallPortFilter
        if ($app.Program -ieq $Python -and "$($ports.Protocol)" -eq 'UDP' -and "$($ports.LocalPort)" -eq "$PortNumber") { return $true }
    }
    $false
}

function Get-BlockRules([string]$Python) {
    # Regole create da Windows quando all'avviso del firewall si risponde "Annulla":
    # vincono su qualsiasi regola che consente.
    @(Get-NetFirewallApplicationFilter -Program $Python -ErrorAction SilentlyContinue |
        Get-NetFirewallRule -ErrorAction SilentlyContinue |
        Where-Object { "$($_.Enabled)" -eq 'True' -and "$($_.Direction)" -eq 'Inbound' -and "$($_.Action)" -eq 'Block' })
}

# ---------------------------------------------------------------- parte da amministratore

function Invoke-AdminStage {
    $code = 0
    try {
        if ($UsbipInstaller) {
            Assert-Sha256 $UsbipInstaller $Pins.UsbipSha256[(Get-Arch)]
            Say INFO "installo usbip-win2 $($Pins.UsbipVersion) (driver)"
            $p = Start-Process -FilePath $UsbipInstaller -Wait -PassThru -ArgumentList @(
                '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/RESTARTEXITCODE=$RebootExitCode")
            if ($p.ExitCode -eq $RebootExitCode) {
                Say OK "usbip-win2 installato (serve un riavvio)"
                $code = $RebootExitCode
            } elseif ($p.ExitCode -ne 0) {
                throw "Installer di usbip-win2 terminato con codice $($p.ExitCode)."
            } else {
                Say OK "usbip-win2 installato"
            }
        }
        if ($RemoveBlockRules -and $PythonExe) {
            $blocks = @(Get-BlockRules $PythonExe)
            $blocks | Remove-NetFirewallRule
            Say OK "rimosse $($blocks.Count) regole del firewall che bloccavano Python"
        }
        if ($PythonExe -and $Port) {
            Get-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue | Remove-NetFirewallRule
            # Solo Python, solo UDP sulla porta del ricevitore, solo dalla rete locale; vale
            # anche se Windows considera la rete "pubblica".
            New-NetFirewallRule -DisplayName "xbox-eth-gateway: ricevitore UDP $Port" -Group $FirewallGroup `
                -Direction Inbound -Action Allow -Protocol UDP -LocalPort $Port -RemoteAddress LocalSubnet `
                -Program $PythonExe -Profile Any | Out-Null
            Say OK "firewall: Python può ricevere UDP $Port dalla rete locale"
        }
    } catch {
        Say FAIL "$($_.Exception.Message)"
        $code = 1
    }
    if ($LogFile) { Add-Content -LiteralPath $LogFile -Encoding UTF8 -Value "EXIT|$code" }
    exit $code
}

function Start-AdminStage([hashtable]$Arguments, [string]$TempDir) {
    # Una sola richiesta di amministratore (UAC) per driver e firewall. Gira in una
    # finestra nascosta; i messaggi tornano qui attraverso un file.
    $log = Join-Path $TempDir 'admin.log'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-AdminStage', '-LogFile', "`"$log`"")
    foreach ($k in $Arguments.Keys) {
        $v = $Arguments[$k]
        if ($v -is [bool]) { if ($v) { $argList += "-$k" } }
        else { $argList += "-$k", "`"$v`"" }
    }
    Say INFO "Windows chiederà il permesso di amministratore"
    try {
        $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -Wait -PassThru -ArgumentList $argList
    } catch {
        Say FAIL "permesso di amministratore negato: driver e firewall non sono stati impostati"
        return 1
    }
    $code = 1
    if (Test-Path -LiteralPath $log) {
        foreach ($line in Get-Content -LiteralPath $log -Encoding UTF8) {
            $level, $text = $line -split '\|', 2
            if ($level -eq 'EXIT') { $code = [int]$text; continue }
            if ($level -notin 'OK', 'INFO', 'WARN', 'FAIL') { continue }
            Write-Host (@{ OK = '  ok  '; INFO = '      '; WARN = '  !!  '; FAIL = '  XX  ' }[$level] + $text) `
                -ForegroundColor (@{ OK = 'Green'; INFO = 'Gray'; WARN = 'Yellow'; FAIL = 'Red' }[$level])
        }
    } elseif ($null -ne $p.ExitCode) {
        $code = $p.ExitCode
    }
    $code
}

# ---------------------------------------------------------------- avvio automatico

function Invoke-AutostartScript([hashtable]$Arguments, [switch]$Quiet) {
    # avvio-automatico.ps1, lo stesso dei file avvio-automatico-*.bat; restituisce il
    # suo codice di uscita.
    $script = Join-Path $PSScriptRoot 'avvio-automatico.ps1'
    if ($Quiet) { & $script @Arguments *> $null } else { & $script @Arguments }
    $LASTEXITCODE
}

function Invoke-AutostartStep($Python, [int]$Problems) {
    # Restituisce 'on' (attivo), 'off' (non attivo), 'skipped' o 'failed'.
    if ($AvvioAutomatico -and $NoAvvioAutomatico) { throw "-AvvioAutomatico e -NoAvvioAutomatico insieme: sceglierne uno." }
    if ($CheckOnly) {
        if ((Invoke-AutostartScript @{ Stato = $true }) -eq 0) { return 'on' }
        return 'off'
    }
    if (-not $Python -or $Problems -gt 0) {
        Say INFO "da attivare quando il resto è a posto (dopo: avvio-automatico-attiva.bat)"
        return 'skipped'
    }
    $active = (Invoke-AutostartScript @{ Stato = $true } -Quiet) -eq 0
    if ($NoAvvioAutomatico) {
        $state = if ($active) { 'attivo' } else { 'non attivo' }
        Say INFO "lasciato com'è ($state), come chiesto con -NoAvvioAutomatico"
        if ($active) { return 'on' }
        return 'off'
    }
    if (-not $active -and -not $AvvioAutomatico) {
        $answer = Read-Host "      Avviare il ricevitore automaticamente all'accesso a Windows? [S/n]"
        if (-not ($answer -eq '' -or $answer -match '^[sSyY]')) {
            Say INFO "non attivato: si può attivare dopo con avvio-automatico-attiva.bat"
            return 'off'
        }
    }
    # Anche se è già attivo: così l'attività usa il Python appena trovato.
    if ((Invoke-AutostartScript @{ Attiva = $true; PythonExe = $Python.Exe }) -eq 0) { return 'on' }
    'failed'
}

# ---------------------------------------------------------------- programma principale

function Invoke-Main {
    $todo = New-Object System.Collections.Generic.List[string]
    $reboot = $false
    $arch = Get-Arch
    $tempDir = Join-Path $env:TEMP ('xbox-eth-gateway-setup-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $tempDir | Out-Null
    try {
        if ($CheckOnly) { Say INFO "solo controllo: non scarico e non cambio nulla" }

        # 1. Python
        Say STEP "Python"
        $python = Find-Python
        if ($python) {
            Say OK "Python $($python.Version) ($($python.Exe))"
        } elseif ($CheckOnly) {
            Say WARN "manca Python $($Pins.PythonMin) o successivo"
            $todo.Add('Python')
        } else {
            Install-Python $arch $tempDir
            $python = Find-Python
            if (-not $python) { throw "Python installato ma non trovato: chiudere la finestra e rilanciare installa.bat." }
            Say OK "Python $($python.Version) ($($python.Exe))"
        }
        $port = 0
        if ($python) {
            $port = Get-ReceiverPort $python.Exe
            Say OK "config.toml letto: porta UDP $port"
        }

        # 2. usbip-win2 e 3. firewall: preparo quello che serve all'amministratore
        Say STEP "usbip-win2"
        $usbip = Get-UsbipInfo
        $admin = @{}
        if ($usbip -and $usbip.ExeFound -and (Test-VersionAtLeast $usbip.Version $Pins.UsbipVersion)) {
            Say OK "usbip-win2 $($usbip.Version) ($($usbip.Dir))"
        } else {
            $what = if ($usbip) { "usbip-win2 $($usbip.Version) da aggiornare a $($Pins.UsbipVersion)" } else { "manca usbip-win2" }
            Say WARN $what
            if ($CheckOnly) { $todo.Add('usbip-win2') }
            else {
                $suffix = if ($arch -eq 'ARM64') { 'arm64' } else { 'x64' }
                $v = $Pins.UsbipVersion
                $installer = Join-Path $tempDir "USBip-$v-$suffix.exe"
                Get-File "https://github.com/vadimgrn/usbip-win2/releases/download/v.$v/USBip-$v-$suffix.exe" $installer $Pins.UsbipSha256[$arch]
                $admin['UsbipInstaller'] = $installer
            }
        }

        Say STEP "Firewall"
        if ($python) {
            $blocks = @(Get-BlockRules $python.Exe)
            if ($blocks.Count -gt 0) {
                Say WARN "$($blocks.Count) regole del firewall bloccano Python in ingresso (risposta 'Annulla' all'avviso di Windows)"
                if ($CheckOnly) { $todo.Add('regole del firewall che bloccano Python') }
                else {
                    $answer = Read-Host "      Rimuoverle? Senza, i pacchetti del dongle non arrivano. [S/n]"
                    if ($answer -eq '' -or $answer -match '^[sSyY]') { $admin['RemoveBlockRules'] = $true }
                    else { $todo.Add('regole del firewall che bloccano Python') }
                }
            }
            if (Test-FirewallRule $python.Exe $port) {
                Say OK "regola presente: Python riceve UDP $port dalla rete locale"
            } else {
                Say WARN "manca la regola per ricevere UDP $port"
                if ($CheckOnly) { $todo.Add('regola del firewall') }
                else { $admin['PythonExe'] = $python.Exe; $admin['Port'] = $port }
            }
            if ($admin.ContainsKey('RemoveBlockRules') -and -not $admin.ContainsKey('PythonExe')) { $admin['PythonExe'] = $python.Exe }
        } else {
            Say INFO "da controllare dopo l'installazione di Python"
        }

        if ($admin.Count -gt 0) {
            $code = Start-AdminStage $admin $tempDir
            if ($code -eq $RebootExitCode) { $reboot = $true }
            elseif ($code -ne 0) { $todo.Add('passi da amministratore (usbip-win2 / firewall)') }
            $usbip = Get-UsbipInfo
        }

        # 4. VIIPER
        Say STEP "VIIPER"
        $viiper = Get-ViiperInfo
        if ($viiper -and (Test-VersionAtLeast $viiper.Version $Pins.ViiperVersion)) {
            $note = if ($viiper.Version -ne $Pins.ViiperVersion) { " (provato con $($Pins.ViiperVersion))" } else { '' }
            Say OK "VIIPER $($viiper.Version) ($($viiper.Exe))$note"
        } else {
            $what = if ($viiper) { "VIIPER $($viiper.Version) da aggiornare a $($Pins.ViiperVersion)" } else { "manca VIIPER" }
            Say WARN $what
            if ($CheckOnly) { $todo.Add('VIIPER') }
            elseif (-not $usbip) { Say FAIL "VIIPER non installato perché manca usbip-win2"; $todo.Add('VIIPER') }
            else {
                if (Install-Viiper $arch $tempDir $usbip) { $reboot = $true }
                $viiper = Get-ViiperInfo
                if ($viiper) { Say OK "VIIPER $($viiper.Version) ($($viiper.Exe))" }
                else { Say FAIL "VIIPER non risulta installato"; $todo.Add('VIIPER') }
            }
        }

        # 5. PATH dell'utente: così trovano usbip.exe anche il VIIPER dell'avvio automatico
        # e chi scrive "usbip" a mano. avvia.bat funziona anche senza.
        Say STEP "PATH"
        if ($usbip) {
            $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', -not $CheckOnly)
            try {
                $result = Add-DirToPathValue $key $usbip.Dir -CheckOnly:$CheckOnly
            } finally { $key.Close() }
            if ($result -eq 'present') {
                Say OK "$($usbip.Dir) è già nel PATH dell'utente"
            } elseif ($result -eq 'missing') {
                Say INFO "$($usbip.Dir) non è nel PATH dell'utente (avvia.bat funziona lo stesso)"
            } else {
                Send-EnvironmentChange
                Say OK "aggiunto $($usbip.Dir) al PATH dell'utente (vale per le finestre aperte da ora)"
            }
        } else {
            Say INFO "da fare dopo l'installazione di usbip-win2"
        }

        # 6. Avvio automatico
        Say STEP "Avvio automatico"
        $autostart = Invoke-AutostartStep $python $todo.Count
        if ($autostart -eq 'failed') { $todo.Add('avvio automatico') }

        # Riepilogo
        Write-Host ''
        if ($todo.Count -gt 0) {
            $verb = if ($CheckOnly) { 'Da fare' } else { 'Non riuscito' }
            Say FAIL "$($verb): $($todo -join ', ')."
            if (-not $CheckOnly) { Say INFO "Correggere e rilanciare installa.bat: i passi già fatti vengono saltati." }
            return 1
        }
        $next = if ($autostart -eq 'on') { 'il ricevitore parte da solo all''accesso a Windows' } else { 'avviare avvia.bat' }
        if ($reboot) {
            Say WARN "Riavviare il PC per finire l'installazione del driver; dopo, $next."
            return $RebootExitCode
        }
        $done = if ($CheckOnly) { 'Tutto a posto' } else { 'PC pronto' }
        if ($autostart -eq 'on' -and -not $CheckOnly) {
            Say OK "$($done): il ricevitore parte da solo al prossimo accesso a Windows; per usarlo subito, avvia.bat."
            Say INFO "avvia.bat lo avvia in background, senza finestra; ferma.bat lo ferma."
        } else {
            Say OK "$($done): per usare il gateway, doppio clic su avvia.bat (senza finestra; ferma.bat lo ferma)."
        }
        return 0
    } catch {
        Say FAIL "$($_.Exception.Message)"
        return 1
    } finally {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Send-EnvironmentChange {
    # Avvisa Esplora risorse che il PATH è cambiato (come fa il pannello Variabili d'ambiente).
    if (-not ('XEG.Native' -as [type])) {
        Add-Type -Namespace XEG -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out UIntPtr result);
'@
    }
    $result = [UIntPtr]::Zero
    [void][XEG.Native]::SendMessageTimeout([IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result)
}

if ($AdminStage) { Invoke-AdminStage }
if ($env:XEG_SETUP_NO_MAIN) { return }   # usato dai test per caricare solo le funzioni
exit (Invoke-Main)
