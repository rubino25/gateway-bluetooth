<#
  Ricevitore in background: VIIPER e ricevitore senza finestra (pythonw.exe
  launcher.py --auto), con i messaggi in avvio-automatico.log. Subito o all'accesso a
  Windows.

    avvio-automatico.ps1 -Avvia       lo avvia adesso e aspetta che sia pronto (avvia.bat)
    avvio-automatico.ps1 -Ferma       lo ferma (ferma.bat); l'avvio automatico resta
    avvio-automatico.ps1 -Attiva      attiva l'avvio automatico per l'utente corrente
    avvio-automatico.ps1 -Disattiva   ferma il ricevitore in background e toglie l'avvio
    avvio-automatico.ps1 -Stato       dice se è attivo e se sta girando (azione di default)

  Facoltativi:
    -PythonExe <python.exe>   Python da usare (lo passa installa.ps1); senza, py -3 o python
    -Nome <nome>              nome dell'attività pianificata (per le prove)
    -RitardoSecondi <s>       attesa dopo l'accesso, per dare tempo alla rete (default 30)

  Meccanismo: un'attività dell'Utilità di pianificazione "all'accesso" dell'utente
  corrente, che non chiede l'amministratore. Se Windows non la lascia creare, ripiega
  su un collegamento nella cartella Esecuzione automatica. Una sola istanza, riavvio
  dopo gli errori e arresto pulito li gestisce launcher.py --auto.

  Codici di uscita: 0 riuscito (con -Stato: attivo), 3 con -Stato: non attivo,
  1 errore, 2 parametri sbagliati.
#>
[CmdletBinding()]
param(
    [switch]$Avvia,
    [switch]$Attiva,
    [switch]$Disattiva,
    [switch]$Ferma,
    [switch]$Stato,
    [string]$PythonExe = '',
    [string]$Nome = 'xbox-eth-gateway ricevitore',
    [ValidateRange(0, 3600)][int]$RitardoSecondi = 30
)

$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$Launcher = Join-Path $Here 'launcher.py'
$AutoLog = Join-Path $Here 'avvio-automatico.log'
$StopEventName = 'Local\xbox-eth-gateway-stop'   # come STOP_EVENT in launcher.py
$StartupDir = [Environment]::GetFolderPath('Startup')
$PythonMin = [version]'3.11'
$NotActiveExitCode = 3

function Say([string]$Level, [string]$Text) {
    $color = @{ OK = 'Green'; INFO = 'Gray'; WARN = 'Yellow'; FAIL = 'Red' }[$Level]
    $prefix = @{ OK = '  ok  '; INFO = '      '; WARN = '  !!  '; FAIL = '  XX  ' }[$Level]
    Write-Host ($prefix + $Text) -ForegroundColor $color
}

function Get-ShortcutPath([string]$Name) { Join-Path $StartupDir "$Name.lnk" }

function Get-AutostartTask([string]$Name) {
    Get-ScheduledTask -TaskPath '\' -TaskName $Name -ErrorAction SilentlyContinue
}

function Resolve-Pythonw([string]$Python) {
    # pythonw.exe accanto al python.exe scelto: parte senza finestra.
    $ErrorActionPreference = 'Continue'
    $candidates = New-Object System.Collections.Generic.List[object]
    if ($Python) { $candidates.Add(@($Python)) }
    else {
        if (Get-Command py.exe -ErrorAction SilentlyContinue) { $candidates.Add(@('py.exe', '-3')) }
        $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notmatch '\\WindowsApps\\') { $candidates.Add(@($cmd.Source)) }
    }
    foreach ($c in $candidates) {
        $exe = $c[0]
        if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
        $rest = @($c | Select-Object -Skip 1)
        $out = @(& $exe @rest -c "import sys; print(sys.executable); print('%d.%d' % sys.version_info[:2])" 2>$null)
        if ($LASTEXITCODE -ne 0 -or $out.Count -lt 2) { continue }
        try { if ([version]$out[1].Trim() -lt $PythonMin) { continue } } catch { continue }
        $pythonw = Join-Path (Split-Path -Parent $out[0].Trim()) 'pythonw.exe'
        if (Test-Path -LiteralPath $pythonw) { return $pythonw }
    }
    $null
}

function Get-AutoProcess {
    # Il launcher in modalità automatica: pythonw.exe con launcher.py e --auto. Va sempre
    # chiamata dentro @(...): un solo oggetto CIM non ha .Count. Anche
    # quello di un'altra copia della cartella: il mutex di launcher.py ne fa girare uno solo.
    @(Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -match 'launcher\.py' -and $_.CommandLine -match '--auto' })
}

function Enable-Autostart([string]$Name, [string]$Pythonw, [int]$Delay) {
    $arguments = "`"$Launcher`" --auto"
    $description = "Avvia VIIPER e il ricevitore di xbox-eth-gateway all'accesso ($Here). Creata da avvio-automatico.ps1."
    $user = "$env:USERDOMAIN\$env:USERNAME"
    try {
        $action = New-ScheduledTaskAction -Execute $Pythonw -Argument $arguments -WorkingDirectory $Here
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
        if ($Delay -gt 0) { $trigger.Delay = 'PT{0}S' -f $Delay }
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
        # Priorità 4 = normale (quella di default delle attività è "sotto il normale" e la
        # erediterebbe anche VIIPER). Nessun limite di durata, anche a batteria. Il
        # riavvio dopo un errore lo fa già launcher.py; questo vale se si chiude lui.
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -Priority 4 `
            -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
        Register-ScheduledTask -TaskName $Name -TaskPath '\' -Action $action -Trigger $trigger `
            -Principal $principal -Settings $settings -Description $description -Force | Out-Null
        $registered = $true
    } catch {
        $registered = $false
        Say WARN "attività pianificata non creata ($($_.Exception.Message.Trim())): uso la cartella Esecuzione automatica"
    }
    if ($registered) {
        $lnk = Get-ShortcutPath $Name
        if (Test-Path -LiteralPath $lnk) { Remove-Item -LiteralPath $lnk -Force }   # ripiego di una volta precedente
        return 'task'
    }
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut((Get-ShortcutPath $Name))
    $lnk.TargetPath = $Pythonw
    $lnk.Arguments = "$arguments --delay $Delay"
    $lnk.WorkingDirectory = $Here
    $lnk.Description = $description
    $lnk.Save()
    'shortcut'
}

function Disable-Autostart([string]$Name) {
    $removed = New-Object System.Collections.Generic.List[string]
    if (Get-AutostartTask $Name) {
        Unregister-ScheduledTask -TaskName $Name -TaskPath '\' -Confirm:$false
        $removed.Add("attività pianificata '$Name'")
    }
    $lnk = Get-ShortcutPath $Name
    if (Test-Path -LiteralPath $lnk) {
        Remove-Item -LiteralPath $lnk -Force
        $removed.Add('collegamento in Esecuzione automatica')
    }
    $removed.ToArray()
}

function Stop-AutoReceiver([int]$TimeoutSeconds = 20) {
    # Chiede a launcher.py di fermarsi (toglie il pad virtuale e chiude VIIPER); se non
    # lo fa entro il tempo, chiude i processi. Restituisce 'none', 'stopped' o 'killed'.
    if (@(Get-AutoProcess).Count -eq 0) { return 'none' }
    $stopEvent = $null
    if ([System.Threading.EventWaitHandle]::TryOpenExisting($StopEventName, [ref]$stopEvent)) {
        try { [void]$stopEvent.Set() } finally { $stopEvent.Dispose() }
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (@(Get-AutoProcess).Count -eq 0) { return 'stopped' }
        Start-Sleep -Milliseconds 300
    }
    $ErrorActionPreference = 'Continue'   # lo stderr di taskkill non deve diventare un'eccezione
    foreach ($p in Get-AutoProcess) { & taskkill.exe /PID $p.ProcessId /T /F 2>&1 | Out-Null }
    'killed'
}

function Show-StopResult([string]$Result) {
    switch ($Result) {
        'none' { Say INFO "il ricevitore in background non era in esecuzione" }
        'stopped' { Say OK "ricevitore in background fermato (pad virtuale e VIIPER chiusi)" }
        'killed' { Say WARN "il ricevitore non si è fermato da solo: processi chiusi" }
    }
}

function Get-LogSize {
    if (Test-Path -LiteralPath $AutoLog) { return (Get-Item -LiteralPath $AutoLog).Length }
    0
}

function Read-LogFrom([long]$Offset) {
    # Righe del log scritte dopo $Offset; il file resta aperto da launcher.py.
    if (-not (Test-Path -LiteralPath $AutoLog)) { return @() }
    $fs = [IO.File]::Open($AutoLog, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        if ($fs.Length -lt $Offset) { $Offset = 0 }   # log ruotato nel frattempo
        [void]$fs.Seek($Offset, 'Begin')
        $text = (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd()
    } finally { $fs.Dispose() }
    @($text -split "`r?`n" | Where-Object { $_.Trim() })
}

function Start-AutoReceiver([string]$Pythonw, [int]$TimeoutSeconds = 30) {
    # Avvia subito launcher.py --auto, come all'accesso, e segue il log finché il
    # ricevitore è in ascolto. Restituisce Result = 'running' (c'era già), 'started',
    # 'unconfirmed' (parte, ma il log non lo conferma) o 'failed', più Message.
    if (@(Get-AutoProcess).Count -gt 0) { return [pscustomobject]@{ Result = 'running'; Message = '' } }
    $offset = Get-LogSize
    $proc = Start-Process -FilePath $Pythonw -ArgumentList "`"$Launcher`"", '--auto' -WorkingDirectory $Here -PassThru
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        foreach ($line in Read-LogFrom $offset) {
            if ($line -match 'in ascolto su UDP') { return [pscustomobject]@{ Result = 'started'; Message = $line } }
            if ($line -match 'già in esecuzione') { return [pscustomobject]@{ Result = 'running'; Message = $line } }
            if ($line -match '^\S+ \S+ (ERROR|CRITICAL) +(.*)$') {
                # In background riproverebbe all'infinito: meglio fermarlo e dirlo subito.
                [void](Stop-AutoReceiver 10)
                return [pscustomobject]@{ Result = 'failed'; Message = $Matches[2] }
            }
        }
        if ($proc.HasExited) {
            return [pscustomobject]@{ Result = 'failed'; Message = "launcher.py terminato subito (codice $($proc.ExitCode))" }
        }
    }
    [pscustomobject]@{ Result = 'unconfirmed'; Message = '' }
}

function Show-Status([string]$Name) {
    # Restituisce $true se l'avvio automatico è attivo.
    $task = Get-AutostartTask $Name
    $active = $false
    if ($task) {
        $active = $task.State -ne 'Disabled'
        if ($active) { Say OK "attivo: attività pianificata '$Name' (stato $($task.State))" }
        else { Say WARN "attività pianificata '$Name' presente ma disabilitata nell'Utilità di pianificazione" }
        $a = @($task.Actions)[0]
        if ($a) {
            Say INFO "comando: $($a.Execute) $($a.Arguments)"
            if ("$($a.Arguments)".IndexOf($Launcher, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
                Say WARN "punta a un'altra cartella: rilanciare avvio-automatico-attiva.bat da questa"
            }
        }
        $info = Get-ScheduledTaskInfo -InputObject $task -ErrorAction SilentlyContinue
        if ($info -and $info.LastRunTime -and $info.LastRunTime.Year -gt 2000) {
            Say INFO ("ultimo avvio: {0:g}, risultato 0x{1:X}" -f $info.LastRunTime, $info.LastTaskResult)
        }
    }
    if (Test-Path -LiteralPath (Get-ShortcutPath $Name)) {
        $active = $true
        Say OK "attivo: collegamento in Esecuzione automatica ($(Get-ShortcutPath $Name))"
    }
    if (-not $task -and -not $active) { Say INFO "avvio automatico non attivo (per attivarlo: avvio-automatico-attiva.bat)" }
    $procs = @(Get-AutoProcess)
    if ($procs.Count -gt 0) { Say OK "ricevitore in background in esecuzione (PID $(@($procs.ProcessId) -join ', '))" }
    else { Say INFO "ricevitore in background non in esecuzione" }
    Say INFO "log: $AutoLog"
    $active
}

function Invoke-Main {
    $chosen = @($Avvia, $Attiva, $Disattiva, $Ferma, $Stato | Where-Object { $_.IsPresent })
    if ($chosen.Count -gt 1) { Say FAIL "indicare una sola azione: -Avvia, -Ferma, -Attiva, -Disattiva o -Stato"; return 2 }
    try {
        if ($Avvia) {
            $pythonw = Resolve-Pythonw $PythonExe
            if (-not $pythonw) {
                Say FAIL "non trovo pythonw.exe di Python $PythonMin o successivo: lanciare prima installa.bat"
                return 1
            }
            $r = Start-AutoReceiver $pythonw
            switch ($r.Result) {
                'running' {
                    # Senza messaggio: c'è già quello in background. Con: lo ha detto launcher.py,
                    # quindi è aperto quello con la finestra.
                    if ($r.Message) { Say OK "il ricevitore è già aperto in un'altra finestra (avvia-con-finestra.bat)" }
                    else { Say OK "il ricevitore è già in esecuzione in background (per fermarlo: ferma.bat)" }
                    return 0
                }
                'started' {
                    Say OK "VIIPER e ricevitore avviati in background, senza finestra"
                    Say INFO "per fermarli: ferma.bat; messaggi in $AutoLog"
                    return 0
                }
                'unconfirmed' {
                    Say WARN "avviato, ma il log non conferma ancora che il ricevitore sia in ascolto"
                    Say INFO "controllare $AutoLog, oppure usare avvia-con-finestra.bat"
                    return 0
                }
                default {
                    Say FAIL $r.Message
                    Say INFO "niente resta in esecuzione. Per vedere tutti i messaggi: avvia-con-finestra.bat"
                    return 1
                }
            }
        }
        if ($Attiva) {
            $pythonw = Resolve-Pythonw $PythonExe
            if (-not $pythonw) {
                Say FAIL "non trovo pythonw.exe di Python $PythonMin o successivo: lanciare prima installa.bat"
                return 1
            }
            $how = Enable-Autostart $Nome $pythonw $RitardoSecondi
            $where = if ($how -eq 'task') { "attività pianificata '$Nome'" } else { "collegamento in Esecuzione automatica" }
            Say OK "avvio automatico attivo ($where): parte $RitardoSecondi s dopo l'accesso a Windows, senza finestra"
            Say INFO "messaggi in $AutoLog; per toglierlo: avvio-automatico-disattiva.bat"
            return 0
        }
        if ($Disattiva) {
            $removed = @(Disable-Autostart $Nome)
            Show-StopResult (Stop-AutoReceiver)
            if ($removed.Count -gt 0) { Say OK "avvio automatico disattivato (tolto: $($removed -join ', '))" }
            else { Say INFO "l'avvio automatico non era attivo" }
            return 0
        }
        if ($Ferma) {
            Show-StopResult (Stop-AutoReceiver)
            return 0
        }
        if (Show-Status $Nome) { return 0 }
        return $NotActiveExitCode
    } catch {
        Say FAIL $_.Exception.Message
        return 1
    }
}

if ($env:XEG_AUTOSTART_NO_MAIN) { return }   # usato dai test per caricare solo le funzioni
exit (Invoke-Main)
