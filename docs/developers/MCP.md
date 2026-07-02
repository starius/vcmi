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

## Local LLM smoke agent

`client/mcp/examples/vcmi_mcp_llm_agent.py` is a minimal one-step agent that connects an OpenAI-compatible local model server to the VCMI MCP HTTP endpoint. It reads `vcmi.get_state`, asks the model for one JSON action, and calls the selected MCP tool.

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

The smoke agent intentionally allows only a small action set (`vcmi.end_turn`, `vcmi.move_hero`, `vcmi.build_town_building`). If the model returns invalid JSON or an unsupported tool, the script falls back to `vcmi.end_turn` unless `--no-fallback-end-turn` is passed.

## Tools

Current tools:

- `vcmi.get_state`: returns visible player state as JSON text
- `vcmi.move_hero`: requests movement of an owned hero to `x`, `y`, optional `z`
- `vcmi.end_turn`: ends the active adventure-map turn
- `vcmi.answer_query`: answers a pending VCMI query/dialog by `query_id` and optional integer `answer`
- `vcmi.build_town_building`: requests construction by numeric town and building id
- `vcmi.battle_defend`: defends with the active battle stack
- `vcmi.battle_wait`: waits with the active battle stack
- `vcmi.battle_end_tactics`: ends the active tactics phase

The state payload includes:

- controlled player id/color, team, status, resources
- active turn flag
- pending query metadata
- owned heroes with position, movement, mana, level, primary skills, and army
- owned towns with position, heroes, buildings, fort level, and garrison
- minimal battle/tactics activity flags

## Development Notes

`client/mcp/McpProtocol.*` contains the MCP/JSON-RPC message handler and is unit-tested from `vcmitest`.

`client/mcp/CMcpPlayerInterface.*` is the actual VCMI player interface. Keep game actions routed through `CCallback`; do not mutate `CGameState` directly from MCP handlers.

`client/mcp/McpHttpServer.*` owns a small loopback-only Boost.Beast HTTP server. It is started only for `McpAI` players and stopped from `finish()`.

Future tool additions should stay narrow and server-validated. Prefer adding one high-level action at a time with a state snapshot field that lets an MCP client verify when the action is applicable.
