# BlueprintLens

BlueprintLens is an Unreal Engine editor plugin that exports Blueprint assets to agent-readable JSON and provides an experimental intent-patch commandlet for controlled Blueprint edits.

It was built to help AI coding agents inspect Blueprint-heavy projects without relying on screenshots or manually pasted node snippets.

## Why It Exists

Raw `.uasset` files are binary and engine-version-sensitive. Instead of trying to parse them directly, BlueprintLens runs inside Unreal, loads Blueprint assets through the editor/runtime APIs, and exports semantic information:

- Blueprint identity and parent/generated classes
- member variables and defaults
- components from the Simple Construction Script
- graphs
- nodes
- pins
- pin links
- stable node aliases
- graph and asset summaries
- writer-oriented analysis for variable writes, timers, material parameter writes, interface messages, and latent actions
- dependencies

The exporter can also write a compact Markdown summary from the same scan for quick model review.

## Install

Copy this folder into your project:

```text
YourProject/
  Plugins/
    BlueprintLens/
      BlueprintLens.uplugin
```

Rebuild the editor target.

## Export Blueprints

Scan `/Game`:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\YourProject.uproject" `
  -run=BlueprintLens `
  -Path=/Game `
  -Out="C:\Path\To\YourProject\Saved\BlueprintLens\game.json" `
  -unattended -nop4 -nosplash
```

Scan one asset:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\YourProject.uproject" `
  -run=BlueprintLens `
  -Asset=/Game/MyFolder/BP_MyActor.BP_MyActor `
  -Out="C:\Path\To\YourProject\Saved\BlueprintLens\BP_MyActor.json" `
  -unattended -nop4 -nosplash
```

Export JSON and a compact Markdown summary together:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\YourProject.uproject" `
  -run=BlueprintLens `
  -Path=/Game `
  -Out="C:\Path\To\YourProject\Saved\BlueprintLens\game.json" `
  -MarkdownOut="C:\Path\To\YourProject\Saved\BlueprintLens\game.md" `
  -unattended -nop4 -nosplash
```

Compile each exported Blueprint and include validation results in the JSON:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\YourProject.uproject" `
  -run=BlueprintLens `
  -Path=/Game `
  -Out="C:\Path\To\YourProject\Saved\BlueprintLens\game.json" `
  -Validate `
  -unattended -nop4 -nosplash
```

Use the CI-friendly wrapper:

```powershell
.\Scripts\Export-BlueprintLens.ps1 `
  -Project "C:\Path\To\YourProject.uproject" `
  -UnrealEditorCmd "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  -Path /Game
```

Compare two exports:

```powershell
.\Scripts\Compare-BlueprintLensExport.ps1 `
  -Base "Saved\BlueprintLens\before.json" `
  -Head "Saved\BlueprintLens\after.json" `
  -Out "Saved\BlueprintLens\diff.md"
```

See `Docs\blueprint-lens-schema.md` for the export shape.

## Apply Experimental Patches

BlueprintLensApply accepts an intent patch and applies it through Unreal editor APIs:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\YourProject.uproject" `
  -run=BlueprintLensApply `
  -Patch="C:\Path\To\patch.json" `
  -unattended -nop4 -nosplash
```

Patch skeleton:

```json
{
  "schema": "blueprint-lens-patch.v1",
  "operations": []
}
```

Supported early operations:

- `create_blueprint`
- `add_variable`
- `add_function_graph`
- `alias_node`
- `add_variable_get`
- `add_variable_set`
- `add_call_function`
- `add_custom_event`
- `add_branch`
- `add_comment`
- `set_pin_default`
- `connect_pins`
- `disconnect_pin`
- `delete_node`
- `mark_blueprint_modified`
- `refresh_blueprint_nodes`

## Limitations

BlueprintLens is a prototype:

- It does not perfectly reconstruct the editor visual graph.
- It does not export every hidden compiler-generated detail.
- It does not fully model inherited Blueprint state.
- It does not convert JSON back into `.uasset` files.
- Write support is experimental and should be verified in-editor.
- You must run the Unreal version that owns the project.

## Recommended Use

Use BlueprintLens to:

- scan projects before AI work starts
- inspect Blueprint structure
- search for stale nodes or conflicting writers
- review graph-level changes with export diffs
- produce reviewable context for Copilot/Codex
- author small controlled patches through Unreal APIs

For production gameplay systems that are latency-sensitive or stateful, prefer moving authoritative logic to C++ and leaving Blueprints as wrappers/presentation.
