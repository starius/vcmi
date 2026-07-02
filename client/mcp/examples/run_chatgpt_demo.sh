#!/usr/bin/env bash
set -euo pipefail

timestamp="$(date -u +%Y%m%dT%H%M%SZ)"

VCMI_CLIENT="${VCMI_CLIENT:-./vcmiclient}"
VCMI_MAP="${VCMI_MAP:-Maps/Dwarven Gold.h3m}"
VCMI_MCP_PORT="${VCMI_MCP_PORT:-3033}"
VCMI_MCP_TOKEN="${VCMI_MCP_TOKEN:-}"
VCMI_MCP_CONNECTION="${VCMI_MCP_CONNECTION:-server}"
VCMI_MCP_FORWARDER="${VCMI_MCP_FORWARDER:-}"
VCMI_MCP_PUBLIC_URL="${VCMI_MCP_PUBLIC_URL:-}"
VCMI_DEMO_DIR="${VCMI_DEMO_DIR:-/tmp/vcmi-chatgpt-demo-$timestamp}"
VCMI_GUI="${VCMI_GUI:-0}"
VCMI_CLOUDFLARED="${VCMI_CLOUDFLARED:-cloudflared}"
VCMI_NGROK="${VCMI_NGROK:-ngrok}"

case "$VCMI_MCP_CONNECTION" in
	server | tunnel | local)
		;;
	*)
		echo "Unsupported VCMI_MCP_CONNECTION: $VCMI_MCP_CONNECTION" >&2
		echo "Use one of: server, tunnel, local" >&2
		exit 2
		;;
esac

mkdir -p "$VCMI_DEMO_DIR"

export VCMI_MCP_PORT
export VCMI_MCP_TOKEN
export VCMI_MCP_TRACE="$VCMI_DEMO_DIR/mcp-red.jsonl"

local_origin="http://127.0.0.1:$VCMI_MCP_PORT"
local_endpoint="$local_origin/mcp"
public_endpoint="$VCMI_MCP_PUBLIC_URL"
forwarder_pid=""
forwarder_log="$VCMI_DEMO_DIR/forwarder.log"

cleanup()
{
	if [[ -n "$forwarder_pid" ]]; then
		kill "$forwarder_pid" 2>/dev/null || true
	fi
}

trap cleanup EXIT

wait_for_cloudflared_url()
{
	for _ in $(seq 1 30); do
		local base_url
		base_url="$(grep -Eo 'https://[-[:alnum:].]+trycloudflare.com' "$forwarder_log" 2>/dev/null | head -n 1 || true)"
		if [[ -n "$base_url" ]]; then
			public_endpoint="${base_url%/}/mcp"
			return 0
		fi
		sleep 1
	done
	return 1
}

wait_for_ngrok_url()
{
	for _ in $(seq 1 30); do
		local base_url
		base_url="$(python3 - <<'PY' 2>/dev/null || true
import json
import urllib.request

try:
	with urllib.request.urlopen("http://127.0.0.1:4040/api/tunnels", timeout=1) as response:
		data = json.load(response)
except Exception:
	raise SystemExit(0)

for tunnel in data.get("tunnels", []):
	url = tunnel.get("public_url", "")
	if url.startswith("https://"):
		print(url)
		break
PY
)"
		if [[ -n "$base_url" ]]; then
			public_endpoint="${base_url%/}/mcp"
			return 0
		fi
		sleep 1
	done
	return 1
}

require_command()
{
	local command_path="$1"
	local label="$2"

	if [[ -x "$command_path" ]]; then
		return 0
	fi

	if command -v "$command_path" >/dev/null 2>&1; then
		return 0
	fi

	echo "$label executable was not found: $command_path" >&2
	return 1
}

start_forwarder()
{
	if [[ "$VCMI_MCP_CONNECTION" != "server" || -z "$VCMI_MCP_FORWARDER" ]]; then
		return 0
	fi

	case "$VCMI_MCP_FORWARDER" in
		cloudflared)
			require_command "$VCMI_CLOUDFLARED" "cloudflared" || exit 2
			"$VCMI_CLOUDFLARED" tunnel --url "$local_origin" > "$forwarder_log" 2>&1 &
			forwarder_pid=$!
			if ! wait_for_cloudflared_url; then
				echo "cloudflared started, but no public URL was detected yet. See $forwarder_log" >&2
			fi
			;;
		ngrok)
			require_command "$VCMI_NGROK" "ngrok" || exit 2
			"$VCMI_NGROK" http "$VCMI_MCP_PORT" --log=stdout > "$forwarder_log" 2>&1 &
			forwarder_pid=$!
			if ! wait_for_ngrok_url; then
				echo "ngrok started, but no public URL was detected yet. See $forwarder_log" >&2
			fi
			;;
		*)
			echo "Unsupported VCMI_MCP_FORWARDER: $VCMI_MCP_FORWARDER" >&2
			echo "Use one of: cloudflared, ngrok" >&2
			exit 2
			;;
	esac
}

mode_args=(--headless)
if [[ "$VCMI_GUI" == "1" ]]; then
	mode_args=(--spectate --autoSkip --spectate-skip-battle-result)
fi

ai_args=(
	--ai McpAI
	--ai Nullkiller2
	--ai Nullkiller2
	--ai Nullkiller2
	--ai Nullkiller2
	--ai Nullkiller2
	--ai Nullkiller2
	--ai Nullkiller2
)

start_forwarder

cat <<EOF
VCMI ChatGPT MCP demo
  map:        $VCMI_MAP
  mode:       $VCMI_MCP_CONNECTION
  local:      $local_endpoint
  public:     ${public_endpoint:-<start ngrok/cloudflared and use its HTTPS URL with /mcp>}
  token:      ${VCMI_MCP_TOKEN:-<none>}
  trace:      $VCMI_MCP_TRACE
  log dir:    $VCMI_DEMO_DIR
  executable: $VCMI_CLIENT
  forwarder:  ${VCMI_MCP_FORWARDER:-<none>}
EOF

if [[ "$VCMI_MCP_CONNECTION" == "server" ]]; then
	cat <<EOF

ChatGPT server URL mode:
  Use the public HTTPS /mcp URL in ChatGPT Developer Mode.
  For this quick demo, choose No Authentication and leave VCMI_MCP_TOKEN empty.
EOF
elif [[ "$VCMI_MCP_CONNECTION" == "tunnel" ]]; then
	cat <<EOF

ChatGPT tunnel mode:
  Use OpenAI Secure MCP Tunnel and point tunnel-client at $local_endpoint.
EOF
fi

"$VCMI_CLIENT" \
	--logLocation "$VCMI_DEMO_DIR" \
	"${mode_args[@]}" \
	--testmap "$VCMI_MAP" \
	"${ai_args[@]}" \
	"$@" 2>&1 | tee "$VCMI_DEMO_DIR/vcmiclient.stdout.log"
