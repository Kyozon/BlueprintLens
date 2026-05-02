# Changelog

## Unreleased

- Added optional `-MarkdownOut=<path>` support to `BlueprintLens` for compact agent-readable Markdown summaries beside the JSON export.
- Added stable node aliases, graph summaries, asset summaries, interface exports, and writer-oriented analysis fields to Blueprint exports.
- Added optional `-Validate` support to compile exported Blueprints and record validation results in JSON.
- Added `BlueprintLensApply` operations for `add_custom_event`, `add_branch`, and `add_comment`.
- Added `Scripts\Export-BlueprintLens.ps1` for CI-friendly export runs.
- Added `Scripts\Compare-BlueprintLensExport.ps1` for Markdown diffs between BlueprintLens JSON exports.
- Added export schema documentation under `Docs\blueprint-lens-schema.md`.
- Added small JSON fixtures under `Tests\Fixtures` for diff-script smoke checks.

## 0.1.0

- Added `BlueprintLens` export commandlet.
- Exports Blueprint identity, variables, components, graphs, nodes, pins, links, and dependencies.
- Added `BlueprintLensApply` experimental patch commandlet.
- Supports basic Blueprint creation, variables, function graphs, function-call nodes, variable get/set nodes, pin defaults, pin connections, pin disconnections, node deletion, and marking Blueprints modified.
- Validated against a template C++ project and a Lyra-derived Speed of Light project.
