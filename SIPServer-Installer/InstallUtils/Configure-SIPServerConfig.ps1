<#
.SYNOPSIS
    SIPServer setup: applies the operator's choices for 00_asr.xml / vosk.conf.xml and writes the installation summary.

.DESCRIPTION
    Called by the MSI (deferred, as SYSTEM) after DeployMissingConfigs and SetIP. Before the previous version was
    removed, setup copied conf\dialplan\00_asr.xml and conf\autoload_configs\vosk.conf.xml to -Backup.
      00_asr.xml     is an MSI file, so the upgrade has just replaced it with this version's default.
                     -OverwriteAsrDialplan "1": keep the default.  Otherwise: the site's previous file is put back.
      vosk.conf.xml  is not an MSI file (copied from conf_defaults only when missing).
                     -OverwriteVosk "1": replaced with this version's default.  Otherwise: left as it is.
    Every file replaced is first saved to -HistoryRoot\<date>. Then it reports (no changes) the server addresses in
    conf\vars.xml (set from the IP page by SetIP.bat) and the event socket ACL, which must be edited by hand.
    Writes -SummaryPath (UTF-16; shown to the operator at the end of an interactive install). Never fails the setup.
#>
param(
    [Parameter(Mandatory = $true)] [string]$AppDir,
    [Parameter(Mandatory = $true)] [string]$Backup,
    [string]$OverwriteAsrDialplan = '',
    [string]$OverwriteVosk = '',
    [string]$TcpAdr = '',
    [string]$UdpAdr = '',
    [string]$Upgrading = '',
    [string]$HistoryRoot = 'C:\inetpub\SIPServerConfigBackup',
    [string]$SummaryPath = 'C:\inetpub\SIPServerConfigBackup\InstallSummary.txt',
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
$AppDir = [IO.Path]::GetFullPath($AppDir.TrimEnd('.').TrimEnd('\'))
$Backup = [IO.Path]::GetFullPath($Backup.TrimEnd('.').TrimEnd('\'))
$conf = Join-Path $AppDir 'conf'
$defaults = Join-Path $AppDir 'conf_defaults'
$overwriteAsr = $OverwriteAsrDialplan.Trim() -eq '1'
$overwriteVoskFlag = $OverwriteVosk.Trim() -eq '1'
$history = Join-Path $HistoryRoot ("before_v$($Version)_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))

$summary = New-Object System.Collections.Generic.List[string]
function S([string]$t) { $summary.Add($t) }
function Write-Log([string]$m) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"
    [Console]::Out.WriteLine($line)
    try {
        if (-not (Test-Path $HistoryRoot)) { New-Item -ItemType Directory -Path $HistoryRoot -Force | Out-Null }
        Add-Content -Path (Join-Path $HistoryRoot 'ConfigSetup.log') -Value $line -Encoding UTF8
    } catch { }
}
function Save-ToHistory([string]$source, [string]$rel) {
    $dest = Join-Path $history $rel
    New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $dest -Force
}

try {
    Write-Log "Start (version $Version): app='$AppDir' overwrite00asr=$overwriteAsr overwriteVosk=$overwriteVoskFlag tcp='$TcpAdr' udp='$UdpAdr' upgrading='$Upgrading'"
    S "SIPServer $Version - installation summary ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
    S ""
    S "CONFIGURATION FILES ($conf)"

    # --- 00_asr.xml (ASR dialplan) --------------------------------------------------------------------------
    $asrRel = 'dialplan\00_asr.xml'
    $asrLive = Join-Path $conf $asrRel
    $asrSaved = Join-Path $Backup '00_asr.xml'
    try {
        if (Test-Path -LiteralPath $asrSaved) {
            Save-ToHistory $asrSaved $asrRel
            if ($overwriteAsr) {
                S "  00_asr.xml (ASR dialplan): REPLACED with this version's default (as selected). Previous file saved."
            } else {
                Copy-Item -LiteralPath $asrSaved -Destination $asrLive -Force
                S "  00_asr.xml (ASR dialplan): kept (the site's previous file was restored)."
            }
        } elseif ($Upgrading) {
            S "  00_asr.xml (ASR dialplan): WARNING - the previous file could not be saved; this version's default is installed."
            S "    If the site had changes, restore it from the dated $([IO.Path]::GetDirectoryName($AppDir))\SIPServer_<date> backup."
        } else {
            S "  00_asr.xml (ASR dialplan): new installation - default installed."
        }
    } catch { S "  00_asr.xml: ERROR $($_.Exception.Message)"; Write-Log "ERROR 00_asr.xml: $($_.Exception.Message)" }

    # --- vosk.conf.xml (ASR engine connection) -------------------------------------------------------------
    $voskRel = 'autoload_configs\vosk.conf.xml'
    $voskLive = Join-Path $conf $voskRel
    $voskDefault = Join-Path $defaults $voskRel
    $voskSaved = Join-Path $Backup 'vosk.conf.xml'
    try {
        if (Test-Path -LiteralPath $voskSaved) {
            if ($overwriteVoskFlag -and (Test-Path -LiteralPath $voskDefault)) {
                Save-ToHistory $voskSaved $voskRel
                Copy-Item -LiteralPath $voskDefault -Destination $voskLive -Force
                S "  vosk.conf.xml (ASR engine): REPLACED with this version's default (as selected). Previous file saved."
            } else {
                S "  vosk.conf.xml (ASR engine): kept unchanged."
            }
        } elseif (Test-Path -LiteralPath $voskLive) {
            S "  vosk.conf.xml (ASR engine): was missing - default provided."
        } else {
            S "  vosk.conf.xml (ASR engine): MISSING - copy it from $voskDefault."
        }
    } catch { S "  vosk.conf.xml: ERROR $($_.Exception.Message)"; Write-Log "ERROR vosk.conf.xml: $($_.Exception.Message)" }
    S "  Other configuration files: kept; any missing file was provided from conf_defaults."

    # --- Bundled Vosk engine (optional VoskASR feature, "Install the bundled Vosk ASR engine" checkbox) -----
    S ""
    S "ASR ENGINE (VoskASR service)"
    if (Test-Path -LiteralPath (Join-Path $AppDir 'Vosk\Vosk_server.exe')) {
        S "  Bundled Vosk engine: INSTALLED - model and VoskASR service status: see VOSK ASR MODEL AND SERVICE below."
    } else {
        S "  Bundled Vosk engine: NOT installed (checkbox left unticked)."
        S "  NOTE: a running Vosk engine service is REQUIRED for ASR to function. Either re-run setup and tick"
        S "  'Install the bundled Vosk ASR engine', or point conf\autoload_configs\vosk.conf.xml at a Vosk server elsewhere."
    }
    if (Test-Path -LiteralPath $history) { S "  Replaced/previous files saved to: $history" }

    # --- vars.xml addresses (set from the IP page by SetIP.bat) ----------------------------------------------
    S ""
    S "SERVER ADDRESSES (conf\vars.xml)"
    try {
        $vars = Get-Content -LiteralPath (Join-Path $conf 'vars.xml') -Raw
        $local = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress })
        foreach ($item in @(@('setIPv4', 'Admin / SIP (TCP)', $TcpAdr), @('multicastIP', 'AES67 / streaming (UDP)', $UdpAdr))) {
            $m = [regex]::Match($vars, 'data="' + $item[0] + '=([^"]+)"')
            $val = if ($m.Success) { $m.Groups[1].Value } else { '' }
            if (-not $val) { S "  $($item[1]): $($item[0]) NOT FOUND in vars.xml - check the file."; continue }
            $line = "  $($item[1]): $($item[0]) = $val"
            if ($local -contains $val) { $line += "  (assigned to a network adapter on this server)" }
            else { $line += "  WARNING: no network adapter on this server has this address" }
            S $line
            if ($item[2] -and $item[2] -ne $val) {
                S "    NOTE: the IP page had $($item[2]), but vars.xml keeps $val (setup only replaces the factory address). Edit vars.xml if $($item[2]) is correct."
            }
        }
    } catch { S "  Could not read vars.xml: $($_.Exception.Message)" }

    # --- Event socket ACL (manual) -------------------------------------------------------------------------
    S ""
    S "EVENT SOCKET ACCESS (ESL, port 8021) - REVIEW AND EDIT BY HAND IF NEEDED"
    try {
        $es = Get-Content -LiteralPath (Join-Path $conf 'autoload_configs\event_socket.conf.xml') -Raw
        $es = [regex]::Replace($es, '<!--.*?-->', '', 'Singleline')
        $m = [regex]::Match($es, 'name="apply-inbound-acl"\s+value="([^"]+)"')
        $aclName = if ($m.Success) { $m.Groups[1].Value } else { '' }
        S "  conf\autoload_configs\event_socket.conf.xml: apply-inbound-acl = $(if ($aclName) { $aclName } else { '(not set: no ACL)' })"
        if ($aclName -eq 'loopback.auto') {
            S "    Only programs on this server may connect. A dashboard on another server will be refused (Access Denied)."
        } elseif ($aclName) {
            $acl = [xml](Get-Content -LiteralPath (Join-Path $conf 'autoload_configs\acl.conf.xml') -Raw)
            $list = $acl.SelectSingleNode("//list[@name='$aclName']")
            if ($list) {
                $nodes = @($list.SelectNodes('node') | ForEach-Object { "$($_.type) $($_.cidr)$($_.domain)" })
                S "  conf\autoload_configs\acl.conf.xml: list '$aclName' (default $($list.default)): $($nodes -join ', ')"
            } else {
                S "  WARNING: acl.conf.xml has no list '$aclName' - SIPServer will refuse every event socket connection."
            }
        }
        S "  To allow a server (e.g. the dashboard): add <node type=""allow"" cidr=""<its IPv4>/32""/> to that list."
        S "  Use plain IPv4 /32 entries (::ffff: entries are rejected). Restart SIPServer to apply."
    } catch { S "  Could not read the ACL files: $($_.Exception.Message)" }

    S ""
    S "Start (or restart) the SIPServer service when ready. Log: $(Join-Path $HistoryRoot 'ConfigSetup.log')"
}
catch {
    S "ERROR: $($_.Exception.Message)"
    Write-Log "ERROR: $($_.Exception.Message)"
}
finally {
    try {
        New-Item -ItemType Directory -Path (Split-Path $SummaryPath -Parent) -Force | Out-Null
        Set-Content -Path $SummaryPath -Value ($summary -join "`r`n") -Encoding Unicode
        foreach ($l in $summary) { Write-Log "  | $l" }
    } catch { }
    if (Test-Path -LiteralPath $Backup) { Remove-Item -LiteralPath $Backup -Recurse -Force -ErrorAction SilentlyContinue }
}
exit 0
