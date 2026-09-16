# Commit messages

Every commit and every PR description follows this format. CI hard-fails
anything that doesn't (`.github/workflows/commit_format.yml`), and the local
`commit-msg` hook rejects it before it is even committed. The validator is `scripts/commit-format/check.sh`.

```
type(scope): summary                       <- <=72 chars, blank line after

What: what changed, concretely.
Why: the problem this solves / the reason for the change.
Previous behavior: what the code did before this change.
Blast radius: what this can break — surfaces, services, data, clients.
Other options considered: alternatives weighed and why they lost.
Testing: what you ran and what you did by hand, on what.
```

- `type` is one of `feat fix perf refactor docs test build ci chore style
  release revert`. `scope` is optional; `!` before the colon marks a
  breaking change (`feat(api)!: …`).
- All six sections are required, matched case-insensitively at the start
  of a line, in any order. Content may follow on the same line or on the
  lines below.
- `What:`, `Why:` and `Testing:` must have real text. `Previous behavior:`,
  `Blast radius:` and `Other options considered:` must be present but may
  say `none` — the point is to make the author claim it consciously, not
  to pad. `Previous behavior: none` means the behavior is brand new.
- `Testing:` is the evidence: the test command you ran, the manual steps
  you took and on what machine/build, or why it can't be tested. "ran
  tests" is not evidence; "go test ./services/api/... passes, recorded a
  Fortnite match on the slow laptop" is. Frontend / desktop UI changes
  attach screenshots or a recording to the PR description's `Testing:`.
- Git trailers (`Co-Authored-By:`, `Claude-Session:`, …) go after the
  sections and are ignored by the check.
- Exempt: merge commits, `Revert "…"` commits, bot-authored commits, and
  `release:` commits (body optional — the context is in the release notes).
- PR descriptions use the same six sections (no subject-line rule). The PR
  template pre-fills them.

One-time setup per clone, so the hook and template are active:

```
scripts/commit-format/install.sh
```

Changing this rule, the validator, the hook, or any workflow under
`.github/` takes a PR approved by a code owner (see `.github/CODEOWNERS`);
branch protection applies to admins too, so there is no bypass. Do not
try to weaken the check inside a feature PR — it will be blocked.

Agents writing commits: put the sections in the `-m` body (or a `-F` file);
don't rely on the template. A commit rejected by the hook is not lost — the
message is left in `.git/COMMIT_EDITMSG`; fix and `git commit -eF` it.
