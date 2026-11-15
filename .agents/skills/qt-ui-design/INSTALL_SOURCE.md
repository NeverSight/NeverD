# Installation source

- Repository: https://github.com/TheQtCompanyRnD/agent-skills
- Upstream path: `skills/qt-ui-design`
- Commit: `71d6c10da78b9a764468ae11c86ab3bc4ca4921f`
- Installed: 2026-09-09
- Method: Codex skill-installer, Git sparse checkout

The upstream skill files were originally installed without modification. NeverD
now adds a desktop design profile in `references/neverd-desktop.md` and Windows
native-frame notes in `references/windows.md`, and routes to them from
`SKILL.md`. The Windows notes were added on 2026-10-26 from NeverD PR #1805.
Section 1.6 of `SKILL.md` and the crispness item in its
audit checklist are also NeverD additions, drawn from an audit of NeverD's
icons. The states item in the same checklist is a NeverD addition drawn from a
review of NeverD's control states on 2026-11-09; the profile's "Control
states" section holds its rules. The languages item beside it comes from the
same day's survey of every dialog in every bundled language, and the
profile's "Languages and direction" section holds its rules. Section 1.7 of
`SKILL.md` and the disabled-look, command-load, Return-key and seams items in
that checklist are NeverD additions from the review of Binary comparison on
2026-11-12, measured against the sources of Windows 11's, Fluent 2's and
Visual Studio Code's control states; the profile's "Control states" and
"Judging a look" sections hold NeverD's values and the user's verdicts.
Section 1.8 and the every-window, default-at-open and one-drawing items come
from the audit of every NeverD window on the same day; the profile's
"Auditing every window" section holds what it found, what it left, what
keeps its fixes and their checks true on Windows and macOS, and the user's
correction that every window keeps one accent, which section 1.7 now states
as a look apart from the Return key. The profile's "Style at a glance"
section, added on 2026-11-14, gathers NeverD's color roles, type and sizes
and the rules every window keeps on one page, each pointing to the section
that holds its reasons. The
upstream copyright, license, and general guidance are retained. This
file records installation provenance and those local additions. The local
`references/macos-qt.md` guide records Cocoa menu defaults, precision input,
platform font sizes, docking style inheritance, Retina capture sampling and
native validation constraints reproduced in NeverD; `SKILL.md` routes macOS
work to it.
