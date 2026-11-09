#!/bin/sh
# cube-hook.sh <claude|codex>
#
# Forward an agent hook event (JSON on stdin) to the status display, so it can show "APPROVAL NEEDED" while Claude
# Code / Codex waits on you and clear the card once you have answered.
#
# Only a few small fields are sent, never the raw event (a PostToolUse event carries the tool's whole output, far
# more than the device's RAM). Anything credential-shaped is masked before it leaves this machine.
#
# Settings come from the environment or from ~/.config/hellocubic/env (written by install.sh):
#   CUBE_URL    http://<cube-ip>          (or http://localhost:8080 through an SSH tunnel)
#   CUBE_TOKEN  the cube's api_token
#
# Requires curl and jq. It must never get in the agent's way, whatever happens to the cube:
#   - it always exits 0 and prints nothing (stdout of some hooks is read by the agent)
#   - connecting gives up after 1 s and the whole request after 2 s
#   - after a failure it does not try again for 30 s ("circuit breaker"), so an unreachable cube costs at most
#     one short delay every 30 s, not one per tool call

exec >/dev/null 2>&1

agent=${1:-claude}
config=${CUBE_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/hellocubic/env}
# shellcheck disable=SC1090
[ -r "$config" ] && . "$config"
[ -n "$CUBE_URL" ] || exit 0
command -v jq >/dev/null 2>&1 || exit 0
command -v curl >/dev/null 2>&1 || exit 0

# Circuit breaker: a marker file with the time of the last failure
down=${TMPDIR:-/tmp}/cube-hook-down-$(id -u)
now=$(date +%s)
if [ -r "$down" ]; then
  last=$(cat "$down" 2>/dev/null)
  case $last in ''|*[!0-9]*) last=0 ;; esac
  [ $((now - last)) -lt 30 ] && exit 0
fi

payload=$(jq -c 'def redact:
  gsub("(?<k>(?i)(bearer|token|key|secret|password|passwd|pwd|auth[a-z]*)[\"'"'"']?\\s*[=: ]\\s*[\"'"'"']?)[^\\s\"'"'"'&|;]+"; "\(.k)***")
  | gsub("(sk-[A-Za-z0-9_-]{8,}|eyJ[A-Za-z0-9_-]{10,}(\\.[A-Za-z0-9_-]+){0,2}|gh[pousr]_[A-Za-z0-9]{10,}|xox[abprs]-[A-Za-z0-9-]{10,}|AKIA[0-9A-Z]{12,})"; "***");
{
  event:   (.hook_event_name   // ""),
  type:    (.notification_type // ""),
  session: (.session_id        // ""),
  cwd:     (.cwd               // ""),
  tool:    (.tool_name         // ""),
  detail:  ((.tool_input.command // .tool_input.file_path // .tool_input.path // "")
            | tostring | redact | .[0:60]),
  message: ((.message // .last_assistant_message // "") | tostring
            | gsub("\\s+"; " ") | redact | .[0:80])
}' 2>/dev/null) || exit 0
[ -n "$payload" ] || exit 0

if [ -n "$CUBE_TOKEN" ]; then
  curl -s -f --connect-timeout 1 --max-time 2 -X POST -H 'Content-Type: application/json' -H "Authorization: Bearer $CUBE_TOKEN" \
       --data "$payload" "$CUBE_URL/api/v1/hook?agent=$agent" || printf '%s' "$now" > "$down"
else
  curl -s -f --connect-timeout 1 --max-time 2 -X POST -H 'Content-Type: application/json' \
       --data "$payload" "$CUBE_URL/api/v1/hook?agent=$agent" || printf '%s' "$now" > "$down"
fi
exit 0
