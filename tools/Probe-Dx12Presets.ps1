param(
    [string] $PortableRoot = "C:\Users\noswi\Desktop\foobar2000-portable-dx12-dev",

    [string] $ComponentDll = "",

    [string[]] $Presets = @(),

    [string] $PresetList = "",

    [int] $DelaySeconds = 4,

    [switch] $KeepLastRunning
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $PortableRoot -PathType Container)) {
    throw "Portable foobar root not found: $PortableRoot"
}

$foobarExe = Join-Path $PortableRoot "foobar2000.exe"
if (-not (Test-Path -LiteralPath $foobarExe -PathType Leaf)) {
    throw "foobar2000.exe not found: $foobarExe"
}

$logPath = Join-Path $PortableRoot "profile\foo_vis_milk2_dx12.log"
$componentTarget = Join-Path $PortableRoot "profile\user-components-x64\foo_vis_milk2\foo_vis_milk2.dll"

if ($ComponentDll -ne "") {
    if (-not (Test-Path -LiteralPath $ComponentDll -PathType Leaf)) {
        throw "Component DLL not found: $ComponentDll"
    }
    Copy-Item -LiteralPath $ComponentDll -Destination $componentTarget -Force
}

$presetQueue = New-Object System.Collections.Generic.List[string]
foreach ($preset in $Presets) {
    if (-not [string]::IsNullOrWhiteSpace($preset)) {
        $presetQueue.Add($preset.Trim())
    }
}

if ($PresetList -ne "") {
    if (-not (Test-Path -LiteralPath $PresetList -PathType Leaf)) {
        throw "Preset list not found: $PresetList"
    }
    foreach ($line in [System.IO.File]::ReadLines($PresetList)) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -gt 0 -and -not $trimmed.StartsWith("#")) {
            $presetQueue.Add($trimmed)
        }
    }
}

if ($presetQueue.Count -eq 0) {
    throw "No presets supplied. Use -Presets or -PresetList."
}

function Stop-Foobar {
    Get-Process foobar2000 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
}

function Get-ProbeResult([string] $Marker) {
    $lines = Get-Content -LiteralPath $logPath
    $markerLine = ($lines | Select-String -SimpleMatch $Marker | Select-Object -Last 1).LineNumber
    if (-not $markerLine) {
        return [pscustomobject]@{
            Status = "MissingMarker"
            ShaderLine = ""
            Errors = 0
            TextureFailures = 0
            PsoFailures = 0
        }
    }

    $after = $lines[($markerLine - 1)..($lines.Count - 1)]
    $shaderLine = ($after | Select-String -Pattern 'shader probe preset=' | Select-Object -Last 1).Line
    $errors = ($after | Select-String -Pattern 'shader probe failed|shader unavailable|cache_fail=[1-9]').Count
    $textureFailures = ($after | Select-String -Pattern 'texture slot \d+ failed|texture load failed').Count
    $psoFailures = ($after | Select-String -Pattern 'PSO create failed').Count

    $status = "Unknown"
    if ($shaderLine -match 'ok=2/2') {
        $status = "ShaderOk"
    } elseif ($shaderLine -match 'fixed_pipeline=1|shader_count=0') {
        $status = "FixedPipeline"
    } elseif ($errors -gt 0 -or $textureFailures -gt 0 -or $psoFailures -gt 0) {
        $status = "Failed"
    }

    [pscustomobject]@{
        Status = $status
        ShaderLine = $shaderLine
        Errors = $errors
        TextureFailures = $textureFailures
        PsoFailures = $psoFailures
    }
}

$runId = Get-Date -Format "yyyyMMdd-HHmmss"
Add-Content -LiteralPath $logPath -Value "===== dx12 probe batch $runId count=$($presetQueue.Count) ====="

$results = New-Object System.Collections.Generic.List[object]
for ($i = 0; $i -lt $presetQueue.Count; $i++) {
    $preset = $presetQueue[$i]
    Stop-Foobar

    $marker = "===== dx12 probe $runId preset=$preset ====="
    Add-Content -LiteralPath $logPath -Value $marker

    $env:FOO_VIS_MILK2_DX12_DEV = "1"
    $env:FOO_VIS_MILK2_DX12_LOG = $logPath
    $env:FOO_VIS_MILK2_DX12_WARP_SHADER = "1"
    $env:FOO_VIS_MILK2_DX12_COMP_SHADER = "1"
    $env:FOO_VIS_MILK2_DX12_POSTPROCESS = "1"
    $env:FOO_VIS_MILK2_DX12_BLUR = "1"
    $env:FOO_VIS_MILK2_DX12_PRESET_BACKGROUNDS = "1"
    $env:FOO_VIS_MILK2_DX12_START_PRESET = $preset
    Remove-Item Env:\FOO_VIS_MILK2_DX12_TEXTURE_CYCLE -ErrorAction SilentlyContinue

    Start-Process -FilePath $foobarExe -WorkingDirectory $PortableRoot -WindowStyle Hidden
    Start-Sleep -Seconds $DelaySeconds

    $probe = Get-ProbeResult $marker
    $results.Add([pscustomobject]@{
        Preset = $preset
        Status = $probe.Status
        Errors = $probe.Errors
        TextureFailures = $probe.TextureFailures
        PsoFailures = $probe.PsoFailures
        ShaderLine = $probe.ShaderLine
    })
}

if (-not $KeepLastRunning) {
    Stop-Foobar
}

$results | Format-Table -AutoSize

""
"Summary:"
$results | Group-Object Status | Sort-Object Name | ForEach-Object {
    [pscustomobject]@{
        Status = $_.Name
        Count = $_.Count
    }
} | Format-Table -AutoSize
