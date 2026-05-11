param(
    [Parameter(Mandatory = $true)]
    [string] $PresetDirectory,

    [int] $SampleLimit = 0,

    [switch] $ListShaderPresets,

    [switch] $ListDx12Hazards
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
$dx12Hazards = [ordered]@{
    Tex2D = 0
    Tex3D = 0
    LegacySamplerDeclaration = 0
    LegacyBuiltInSamplerDeclaration = 0
    ExplicitTextureSamplerDeclaration = 0
    PossiblyScalarTex2DCoordinate = 0
}
$dx12HazardExamples = @{}
foreach ($key in $dx12Hazards.Keys) {
    $dx12HazardExamples[$key] = New-Object System.Collections.Generic.List[string]
}

function Add-Dx12Hazard([string] $Name, [string] $File) {
    if (-not $seen[$Name]) {
        $seen[$Name] = $true
        $dx12Hazards[$Name]++
        if ($ListDx12Hazards -and $dx12HazardExamples[$Name].Count -lt 50) {
            $dx12HazardExamples[$Name].Add($File)
        }
    }
}

function Get-SamplerRootName([string] $SamplerName) {
    $root = $SamplerName
    if ($root.StartsWith("sampler_")) {
        $root = $root.Substring(8)
    }
    if ($root -match '^(fw|fc|pw|pc|wf|cf|wp|cp)_(.+)$') {
        $root = $Matches[2]
    }
    return $root
}

function Test-BuiltInSamplerRoot([string] $RootName) {
    return $RootName -eq "" -or
        $RootName -eq "main" -or
        $RootName -eq "fc_main" -or
        $RootName -eq "pc_main" -or
        $RootName -eq "fw_main" -or
        $RootName -eq "pw_main" -or
        $RootName.StartsWith("blur") -or
        $RootName.StartsWith("noise")
}

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

        if ($line -match 'tex2D|tex2d|tex2Dbias|tex2Dlod') {
            Add-Dx12Hazard "Tex2D" $file
        }
        if ($line -match 'tex3D|tex3d|tex3Dbias|tex3Dlod') {
            Add-Dx12Hazard "Tex3D" $file
        }
        if ($line -match 'tex2D\s*\([^,]+,\s*[A-Za-z_][A-Za-z0-9_\.]*\s*[\*/+\-]') {
            Add-Dx12Hazard "PossiblyScalarTex2DCoordinate" $file
        }
        if ($line -match '^\s*(?:warp|comp)_\d+=`\s*sampler(?:2D)?\s+(sampler_[A-Za-z0-9_\.]+)') {
            Add-Dx12Hazard "LegacySamplerDeclaration" $file
            $root = Get-SamplerRootName $Matches[1]
            if (Test-BuiltInSamplerRoot $root) {
                Add-Dx12Hazard "LegacyBuiltInSamplerDeclaration" $file
            } else {
                Add-Dx12Hazard "ExplicitTextureSamplerDeclaration" $file
            }
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

""
"DX12 shader compatibility signals:"
$dx12Hazards.GetEnumerator() | ForEach-Object {
    [pscustomobject]@{
        Signal = $_.Key
        Presets = $_.Value
        Percent = if ($features.Total -gt 0) { [math]::Round(($_.Value / $features.Total) * 100.0, 2) } else { 0.0 }
    }
} | Format-Table -AutoSize

if ($ListDx12Hazards) {
    foreach ($key in $dx12Hazards.Keys) {
        if ($dx12HazardExamples[$key].Count -gt 0) {
            ""
            "${key} examples:"
            $dx12HazardExamples[$key] | Select-Object -First 50
        }
    }
}
