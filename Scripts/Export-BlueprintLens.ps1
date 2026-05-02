param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [string]$UnrealEditorCmd = "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe",

    [string]$Path = "/Game",

    [string]$Asset = "",

    [string]$OutDir = "",

    [switch]$NoMarkdown
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Project)) {
    throw "Project file not found: $Project"
}

if (-not (Test-Path -LiteralPath $UnrealEditorCmd)) {
    throw "UnrealEditor-Cmd.exe not found: $UnrealEditorCmd"
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $projectRoot = Split-Path -Parent $Project
    $OutDir = Join-Path $projectRoot "Saved\BlueprintLens"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$safeName = if ([string]::IsNullOrWhiteSpace($Asset)) {
    $Path.Trim("/").Replace("/", "_")
} else {
    $Asset.Trim("/").Replace("/", "_").Replace(".", "_")
}

if ([string]::IsNullOrWhiteSpace($safeName)) {
    $safeName = "Game"
}

$jsonOut = Join-Path $OutDir "$safeName.json"
$markdownOut = Join-Path $OutDir "$safeName.md"

$arguments = @(
    "`"$Project`"",
    "-run=BlueprintLens",
    "-Out=`"$jsonOut`"",
    "-unattended",
    "-nop4",
    "-nosplash"
)

if ([string]::IsNullOrWhiteSpace($Asset)) {
    $arguments += "-Path=$Path"
} else {
    $arguments += "-Asset=$Asset"
}

if (-not $NoMarkdown) {
    $arguments += "-MarkdownOut=`"$markdownOut`""
}

& $UnrealEditorCmd @arguments
if ($LASTEXITCODE -ne 0) {
    throw "BlueprintLens export failed with exit code $LASTEXITCODE"
}

[pscustomobject]@{
    Json = $jsonOut
    Markdown = if ($NoMarkdown) { $null } else { $markdownOut }
}
