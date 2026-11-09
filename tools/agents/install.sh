#!/bin/sh
# Install the status-display hooks for Claude Code and/or Codex on this computer.
#
#   ./install.sh                       interactive: asks for the cube's address and API token
#   ./install.sh --url http://192.168.1.50 [--claude] [--codex]     (token from $CUBE_TOKEN or a prompt)
#   ./install.sh --uninstall           remove the hooks and the saved settings
#   ./install.sh --dry-run             show what would change, change nothing
#
# What it does (all idempotent, your other hooks are kept):
#   1. saves the address and token in ~/.config/hellocubic/env (mode 600), never in the agents' config files
#   2. copies cube-hook.sh to ~/.local/bin
#   3. merges the hook entries into ~/.claude/settings.json and/or ~/.codex/hooks.json (a .bak copy is kept once)
#   4. checks that the cube answers
# Needs curl and jq.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
bin_dir=${HOME}/.local/bin
conf_dir=${XDG_CONFIG_HOME:-$HOME/.config}/hellocubic
conf=$conf_dir/env
claude_file=${HOME}/.claude/settings.json
codex_file=${HOME}/.codex/hooks.json

url=${CUBE_URL:-}
want_claude=0
want_codex=0
uninstall=0
dry=0

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
run() { if [ "$dry" -eq 1 ]; then say "[dry-run] $*"; else "$@"; fi; }

while [ $# -gt 0 ]; do
  case $1 in
    --url) url=$2; shift ;;
    --claude) want_claude=1 ;;
    --codex) want_codex=1 ;;
    --uninstall) uninstall=1 ;;
    --dry-run) dry=1 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
  shift
done

command -v jq >/dev/null 2>&1 || die "jq is required (apt install jq / brew install jq)"
command -v curl >/dev/null 2>&1 || die "curl is required"

if [ "$want_claude" -eq 0 ] && [ "$want_codex" -eq 0 ]; then
  [ -d "$HOME/.claude" ] && want_claude=1
  [ -d "$HOME/.codex" ] && want_codex=1
  if [ "$want_claude" -eq 0 ] && [ "$want_codex" -eq 0 ]; then
    die "neither ~/.claude nor ~/.codex exists: run an agent once, or pass --claude / --codex"
  fi
fi

# merge <template> <target>: add the template's hook groups to the target, replacing earlier cube-hook entries
merge() {
  tmpl=$1; target=$2
  if [ -f "$target" ]; then
    jq empty "$target" 2>/dev/null || die "$target is not valid JSON: fix or move it first"
    [ -f "$target.bak" ] || run cp "$target" "$target.bak"
    current=$target
  else
    run mkdir -p "$(dirname "$target")"
    current=
  fi
  merged=$(jq --slurpfile t "$tmpl" '
    def ours: ((.hooks // []) | any(.[]; ((.command // "") | contains("cube-hook.sh"))));
    .hooks = (reduce ($t[0].hooks | to_entries[]) as $e (.hooks // {};
        .[$e.key] = (((.[$e.key] // []) | map(select(ours | not))) + $e.value)))
    | (if ($t[0].description and (.description | not)) then .description = $t[0].description else . end)
  ' "${current:-/dev/null}" 2>/dev/null || jq --slurpfile t "$tmpl" -n '$t[0]')
  if [ "$dry" -eq 1 ]; then say "[dry-run] would write $target"; else printf '%s\n' "$merged" > "$target"; fi
}

# unmerge <target>: drop every cube-hook entry, and empty events
unmerge() {
  target=$1
  [ -f "$target" ] || return 0
  cleaned=$(jq '
    def ours: ((.hooks // []) | any(.[]; ((.command // "") | contains("cube-hook.sh"))));
    if .hooks then .hooks |= (with_entries(.value |= map(select(ours | not))) | with_entries(select(.value | length > 0))) else . end
    | if (.hooks | length) == 0 then del(.hooks) else . end' "$target")
  if [ "$dry" -eq 1 ]; then say "[dry-run] would clean $target"; else printf '%s\n' "$cleaned" > "$target"; fi
}

if [ "$uninstall" -eq 1 ]; then
  [ "$want_claude" -eq 1 ] && unmerge "$claude_file"
  [ "$want_codex" -eq 1 ] && unmerge "$codex_file"
  run rm -f "$bin_dir/cube-hook.sh" "$conf"
  say "Removed. (Backups, if any, are next to the config files as *.bak.)"
  exit 0
fi

if [ -z "$url" ]; then
  printf "Cube address (for example http://192.168.1.50): "
  read -r url
fi
url=${url%/}
case $url in http://*|https://*) ;; *) die "the address must start with http://" ;; esac

token=${CUBE_TOKEN:-}
if [ -z "$token" ]; then
  printf "Cube API token (the api_token in its config.json; input hidden): "
  stty -echo 2>/dev/null || true
  read -r token
  stty echo 2>/dev/null || true
  printf '\n'
fi
[ -n "$token" ] || die "an API token is required"

say "Checking the cube at $url ..."
if ! curl -s --max-time 5 -H "Authorization: Bearer $token" "$url/api/v1/system" | jq -e '.firmware' >/dev/null 2>&1; then
  die "the cube did not answer correctly at $url with this token (is it on, and is the token right?)"
fi

run mkdir -p "$conf_dir" "$bin_dir"
if [ "$dry" -eq 1 ]; then
  say "[dry-run] would write $conf"
else
  ( umask 077; printf 'CUBE_URL=%s\nCUBE_TOKEN=%s\n' "$url" "$token" > "$conf" )
fi
run cp "$here/cube-hook.sh" "$bin_dir/cube-hook.sh"
run chmod +x "$bin_dir/cube-hook.sh"

[ "$want_claude" -eq 1 ] && { merge "$here/claude-hooks.json" "$claude_file"; say "Claude Code: hooks added to $claude_file"; }
[ "$want_codex" -eq 1 ] && { merge "$here/codex-hooks.json" "$codex_file"; say "Codex: hooks added to $codex_file (Codex asks you to trust new hooks once)"; }

say "Done. Restart running Claude Code / Codex sessions so they pick the hooks up."
say "Try it: ask an agent to run a command that needs approval; the cube should show APPROVAL NEEDED."
