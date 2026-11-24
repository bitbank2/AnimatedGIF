param(
    [string]$ProfileLabel = "upstream",
    [string]$ExtraFlags = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$Sketch = "$PSScriptRoot\esp8266_sketch"
$BuildDir = "$env:TEMP\agif_esp8266_$($ProfileLabel -replace '[^a-zA-Z0-9_-]', '_')"
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Path $BuildDir | Out-Null

$ArgsList = @(
    "compile",
    "--fqbn", "esp8266:esp8266:generic:eesz=4M2M,xtal=160,ip=lm2n",
    "--library", $RepoRoot,
    "--build-path", $BuildDir,
    "--output-dir", $BuildDir
)
if ($ExtraFlags -ne "") {
    $ArgsList += "--build-property"
    $ArgsList += "build.extra_flags=$ExtraFlags"
}
$ArgsList += $Sketch

$ErrorActionPreference = "Continue"
$out = & arduino-cli @ArgsList 2>&1 | ForEach-Object { "$_" }
$rc = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($rc -ne 0) {
    $out | Write-Host
    exit $rc
}

$Elf = Get-ChildItem $BuildDir -Filter "*.elf" | Select-Object -First 1
$Bin = Get-ChildItem $BuildDir -Filter "*.bin" | Select-Object -First 1
$Nm = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp8266\tools" -Recurse -Filter "xtensa-lx106-elf-nm.exe" | Select-Object -First 1

$gifObjSize = "unknown"
if ($Nm -and $Elf) {
    $symLine = & $Nm.FullName -S --size-sort $Elf.FullName | Select-String "\bgif$"
    if ($symLine) {
        $parts = ($symLine.Line.Trim() -split "\s+")
        if ($parts.Count -ge 2) {
            $gifObjSize = [Convert]::ToInt64($parts[1], 16)
        }
    }
}

$ramLine = $out | Select-String "IRAM:|DATA|RODATA|BSS" | Out-String
$flashBytes = if ($Bin) { $Bin.Length } else { 0 }

# Parse esp8266 memory summary lines
$dataB = 0; $rodataB = 0; $bssB = 0; $iramB = 0
foreach ($line in $out) {
    if ($line -match "(DATA|RODATA|BSS|IROM|IRAM)\s+(\d+)") {
        switch ($Matches[1]) {
            "DATA" { $dataB = [int]$Matches[2] }
            "RODATA" { $rodataB = [int]$Matches[2] }
            "BSS" { $bssB = [int]$Matches[2] }
            "IROM" { $iromB = [int]$Matches[2] }
            "IRAM" { $iramB = [int]$Matches[2] }
        }
    }
}
$dramTotal = $dataB + $rodataB + $bssB

Write-Output ("PROFILE={0} | sizeof(AnimatedGIF)={1} | BIN={2} B | IROM={3} B | DATA={4} RODATA={5} BSS={6} | DRAM={7} B" -f $ProfileLabel, $gifObjSize, $flashBytes, $iromB, $dataB, $rodataB, $bssB, $dramTotal)
$out | Select-String "Executable segment sizes|IROM|IRAM|DATA|RODATA|BSS|Sketch uses|Global variables" | ForEach-Object { "  $_" }
