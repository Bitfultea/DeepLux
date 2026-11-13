# Documentation Index

Use this page to find the current user, developer, architecture, and acceptance documents. The live project status is maintained in [STATUS](STATUS.md); dated reports and design records remain historical evidence and do not override it.

## Use DeepLux

- [User manual](user-manual.md): workflows, parameters, results, Agent, and SAM.
- [Quick start](quick-start.md): build and run a first workflow.
- [Plugin manual](plugins.md): plugin inputs, configuration, execution, and result checks.
- [CLI reference](cli.md): command-line project and workflow operations.
- [Troubleshooting](troubleshooting.md): common build, plugin, and runtime failures.
- [Windows build and deployment](windows-build.md): MSVC build, tests, and packaging.

## Develop DeepLux

- [Contributing](../CONTRIBUTING.md): local checks and pull-request requirements.
- [Plugin ABI contract](PLUGIN_ABI.md): compatibility checks and plugin rebuild requirements.
- [Architecture](architecture.md): module boundaries and runtime responsibilities.
- [Agent capability plan](agent-capability-improvement-plan.md): Agent scope and acceptance matrices.
- [Reusable engineering skills](skills/README.md): repository workflows and validation practices.

## Design and Acceptance

- [Background execution acceptance](reports/background-execution-acceptance.md): implementation evidence, deviations, and unverified gates.
- [Superpowers specifications](superpowers/specs/): reviewed designs.
- [Superpowers plans](superpowers/plans/): implementation plans and review evidence.
- [Baseline records](baseline/): dated inventories and historical acceptance snapshots.

## Status Rules

- `docs/STATUS.md` is the only live summary of delivery state. It must identify its date, commit, verification command, and evidence report.
- A dated acceptance report records only the environment and source revision stated in that report. A newer status page may supersede its project-state summary, but not its historical test evidence.
- Design documents describe intent unless their status explicitly says implemented; implementation status must link to current code or acceptance evidence.
- Changelog entries remain under `Unreleased` until a release is explicitly published.
