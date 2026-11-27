<#
.SYNOPSIS
    SIPServer setup: installs or checks the bundled Vosk model, points the VoskASR service at a model, starts it,
    and adds the result to the installation summary.

.DESCRIPTION
    Called by the MSI (deferred, as SYSTEM) when the VoskASR feature is installed, after the VoskASR service has
    been created by nssm. The model is NOT an MSI file: the MSI ships it as a zip in Vosk\model_package, so an
    upgrade never deletes or half-replaces a working model.

    Bundled model (models\vosk-model-small-en-us-0.15), compared file by file with the bundled zip:
      matches           -> left alone
      missing           -> installed from the zip
      incomplete/differs-> replaced from the zip; the previous folder is kept as <name>.previous
      zip missing/bad, no disk space, extraction error -> left as it is, and the summary says how to install by hand
    (Text files that differ only in line endings count as matching.)

    Model used by the service: if the service previously ran another model (e.g. vosk-model-en-us-0.22, set by
    VOSKModelChangeTOBIGONE.bat) and that model is still complete, it is kept. Otherwise the bundled model is used.
    The service is then started and the script waits until the server listens on its port.

    Appends a "VOSK ASR MODEL AND SERVICE" section to -SummaryPath (UTF-16, written earlier by
    Configure-SIPServerConfig.ps1). Never fails the setup.
#>
param(
    [Parameter(Mandatory = $true)] [string]$AppDir,
    [string]$PrevParamsFile = '',
    [string]$HistoryRoot = 'C:\inetpub\SIPServerConfigBackup',
    [string]$SummaryPath = 'C:\inetpub\SIPServerConfigBackup\InstallSummary.txt',
    [int]$DefaultPort = 2700,
    [string]$ServiceName = 'VoskASR'
)

$ErrorActionPreference = 'Stop'
$AppDir = [IO.Path]::GetFullPath($AppDir.TrimEnd('.').TrimEnd('\'))
$voskDir = Join-Path $AppDir 'Vosk'
$modelsDir = Join-Path $voskDir 'models'
$modelName = 'vosk-model-small-en-us-0.15'
$bundledModel = Join-Path $modelsDir $modelName
$zipPath = Join-Path $voskDir "model_package\$modelName.zip"
$nssm = Join-Path $AppDir 'nssm.exe'
$voskLog = Join-Path $voskDir 'vosk_server.log'
$downloadUrl = "https://alphacephei.com/vosk/models/$modelName.zip"

$summary = New-Object System.Collections.Generic.List[string]
function S([string]$t) { $summary.Add($t) }
function Write-Log([string]$m) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') [vosk] $m"
    [Console]::Out.WriteLine($line)
    try {
        if (-not (Test-Path $HistoryRoot)) { New-Item -ItemType Directory -Path $HistoryRoot -Force | Out-Null }
        Add-Content -Path (Join-Path $HistoryRoot 'ConfigSetup.log') -Value $line -Encoding UTF8
    } catch { }
}

# A model folder Vosk can load: conf\model.conf, am\final.mdl and a graph (HCLG.fst, or HCLr.fst + Gr.fst).
function Test-ModelComplete([string]$dir) {
    if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { return $false }
    $has = { param($r) Test-Path -LiteralPath (Join-Path $dir $r) -PathType Leaf }
    return (& $has 'conf\model.conf') -and (& $has 'am\final.mdl') -and
           ((& $has 'graph\HCLG.fst') -or ((& $has 'graph\HCLr.fst') -and (& $has 'graph\Gr.fst')))
}

function Get-StreamHash([IO.Stream]$s) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($s)) } finally { $sha.Dispose() }
}

# Compare a model folder with the zip. Returns 'missing', 'matches', or 'differs: <reason>'.
function Compare-ModelWithZip([string]$dir, $zip, [string]$prefix) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return 'missing' }
    $bad = New-Object System.Collections.Generic.List[string]
    foreach ($e in $zip.Entries) {
        if (-not $e.FullName.StartsWith($prefix) -or $e.FullName.EndsWith('/')) { continue }
        $rel = $e.FullName.Substring($prefix.Length).Replace('/', '\')
        $f = Join-Path $dir $rel
        if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { $bad.Add("$rel missing"); continue }
        $len = (Get-Item -LiteralPath $f).Length
        if ($len -eq $e.Length) {
            $zs = $e.Open(); try { $zh = Get-StreamHash $zs } finally { $zs.Dispose() }
            $fs = [IO.File]::OpenRead($f); try { $fh = Get-StreamHash $fs } finally { $fs.Dispose() }
            if ($zh -eq $fh) { continue }
        }
        if ($e.Length -lt 65536 -and $len -lt 65536) {
            # small text files: ignore CR/LF differences
            $zs = New-Object IO.StreamReader($e.Open()); try { $zt = $zs.ReadToEnd() } finally { $zs.Dispose() }
            $ft = [IO.File]::ReadAllText($f)
            if ($zt.Replace("`r", '') -eq $ft.Replace("`r", '')) { continue }
        }
        $bad.Add("$rel differs")
    }
    if ($bad.Count -eq 0) { return 'matches' }
    return "differs: $(($bad | Select-Object -First 4) -join ', ')$(if ($bad.Count -gt 4) { " (+$($bad.Count - 4) more)" })"
}

function Add-ManualSteps {
    S "  ACTION REQUIRED - install the model by hand:"
    S "    1. Get $modelName.zip (from another SIPServer installation, or $downloadUrl)."
    S "    2. Extract it so that $bundledModel\am\final.mdl exists."
    S "    3. Start the VoskASR service (services.msc, or: sc start VoskASR)."
}

try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Write-Log "Start: app='$AppDir'"
    S ""
    S "VOSK ASR MODEL AND SERVICE"

    # --- 1. Bundled model: check, install or replace ----------------------------------------------------------
    $bundledOk = $false
    $zip = $null
    try {
        if (-not (Test-Path -LiteralPath $zipPath)) { throw "the bundled model package $zipPath is missing" }
        try { $zip = [IO.Compression.ZipFile]::OpenRead($zipPath) } catch { throw "the bundled model package is unreadable ($($_.Exception.Message))" }
        $prefix = "$modelName/"
        if (-not ($zip.Entries | Where-Object { $_.FullName -eq "${prefix}am/final.mdl" })) { throw "the bundled model package does not contain $modelName/am/final.mdl" }

        $state = Compare-ModelWithZip $bundledModel $zip $prefix
        Write-Log "bundled model state: $state"
        if ($state -eq 'matches') {
            S "  Bundled model ${modelName}: already installed and up to date - left unchanged."
            $bundledOk = $true
        } else {
            $need = ($zip.Entries | Measure-Object Length -Sum).Sum
            $free = (New-Object IO.DriveInfo([IO.Path]::GetPathRoot($voskDir))).AvailableFreeSpace
            if ($free -lt $need * 1.2) { throw "not enough disk space ($([math]::Round($free/1MB)) MB free, $([math]::Round($need/1MB)) MB needed)" }

            $staging = Join-Path $modelsDir ".setup-staging"
            if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
            New-Item -ItemType Directory -Path $staging -Force | Out-Null
            $zip.Dispose(); $zip = $null
            [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $staging)
            $extracted = Join-Path $staging $modelName
            if (-not (Test-ModelComplete $extracted)) { throw "the extracted model is incomplete" }

            $previous = "$bundledModel.previous"
            if ($state -ne 'missing') {
                if (Test-Path -LiteralPath $previous) { Remove-Item -LiteralPath $previous -Recurse -Force }
                Move-Item -LiteralPath $bundledModel -Destination $previous
            }
            Move-Item -LiteralPath $extracted -Destination $bundledModel
            Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue

            if ($state -eq 'missing') {
                S "  Bundled model ${modelName}: was not installed - INSTALLED from the setup package."
            } else {
                S "  Bundled model ${modelName}: was out of date or incomplete ($($state.Substring(9))) - REPLACED from the setup package."
                S "    The previous folder was kept as $previous (delete it once ASR is confirmed working)."
            }
            $bundledOk = Test-ModelComplete $bundledModel
        }
    } catch {
        Write-Log "ERROR bundled model: $($_.Exception.Message)"
        $bundledOk = Test-ModelComplete $bundledModel
        S "  Bundled model ${modelName}: COULD NOT BE INSTALLED - $($_.Exception.Message)."
        if ($bundledOk) { S "    The model already on this server looks complete and was left in place." }
        else { Add-ManualSteps }
    } finally {
        if ($zip) { $zip.Dispose() }
        $stg = Join-Path $modelsDir ".setup-staging"
        if (Test-Path -LiteralPath $stg) { Remove-Item -LiteralPath $stg -Recurse -Force -ErrorAction SilentlyContinue }
    }

    # --- 2. Which model the service runs -------------------------------------------------------------------
    $port = $DefaultPort
    $prevModel = ''
    if ($PrevParamsFile -and (Test-Path -LiteralPath $PrevParamsFile)) {
        $m = [regex]::Match((Get-Content -LiteralPath $PrevParamsFile -Raw), 'AppParameters\s+REG_\w+\s+(.+?)\s+(\d+)\s*$', 'Multiline')
        if ($m.Success) { $prevModel = $m.Groups[1].Value.Trim('"'); $port = [int]$m.Groups[2].Value }
        Remove-Item -LiteralPath $PrevParamsFile -Force -ErrorAction SilentlyContinue
    }
    Write-Log "previous service model='$prevModel' port=$port"

    $useModel = $bundledModel
    if ($prevModel -and ([IO.Path]::GetFullPath($prevModel).TrimEnd('\') -ne $bundledModel)) {
        if (Test-ModelComplete $prevModel) {
            $useModel = [IO.Path]::GetFullPath($prevModel).TrimEnd('\')
            S "  Service model: kept the site's choice $useModel (it is complete)."
        } else {
            S "  Service model: the previous model $prevModel is missing or incomplete - switched to the bundled $modelName."
        }
    } else {
        S "  Service model: $modelName (bundled)."
    }
    & $nssm set $ServiceName AppParameters "$useModel $port" | Out-Null
    Write-Log "nssm AppParameters='$useModel $port' (exit $LASTEXITCODE)"

    # --- 3. Start the service and check it listens -------------------------------------------------------------
    if (-not (Test-ModelComplete $useModel)) {
        S "  VoskASR service: NOT started - no complete model. ASR will not work until the model is installed (see above)."
    } else {
        $logStart = if (Test-Path -LiteralPath $voskLog) { (Get-Item -LiteralPath $voskLog).Length } else { 0 }
        & sc.exe start $ServiceName | Out-Null
        $timeout = if ($useModel -eq $bundledModel) { 60 } else { 300 }   # the large model takes minutes to load
        $deadline = (Get-Date).AddSeconds($timeout)
        $listening = $false
        while ((Get-Date) -lt $deadline) {
            if (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) { $listening = $true; break }
            Start-Sleep -Seconds 2
        }
        $svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
        $newLog = ''
        try {
            if (Test-Path -LiteralPath $voskLog) {
                $fs = [IO.File]::Open($voskLog, 'Open', 'Read', 'ReadWrite')
                try { if ($fs.Length -ge $logStart) { [void]$fs.Seek($logStart, 'Begin') }; $newLog = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
            }
        } catch { }
        if ($listening) {
            S "  VoskASR service: RUNNING - model loaded, listening on port $port."
        } elseif ($newLog -match 'Failed to load model|does not contain model files') {
            S "  VoskASR service: FAILED - the engine could not load $useModel. ASR will not work."
            S "    See $voskLog. Reinstall the model (re-run setup, or install it by hand)."
        } else {
            S "  VoskASR service: $(if ($svc) { $svc.Status } else { 'not found' }) - not yet listening on port $port after $timeout s."
            S "    The model may still be loading. Check later: services.msc, and $voskLog."
        }
        $errs = @($newLog -split "`r?`n" | Where-Object { $_ -match 'ERROR|Failed' } | Select-Object -Last 3)
        foreach ($l in $errs) { S "    log: $($l.Trim())" }
        Write-Log "service status=$($svc.Status) listening=$listening"
    }
}
catch {
    S "  ERROR: $($_.Exception.Message)"
    Add-ManualSteps
    Write-Log "ERROR: $($_.Exception.Message)"
}
finally {
    try {
        New-Item -ItemType Directory -Path (Split-Path $SummaryPath -Parent) -Force | Out-Null
        Add-Content -Path $SummaryPath -Value ($summary -join "`r`n") -Encoding Unicode
        foreach ($l in $summary) { Write-Log "  | $l" }
    } catch { }
}
exit 0
