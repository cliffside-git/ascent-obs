#!/usr/bin/env bash
# One-time setup per clone: turn on the commit-msg hook and the message
# template so the commit format is enforced (and pre-filled) locally.
#
#   scripts/commit-format/install.sh
set -eu
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath .githooks
git config commit.template .gitmessage
printf 'commit-format: hook and template enabled in %s\n' "$(pwd)"
printf 'Try it:  git commit --allow-empty -m "bad message"   (should be rejected)\n'
