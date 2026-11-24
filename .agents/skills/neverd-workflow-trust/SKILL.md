---
name: neverd-workflow-trust
description: >
  Keep NeverD GitHub workflows executing the trusted repository definition
  so a fork cannot replace the workflow file or the actions it runs. Use
  when editing .github/workflows, action pins, pull_request triggers,
  reusable workflows, or when the user mentions a workflow fork, an action
  tag, a commit SHA pin, or /neverd-workflow-trust.
---

# Workflow trust

The workflow file executed for NeverSight/NeverD is the file on the trusted
branch. A fork's `.github/workflows` copy is not that file. A tag or branch
on a third-party action is not a fixed action: the owner can move it onto
another commit, which is how a rewritten tag replaces the step.

Two tests in `scripts/tests/test_ci_configuration.py` own the check:
`test_third_party_actions_are_pinned_to_commit_shas` and
`test_workflows_keep_fork_controlled_definitions_out`. Read the current
workflow files before changing them. Run both tests before finishing.

## Triggers

These events load the workflow from the pull request merge commit. A fork
controls that commit, so GitHub would execute the fork's workflow file:

- `pull_request`
- `pull_request_review`
- `pull_request_review_comment`

Do not add them. Mentioning `github.event.pull_request` inside a
`workflow_dispatch` concurrency group is not one of these triggers.

`pull_request_target` and `workflow_run` load the workflow from the default
branch. Keep that definition:

- `pull_request_target` may pass the pull request number, head ref, and head
  repository to `gh` as environment values. It must not check out a
  repository, the pull request head, or the merge ref, and it must not run a
  script taken from the pull request. `cancel-closed-pr-runs.yml` is the
  existing shape.
- `workflow_run` checks out `github.sha` or a commit the trusted trigger job
  has already bound, with `persist-credentials: false`. It must not check out
  `github.event.workflow_run.head_sha` or the triggering head repository.
  Cloning a separate pinned upstream project for a qualification job is not a
  checkout of the untrusted workflow head.

`workflow_dispatch` and `workflow_call` run the workflow the caller selected
in this repository. `push` runs the pushed commit, so only a writer of this
repository can change the file that runs.

## Actions and reusable workflows

A third-party `uses:` value is `owner/repo@` plus the full 40-character
commit SHA already used for that action in `.github/`. Copy that SHA and its
version comment. A tag, branch, or major version is movable. A fork of the
action is a different `owner/repo`; do not point a step at one.

A reusable workflow in this repository is
`uses: ./.github/workflows/<file>.yml` with no `@ref`. The caller and the
callee are then the same commit. A reusable workflow in another repository
uses a 40-character SHA.

A checkout that does not push sets `persist-credentials: false`, so the job
token is not written into the workspace git config.

## Finish

```bash
python3 -m unittest \
  scripts.tests.test_ci_configuration.CiConfigurationTests.test_third_party_actions_are_pinned_to_commit_shas \
  scripts.tests.test_ci_configuration.CiConfigurationTests.test_workflows_keep_fork_controlled_definitions_out
```

A failure is a workflow that a fork can replace, or a test that no longer
rejects that shape. Repair the workflow. Do not weaken the test to allow the
trigger or the movable ref.
