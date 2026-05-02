# BlueprintLens Export Schema

Schema id: `blueprint-lens.v1`

The export is JSON-first. Optional Markdown output is generated from the same in-memory data and should be treated as a readable summary, not the source of truth.

## Root

- `schema`: schema id.
- `project`: Unreal project name.
- `path`: requested package path.
- `assetCount`: exported Blueprint count.
- `skippedUnsupportedAssetCount`: Blueprint-like assets that could not be loaded as `UBlueprint`.
- `loadErrorCount`: count of Blueprint-like load failures.
- `assets`: exported Blueprint assets.
- `errors`: load or export errors that did not stop the commandlet.
- `validation`: present only when `-Validate` is supplied.

## Asset

- `name`: Blueprint asset name.
- `objectPath`: full object path.
- `packageName`: package path.
- `assetClass`: Asset Registry class name.
- `blueprintClass`: loaded Blueprint object class.
- `parentClass`: Blueprint parent class path.
- `generatedClass`: generated class path.
- `variables`: member variables declared on the Blueprint.
- `components`: Simple Construction Script components.
- `interfaces`: implemented interfaces and their generated graphs.
- `graphs`: exported graphs.
- `summary`: asset-level counts aggregated from graphs.
- `analysis`: asset-level graph analysis aggregated from graphs.
- `dependencies`: Asset Registry package dependencies.

## Graph

- `name`: graph object name.
- `kind`: `ubergraph`, `function`, `macro`, `delegateSignature`, or `interface`.
- `schema`: graph schema class path.
- `nodes`: exported graph nodes.
- `summary`: node/link/writer summary counts.
- `analysis`: variable reads/writes, function calls, timer calls, material parameter writes, interface messages, and latent actions inferred from node classes/titles.

## Node

- `guid`: Unreal node GUID.
- `stableAlias`: human-friendly graph-scoped alias for patch authoring and review.
- `name`: UObject name.
- `class`: node class path.
- `title`: editor node title.
- `comment`: node comment.
- `x`, `y`: graph position.
- `pins`: exported pins.

`stableAlias` is intended to be readable and deterministic for one export, but `guid` remains the strongest identity when comparing two exports.

## Pin

- `id`: pin GUID.
- `name`: pin name.
- `direction`: `input`, `output`, or `unknown`.
- `type`: compact pin type string.
- `defaultValue`: optional default value.
- `defaultObject`: optional default object path.
- `links`: connected node/pin targets.

## Validation

When `-Validate` is supplied, BlueprintLens compiles each exported Blueprint and adds:

- `validation.assetCount`: number of compiled assets.
- `validation.errorCount`: number of assets whose Blueprint status is `error`.
- `validation.results`: per-asset result objects with `objectPath`, `success`, `status`, and `generatedClass`.
