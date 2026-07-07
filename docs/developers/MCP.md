# Model Context Protocol

VCMI has an experimental client-side MCP interface for controlling a player from an external agent.

The integration is intentionally client-only:

- the server remains authoritative for all game state changes
- MCP actions are translated to the same `CCallback` requests used by human UI and AI interfaces
- state exposed to MCP is read through the player-specific callback, so it follows the same visibility rules as the controlled player

## Enabling

Assign an AI-controlled player the adventure AI name `McpAI`. During client interface creation, `McpAI` is handled in `client/mcp/` instead of being resolved through the lib-level `AIFactory`.

For debug/headless runs, pass it through the client `--ai` option. The option can be repeated for consecutive player colors; `--ai McpAI` assigns red to MCP. `--testmap` expects a VCMI map resource path, not an absolute filesystem path:

```bash
VCMI_MCP_TOKEN=secret VCMI_MCP_PORT=3033 ./vcmiclient --headless --testmap "Maps/Dwarven Gold.h3m" --ai McpAI
```

For maps with more AI players, pass additional AI names for the following colors, for example `--ai McpAI --ai EmptyAI --ai EmptyAI`.

When the `McpAI` player interface is created, VCMI starts a localhost HTTP MCP endpoint:

```text
http://127.0.0.1:3033/mcp
```

For multiple MCP-controlled players, the default port is offset by player color index (`3033` for red, `3034` for blue, and so on). Set `VCMI_MCP_PORT` to force a specific port.

Set `VCMI_MCP_TOKEN` to require either:

```http
Authorization: Bearer <token>
```

or:

```http
X-VCMI-MCP-Token: <token>
```

The listener binds to loopback only.

## Protocol

The endpoint accepts JSON-RPC 2.0 MCP messages with `POST /mcp`.

Example initialization:

```bash
curl -s http://127.0.0.1:3033/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer secret' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"dev"}}}'
```

List tools:

```bash
curl -s http://127.0.0.1:3033/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer secret' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

Read the current state:

```bash
curl -s http://127.0.0.1:3033/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer secret' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vcmi.get_state","arguments":{}}}'
```

The same state is also available as the MCP resource `vcmi://state`.

## ChatGPT connector demo

For a ChatGPT web connector demo, run a VCMI game with MCP controlling red and `Nullkiller2` controlling the other player colors.

The demo supports three connection modes through `VCMI_MCP_CONNECTION`:

- `server`: expose the local MCP listener through a public HTTPS URL, for example ngrok or cloudflared
- `tunnel`: keep the listener private and connect it through OpenAI Secure MCP Tunnel
- `local`: run only the local MCP listener for curl, tests, or local agents

Server URL mode is the fastest path for Developer Mode testing. OpenAI's Apps SDK deployment docs show the same local-development pattern with a tunnel such as ngrok, mapping an HTTPS public URL ending in `/mcp` to a local MCP listener. ChatGPT Developer Mode supports remote MCP over SSE or streaming HTTP, with OAuth, No Authentication, or Mixed Authentication.

Run the game from a VCMI build output directory:

```bash
cd <build>/bin
VCMI_MCP_CONNECTION=server VCMI_MCP_PORT=3033 VCMI_CLIENT=./vcmiclient \
  <source>/client/mcp/examples/run_chatgpt_demo.sh
```

In another shell on the same host, expose it with a public HTTPS forwarder and use the printed URL with `/mcp` in ChatGPT:

```bash
ngrok http 3033
# use https://<subdomain>.ngrok.app/mcp
```

If ngrok is not configured on the host, cloudflared quick tunnels can be used for ad hoc testing:

```bash
cloudflared tunnel --url http://127.0.0.1:3033
# use https://<subdomain>.trycloudflare.com/mcp
```

The demo script can also start one of these forwarders:

```bash
VCMI_MCP_CONNECTION=server VCMI_MCP_FORWARDER=cloudflared \
  VCMI_CLIENT=./vcmiclient \
  <source>/client/mcp/examples/run_chatgpt_demo.sh
```

On a Nix-based host without a global `cloudflared` install:

```bash
CLOUDFLARED="$(nix --extra-experimental-features 'nix-command flakes' build --no-link --print-out-paths nixpkgs#cloudflared)/bin/cloudflared"
VCMI_MCP_CONNECTION=server VCMI_MCP_FORWARDER=cloudflared \
  VCMI_CLOUDFLARED="$CLOUDFLARED" VCMI_CLIENT=./vcmiclient \
  <source>/client/mcp/examples/run_chatgpt_demo.sh
```

The script prints the MCP endpoint and creates a directory like `/tmp/vcmi-chatgpt-demo-YYYYMMDDTHHMMSSZ` containing:

- `mcp-red.jsonl`: MCP lifecycle and tool-call trace
- `vcmiclient.stdout.log`: stdout/stderr from the client
- `forwarder.log`: forwarder output when `VCMI_MCP_FORWARDER` is used
- VCMI log files written via `--logLocation`

In ChatGPT Developer Mode, create an app from the public server URL and choose `No Authentication` for this quick demo. Leave `VCMI_MCP_TOKEN` empty for that path; ChatGPT does not present arbitrary static API keys in Developer Mode. `VCMI_MCP_TOKEN` remains useful for curl, local scripts, and API clients that can set `Authorization: Bearer <token>`.

For tunnel mode instead, create a tunnel in OpenAI Platform tunnel settings, then run `tunnel-client` on the same host that can reach VCMI:

```bash
export CONTROL_PLANE_API_KEY="sk-..."

tunnel-client init \
  --profile vcmi-mcp \
  --tunnel-id tunnel_... \
  --mcp-server-url http://127.0.0.1:3033/mcp

tunnel-client doctor --profile vcmi-mcp --explain
tunnel-client run --profile vcmi-mcp
```

Start VCMI with `VCMI_MCP_CONNECTION=tunnel` for that workflow. In ChatGPT connector settings, choose `Tunnel` as the connection type and select or paste the tunnel id. The demo MCP listener still binds only to `127.0.0.1`; no inbound firewall rule or public tunnel URL is needed.

For this tunnel demo, do not set `VCMI_MCP_TOKEN` unless your tunnel profile is configured to forward the matching `Authorization: Bearer` header. The listener is private to localhost, and tunnel access is controlled by OpenAI tunnel identity and workspace permissions.

Suggested ChatGPT prompt:

```text
Use the VCMI connector and play red for many turns.

Start by calling vcmi.get_state with {"select":["summary","resources","heroes","towns","tavern"]} and vcmi.get_action_space. For every owned hero with movement, call vcmi.get_reachable with a practical radius and max_options. Prefer vcmi.execute_plan over one-action calls: batch allowed town builds, recruitment, useful hero visits/moves using returned route_id values, query answers, and end_turn into one plan for the day. Use dry_run first for any risky plan.

After each execute_plan, read the returned updates and state. On later turns use vcmi.get_updates with the last revision and only refresh selected state sections you need. If a battle starts, do not micromanage it; the MCP interface auto-handles battle stacks. If a pending query blocks execution, answer it if the choice is clear, otherwise choose the first/default option. Keep playing until several days have passed or red has explored, collected objects, and fought at least one battle.
```

Faster prompt variant:

```text
Use the VCMI connector and play red quickly for many turns.

For each red turn, start with vcmi.get_day_context. It returns state, build options, compact recommended movement targets, and a suggested execute_plan skeleton. Edit that suggestedPlan if needed, then call vcmi.execute_plan once for the whole day. Use only canonical plan action types: build, recruit, move_hero, visit_object, answer_query, end_turn. For movement, prefer recommendedTargets[*].planAction exactly as returned. Do not call vcmi.get_reachable repeatedly unless a move reveals new terrain that changes the plan.

If execute_plan returns partial, fix only the failed/remaining actions. Battles are auto-handled. End the turn when no strong build/recruit/move remains.
```

ChatGPT receives the complete callable API through MCP `tools/list`. The `vcmi.get_action_space` tool is an additional game-aware helper that returns the currently relevant object ids, build options, pending query id, battle flags, and action candidates.

## Batched Planning

For normal adventure-map play, prefer this loop:

1. Read `vcmi.get_day_context`.
2. Use its `suggestedPlan.actions` and `heroReachability[*].recommendedTargets[*].planAction` entries as the basis for the day.
3. Submit one `vcmi.execute_plan` call for build, recruit, movement, query answer, and end-turn actions.
4. Use the returned `updates` and optional selected `state` to decide whether one more plan is needed before ending the turn.

`vcmi.execute_plan` accepts:

```json
{
  "plan_id": "day-1-main",
  "dry_run": false,
  "max_updates": 200,
  "return_select": ["summary", "resources", "heroes", "towns", "updates"],
  "actions": [
    {"id": "build-1", "type": "build", "town_id": 17, "building_id": 5},
    {"id": "move-1", "type": "visit_object", "hero_id": 34, "object_id": 101, "route_id": "h34:s18,4,0:d20,2,0:l0:m1200"},
    {"id": "end", "type": "end_turn"}
  ]
}
```

Supported plan action types:

- `build`: `town_id`, `building_id`
- `recruit`: `source_id` or `town_id`, `level`, optional `creature_id`, optional `amount`, optional `destination_id`
- `move_hero`: `hero_id`, `x`, `y`, optional `z`, optional `route_id`
- `visit_object`: `hero_id`, `object_id`, optional `route_id`
- `answer_query`: `query_id`, optional `answer`
- `end_turn`: no extra fields

`vcmi.execute_plan` also accepts common aliases and tool-shaped plan actions, for example:

```json
{"tool":"vcmi.move_hero_to_object","arguments":{"hero_id":990,"object_id":181,"route_id":"..."}}
```

Tool-shaped actions are normalized to canonical action types before validation.

Plan execution is sequential. It stops at the first rejected action, pending query, active battle, or ended turn. The result includes `executed`, `failed`, `remaining`, `fromRevision`, `toRevision`, and `updates`. Use `dry_run=true` to validate object ownership, build availability, recruitment availability, and route availability without submitting server requests.

Movement actions should use `route_id` from `vcmi.get_reachable` whenever possible. The route id ties the order to the hero start, destination, pathfinder layer, and remaining movement; if it no longer matches, the MCP layer recomputes/rejects instead of blindly sending an obsolete move.

## State Selection and Updates

`vcmi.get_state` can return the full visible state or only selected sections:

```json
{"select":["summary","resources","heroes","towns","tavern","battle","updates"],"since_revision":42,"max_updates":100}
```

Common selectors:

- `summary`: turn, pending query, battle flags, and resources
- `resources`: player resources
- `heroes` or `ownedHeroes`: owned hero details
- `towns` or `ownedTowns`: owned town details, including build and recruitment options
- `tavern`: visible tavern hero pools in owned towns
- `battle`: minimal battle flags
- `updates`: revisioned state changes since `since_revision`

`vcmi.get_updates` returns only the journal:

```json
{"since_revision":42,"max_updates":200}
```

The update journal currently records request send/apply results, player turn starts/ends, pending query changes, battle start/end/tactics/auto-stack events, hero movement/creation/visits/mana/movement changes, tile reveal/hide events, visible object changes/removal, town building changes, and recruitment availability changes. Updates are player-visible callback events; they do not expose hidden map state.

If you have an X/VNC display and want a spectator window instead of headless mode, set `VCMI_GUI=1` before running the script.

## Local LLM smoke agent

`client/mcp/examples/vcmi_mcp_llm_agent.py` is a minimal one-step agent that connects an OpenAI-compatible local model server to the VCMI MCP HTTP endpoint. It reads selected `vcmi.get_state` data and `vcmi.get_action_space`, asks the model for one JSON tool call, and calls the selected MCP tool. The allowed set includes `vcmi.execute_plan`, so a capable model can batch a small day plan into one MCP roundtrip.

Example with `llama.cpp` serving a local GGUF model:

```bash
mkdir -p /tmp/vcmi-mcp-models
curl -L --fail \
  -o /tmp/vcmi-mcp-models/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf \
  https://huggingface.co/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf

LLAMA_SERVER="$(nix --extra-experimental-features 'nix-command flakes' eval --raw nixpkgs#llama-cpp.outPath)/bin/llama-server"
"$LLAMA_SERVER" \
  -m /tmp/vcmi-mcp-models/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf \
  --host 127.0.0.1 \
  --port 8080 \
  -c 2048
```

In another shell, start VCMI with an MCP-controlled player:

```bash
VCMI_MCP_TOKEN=secret VCMI_MCP_PORT=3033 ./vcmiclient \
  --headless \
  --testmap "Maps/Dwarven Gold.h3m" \
  --ai McpAI --ai EmptyAI --ai EmptyAI
```

Then run one LLM-selected action from the VCMI source root:

```bash
VCMI_MCP_TOKEN=secret \
OPENAI_BASE_URL=http://127.0.0.1:8080/v1 \
OPENAI_MODEL=local-model \
python3 client/mcp/examples/vcmi_mcp_llm_agent.py
```

The smoke agent allows the main read/action tools but still performs only one model-selected tool call. If the model returns invalid JSON or an unsupported tool, the script falls back to `vcmi.end_turn` unless `--no-fallback-end-turn` is passed.

## Tools

Current tools:

- `vcmi.get_state`: returns visible player state as JSON text, optionally filtered by selectors
- `vcmi.get_updates`: returns revisioned visible state updates since a previous revision
- `vcmi.get_action_space`: returns action guidance, current object ids, build options, and relevant action candidates
- `vcmi.get_day_context`: returns compact whole-day context, recommended reachable targets, and a suggested `execute_plan`
- `vcmi.get_visible_map`: returns player-visible adventure-map tiles and visible/owned objects around an owned hero or coordinate window
- `vcmi.get_movement_options`: returns pathfinder-derived destinations and visible object targets for an owned hero
- `vcmi.get_reachable`: returns compact this-day reachable objects, recommended targets, and route ids for an owned hero; full tiles and path previews are opt-in
- `vcmi.execute_plan`: executes a sequential batch of build, recruit, movement, query, and end-turn actions
- `vcmi.get_battle_state`: returns visible battle stacks, active stack metadata, legal move hexes, and legal attack targets; battles are auto-handled by default
- `vcmi.move_hero`: requests movement of an owned hero to `x`, `y`, optional `z`, preferably with a `route_id`
- `vcmi.move_hero_to_object`: requests movement of an owned hero to a visible object id returned by reachable/movement tools
- `vcmi.end_turn`: ends the active adventure-map turn
- `vcmi.answer_query`: answers a pending VCMI query/dialog by `query_id` and optional integer `answer`
- `vcmi.build_town_building`: requests construction by numeric town and building id
- `vcmi.battle_defend`: defends with the active battle stack
- `vcmi.battle_wait`: waits with the active battle stack
- `vcmi.battle_move`: moves the active battle stack to a legal battle hex
- `vcmi.battle_shoot`: shoots a legal target stack with the active battle stack
- `vcmi.battle_melee_attack`: performs a melee attack against a legal target stack, optionally using a returned `attack_from_hex`
- `vcmi.battle_end_tactics`: ends the active tactics phase

The state payload includes:

- a monotonically increasing `revision`
- controlled player id/color, team, status, resources
- active turn flag
- pending query metadata
- owned heroes with position, movement, mana, level, primary skills, and army
- owned towns with position, heroes, buildings, fort level, garrison, build options, and recruitment options
- visible tavern hero options in owned towns
- minimal battle/tactics activity flags

The visible map and movement payloads are bounded by request arguments (`radius`, `max_tiles`, `max_options`) to keep LLM context manageable. `full_visible=true` on `vcmi.get_visible_map` scans all currently revealed tiles but is still capped by `max_tiles`.

Visibility rules:

- Map reads use the player callback (`getTile`, `getObj`, `getTopObj`, `getTilesInRange`, `isVisibleFor`) and do not expose hidden fog-of-war tiles.
- Visible object lists filter every object id through `getObj`, so hidden events and hidden map objects are omitted.
- Owned heroes and towns are included through the same player-specific state APIs a normal player interface uses.
- Movement options are calculated with `SingleHeroPathfinderConfig` through the player callback and only emitted for visible destinations.
- Battle state is limited to battles involving the controlled player and data available through `CPlayerBattleCallback`.
- `vcmi.execute_plan` only submits normal `CCallback` requests. The server still validates every build, recruitment, movement, query answer, and turn end.

Additional MCP resources:

- `vcmi://state`: same JSON as `vcmi.get_state`
- `vcmi://action-space`: same JSON as `vcmi.get_action_space`
- `vcmi://battle-state`: same JSON as `vcmi.get_battle_state`
- `vcmi://agent-guide`: short text operating guide for an LLM agent

Set `VCMI_MCP_TRACE=/path/to/file.jsonl` or `VCMI_MCP_TRACE_DIR=/path/to/dir` to record MCP lifecycle and tool-call trace events as JSON Lines. The trace includes interface start/finish, HTTP listener start, turn start, pending-query changes, tactics phase, active stack, battle start/end, and every MCP tool call with arguments and result. Use the trace together with `vcmi.get_updates` results to reconstruct what the LLM asked for and what the player-visible game state reported afterward.

## Development Notes

`client/mcp/McpProtocol.*` contains the MCP/JSON-RPC message handler and is unit-tested from `vcmitest`.

`client/mcp/CMcpPlayerInterface.*` is the actual VCMI player interface. Keep game actions routed through `CCallback`; do not mutate `CGameState` directly from MCP handlers.

`client/mcp/McpHttpServer.*` owns a small loopback-only Boost.Beast HTTP server. It is started only for `McpAI` players and stopped from `finish()`.

Future tool additions should stay narrow and server-validated. Prefer adding one high-level action at a time with a state snapshot field that lets an MCP client verify when the action is applicable.
