"""Client LLM asynchrone avec streaming SSE (OpenAI-compatible + Anthropic natif).

Aucune dépendance externe : la lecture réseau tourne dans un thread démon qui pousse les lignes
dans une `asyncio.Queue` ; la boucle d'événements (Textual) n'est donc jamais bloquée.
"""
from __future__ import annotations

import asyncio
import json
import socket
import threading
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import AsyncIterator, Dict, List, Optional, Tuple, Union

from .config import Config, bund_home
from .events import Notice, StreamEnd, StreamError, TextDelta, ThinkDelta, ToolCall

Event = Union[ThinkDelta, TextDelta, ToolCall, Notice, StreamError, StreamEnd]


# ---------------------------------------------------------------- analyse d'un évènement
THINK_KEYS = ("reasoning_content", "reasoning", "thinking", "think", "reasoning_text")


def think_of(d: object) -> str:
    """Détecte la réflexion dans un morceau JSON, quel que soit le fournisseur
    (`reasoning_content`, `reasoning`, `thinking`…, ou `reasoning_details` d'OpenRouter)."""
    if not isinstance(d, dict):
        return ""
    for k in THINK_KEYS:
        v = d.get(k)
        if isinstance(v, str) and v:
            return v
        if isinstance(v, dict):
            t = v.get("content") or v.get("text") or v.get("summary")
            if isinstance(t, str) and t:
                return t
    det = d.get("reasoning_details")
    if isinstance(det, list):
        return "".join((x.get("text") or x.get("summary") or "") for x in det if isinstance(x, dict))
    return ""


def text_of(v: object) -> str:
    """Le contenu peut être une chaîne ou une liste de parties {type:'text', text:…}."""
    if isinstance(v, str):
        return v
    if isinstance(v, list):
        return "".join(p.get("text", "") for p in v if isinstance(p, dict) and isinstance(p.get("text"), str))
    return ""


def loads_args(raw: object) -> dict:
    if isinstance(raw, dict):
        return raw
    if not isinstance(raw, str) or not raw.strip():
        return {}
    try:
        obj = json.loads(raw)
        return obj if isinstance(obj, dict) else {"_raw": raw}
    except ValueError:
        return {"_raw": raw}


@dataclass
class Parsed:
    think: Optional[str] = None
    text: Optional[str] = None
    done: bool = False
    error: Optional[str] = None
    calls: List[ToolCall] = field(default_factory=list)


class StreamParser:
    """Analyse les évènements d'un flux : OpenAI-compatible (SSE), Anthropic (SSE) et Ollama natif (NDJSON).
    Les appels d'outils arrivent en morceaux : ils sont assemblés puis rendus complets."""

    def __init__(self, anthropic: bool = False) -> None:
        self.anthropic = anthropic
        self.partial: Dict[int, dict] = {}

    def finish(self) -> List[ToolCall]:
        out = [ToolCall(c["name"], loads_args(c["args"]), c["id"])
               for _, c in sorted(self.partial.items()) if c["name"]]
        self.partial = {}
        return out

    def feed(self, data: str) -> Parsed:
        data = data.strip()
        if not data:
            return Parsed()
        if data == "[DONE]":
            return Parsed(done=True, calls=self.finish())
        try:
            obj = json.loads(data)
        except ValueError:
            return Parsed()
        if not isinstance(obj, dict):
            return Parsed()
        if self.anthropic or obj.get("type") in ("content_block_delta", "content_block_start", "message_stop"):
            return self._anthropic(obj, data)
        if "choices" not in obj and isinstance(obj.get("message"), dict):
            return self._ollama(obj)
        return self._openai(obj, data)

    # -- Anthropic
    def _anthropic(self, obj: dict, data: str) -> Parsed:
        t, idx = obj.get("type"), obj.get("index", 0)
        if t == "content_block_start":
            cb = obj.get("content_block") or {}
            if cb.get("type") == "tool_use":
                self.partial[idx] = {"id": cb.get("id", ""), "name": cb.get("name", ""), "args": ""}
        elif t == "content_block_delta":
            d = obj.get("delta") or {}
            dt = d.get("type")
            if dt == "text_delta":
                return Parsed(text=d.get("text") or "")
            if dt == "thinking_delta":
                return Parsed(think=d.get("thinking") or "")
            if dt == "input_json_delta" and idx in self.partial:
                self.partial[idx]["args"] += d.get("partial_json") or ""
        elif t == "message_stop":
            return Parsed(done=True, calls=self.finish())
        elif t == "error":
            e = obj.get("error") or {}
            return Parsed(error=e.get("message") or data)
        return Parsed()

    # -- Ollama natif (/api/chat, une ligne JSON par évènement)
    def _ollama(self, obj: dict) -> Parsed:
        m = obj["message"]
        out = Parsed(think=think_of(m) or None, text=text_of(m.get("content")) or None)
        for tc in m.get("tool_calls") or []:
            f = (tc or {}).get("function") or {}
            if f.get("name"):
                out.calls.append(ToolCall(f["name"], loads_args(f.get("arguments")), str(tc.get("id") or "")))
        if obj.get("done"):
            out.done = True
        return out

    # -- OpenAI et compatibles
    def _openai(self, obj: dict, data: str) -> Parsed:
        choices = obj.get("choices") or []
        if not choices and obj.get("error"):
            e = obj["error"]
            return Parsed(error=(e.get("message") if isinstance(e, dict) else str(e)) or data)
        out = Parsed()
        for ch in choices:
            d = ch.get("delta") or ch.get("message") or {}
            th, tx = think_of(d), text_of(d.get("content"))
            if th:
                out.think = (out.think or "") + th
            if tx:
                out.text = (out.text or "") + tx
            for tc in d.get("tool_calls") or []:
                slot = self.partial.setdefault(tc.get("index", 0), {"id": "", "name": "", "args": ""})
                if tc.get("id"):
                    slot["id"] = tc["id"]
                f = tc.get("function") or {}
                if f.get("name") and not slot["name"]:
                    slot["name"] = f["name"]
                a = f.get("arguments")
                if isinstance(a, str):
                    slot["args"] += a
                elif isinstance(a, dict):
                    slot["args"] = json.dumps(a)
            if ch.get("finish_reason") and self.partial:
                out.calls += self.finish()
        return out


def parse_data(data: str, anthropic: bool) -> Parsed:
    """Compatibilité : analyse isolée d'une ligne `data: ...` (sans assemblage d'appels d'outils)."""
    return StreamParser(anthropic).feed(data)


# ---------------------------------------------------------------- séparation <think> … </think>
class ThinkSplitter:
    """Sépare en direct le texte et les blocs <think>…</think>, même coupés entre deux jetons."""

    OPEN, CLOSE = "<think>", "</think>"

    def __init__(self) -> None:
        self.buf = ""
        self.in_think = False

    def feed(self, chunk: str) -> List[Tuple[str, str]]:
        self.buf += chunk
        out: List[Tuple[str, str]] = []
        while self.buf:
            tag = self.CLOSE if self.in_think else self.OPEN
            kind = "think" if self.in_think else "text"
            i = self.buf.find(tag)
            if i >= 0:
                if i:
                    out.append((kind, self.buf[:i]))
                self.buf = self.buf[i + len(tag):]
                self.in_think = not self.in_think
                continue
            keep = 0                                   # suffixe pouvant être le début d'une balise
            for n in range(min(len(tag) - 1, len(self.buf)), 0, -1):
                if tag.startswith(self.buf[-n:]):
                    keep = n
                    break
            emit = self.buf[:len(self.buf) - keep]
            if emit:
                out.append((kind, emit))
            self.buf = self.buf[len(self.buf) - keep:]
            break
        return out

    def flush(self) -> List[Tuple[str, str]]:
        out = [("think" if self.in_think else "text", self.buf)] if self.buf else []
        self.buf = ""
        return out


# ---------------------------------------------------------------- requête
def _tool_defs(cfg: Config, tools: List[dict]) -> List[dict]:
    if cfg.is_anthropic:
        return [{"name": t["name"], "description": t["description"], "input_schema": t["parameters"]} for t in tools]
    return [{"type": "function", "function": t} for t in tools]


def build_request(cfg: Config, system: str, user: str, stream: bool,
                  tools: Optional[List[dict]] = None) -> Tuple[str, dict, bytes]:
    anth = cfg.is_anthropic
    headers = {"Content-Type": "application/json", "Accept": "text/event-stream" if stream else "application/json"}
    key = cfg.api_key()
    base = cfg.base_url.rstrip("/")
    if anth:
        headers["anthropic-version"] = "2023-06-01"
        if key:
            headers["x-api-key"] = key
        body: dict = {"model": cfg.model, "max_tokens": 4096, "system": system,
                      "messages": [{"role": "user", "content": user}]}
        if cfg.think_budget >= 1024:
            body["max_tokens"] = cfg.think_budget + 4096
            body["thinking"] = {"type": "enabled", "budget_tokens": cfg.think_budget}
        url = base + "/messages"
    else:
        if key:
            headers["Authorization"] = f"Bearer {key}"
        body = {"model": cfg.model, "temperature": 0.2,
                "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]}
        if cfg.is_ollama_native:
            body = {"model": cfg.model, "options": {"temperature": 0.2}, "messages": body["messages"]}
            if cfg.think_budget > 0:
                body["think"] = True
            url = base + "/chat"
        else:
            url = base + "/chat/completions"
    if tools:
        body["tools"] = _tool_defs(cfg, tools)
    body["stream"] = bool(stream)
    return url, headers, json.dumps(body).encode("utf-8")


def _error_message(raw: bytes) -> str:
    txt = raw.decode("utf-8", "replace").strip()
    try:
        obj = json.loads(txt)
        e = obj.get("error", obj)
        if isinstance(e, dict):
            return str(e.get("message") or txt)[:600]
        if isinstance(e, str):
            return e[:600]
    except ValueError:
        pass
    return txt[:600] or "pas de réponse"


def _parse_full_json(raw: bytes, anthropic: bool) -> Tuple[str, str, List[ToolCall]]:
    """Réponse non streamée -> (réflexion, texte, appels d'outils). Lève ValueError si c'est une erreur."""
    obj = json.loads(raw.decode("utf-8", "replace"))
    if not isinstance(obj, dict):
        raise ValueError("réponse inattendue")
    calls: List[ToolCall] = []
    if anthropic or obj.get("type") == "message":
        blocks = obj.get("content") or []
        think = "".join(b.get("thinking", "") for b in blocks if b.get("type") == "thinking")
        text = "".join(b.get("text", "") for b in blocks if b.get("type") == "text")
        for b in blocks:
            if b.get("type") == "tool_use":
                calls.append(ToolCall(b.get("name", ""), loads_args(b.get("input")), b.get("id", "")))
        if obj.get("type") == "error":
            raise ValueError("erreur")
        return think, text, calls
    if "choices" in obj:
        m = (obj.get("choices") or [{}])[0].get("message") or {}
        for tc in m.get("tool_calls") or []:
            f = (tc or {}).get("function") or {}
            if f.get("name"):
                calls.append(ToolCall(f["name"], loads_args(f.get("arguments")), str(tc.get("id") or "")))
        return think_of(m), text_of(m.get("content")), calls
    if isinstance(obj.get("message"), dict):                      # Ollama natif sans flux
        m = obj["message"]
        for tc in m.get("tool_calls") or []:
            f = (tc or {}).get("function") or {}
            if f.get("name"):
                calls.append(ToolCall(f["name"], loads_args(f.get("arguments")), ""))
        return think_of(m), text_of(m.get("content")), calls
    raise ValueError("réponse sans contenu")


def _worker(url, headers, body, timeout, loop, q, stop, holder, stream) -> None:
    """Thread démon : lit la réponse HTTP et pousse (kind, payload) dans la file asyncio."""
    def put(item) -> None:
        try:
            loop.call_soon_threadsafe(q.put_nowait, item)
        except RuntimeError:       # boucle déjà fermée
            pass

    try:
        req = urllib.request.Request(url, data=body, headers=headers, method="POST")
        resp = urllib.request.urlopen(req, timeout=timeout)
        holder.append(resp)
        ctype = (resp.headers.get("Content-Type") or "").lower()
        if stream and ("event-stream" in ctype or "ndjson" in ctype or "x-json-stream" in ctype):
            for raw in resp:
                if stop.is_set():
                    break
                put(("line", raw.decode("utf-8", "replace")))
        else:
            put(("body", resp.read()))
        put(("end", None))
    except urllib.error.HTTPError as e:
        try:
            raw = e.read()
        except Exception:
            raw = b""
        retry_after = e.headers.get("Retry-After") if e.headers else None
        put(("http_error", (e.code, raw, retry_after)))
    except Exception as e:                                           # réseau, DNS, timeout…
        if not stop.is_set():
            put(("net_error", str(e)))
    finally:
        put(("eof", None))


HEARTBEAT = 8          # secondes sans donnée avant d'afficher « en attente du serveur… »


async def stream_chat(cfg: Config, system: str, user: str,
                      tools: Optional[List[dict]] = None) -> AsyncIterator[Event]:
    """Génère ThinkDelta / TextDelta / ToolCall au fil de l'eau, puis StreamEnd (ou StreamError).
    Notice : informations passagères (attente, nouvelle tentative)."""
    anth = cfg.is_anthropic
    key_missing = cfg.mode != "local" and cfg.api_key_env and not cfg.api_key()
    if key_missing:
        yield StreamError(f"clé absente : exporte {cfg.api_key_env} ou place-la dans {bund_home() / 'secrets' / cfg.api_key_env}")
        return
    stream = cfg.stream
    attempt = 0
    host = cfg.base_url.split("//")[-1].split("/")[0]
    while True:
        url, headers, body = build_request(cfg, system, user, stream, tools)
        loop = asyncio.get_running_loop()
        q: asyncio.Queue = asyncio.Queue()
        stop = threading.Event()
        holder: list = []
        th = threading.Thread(target=_worker, daemon=True,
                              args=(url, headers, body, cfg.timeout, loop, q, stop, holder, stream))
        th.start()
        yield Notice(f"connexion à {host}…")
        parser = StreamParser(anth)
        splitter = ThinkSplitter()
        got_any = False
        failure: Optional[StreamError] = None
        done = False
        waited = 0
        getter: Optional[asyncio.Future] = None
        try:
            while True:
                if getter is None:
                    getter = asyncio.ensure_future(q.get())
                finished, _ = await asyncio.wait({getter}, timeout=HEARTBEAT)
                if not finished:                      # le thread réseau n'a encore rien reçu
                    waited += HEARTBEAT
                    yield Notice(f"en attente de {host}… {waited}s")
                    continue
                kind, payload = getter.result()
                getter = None
                if kind == "eof":
                    break
                if kind == "line":
                    line = payload.strip()
                    if line.startswith("data:"):
                        data = line[5:]
                    elif line.startswith("{"):           # NDJSON (Ollama natif) ou erreur JSON brute
                        data = line
                    else:
                        continue
                    p = parser.feed(data)
                    if p.error:
                        failure = StreamError(p.error)
                        break
                    if p.think:
                        got_any = True
                        yield ThinkDelta(p.think)
                    if p.text:
                        got_any = True
                        for k, s in splitter.feed(p.text):
                            yield ThinkDelta(s) if k == "think" else TextDelta(s)
                    for c in p.calls:
                        got_any = True
                        yield c
                    if p.done:
                        done = True
                        break
                elif kind == "body":
                    try:
                        think, text, calls = _parse_full_json(payload, anth)
                    except ValueError:
                        failure = StreamError(_error_message(payload))
                        continue
                    got_any = True
                    if think:
                        yield ThinkDelta(think)
                    for k, s in ThinkSplitter().feed(text):
                        yield ThinkDelta(s) if k == "think" else TextDelta(s)
                    for c in calls:
                        yield c
                elif kind == "http_error":
                    code, raw, retry_after = payload
                    failure = StreamError(_error_message(raw), status=code,
                                          retryable=code in (408, 429, 500, 502, 503, 504))
                    if retry_after and retry_after.isdigit():
                        failure.message += f" (réessayer dans {retry_after}s)"
                    break
                elif kind == "net_error":
                    failure = StreamError(payload, retryable=True)
                    break
            for k, s in splitter.flush():
                if s:
                    yield ThinkDelta(s) if k == "think" else TextDelta(s)
            for c in parser.finish():                    # appels d'outils non terminés par un finish_reason
                got_any = True
                yield c
        finally:
            if getter is not None and not getter.done():
                getter.cancel()
            stop.set()
            for r in holder:                       # débloque le thread si on annule en plein flux
                try:
                    sock = r.fp.raw._sock           # type: ignore[attr-defined]
                    sock.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    r.close()
                except Exception:
                    pass
        if failure and not got_any:
            if failure.retryable and attempt < cfg.max_retries:
                attempt += 1
                yield Notice(f"{failure.message[:120]} — nouvelle tentative {attempt}/{cfg.max_retries}", chat=True)
                await asyncio.sleep(min(2 * attempt, 8))
                continue
            yield failure
            return
        if failure:
            yield failure
            return
        if not got_any and not done:
            yield StreamError("réponse vide du modèle")
            return
        yield StreamEnd()
        return


async def complete(cfg: Config, system: str, user: str) -> str:
    """Version non interactive : renvoie le texte complet (réflexion encadrée par <think>)."""
    think, text = [], []
    async for ev in stream_chat(cfg, system, user):
        if isinstance(ev, ThinkDelta):
            think.append(ev.text)
        elif isinstance(ev, TextDelta):
            text.append(ev.text)
        elif isinstance(ev, StreamError):
            raise RuntimeError(ev.message)
    t = "".join(think).strip()
    return (f"<think>{t}</think>\n" if t else "") + "".join(text)


def list_models(cfg: Config, timeout: int = 15) -> List[str]:
    """Liste les modèles proposés par le fournisseur (appel bloquant : à lancer dans un thread)."""
    base = cfg.base_url.rstrip("/")
    url = base + ("/tags" if cfg.is_ollama_native else "/models")
    headers = {"Accept": "application/json", "User-Agent": "bund"}
    key = cfg.api_key()
    if cfg.is_anthropic:
        headers["anthropic-version"] = "2023-06-01"
        if key:
            headers["x-api-key"] = key
    elif key:
        headers["Authorization"] = f"Bearer {key}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=timeout) as r:
            obj = json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HTTP {e.code} : {_error_message(e.read())}")
    except Exception as e:
        raise RuntimeError(str(e))
    items = obj.get("data") if isinstance(obj, dict) and "data" in obj else (obj.get("models") if isinstance(obj, dict) else obj)
    ids = []
    for m in items or []:
        if isinstance(m, dict):
            v = m.get("id") or m.get("name") or m.get("model")
            if isinstance(v, str):
                ids.append(v.removeprefix("models/") if hasattr(v, "removeprefix") else v)
        elif isinstance(m, str):
            ids.append(m)
    return sorted(set(ids))
