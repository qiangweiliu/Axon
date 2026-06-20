#!/usr/bin/env python3
"""
anthropic_proxy.py — Anthropic-to-OpenAI API proxy for Claude Code.

Claude Code sends Anthropic API format. This proxy translates to
OpenAI-compatible API calls, so Claude Code can use any OpenAI-compatible
endpoint (like https://apihub.agnes-ai.com/v1).

Usage:
  python3 anthropic_proxy.py [port]
  
  Then configure Claude Code:
    cc config set ANTHROPIC_BASE_URL http://localhost:8090
    cc config set ANTHROPIC_AUTH_TOKEN sk-any-value
    cc switch deepseek-v4-flash
"""

import os
import sys
import json
import time
import requests
from flask import Flask, request, Response, stream_with_context

app = Flask(__name__)

# Target OpenAI-compatible endpoint
OPENAI_BASE = os.environ.get("OPENAI_BASE_URL", "https://apihub.agnes-ai.com/v1")
OPENAI_KEY  = os.environ.get("OPENAI_API_KEY", "")
OPENAI_MODEL = os.environ.get("OPENAI_MODEL", "deepseek-v4-flash")

@app.route("/v1/messages", methods=["POST"])
def messages():
    """Anthropic /v1/messages endpoint → translate to OpenAI chat completions."""
    data = request.get_json(silent=True) or {}
    anthropic_model = data.get("model", OPENAI_MODEL)
    max_tokens = data.get("max_tokens", 4096)
    system_prompt = data.get("system", "")
    anthropic_msgs = data.get("messages", [])
    stream = data.get("stream", False)

    # Convert Anthropic messages → OpenAI messages
    openai_msgs = []
    if system_prompt:
        openai_msgs.append({"role": "system", "content": system_prompt})
    for msg in anthropic_msgs:
        role = msg.get("role", "user")
        content = msg.get("content", "")
        # Anthropic content can be a list of blocks (text, image, tool_use, tool_result)
        if isinstance(content, list):
            # Extract just the text from blocks
            texts = []
            for block in content:
                if block.get("type") == "text":
                    texts.append(block.get("text", ""))
                elif block.get("type") == "tool_result":
                    texts.append(json.dumps(block.get("content", "")))
            content = "\n".join(texts)
        openai_msgs.append({"role": role, "content": content})

    # Build OpenAI request
    openai_payload = {
        "model": anthropic_model,
        "messages": openai_msgs,
        "max_tokens": max_tokens,
        "stream": stream,
    }

    headers = {
        "Authorization": f"Bearer {OPENAI_KEY}",
        "Content-Type": "application/json",
    }

    # Set a generous timeout
    timeout = int(os.environ.get("HTTP_TIMEOUT", "120"))

    if not stream:
        # Non-streaming: translate response back to Anthropic format
        try:
            resp = requests.post(
                f"{OPENAI_BASE}/chat/completions",
                json=openai_payload,
                headers=headers,
                timeout=timeout,
            )
            resp.raise_for_status()
            oai = resp.json()
        except Exception as e:
            return {"type": "error", "error": {"message": str(e)}}, 500

        # Translate OpenAI response → Anthropic format
        choice = oai.get("choices", [{}])[0]
        msg = choice.get("message", {})
        content = msg.get("content", "")

        anthropic_resp = {
            "id": oai.get("id", ""),
            "type": "message",
            "role": "assistant",
            "content": [{"type": "text", "text": content}],
            "model": anthropic_model,
            "usage": {
                "input_tokens": oai.get("usage", {}).get("prompt_tokens", 0),
                "output_tokens": oai.get("usage", {}).get("completion_tokens", 0),
            },
            "stop_reason": _stop_reason(choice.get("finish_reason", "")),
        }
        return json.dumps(anthropic_resp), 200, {"Content-Type": "application/json"}

    else:
        # Streaming: forward SSE, translate each chunk to Anthropic SSE format
        try:
            upstream = requests.post(
                f"{OPENAI_BASE}/chat/completions",
                json=openai_payload,
                headers=headers,
                stream=True,
                timeout=timeout,
            )
        except Exception as e:
            return {"error": {"message": str(e)}}, 500

        def generate():
            yield 'data: {"type":"message_start","message":{"id":"msg_0","type":"message","role":"assistant","content":[],"model":"%s"}}\n\n' % anthropic_model
            yield 'data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}\n\n'

            for line in upstream.iter_lines(decode_unicode=True):
                if not line or not line.startswith("data: "):
                    continue
                payload = line[6:]  # strip "data: "
                if payload.strip() == "[DONE]":
                    continue
                try:
                    chunk = json.loads(payload)
                except json.JSONDecodeError:
                    continue
                delta = chunk.get("choices", [{}])[0].get("delta", {})
                text = delta.get("content", "")
                if text:
                    yield 'data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":%s}}\n\n' % json.dumps(text)

            usage = {}
            yield 'data: {"type":"content_block_stop","index":0}\n\n'
            yield 'data: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":%s}\n\n' % json.dumps(usage)
            yield 'data: [DONE]\n\n'

        return Response(stream_with_context(generate()), mimetype="text/event-stream")


@app.route("/health", methods=["GET"])
def health():
    return {"status": "ok", "upstream": OPENAI_BASE, "model": OPENAI_MODEL}


def _stop_reason(finish_reason: str) -> str:
    mapping = {
        "stop": "end_turn",
        "length": "max_tokens",
        "tool_calls": "tool_use",
    }
    return mapping.get(finish_reason, "end_turn")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
    print(f"Anthropic proxy listening on :{port}")
    print(f"  → upstream: {OPENAI_BASE}")
    print(f"  → model:    {OPENAI_MODEL}")
    print(f"Configure Claude Code:")
    print(f"  cc config set ANTHROPIC_BASE_URL http://localhost:{port}")
    print(f"  cc config set ANTHROPIC_AUTH_TOKEN sk-any")
    print(f"  cc switch {OPENAI_MODEL}")
    app.run(host="0.0.0.0", port=port, debug=False)
