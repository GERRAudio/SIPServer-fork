# Build check (SIPServer_Installer.wixproj, CheckVoskSmallModel): the bundled Vosk small model zip must exist and
# contain a loadable model. The Vosk repo git-ignores models\, so this file is a local, untracked build input.
param([Parameter(Mandatory = $true)] [string]$Zip)
$name = [IO.Path]::GetFileNameWithoutExtension($Zip)
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (-not (Test-Path -LiteralPath $Zip)) { throw "missing: $Zip" }
    $z = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $have = @{}; foreach ($e in $z.Entries) { $have[$e.FullName] = $e.Length }
        foreach ($r in 'am/final.mdl', 'conf/model.conf', 'graph/HCLr.fst', 'graph/Gr.fst', 'graph/phones/word_boundary.int', 'ivector/final.ie') {
            $k = "$name/$r"
            if (-not $have.ContainsKey($k) -or $have[$k] -le 0) { throw "$Zip does not contain $k" }
        }
    } finally { $z.Dispose() }
    Write-Host "Vosk model package OK: $Zip"
    exit 0
} catch {
    Write-Host "error : Bundled Vosk model package is not usable - $($_.Exception.Message). Download $name.zip from https://alphacephei.com/vosk/models/ into the Vosk repo's models folder."
    exit 1
}
