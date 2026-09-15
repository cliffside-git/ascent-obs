#!/usr/bin/env bash
# Self-test for check.sh. Runs in CI before the real check so a broken
# validator fails loudly instead of silently passing every commit.
#
#   scripts/commit-format/test.sh
set -u
cd "$(dirname "$0")"
CHECK=./check.sh
pass=0 fail=0
tmp="$(mktemp)"; trap 'rm -f "$tmp"' EXIT

# expect <ok|bad> <label> <mode> <<'MSG' ... MSG
expect() {
  local want="$1" label="$2" mode="$3" got
  cat > "$tmp"
  if "$CHECK" "$mode" "$tmp" >/dev/null 2>&1; then got=ok; else got=bad; fi
  if [ "$got" = "$want" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'FAIL [%s] expected %s, got %s\n' "$label" "$want" "$got"
    "$CHECK" "$mode" "$tmp" 2>&1 | sed 's/^/    /'
  fi
}

expect ok "full message" --file <<'MSG'
fix(recorder): wait past READY so game capture is not abandoned

What: the recorder now waits for the hook to report a frame before
giving up on game capture.

Why: on slow launches READY arrives before the first frame and we fell
back to display capture.

Previous behavior: READY alone was treated as "hooked", so a slow first
frame meant an immediate fallback to display capture.

Blast radius: desktop recorder start path only.

Other options considered: a fixed 5s sleep; rejected, still racy on
very slow machines.

Testing: launched Fortnite cold 5x on the slow laptop, game capture
engaged every time; recorder unit tests pass.
MSG

expect ok "same-line sections, mixed case, trailers" --file <<'MSG'
feat(api)!: drop the v1 clips endpoint

WHAT: removes /v1/clips.
why: nothing has called it since 0.4.
Previous Behavior: /v1/clips returned the same payload as /v2/clips.
Blast Radius: none
Other Options Considered: none
TESTING: go test ./... and hit /v1/clips on staging, got 404.

Co-Authored-By: Someone <x@example.com>
Claude-Session: https://example.com/s/1
MSG

expect ok "lead-in prose before sections, none for previous behavior" --file <<'MSG'
feat(overlay): add an input overlay source

Routine dependency bump.

What: a new OBS source that draws keypresses.
Why: viewers asked for it.
Previous behavior: none
Blast radius: new source only; nothing existing changes.
Other options considered: n/a
Testing: cargo test passes; ran the desktop app and checked the overlay.
MSG

expect ok "release commits skip body" --file <<'MSG'
release: desktop 0.5.8 — per-game opt-out, AV1, bigger storage
MSG

expect ok "merge commit skipped" --file <<'MSG'
Merge branch 'main' into kyle/thing
MSG

expect ok "revert skipped" --file <<'MSG'
Revert "feat(api): thing"

This reverts commit abc123.
MSG

expect ok "fixup skipped locally" --file <<'MSG'
fixup! feat(api): thing
MSG

expect ok "comment lines ignored" --file <<'MSG'
docs: explain the golden file
# Please enter the commit message for your changes.

What: adds a README next to the golden.
Why: people kept regenerating it.
Previous behavior: no README; the file looked safe to regenerate.
# On branch main
Blast radius: none
Other options considered: none
Testing: docs only; rendered the markdown locally.
MSG

expect ok "CRLF line endings" --file < <(printf 'ci: thing\r\n\r\nWhat: x.\r\nWhy: y.\r\nPrevious behavior: z.\r\nBlast radius: none\r\nOther options considered: none\r\nTesting: ran it.\r\n')

expect bad "no body" --file <<'MSG'
fix(recorder): wait past READY
MSG

expect bad "bad subject type" --file <<'MSG'
Update release-notes.md

What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "subject too long" --file <<'MSG'
fix(recorder): this subject line is far too long and keeps going and going past seventy-two

What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "no blank line after subject" --file <<'MSG'
fix(recorder): thing
What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "missing section" --file <<'MSG'
fix(recorder): thing

What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Testing: ran it.
MSG

expect bad "missing previous behavior" --file <<'MSG'
fix(recorder): thing

What: x.
Why: y.
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "missing testing section" --file <<'MSG'
fix(recorder): thing

What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
MSG

expect bad "empty required section" --file <<'MSG'
fix(recorder): thing

What:
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "'none' in a required section" --file <<'MSG'
fix(recorder): thing

What: x.
Why: none
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "'none' in testing" --file <<'MSG'
fix(recorder): thing

What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing: none
MSG

expect bad "empty optional section" --file <<'MSG'
fix(recorder): thing

What: x.
Why: y.
Previous behavior:
Blast radius: none
Other options considered: none
Testing: ran it.
MSG

expect bad "trailer only, no section content" --file <<'MSG'
fix(recorder): thing

What: x.
Why: y.
Previous behavior: z.
Blast radius: none
Other options considered: none
Testing:
Co-Authored-By: Someone <x@example.com>
MSG

expect bad "release subject still needs a valid subject" --file <<'MSG'
Release 0.5.8
MSG

expect ok "PR body with template comments" --body <<'MSG'
What:
<!-- what changed -->
Adds the commit format check.
Why:
<!-- why -->
Commit messages were inconsistent.
Previous behavior:
<!-- before -->
Anything went.
Blast radius: CI only.
Other options considered: commitlint; rejected, needs node in every repo.
Testing:
<!-- how you tested it; screenshots for frontend changes -->
Self-test passes; opened a PR with a bad commit and watched it fail.
MSG

expect bad "PR body with only template" --body <<'MSG'
What:
<!-- what changed -->
Why:
<!-- why -->
Previous behavior:
<!-- before -->
Blast radius:
Other options considered:
Testing:
<!-- how you tested it; screenshots for frontend changes.
     Second comment line. -->
MSG

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
