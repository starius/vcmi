#!/usr/bin/env python3
#
# Minimal local-LLM agent for VCMI MCP.
#
# This script intentionally uses only Python's standard library so it can run
# on build/test machines without installing Python packages.

import argparse
import json
import os
import sys
import urllib.error
import urllib.request


DEFAULT_MCP_URL = "http://127.0.0.1:3033/mcp"
DEFAULT_OPENAI_BASE_URL = "http://127.0.0.1:8080/v1"
DEFAULT_MODEL = "local-model"

SYSTEM_PROMPT = """You control one VCMI player through MCP tools.
Return only one JSON object and no Markdown.
The JSON object must have exactly these keys:
{"tool":"vcmi.end_turn","arguments":{},"reason":"short reason"}

Allowed tools for this smoke agent:
- vcmi.get_visible_map with {"hero_id": number, "radius": number}
- vcmi.get_movement_options with {"hero_id": number, "radius": number, "max_options": number}
- vcmi.get_battle_state with {}
- vcmi.end_turn with {}
- vcmi.move_hero with {"hero_id": number, "x": number, "y": number, "z": number}
- vcmi.move_hero_to_object with {"hero_id": number, "object_id": number}
- vcmi.build_town_building with {"town_id": number, "building_id": number}
- vcmi.answer_query with {"query_id": number, "answer": number}
- vcmi.battle_defend with {}
- vcmi.battle_wait with {}
- vcmi.battle_move with {"hex": number}
- vcmi.battle_shoot with {"target_stack_id": number}
- vcmi.battle_melee_attack with {"target_stack_id": number, "attack_from_hex": number}
- vcmi.battle_end_tactics with {}

Rules:
- If unsure, choose vcmi.end_turn.
- Use only ids, coordinates, and battle hexes that appear in the action-space JSON.
- Do not invent tools.
"""


class JsonRpcClient:
	def __init__(self, url, token=None, timeout=30):
		self.url = url
		self.token = token
		self.timeout = timeout
		self.next_id = 1

	def request(self, method, params=None):
		request_id = self.next_id
		self.next_id += 1
		payload = {
			"jsonrpc": "2.0",
			"id": request_id,
			"method": method,
		}
		if params is not None:
			payload["params"] = params

		headers = {"Content-Type": "application/json"}
		if self.token:
			headers["Authorization"] = "Bearer " + self.token

		response = post_json(self.url, payload, headers, self.timeout)
		if "error" in response:
			raise RuntimeError(f"MCP {method} failed: {response['error']}")
		return response["result"]


def post_json(url, payload, headers, timeout):
	data = json.dumps(payload).encode("utf-8")
	request = urllib.request.Request(url, data=data, headers=headers, method="POST")
	try:
		with urllib.request.urlopen(request, timeout=timeout) as response:
			body = response.read().decode("utf-8")
	except urllib.error.HTTPError as error:
		body = error.read().decode("utf-8", errors="replace")
		raise RuntimeError(f"HTTP {error.code} from {url}: {body}") from error
	except urllib.error.URLError as error:
		raise RuntimeError(f"Could not connect to {url}: {error}") from error
	return json.loads(body)


def call_model(args, action_space_json, tools):
	base_url = args.openai_base_url.rstrip("/")
	url = base_url + "/chat/completions"
	tool_names = [tool.get("name", "") for tool in tools]
	user_prompt = (
		"Available MCP tools:\n"
		+ json.dumps(tool_names, indent=2)
		+ "\n\nCurrent VCMI action-space JSON:\n"
		+ action_space_json
		+ "\n\nChoose the next single action."
	)
	payload = {
		"model": args.model,
		"messages": [
			{"role": "system", "content": SYSTEM_PROMPT},
			{"role": "user", "content": user_prompt},
		],
		"temperature": args.temperature,
		"max_tokens": args.max_tokens,
	}
	headers = {"Content-Type": "application/json"}
	if args.openai_api_key:
		headers["Authorization"] = "Bearer " + args.openai_api_key

	response = post_json(url, payload, headers, args.timeout)
	return response["choices"][0]["message"]["content"]


def extract_json_object(text):
	start = text.find("{")
	if start == -1:
		raise ValueError("model response did not contain a JSON object")

	depth = 0
	in_string = False
	escaped = False
	for index in range(start, len(text)):
		character = text[index]
		if in_string:
			if escaped:
				escaped = False
			elif character == "\\":
				escaped = True
			elif character == "\"":
				in_string = False
			continue

		if character == "\"":
			in_string = True
		elif character == "{":
			depth += 1
		elif character == "}":
			depth -= 1
			if depth == 0:
				return json.loads(text[start:index + 1])

	raise ValueError("model response contained an unterminated JSON object")


def normalize_action(raw_response):
	action = extract_json_object(raw_response)
	if not isinstance(action, dict):
		raise ValueError("model action must be a JSON object")
	tool = action.get("tool")
	arguments = action.get("arguments", {})
	if not isinstance(tool, str):
		raise ValueError("model action must contain string field 'tool'")
	if not isinstance(arguments, dict):
		raise ValueError("model action field 'arguments' must be an object")

	allowed_tools = {
		"vcmi.get_visible_map",
		"vcmi.get_movement_options",
		"vcmi.get_battle_state",
		"vcmi.end_turn",
		"vcmi.move_hero",
		"vcmi.move_hero_to_object",
		"vcmi.build_town_building",
		"vcmi.answer_query",
		"vcmi.battle_defend",
		"vcmi.battle_wait",
		"vcmi.battle_move",
		"vcmi.battle_shoot",
		"vcmi.battle_melee_attack",
		"vcmi.battle_end_tactics",
	}
	if tool not in allowed_tools:
		raise ValueError(f"model selected unsupported tool: {tool}")
	return {
		"tool": tool,
		"arguments": arguments,
		"reason": action.get("reason", ""),
	}


def tool_result_text(result):
	content = result.get("content", [])
	if not content:
		return json.dumps(result, indent=2)
	first = content[0]
	if isinstance(first, dict) and "text" in first:
		return first["text"]
	return json.dumps(result, indent=2)


def run_once(args):
	mcp = JsonRpcClient(args.mcp_url, args.mcp_token, args.timeout)
	initialize = mcp.request("initialize", {
		"protocolVersion": "2025-06-18",
		"capabilities": {},
		"clientInfo": {"name": "vcmi-mcp-llm-agent", "version": "0.1.0"},
	})
	print("MCP server:", json.dumps(initialize.get("serverInfo", {}), sort_keys=True))

	tools = mcp.request("tools/list").get("tools", [])
	action_space_result = mcp.request("tools/call", {
		"name": "vcmi.get_action_space",
		"arguments": {},
	})
	action_space_json = tool_result_text(action_space_result)
	print("Action-space bytes:", len(action_space_json))

	raw_response = call_model(args, action_space_json, tools)
	print("Model response:", raw_response.strip())
	try:
		action = normalize_action(raw_response)
	except ValueError as error:
		if not args.fallback_end_turn:
			raise
		print(f"Invalid model action ({error}); falling back to vcmi.end_turn")
		action = {"tool": "vcmi.end_turn", "arguments": {}, "reason": "fallback"}

	print("Selected action:", json.dumps(action, sort_keys=True))
	if args.dry_run:
		return

	result = mcp.request("tools/call", {
		"name": action["tool"],
		"arguments": action["arguments"],
	})
	print("Tool result:", json.dumps(result, indent=2))


def parse_args(argv):
	parser = argparse.ArgumentParser(description="Run one local-LLM action through VCMI MCP.")
	parser.add_argument("--mcp-url", default=os.environ.get("VCMI_MCP_URL", DEFAULT_MCP_URL))
	parser.add_argument("--mcp-token", default=os.environ.get("VCMI_MCP_TOKEN"))
	parser.add_argument("--openai-base-url", default=os.environ.get("OPENAI_BASE_URL", DEFAULT_OPENAI_BASE_URL))
	parser.add_argument("--openai-api-key", default=os.environ.get("OPENAI_API_KEY"))
	parser.add_argument("--model", default=os.environ.get("OPENAI_MODEL", DEFAULT_MODEL))
	parser.add_argument("--temperature", type=float, default=0.0)
	parser.add_argument("--max-tokens", type=int, default=160)
	parser.add_argument("--timeout", type=float, default=30)
	parser.add_argument("--dry-run", action="store_true")
	parser.add_argument("--no-fallback-end-turn", dest="fallback_end_turn", action="store_false")
	parser.set_defaults(fallback_end_turn=True)
	return parser.parse_args(argv)


def main(argv):
	args = parse_args(argv)
	run_once(args)
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
