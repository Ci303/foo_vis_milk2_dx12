param(
    [Parameter(Mandatory = $true)]
    [string] $PresetDirectory,

    [int] $SampleLimit = 0,

    [switch] $ListShaderPresets
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $PresetDirectory -PathType Container)) {
    throw "Preset directory not found: $PresetDirectory"
}

$features = [ordered]@{
    Total = 0
    WarpShader = 0
    CompShader = 0
    AnyShader = 0
    GeneratedLegacyShader = 0
    PerFrame = 0
    PerPixel = 0
    Sampler = 0
    RandomSampler = 0
    CustomShape = 0
    TexturedShape = 0
    CustomWave = 0
    WarpText = 0
    CompText = 0
}

$shaderPresets = New-Object System.Collections.Generic.List[string]

$files = [System.IO.Directory]::EnumerateFiles($PresetDirectory, "*.milk", [System.IO.SearchOption]::AllDirectories)
foreach ($file in $files) {
    if ($SampleLimit -gt 0 -and $features.Total -ge $SampleLimit) {
        break
    }

    $features.Total++
    $seen = @{}

    foreach ($line in [System.IO.File]::ReadLines($file)) {
        if ($line.Length -eq 0) {
            continue
        }

        if (-not $seen.WarpShader -and $line.StartsWith("PSVERSION_WARP=")) {
            $seen.WarpShader = $true
        } elseif (-not $seen.CompShader -and $line.StartsWith("PSVERSION_COMP=")) {
            $seen.CompShader = $true
        } elseif (-not $seen.PerFrame -and $line.StartsWith("per_frame_")) {
            $seen.PerFrame = $true
        } elseif (-not $seen.PerPixel -and $line.StartsWith("per_pixel_")) {
            $seen.PerPixel = $true
        } elseif (-not $seen.Sampler -and $line.StartsWith("sampler_")) {
            $seen.Sampler = $true
            if ($line.StartsWith("sampler_rand")) {
                $seen.RandomSampler = $true
            }
        } elseif (-not $seen.CustomShape -and $line -match '^shapecode_\d+_enabled=1') {
            $seen.CustomShape = $true
        } elseif (-not $seen.TexturedShape -and $line -match '^shapecode_\d+_textured=1') {
            $seen.TexturedShape = $true
        } elseif (-not $seen.CustomWave -and $line -match '^wavecode_\d+_enabled=1') {
            $seen.CustomWave = $true
        } elseif (-not $seen.WarpText -and $line.StartsWith("warp_")) {
            $seen.WarpText = $true
        } elseif (-not $seen.CompText -and $line.StartsWith("comp_")) {
            $seen.CompText = $true
        }
    }

    if ($seen.WarpShader) { $features.WarpShader++ }
    if ($seen.CompShader) { $features.CompShader++ }
    if ($seen.WarpShader -or $seen.CompShader) {
        $features.AnyShader++
        if ($ListShaderPresets) {
            $shaderPresets.Add($file)
        }
    }
    if (($seen.WarpShader -or $seen.CompShader) -and -not ($seen.WarpText -or $seen.CompText)) { $features.GeneratedLegacyShader++ }
    if ($seen.PerFrame) { $features.PerFrame++ }
    if ($seen.PerPixel) { $features.PerPixel++ }
    if ($seen.Sampler) { $features.Sampler++ }
    if ($seen.RandomSampler) { $features.RandomSampler++ }
    if ($seen.CustomShape) { $features.CustomShape++ }
    if ($seen.TexturedShape) { $features.TexturedShape++ }
    if ($seen.CustomWave) { $features.CustomWave++ }
    if ($seen.WarpText) { $features.WarpText++ }
    if ($seen.CompText) { $features.CompText++ }
}

$features.GetEnumerator() | ForEach-Object {
    [pscustomobject]@{
        Feature = $_.Key
        Presets = $_.Value
        Percent = if ($features.Total -gt 0) { [math]::Round(($_.Value / $features.Total) * 100.0, 2) } else { 0.0 }
    }
} | Format-Table -AutoSize

if ($ListShaderPresets -and $shaderPresets.Count -gt 0) {
    ""
    "Shader preset examples:"
    $shaderPresets | Select-Object -First 50
}
