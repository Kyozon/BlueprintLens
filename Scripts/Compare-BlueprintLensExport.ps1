param(
    [Parameter(Mandatory = $true)]
    [string]$Base,

    [Parameter(Mandatory = $true)]
    [string]$Head,

    [string]$Out = ""
)

$ErrorActionPreference = "Stop"

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "File not found: $Path"
    }

    Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function New-AssetMap($Export) {
    $map = @{}
    foreach ($asset in @($Export.assets)) {
        if ($null -ne $asset.packageName -and -not [string]::IsNullOrWhiteSpace($asset.packageName)) {
            $map[$asset.packageName] = $asset
        }
    }
    $map
}

function New-GraphMap($Asset) {
    $map = @{}
    foreach ($graph in @($Asset.graphs)) {
        $key = "$($graph.kind):$($graph.name)"
        $map[$key] = $graph
    }
    $map
}

function New-NodeMap($Graph) {
    $map = @{}
    foreach ($node in @($Graph.nodes)) {
        $key = if (-not [string]::IsNullOrWhiteSpace($node.guid)) { $node.guid } else { $node.stableAlias }
        if (-not [string]::IsNullOrWhiteSpace($key)) {
            $map[$key] = $node
        }
    }
    $map
}

function Add-Line([System.Collections.Generic.List[string]]$Lines, [string]$Line = "") {
    [void]$Lines.Add($Line)
}

$baseJson = Read-JsonFile $Base
$headJson = Read-JsonFile $Head
$baseAssets = New-AssetMap $baseJson
$headAssets = New-AssetMap $headJson
$lines = [System.Collections.Generic.List[string]]::new()

Add-Line $lines "# BlueprintLens Diff"
Add-Line $lines
Add-Line $lines "- Base: ``$Base``"
Add-Line $lines "- Head: ``$Head``"
Add-Line $lines

$allAssetKeys = @($baseAssets.Keys + $headAssets.Keys) | Sort-Object -Unique
foreach ($assetKey in $allAssetKeys) {
    $baseAsset = $baseAssets[$assetKey]
    $headAsset = $headAssets[$assetKey]

    if ($null -eq $baseAsset) {
        Add-Line $lines "## Added asset ``$assetKey``"
        Add-Line $lines
        continue
    }

    if ($null -eq $headAsset) {
        Add-Line $lines "## Removed asset ``$assetKey``"
        Add-Line $lines
        continue
    }

    $assetLines = [System.Collections.Generic.List[string]]::new()
    if ($baseAsset.parentClass -ne $headAsset.parentClass) {
        Add-Line $assetLines "- Parent changed: ``$($baseAsset.parentClass)`` -> ``$($headAsset.parentClass)``"
    }

    $baseGraphs = New-GraphMap $baseAsset
    $headGraphs = New-GraphMap $headAsset
    $allGraphKeys = @($baseGraphs.Keys + $headGraphs.Keys) | Sort-Object -Unique

    foreach ($graphKey in $allGraphKeys) {
        $baseGraph = $baseGraphs[$graphKey]
        $headGraph = $headGraphs[$graphKey]
        if ($null -eq $baseGraph) {
            Add-Line $assetLines "- Added graph ``$graphKey``"
            continue
        }
        if ($null -eq $headGraph) {
            Add-Line $assetLines "- Removed graph ``$graphKey``"
            continue
        }

        $baseNodes = New-NodeMap $baseGraph
        $headNodes = New-NodeMap $headGraph
        $allNodeKeys = @($baseNodes.Keys + $headNodes.Keys) | Sort-Object -Unique
        foreach ($nodeKey in $allNodeKeys) {
            $baseNode = $baseNodes[$nodeKey]
            $headNode = $headNodes[$nodeKey]
            if ($null -eq $baseNode) {
                Add-Line $assetLines "- Added node in ``$graphKey``: ``$($headNode.stableAlias)`` ``$($headNode.title)``"
                continue
            }
            if ($null -eq $headNode) {
                Add-Line $assetLines "- Removed node in ``$graphKey``: ``$($baseNode.stableAlias)`` ``$($baseNode.title)``"
                continue
            }
            if ($baseNode.title -ne $headNode.title) {
                Add-Line $assetLines "- Node title changed in ``$graphKey``: ``$($baseNode.title)`` -> ``$($headNode.title)``"
            }
            if (@($baseNode.pins).Count -ne @($headNode.pins).Count) {
                Add-Line $assetLines "- Pin count changed in ``$graphKey`` for ``$($headNode.stableAlias)``: $(@($baseNode.pins).Count) -> $(@($headNode.pins).Count)"
            }
        }
    }

    if ($assetLines.Count -gt 0) {
        Add-Line $lines "## Changed asset ``$assetKey``"
        foreach ($line in $assetLines) {
            Add-Line $lines $line
        }
        Add-Line $lines
    }
}

if ($lines.Count -le 5) {
    Add-Line $lines "No BlueprintLens asset, graph, or node changes detected."
}

$text = $lines -join [Environment]::NewLine
if ([string]::IsNullOrWhiteSpace($Out)) {
    $text
} else {
    $parent = Split-Path -Parent $Out
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    Set-Content -LiteralPath $Out -Value $text
    $Out
}
