# Plugin Cookbook — Plugin Studio: a local web app around recipe.py.
# SPDX-License-Identifier: MIT
"""
Describe an effect in a chat, let an AI write the recipe, edit it, then test, build, install and open
the standalone app with one click each.

    python3 tools/recipe.py studio [--port 8765] [--no-browser]

Python standard library only. The AI runs through one of:
  - Claude Code (`claude` on PATH, signed in): uses your Claude subscription, no API key.
  - The Claude API: needs ANTHROPIC_API_KEY (or an `ant auth login` profile) and `pip install anthropic`.

The server listens on 127.0.0.1 only and every API call must carry a per-run token, so other web pages
open in your browser cannot drive it.
"""

from __future__ import annotations

import http.server
import json
import os
import platform
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from pathlib import Path
from urllib.parse import parse_qs, urlparse

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

ROOT = Path(__file__).resolve().parents[2]
RECIPE_PY = ROOT / "tools" / "recipe.py"
PROMPT = ROOT / "prompts" / "plugin-from-idea.md"
STATIC = Path(__file__).resolve().parent / "static"
BIN = ROOT / "build" / "bin"

ID_RE = re.compile(r"^[a-z][a-z0-9-]{0,62}$")
API_MODEL = "claude-opus-5-5"

STUDIO_NOTES = """

---

## You are running inside Plugin Studio

Plugin Studio is a local app. The user chats with you in one panel; the recipe lives in an editor next
to it. Whenever you output a complete recipe in a ```toml block, the studio puts it in the editor,
saves it as plugins/<id>.plugin.toml, and runs the test suite automatically. Build, install and the
standalone app are buttons in the studio.

- Skip "Step 4": don't explain how to save the file, use GitHub or run commands.
- When the user's message includes the current recipe, treat it as the latest version (they may have
  edited it by hand) and revise that one.
- When the user sends test output or compiler errors, fix the recipe and resend the whole file. Keep the
  explanation to a sentence or two.
- Keep chat text short and reply in the user's language. Code, identifiers and comments in the recipe
  stay in English.
"""


# ---------------------------------------------------------------------------------------------------------------------
# Jobs: a process or an AI call whose output streams to the browser


class Job:
    def __init__(self, kind: str, recipe_id: str | None):
        self.id = secrets.token_hex(6)
        self.kind = kind
        self.recipe_id = recipe_id
        self.events: list[dict] = []
        self.done = False
        self.cond = threading.Condition()
        self.proc: subprocess.Popen | None = None
        self.cancelled = False

    def emit(self, event: dict) -> None:
        with self.cond:
            self.events.append(event)
            self.cond.notify_all()

    def finish(self, ok: bool, **extra) -> None:
        with self.cond:
            self.events.append({"type": "done", "ok": ok, **extra})
            self.done = True
            self.cond.notify_all()

    def cancel(self) -> None:
        self.cancelled = True
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


JOBS: dict[str, Job] = {}
JOBS_LOCK = threading.Lock()
BUILD_LOCK = threading.Lock()  # check/test/build/install share build/ and the harness work dir


def start_job(kind: str, recipe_id: str | None, target) -> Job:
    job = Job(kind, recipe_id)
    with JOBS_LOCK:
        # keep the last few finished jobs around for late subscribers
        finished = [j for j in JOBS.values() if j.done]
        for old in finished[:-20]:
            JOBS.pop(old.id, None)
        JOBS[job.id] = job
    threading.Thread(target=target, args=(job,), daemon=True).start()
    return job


def run_process(job: Job, cmd: list[str], cwd: Path = ROOT) -> int:
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    job.emit({"type": "line", "text": "$ " + " ".join(cmd)})
    try:
        job.proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace", bufsize=1)
    except OSError as e:
        job.emit({"type": "line", "text": f"ERROR could not start {cmd[0]}: {e}"})
        return 127
    assert job.proc.stdout
    for line in job.proc.stdout:
        job.emit({"type": "line", "text": line.rstrip("\n")})
    return job.proc.wait()


def recipe_path(recipe_id: str) -> Path | None:
    for folder in ("plugins", "examples"):
        p = ROOT / folder / f"{recipe_id}.plugin.toml"
        if p.exists():
            return p
    return None


def app_path(recipe_id: str) -> Path | None:
    system = platform.system()
    candidates = {
        "Darwin": [BIN / f"{recipe_id}.app"],
        "Windows": [BIN / f"{recipe_id}.exe"],
    }.get(system, [BIN / recipe_id])
    for c in candidates:
        if c.exists():
            return c
    return None


def tool_job(kind: str, recipe_id: str | None):
    py = sys.executable

    def target(job: Job) -> None:
        with BUILD_LOCK:
            if job.cancelled:
                job.finish(False, cancelled=True)
                return
            path = recipe_path(recipe_id) if recipe_id else None
            if kind in ("check", "test", "build") and not path:
                job.emit({"type": "line", "text": f"ERROR no recipe named {recipe_id!r}"})
                job.finish(False)
                return
            if kind == "check":
                code = run_process(job, [py, str(RECIPE_PY), "check", str(path)])
            elif kind == "test":
                code = run_process(job, [py, str(RECIPE_PY), "test", str(path)])
            elif kind == "build":
                code = run_process(job, [py, str(RECIPE_PY), "build", str(path)])
            elif kind == "install":
                code = run_process(job, [py, str(RECIPE_PY), "install"])
            elif kind == "open":
                code = open_app(job, recipe_id or "")
            else:
                job.emit({"type": "line", "text": f"ERROR unknown action {kind!r}"})
                code = 1
            job.finish(code == 0 and not job.cancelled, code=code, cancelled=job.cancelled)

    return target


def open_app(job: Job, recipe_id: str) -> int:
    app = app_path(recipe_id)
    if not app:
        job.emit({"type": "line", "text": "The standalone app isn't built yet: click Build first."})
        return 1
    job.emit({"type": "line", "text": f"Opening {app.name}"})
    if platform.system() == "Darwin":
        return subprocess.call(["open", str(app)])
    kwargs = {"cwd": str(BIN), "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    if platform.system() == "Windows":
        kwargs["creationflags"] = 0x00000008  # DETACHED_PROCESS
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen([str(app)], **kwargs)
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# AI backends


def system_prompt() -> str:
    return PROMPT.read_text(encoding="utf-8") + STUDIO_NOTES


def claude_cli() -> str | None:
    return shutil.which("claude")


def api_available() -> tuple[bool, str]:
    try:
        import anthropic  # noqa: F401
    except ModuleNotFoundError:
        return False, "pip install anthropic"
    if os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN") or shutil.which("ant"):
        return True, ""
    return False, "set ANTHROPIC_API_KEY"


def backends() -> dict:
    api_ok, api_hint = api_available()
    return {
        "claude": {"available": bool(claude_cli()), "hint": "" if claude_cli() else "install Claude Code"},
        "api": {"available": api_ok, "hint": api_hint, "model": API_MODEL},
    }


def build_messages(messages: list[dict], recipe: str | None) -> list[dict]:
    """The chat as the API sees it. The editor's recipe rides along with the newest user turn only, so
    earlier turns stay byte-identical between requests and the prompt cache keeps hitting."""
    out = [{"role": m["role"], "content": m["content"]} for m in messages if m.get("content")]
    if out and out[-1]["role"] == "user" and recipe and recipe.strip():
        out[-1] = {
            "role": "user",
            "content": "Current recipe in the editor:\n```toml\n" + recipe.strip() + "\n```\n\n" + out[-1]["content"],
        }
    return out


def ai_job(messages: list[dict], recipe: str | None, backend: str):
    def target(job: Job) -> None:
        chosen = backend
        if chosen == "auto":
            info = backends()
            chosen = "claude" if info["claude"]["available"] else "api" if info["api"]["available"] else ""
        if not chosen:
            job.emit({"type": "error", "text": "No AI available. Install and sign in to Claude Code, or set "
                                               "ANTHROPIC_API_KEY and run `pip install anthropic`."})
            job.finish(False)
            return
        job.emit({"type": "backend", "name": chosen})
        msgs = build_messages(messages, recipe)
        try:
            text = run_claude_cli(job, msgs) if chosen == "claude" else run_api(job, msgs)
        except Exception as e:  # report anything to the chat instead of killing the thread
            job.emit({"type": "error", "text": f"{type(e).__name__}: {e}"})
            job.finish(False)
            return
        if text is None:
            job.finish(False, cancelled=job.cancelled)
        else:
            job.finish(True, text=text)

    return target


def run_claude_cli(job: Job, messages: list[dict]) -> str | None:
    # `claude -p` is one-shot, so the conversation goes in as a transcript.
    parts = []
    for m in messages[:-1]:
        parts.append(("User" if m["role"] == "user" else "Assistant") + ":\n" + m["content"])
    transcript = ""
    if parts:
        transcript = "Conversation so far:\n\n" + "\n\n".join(parts) + "\n\n---\n\nThe user's new message:\n\n"
    prompt = transcript + messages[-1]["content"]

    cmd = [claude_cli() or "claude", "-p", "--output-format", "stream-json", "--include-partial-messages",
           "--verbose", "--system-prompt", system_prompt(), "--tools", "", "--no-session-persistence",
           "--strict-mcp-config"]
    if os.environ.get("STUDIO_CLAUDE_MODEL"):
        cmd += ["--model", os.environ["STUDIO_CLAUDE_MODEL"]]

    # Run outside the repository so no project instructions (CLAUDE.md) leak into the prompt.
    cwd = Path.home()
    job.proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    assert job.proc.stdin and job.proc.stdout
    job.proc.stdin.write(prompt)
    job.proc.stdin.close()

    streamed: list[str] = []
    final_text: str | None = None
    error: str | None = None
    for line in job.proc.stdout:
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue
        kind = ev.get("type")
        if kind == "stream_event":
            inner = ev.get("event", {})
            delta = inner.get("delta", {})
            if inner.get("type") == "content_block_delta" and delta.get("type") == "text_delta":
                streamed.append(delta.get("text", ""))
                job.emit({"type": "delta", "text": delta.get("text", "")})
        elif kind == "result":
            if ev.get("is_error"):
                error = str(ev.get("result") or ev.get("subtype") or "Claude Code reported an error")
            else:
                final_text = ev.get("result")
    stderr = job.proc.stderr.read() if job.proc.stderr else ""
    code = job.proc.wait()
    if job.cancelled:
        return None
    if error or code != 0:
        msg = error or stderr.strip() or f"claude exited with code {code}"
        if "authenticat" in msg.lower() or "login" in msg.lower() or "oauth" in msg.lower():
            msg += "\n\nSign in to Claude Code again: run `claude auth login` in a terminal, then try again."
        job.emit({"type": "error", "text": msg})
        return None
    return final_text if final_text is not None else "".join(streamed)


def run_api(job: Job, messages: list[dict]) -> str | None:
    try:
        import anthropic
    except ModuleNotFoundError:
        job.emit({"type": "error", "text": "The Claude API needs the SDK: pip install anthropic"})
        return None

    client = anthropic.Anthropic()
    try:
        with client.beta.messages.stream(
            model=API_MODEL,
            max_tokens=64000,
            output_config={"effort": "high"},
            # Large, frozen system prompt first; cached across turns.
            system=[{"type": "text", "text": system_prompt(), "cache_control": {"type": "ephemeral"}}],
            messages=messages,
            # If a safety classifier declines, retry on the server's default fallback model.
            betas=["server-side-fallback-2026-07-01"],
            fallbacks="default",
        ) as stream:
            for text in stream.text_stream:
                if job.cancelled:
                    return None
                job.emit({"type": "delta", "text": text})
            message = stream.get_final_message()
    except anthropic.AuthenticationError:
        job.emit({"type": "error", "text": "The API key was rejected. Check ANTHROPIC_API_KEY."})
        return None
    except anthropic.RateLimitError:
        job.emit({"type": "error", "text": "Rate limited by the Claude API. Wait a moment and try again."})
        return None
    except anthropic.APIStatusError as e:
        job.emit({"type": "error", "text": f"Claude API error {e.status_code}: {e.message}"})
        return None
    except anthropic.APIConnectionError:
        job.emit({"type": "error", "text": "Could not reach the Claude API. Check your connection."})
        return None

    if message.stop_reason == "refusal":
        job.emit({"type": "error", "text": "Claude declined this request."})
        return None
    text = "".join(b.text for b in message.content if b.type == "text")
    if message.stop_reason == "max_tokens":
        job.emit({"type": "error", "text": "The reply was cut off (too long)."})
    return text


# ---------------------------------------------------------------------------------------------------------------------
# Recipes


def list_recipes() -> list[dict]:
    out = []
    for folder in ("plugins", "examples"):
        for p in sorted((ROOT / folder).glob("*.plugin.toml")):
            rid = p.name[: -len(".plugin.toml")]
            name = rid
            try:
                name = tomllib.loads(p.read_text(encoding="utf-8")).get("plugin", {}).get("name", rid)
            except (tomllib.TOMLDecodeError, OSError):
                pass
            out.append({"id": rid, "name": name, "source": folder, "app": bool(app_path(rid))})
    return out


def save_recipe(text: str) -> tuple[str | None, str | None]:
    """Saves into plugins/ under the id written in the recipe. Returns (id, error)."""
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        return None, f"Not valid TOML: {e}"
    rid = str(data.get("plugin", {}).get("id", ""))
    if not ID_RE.match(rid):
        return None, "The recipe needs [plugin] id = \"...\" (lowercase letters, digits and dashes)."
    dest = ROOT / "plugins" / f"{rid}.plugin.toml"
    dest.parent.mkdir(exist_ok=True)
    dest.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
    return rid, None


# ---------------------------------------------------------------------------------------------------------------------
# HTTP


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "PluginStudio"
    token = ""

    def log_message(self, fmt, *args):  # quiet
        pass

    # -- helpers

    def _host_ok(self) -> bool:
        # Refuse DNS-rebinding attempts: only our own loopback names.
        host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
        return host in ("127.0.0.1", "localhost", "[::1]")

    def _token_ok(self, query: dict) -> bool:
        given = self.headers.get("X-Studio-Token") or (query.get("token") or [""])[0]
        return secrets.compare_digest(given, self.token)

    def _json(self, code: int, payload) -> None:
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > 4_000_000:
            return {}
        try:
            return json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            return {}

    # -- routes

    def do_GET(self):
        if not self._host_ok():
            return self._json(403, {"error": "forbidden host"})
        url = urlparse(self.path)
        query = parse_qs(url.query)

        if url.path in ("/", "/index.html"):
            html = (STATIC / "index.html").read_text(encoding="utf-8").replace("__STUDIO_TOKEN__", self.token)
            body = html.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy",
                             "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'")
            self.end_headers()
            self.wfile.write(body)
            return

        if not self._token_ok(query):
            return self._json(403, {"error": "bad token"})

        if url.path == "/api/state":
            return self._json(200, {"recipes": list_recipes(), "backends": backends(), "platform": platform.system()})

        if url.path == "/api/recipe":
            rid = (query.get("id") or [""])[0]
            path = recipe_path(rid) if ID_RE.match(rid) else None
            if not path:
                return self._json(404, {"error": "no such recipe"})
            return self._json(200, {"id": rid, "text": path.read_text(encoding="utf-8"),
                                    "source": path.parent.name})

        m = re.fullmatch(r"/api/events/([0-9a-f]+)", url.path)
        if m:
            return self._stream(m.group(1))

        return self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self._host_ok():
            return self._json(403, {"error": "forbidden host"})
        url = urlparse(self.path)
        if not self._token_ok(parse_qs(url.query)):
            return self._json(403, {"error": "bad token"})
        body = self._body()

        if url.path == "/api/recipe":
            rid, err = save_recipe(str(body.get("text", "")))
            if err:
                return self._json(400, {"error": err})
            return self._json(200, {"id": rid})

        if url.path == "/api/run":
            kind = str(body.get("kind", ""))
            rid = body.get("id")
            if kind not in ("check", "test", "build", "install", "open"):
                return self._json(400, {"error": "unknown action"})
            if rid is not None and not ID_RE.match(str(rid)):
                return self._json(400, {"error": "bad id"})
            job = start_job(kind, rid, tool_job(kind, rid))
            return self._json(200, {"job": job.id})

        if url.path == "/api/ai":
            messages = body.get("messages") or []
            if not isinstance(messages, list) or not messages or messages[-1].get("role") != "user":
                return self._json(400, {"error": "messages must end with a user turn"})
            clean = [{"role": "assistant" if m.get("role") == "assistant" else "user",
                      "content": str(m.get("content", ""))} for m in messages if isinstance(m, dict)]
            backend = str(body.get("backend", "auto"))
            if backend not in ("auto", "claude", "api"):
                backend = "auto"
            job = start_job("ai", None, ai_job(clean, body.get("recipe"), backend))
            return self._json(200, {"job": job.id})

        m = re.fullmatch(r"/api/cancel/([0-9a-f]+)", url.path)
        if m:
            job = JOBS.get(m.group(1))
            if job:
                job.cancel()
            return self._json(200, {"ok": bool(job)})

        return self._json(404, {"error": "not found"})

    def _stream(self, job_id: str) -> None:
        job = JOBS.get(job_id)
        if not job:
            return self._json(404, {"error": "no such job"})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        sent = 0
        try:
            while True:
                with job.cond:
                    if sent >= len(job.events) and not job.done:
                        job.cond.wait(timeout=15)
                    batch = job.events[sent:]
                    finished = job.done  # the "done" event is appended together with this flag
                if not batch:
                    self.wfile.write(b": keep-alive\n\n")
                for ev in batch:
                    self.wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
                self.wfile.flush()
                sent += len(batch)
                if finished:
                    return
        except (BrokenPipeError, ConnectionResetError):
            return


def serve(port: int = 8765, open_browser: bool = True) -> int:
    Handler.token = secrets.token_urlsafe(24)
    server = None
    for p in (port, 0):
        try:
            server = http.server.ThreadingHTTPServer(("127.0.0.1", p), Handler)
            break
        except OSError:
            continue
    if server is None:
        print("could not open a local port", file=sys.stderr)
        return 1
    server.daemon_threads = True
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    info = backends()
    print(f"Plugin Studio running at {url}  (Ctrl+C to stop)")
    print("  AI: Claude Code " + ("found" if info["claude"]["available"] else "not found") +
          ", Claude API " + ("ready" if info["api"]["available"] else f"not set up ({info['api']['hint']})"))
    if open_browser:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        for job in list(JOBS.values()):
            job.cancel()
        server.server_close()
    return 0
