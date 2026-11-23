# Workflows

A workflow that runs in this repository is the file on the trusted branch. A
fork's copy must not replace that file, and a movable action tag must not
replace a step.

Read the [workflow trust skill](../.agents/skills/neverd-workflow-trust/SKILL.md)
before editing these files. Keep its checks in
`scripts/tests/test_ci_configuration.py` passing. Do not loosen those checks
to accept a fork-controlled trigger or a movable `uses:` ref.
