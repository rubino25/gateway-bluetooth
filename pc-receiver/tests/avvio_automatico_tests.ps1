# Prove di avvio-automatico.ps1 e del passo 6 di installa.ps1, senza toccare
# l'Utilità di pianificazione né la cartella Esecuzione automatica vere.
# Le lancia test_avvio_automatico.py; da sole: powershell -File tests\avvio_automatico_tests.ps1
$ErrorActionPreference = 'Stop'
# Il modulo va caricato prima delle funzioni finte: caricato dopo (in automatico, alla
# prima New-ScheduledTask*) le sostituirebbe con quelle vere.
Import-Module ScheduledTasks
$env:XEG_AUTOSTART_NO_MAIN = '1'
. (Join-Path $PSScriptRoot '..\avvio-automatico.ps1')
$env:XEG_AUTOSTART_NO_MAIN = ''
$fails = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "PASS $name" } else { Write-Host "FAIL $name"; $script:fails++ }
}
function Assert-Mocked([string]$Name) {
    if ((Get-Command $Name).ModuleName) { throw "$Name non è quella finta: prova interrotta per non toccare il PC" }
}

$taskName = 'xeg-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8)   # mai quello vero
$dir = Join-Path $env:TEMP ('xeg avvio ' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $dir | Out-Null
$StartupDir = $dir   # al posto della cartella Esecuzione automatica vera
$lnkPath = Join-Path $dir "$taskName.lnk"
try {
    # Attività pianificata: Register-ScheduledTask finto, le definizioni sono quelle vere.
    function Register-ScheduledTask {
        param($TaskName, $TaskPath, $Action, $Trigger, $Principal, $Settings, $Description, [switch]$Force)
        $script:reg = @{ Name = $TaskName; Path = $TaskPath; Action = $Action; Trigger = $Trigger
                         Principal = $Principal; Settings = $Settings; Force = $Force }
    }
    Assert-Mocked Register-ScheduledTask
    $how = Enable-Autostart $taskName 'C:\Py thon\pythonw.exe' 30
    Check "task: meccanismo $how" ($how -eq 'task')
    Check 'task: nome e cartella' ($reg.Name -eq $taskName -and $reg.Path -eq '\' -and $reg.Force)
    Check "task: programma $($reg.Action.Execute)" ($reg.Action.Execute -eq 'C:\Py thon\pythonw.exe')
    Check "task: argomenti $($reg.Action.Arguments)" ($reg.Action.Arguments -eq "`"$Launcher`" --auto")
    Check 'task: cartella di lavoro' ($reg.Action.WorkingDirectory -eq $Here)
    Check "task: ritardo $($reg.Trigger.Delay)" ($reg.Trigger.Delay -eq 'PT30S')
    Check 'task: all''accesso dell''utente corrente' ($reg.Trigger.UserId -eq "$env:USERDOMAIN\$env:USERNAME")
    Check 'task: senza amministratore' ($reg.Principal.RunLevel -eq 'Limited' -and $reg.Principal.LogonType -eq 'Interactive')
    Check "task: nessun limite di durata ($($reg.Settings.ExecutionTimeLimit))" ($reg.Settings.ExecutionTimeLimit -eq 'PT0S')
    Check 'task: anche a batteria' ($reg.Settings -and -not $reg.Settings.DisallowStartIfOnBatteries -and -not $reg.Settings.StopIfGoingOnBatteries)
    Check 'task: una sola istanza' ($reg.Settings.MultipleInstances -eq 'IgnoreNew')
    Check "task: priorità $($reg.Settings.Priority)" ($reg.Settings.Priority -eq 4)
    Check 'task: riavvio' ($reg.Settings.RestartCount -eq 3 -and $reg.Settings.RestartInterval -eq 'PT1M')

    # Senza attività (Windows la rifiuta): collegamento in Esecuzione automatica.
    function Register-ScheduledTask { throw 'Accesso negato.' }
    Assert-Mocked Register-ScheduledTask
    $how = Enable-Autostart $taskName $env:ComSpec 45
    Check "ripiego: meccanismo $how" ($how -eq 'shortcut' -and (Test-Path -LiteralPath $lnkPath))
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut($lnkPath)
    Check "ripiego: argomenti $($lnk.Arguments)" ($lnk.Arguments -eq "`"$Launcher`" --auto --delay 45")
    Check 'ripiego: cartella di lavoro' ($lnk.WorkingDirectory -eq $Here)

    # Riattivando con l'attività il collegamento di prima sparisce.
    function Register-ScheduledTask { param($TaskName, $TaskPath, $Action, $Trigger, $Principal, $Settings, $Description, [switch]$Force) }
    Assert-Mocked Register-ScheduledTask
    [void](Enable-Autostart $taskName $env:ComSpec 30)
    Check 'task di nuovo: collegamento tolto' (-not (Test-Path -LiteralPath $lnkPath))

    # Disattivazione: toglie quello che c'è; senza nulla non fa nulla.
    $script:unreg = $null
    function Get-ScheduledTask { param($TaskPath, $TaskName) [pscustomobject]@{ TaskName = $TaskName } }
    function Unregister-ScheduledTask { param($TaskName, $TaskPath, [switch]$Confirm) $script:unreg = $TaskName }
    function Register-ScheduledTask { throw 'Accesso negato.' }
    Assert-Mocked Get-ScheduledTask; Assert-Mocked Unregister-ScheduledTask; Assert-Mocked Register-ScheduledTask
    [void](Enable-Autostart $taskName $env:ComSpec 30)   # collegamento, più l'attività finta
    $removed = @(Disable-Autostart $taskName)
    Check "disattiva: tolti $($removed.Count)" ($removed.Count -eq 2 -and $unreg -eq $taskName -and -not (Test-Path -LiteralPath $lnkPath))
    function Get-ScheduledTask { param($TaskPath, $TaskName) $null }
    Assert-Mocked Get-ScheduledTask
    $removed = @(Disable-Autostart $taskName)
    Check "disattiva: niente da togliere ($($removed.Count))" ($removed.Count -eq 0)

    # Arresto: imposta l'evento di launcher.py e aspetta che il processo sparisca.
    $StopEventName = 'Local\xeg-test-' + [guid]::NewGuid().ToString('N')
    $testEvent = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::ManualReset, $StopEventName)
    try {
        function Get-AutoProcess { if ($testEvent.WaitOne(0)) { @() } else { @([pscustomobject]@{ ProcessId = 0 }) } }
        $result = Stop-AutoReceiver 5
        Check "ferma: $result" ($result -eq 'stopped' -and $testEvent.WaitOne(0))
        Check "ferma: niente in esecuzione" ((Stop-AutoReceiver 5) -eq 'none')
    } finally {
        $testEvent.Dispose()
        Remove-Item Function:\Get-AutoProcess
    }

    # Avvio subito (avvia.bat): Start-Process finto che scrive nel log come launcher.py.
    $AutoLog = Join-Path $dir 'avvio-automatico.log'
    Set-Content -LiteralPath $AutoLog -Encoding UTF8 -Value '2026-01-01 00:00:00 ERROR   errore di un avvio precedente'
    function Get-AutoProcess { @() }
    function Start-Process {
        param($FilePath, $ArgumentList, $WorkingDirectory, [switch]$PassThru)
        $script:started = @{ Exe = $FilePath; Args = $ArgumentList -join ' '; Cwd = $WorkingDirectory }
        Add-Content -LiteralPath $AutoLog -Encoding UTF8 -Value $script:logLines
        [pscustomobject]@{ HasExited = $script:exited; ExitCode = 2 }
    }
    $script:exited = $false
    $script:logLines = @('2026-01-01 00:00:01 INFO    avvio automatico (PID 1)', '2026-01-01 00:00:02 INFO    in ascolto su UDP 0.0.0.0:47900')
    $r = Start-AutoReceiver 'C:\Py\pythonw.exe' 5
    Check "avvia: $($r.Result)" ($r.Result -eq 'started')
    Check "avvia: comando $($started.Args)" ($started.Exe -eq 'C:\Py\pythonw.exe' -and $started.Args -eq "`"$Launcher`" --auto" -and $started.Cwd -eq $Here)
    $script:logLines = @('2026-01-01 00:00:03 ERROR   usbip-win2 non trovato (usbip.exe).')
    $r = Start-AutoReceiver 'C:\Py\pythonw.exe' 5
    Check "avvia: errore ($($r.Result): $($r.Message))" ($r.Result -eq 'failed' -and $r.Message -eq 'usbip-win2 non trovato (usbip.exe).')
    $script:logLines = @('2026-01-01 00:00:04 INFO    il ricevitore è già in esecuzione (in background o in un''altra finestra): esco')
    $r = Start-AutoReceiver 'C:\Py\pythonw.exe' 5
    Check "avvia: già aperto con la finestra ($($r.Result))" ($r.Result -eq 'running' -and $r.Message)
    $script:logLines = @(); $script:exited = $true
    $r = Start-AutoReceiver 'C:\Py\pythonw.exe' 5
    Check "avvia: uscito subito ($($r.Result))" ($r.Result -eq 'failed' -and $r.Message -match 'codice 2')
    $script:exited = $false
    $r = Start-AutoReceiver 'C:\Py\pythonw.exe' 1
    Check "avvia: senza conferma ($($r.Result))" ($r.Result -eq 'unconfirmed')
    function Get-AutoProcess { @([pscustomobject]@{ ProcessId = 1 }) }
    $script:started = $null
    $r = Start-AutoReceiver 'C:\Py\pythonw.exe' 5
    Check "avvia: già in background ($($r.Result))" ($r.Result -eq 'running' -and -not $r.Message -and -not $started)
    Remove-Item Function:\Start-Process, Function:\Get-AutoProcess

    # pythonw.exe accanto al Python indicato (quello che lancia pytest).
    if ($env:XEG_TEST_PYTHON) {
        $pw = Resolve-Pythonw $env:XEG_TEST_PYTHON
        Check "pythonw: $pw" ($pw -and (Split-Path -Leaf $pw) -eq 'pythonw.exe' -and (Test-Path -LiteralPath $pw))
    }
    Check 'pythonw: Python inesistente' ($null -eq (Resolve-Pythonw (Join-Path $dir 'python.exe')))
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    # Per sicurezza: se una funzione finta non avesse funzionato, sul PC non resta nulla.
    foreach ($f in 'Register-ScheduledTask', 'Get-ScheduledTask', 'Unregister-ScheduledTask') {
        Remove-Item "Function:\$f" -ErrorAction SilentlyContinue
    }
    if (Get-ScheduledTask -TaskPath '\' -TaskName $taskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskPath '\' -TaskName $taskName -Confirm:$false
        Write-Host "FAIL creata un'attività vera $taskName (tolta)"; $fails++
    }
}

# Passo 6 di installa.ps1, con avvio-automatico.ps1 e Read-Host finti.
$env:XEG_SETUP_NO_MAIN = '1'
. (Join-Path $PSScriptRoot '..\installa.ps1')
$env:XEG_SETUP_NO_MAIN = ''
$py = [pscustomobject]@{ Exe = 'C:\Python\python.exe'; Version = '3.12.10' }
function Invoke-AutostartScript([hashtable]$Arguments, [switch]$Quiet) {
    $script:calls.Add(($Arguments.Keys | Sort-Object) -join '+')
    if ($Arguments.Attiva) { $script:attivaPython = $Arguments.PythonExe; return 0 }
    if ($script:alreadyActive) { 0 } else { 3 }
}
function Read-Host { param($Prompt) $script:asked = $true; $script:answer }
function Reset-Step($active, $answer) {
    $script:calls = New-Object System.Collections.Generic.List[string]
    $script:alreadyActive = $active; $script:answer = $answer; $script:asked = $false; $script:attivaPython = $null
}

Reset-Step $false ''
$r = Invoke-AutostartStep $py 0
Check "installa: Invio = sì ($r)" ($r -eq 'on' -and $asked -and $attivaPython -eq 'C:\Python\python.exe')
Reset-Step $false 'n'
$r = Invoke-AutostartStep $py 0
Check "installa: no ($r)" ($r -eq 'off' -and $asked -and -not ($calls -contains 'Attiva+PythonExe'))
Reset-Step $true ''
$r = Invoke-AutostartStep $py 0
Check "installa: già attivo, niente domanda ($r)" ($r -eq 'on' -and -not $asked -and ($calls -contains 'Attiva+PythonExe'))
Reset-Step $false 'n'
$AvvioAutomatico = [switch]$true
$r = Invoke-AutostartStep $py 0
Check "installa: -AvvioAutomatico ($r)" ($r -eq 'on' -and -not $asked)
$AvvioAutomatico = [switch]$false
$NoAvvioAutomatico = [switch]$true
Reset-Step $false ''
$r = Invoke-AutostartStep $py 0
Check "installa: -NoAvvioAutomatico ($r)" ($r -eq 'off' -and -not $asked -and -not ($calls -contains 'Attiva+PythonExe'))
$NoAvvioAutomatico = [switch]$false
Reset-Step $false ''
$r = Invoke-AutostartStep $py 2
Check "installa: altri passi falliti ($r)" ($r -eq 'skipped' -and $calls.Count -eq 0 -and -not $asked)
$CheckOnly = [switch]$true
Reset-Step $false ''
$r = Invoke-AutostartStep $py 0
Check "installa: solo controllo ($r)" ($r -eq 'off' -and -not $asked -and ($calls -join ',') -eq 'Stato')
$CheckOnly = [switch]$false

Write-Host "failures: $fails"
exit $fails
