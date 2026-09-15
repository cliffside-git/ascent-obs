#!/usr/bin/env bash
# Commit message format check. The format itself is documented in
# AGENTS.md.
#
# Three entry points, one validator:
#
#   check.sh --file <path>            one message file. This is what the
#                                     .githooks/commit-msg hook runs.
#   check.sh --range <base>..<head>   every non-merge commit in a range.
#                                     This is what CI runs on a PR.
#   check.sh --body <path>            a PR description. Sections only, no
#                                     subject-line rule.
#
# Exit 0 when everything passes, 1 with a per-commit report otherwise.
#
# Plain bash on purpose: it has to run as a git hook on Windows (Git for
# Windows ships bash) and on ubuntu runners, in repos that have no package
# manager. Keep it free of anything beyond bash + git + coreutils.

set -u

# ---- rules -----------------------------------------------------------------

# Subject: type(scope)!: summary. Scope optional, `!` marks a breaking change.
TYPES='feat|fix|perf|refactor|docs|test|build|ci|chore|style|release|revert'
SUBJECT_RE="^(${TYPES})(\([^)]+\))?!?: [^ ]"
SUBJECT_MAX=72

# Body sections, matched case-insensitively at the start of a line. Order in
# the message does not matter; presence does.
SECTIONS=("what" "why" "previous behavior" "blast radius" "other options considered" "testing")
# Sections that must carry real text. The rest must be present but may say
# "none" / "n/a" -- the author has to consciously claim there is nothing.
REQUIRE_TEXT=("what" "why" "testing")

# Git trailers (Co-Authored-By:, Signed-off-by:, Claude-Session:, ...) live at
# the bottom of the body and are not section content.
TRAILER_RE='^[A-Za-z][A-Za-z0-9-]*: '

# ---- helpers ---------------------------------------------------------------

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# Prints the section a line opens ("what", "why", ...), or nothing.
section_of() {
  local lc s; lc="$(lower "$1")"
  for s in "${SECTIONS[@]}"; do
    if [[ "$lc" == "$s:"* ]]; then printf '%s' "$s"; return; fi
  done
}

# Read a message from a file into MSG_LINES, dropping what git itself would
# drop before recording the commit (comment lines, everything after the
# scissors line) and, in body mode, single-line HTML comments left over
# from the PR template.
load_message() {
  local path="$1" mode="$2" comment_char line cr in_comment=0
  cr="$(printf '\r')"
  comment_char="$(git config --get core.commentChar 2>/dev/null || true)"
  if [ -z "$comment_char" ] || [ "$comment_char" = "auto" ]; then comment_char='#'; fi
  MSG_LINES=()
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$cr}"
    if [ "$mode" = "commit" ]; then
      case "$line" in
        "$comment_char"*' >8 '*) break ;;          # git commit -v scissors
        "$comment_char"*) continue ;;
      esac
    else
      # HTML comments from the PR template: a line that opens one is dropped,
      # and so is everything up to the line that closes it.
      if [ "$in_comment" = 1 ]; then
        [[ "$line" == *"-->"* ]] && in_comment=0
        continue
      fi
      if [[ "$line" =~ ^[[:space:]]*\<!-- ]]; then
        [[ "$line" == *"-->"* ]] || in_comment=1
        continue
      fi
    fi
    MSG_LINES+=("$line")
  done < "$path"
}

# Validate MSG_LINES. $1 is "commit" or "body". Fills ERRORS.
validate() {
  local mode="$1" i n line subject="" body_start=0
  ERRORS=()
  n=${#MSG_LINES[@]}

  # Subject = first non-blank line. Body = everything after it.
  for ((i = 0; i < n; i++)); do
    if [ -n "${MSG_LINES[$i]//[[:space:]]/}" ]; then
      subject="${MSG_LINES[$i]}"; body_start=$((i + 1)); break
    fi
  done

  if [ "$mode" = "commit" ]; then
    if [ -z "$subject" ]; then
      ERRORS+=("empty message"); return
    fi
    if ! [[ "$subject" =~ $SUBJECT_RE ]]; then
      ERRORS+=("subject must look like 'type(scope): summary' (types: ${TYPES//|/, })")
    fi
    if [ ${#subject} -gt $SUBJECT_MAX ]; then
      ERRORS+=("subject is ${#subject} chars, max ${SUBJECT_MAX}")
    fi
    if [ $body_start -lt $n ] && [ -n "${MSG_LINES[$body_start]//[[:space:]]/}" ]; then
      ERRORS+=("blank line required between subject and body")
    fi
    # Version bumps carry their context in the release notes.
    [[ "$(lower "$subject")" =~ ^release(\(|:) ]] && return
  else
    body_start=0
  fi

  # Drop the trailer block: trailing lines that are blank or `Key: value`.
  # A one-word section label ("Testing: ran it.") looks like a trailer too,
  # so lines that open a section are never treated as one.
  local end=$n
  while [ $end -gt $body_start ]; do
    line="${MSG_LINES[$((end - 1))]}"
    if [ -z "${line//[[:space:]]/}" ] ||
       { [[ "$line" =~ $TRAILER_RE ]] && [ -z "$(section_of "$line")" ]; }; then
      end=$((end - 1))
    else
      break
    fi
  done

  # Walk the body, bucketing non-blank lines under the current section.
  local -A content=() seen=()
  local current="" key rest s r
  for ((i = body_start; i < end; i++)); do
    line="${MSG_LINES[$i]}"
    key="$(section_of "$line")"
    if [ -n "$key" ]; then
      current="$key"; seen[$key]=1
      rest="${line#*:}"
      rest="${rest#"${rest%%[![:space:]]*}"}"
      [ -n "$rest" ] && content[$key]+="$rest "
      continue
    fi
    [ -z "${line//[[:space:]]/}" ] && continue
    # Prose before the first section is fine (a lead-in paragraph).
    [ -z "$current" ] && continue
    content[$current]+="$line "
  done

  for s in "${SECTIONS[@]}"; do
    if [ -z "${seen[$s]:-}" ]; then
      ERRORS+=("missing section '${s^}:'"); continue
    fi
    local text="${content[$s]:-}"
    text="${text%"${text##*[![:space:]]}"}"
    if [ -z "$text" ]; then
      ERRORS+=("section '${s^}:' is empty"); continue
    fi
    for r in "${REQUIRE_TEXT[@]}"; do
      if [ "$r" = "$s" ] && [[ "$(lower "$text")" =~ ^(none|n/a|na|-|tbd|todo)\.?$ ]]; then
        ERRORS+=("section '${s^}:' needs real content, not '${text}'")
      fi
    done
  done
}

report() {
  local label="$1"; shift
  printf '\nFAIL %s\n' "$label"
  local e; for e in "$@"; do printf '  - %s\n' "$e"; done
}

# ---- entry points ----------------------------------------------------------

check_file() {
  local path="$1" git_dir subject="" l
  load_message "$path" commit
  for l in "${MSG_LINES[@]}"; do
    [ -n "${l//[[:space:]]/}" ] && { subject="$l"; break; }
  done
  git_dir="$(git rev-parse --git-dir 2>/dev/null || true)"
  # Things git generates itself, or that get squashed away before review.
  if [ -n "$git_dir" ] && [ -e "$git_dir/MERGE_HEAD" ]; then exit 0; fi
  case "$subject" in
    "Merge "*|"Revert \""*|"fixup! "*|"squash! "*|"amend! "*) exit 0 ;;
  esac
  validate commit
  if [ ${#ERRORS[@]} -gt 0 ]; then
    report "commit message" "${ERRORS[@]}" >&2
    printf '\nFormat: AGENTS.md  (template: .gitmessage)\n' >&2
    printf 'Your message is saved in %s -- fix it with: git commit -eF %s\n' "$path" "$path" >&2
    exit 1
  fi
}

check_range() {
  local range="$1" sha author subject failed=0 total=0
  tmp="$(mktemp)"; trap 'rm -f "${tmp:-}"' EXIT
  for sha in $(git rev-list --no-merges --reverse "$range"); do
    total=$((total + 1))
    author="$(git show -s --format='%an <%ae>' "$sha")"
    subject="$(git show -s --format='%s' "$sha")"
    # Bots (dependabot, renovate, github-actions) write their own format.
    [[ "$author" == *"[bot]"* ]] && continue
    [[ "$subject" == "Revert \""* ]] && continue
    git show -s --format='%B' "$sha" > "$tmp"
    load_message "$tmp" commit
    validate commit
    if [ ${#ERRORS[@]} -gt 0 ]; then
      failed=$((failed + 1))
      report "${sha:0:10} ${subject}" "${ERRORS[@]}"
    fi
  done
  printf '\n%d commit(s) checked, %d failed\n' "$total" "$failed"
  if [ $failed -gt 0 ]; then
    printf 'Format: AGENTS.md. Fix with git rebase -i and reword.\n'
    exit 1
  fi
}

check_body() {
  load_message "$1" body
  validate body
  if [ ${#ERRORS[@]} -gt 0 ]; then
    report "PR description" "${ERRORS[@]}"
    printf '\nFormat: .github/pull_request_template.md\n'
    exit 1
  fi
  printf 'PR description ok\n'
}

case "${1:-}" in
  --file)  check_file "$2" ;;
  --range) check_range "$2" ;;
  --body)  check_body "$2" ;;
  *) sed -n '2,15p' "$0" >&2; exit 2 ;;
esac
