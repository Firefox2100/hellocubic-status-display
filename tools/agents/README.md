# Agent hooks (Claude Code and Codex)

Shows on the cube when an agent is blocked on **you** (a permission prompt or a question), and clears the card once you
have answered. Nothing is decided by the cube: you always approve in the terminal.

```sh
git clone <this repo> && cd hellocubic-status-display
tools/agents/install.sh --url http://<cube-ip>        # asks for the cube's API token (hidden)
```

`install.sh` saves the address and token in `~/.config/hellocubic/env` (mode 600), copies `cube-hook.sh` to
`~/.local/bin`, merges the hook entries into `~/.claude/settings.json` and/or `~/.codex/hooks.json` (your other hooks stay,
a `.bak` copy is made once) and checks the cube answers. Run it again to update; `--uninstall` removes everything;
`--dry-run` shows what would change. Restart running agent sessions afterwards. Needs `curl` and `jq`.

How it works: each hook event is reduced by `cube-hook.sh` to a few short fields (event, session, project folder, tool,
a 60-character command/path, an 80-character message), anything credential-shaped is masked (`***`), and it is sent to
`POST /api/v1/hook?agent=claude|codex`. The script always exits 0 and gives up after 2 s, so an offline cube never slows
an agent. The cube makes one card per agent session (`claude:<session>`), so two agents waiting at once queue up.

| Event | Card |
| --- | --- |
| Claude `Notification` `permission_prompt` / Codex `PermissionRequest` | **APPROVAL NEEDED**, tool and command below |
| Claude `Notification` `idle_prompt`, `agent_needs_input`, `elicitation_dialog` | **WAITING FOR YOU** |
| Codex `Stop` | **WAITING FOR YOU**, only if you have not replied within 60 s |
| `PostToolUse`, `PostToolUseFailure`, `UserPromptSubmit`, `SessionEnd`, Claude `Stop` | clears that session's card |

With `agent_done_cards` on (`POST /api/v1/ui`), a finished turn shows a 20 s **DONE** card instead of just clearing.
Neither agent sends an event at the moment you approve: the card clears when the approved tool finishes, at your next
prompt, or when it expires (`approval_ttl_min`).

## Working, waiting and done

The cube also tracks which sessions are running (`GET /api/v1/agents`). A session is `agent` + a hash of its **full**
session id, so two agents, or two sessions of one agent, are never confused, and a `Stop` only finishes its own session.

| Hook event | Session state |
| --- | --- |
| `UserPromptSubmit`, `PostToolUse`, `PostToolUseFailure` | working |
| permission prompt, question, Codex `PermissionRequest` | waiting for you |
| `Stop` | done (listed for 15 min, a quiet DONE card for 3 min if `agent_done_cards`, which is now on by default) |
| `SessionEnd` | forgotten |

Sessions that go silent (a crashed agent) are dropped after 30 min. The state lives in RAM, so a cube reboot forgets it
until the next event. Where it shows: the **Agents** page (rotates in while any session exists), and the Home footer
("2 AGENTS WORKING", "1 AGENT NEEDS YOU", "1 AGENT DONE"). A DONE card never takes the screen.
