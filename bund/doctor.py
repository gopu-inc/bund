"""Diagnostic de connexion : `bund doctor` ou `/doctor`. Dit précisément où ça bloque (DNS, TCP, clé, flux, outils)."""
from __future__ import annotations

import asyncio
import socket
import sys
import time
import urllib.parse
from typing import Callable

from . import llm
from .config import VERSION, Config, bund_home
from .events import StreamError, TextDelta, ThinkDelta, ToolCall
from .protocol import TOOL_SPECS


async def _probe(cfg: Config, say: Callable[[str], None], user: str, tools, label: str) -> None:
    t0, first, counts = time.monotonic(), None, {"réflexion": 0, "texte": 0, "outil": 0}
    err = None
    names = []

    async def consume() -> None:
        nonlocal first, err
        async for ev in llm.stream_chat(cfg, "Tu es concis.", user, tools=tools):
            if isinstance(ev, (ThinkDelta, TextDelta, ToolCall)) and first is None:
                first = time.monotonic() - t0
            if isinstance(ev, ThinkDelta):
                counts["réflexion"] += len(ev.text)
            elif isinstance(ev, TextDelta):
                counts["texte"] += len(ev.text)
            elif isinstance(ev, ToolCall):
                counts["outil"] += 1
                names.append(ev.name)
            elif isinstance(ev, StreamError):
                err = ev.message

    try:
        await asyncio.wait_for(consume(), max(cfg.timeout, 30))
    except asyncio.TimeoutError:
        say(f"✖ {label} : aucune fin de réponse après {cfg.timeout}s")
        return
    if err:
        say(f"✖ {label} : {err}")
    else:
        say(f"✔ {label} : 1er jeton en {first:.1f}s · réflexion {counts['réflexion']} car. · texte {counts['texte']} car."
            + (f" · outils appelés : {', '.join(names)}" if names else ""))


async def run(cfg: Config, say: Callable[[str], None]) -> None:
    say(f"bund {VERSION} · python {sys.version.split()[0]} · données : {bund_home()}")
    say(f"fournisseur {cfg.provider} · modèle {cfg.model} · {cfg.base_url}")
    u = urllib.parse.urlsplit(cfg.base_url)
    host, port = u.hostname or "", u.port or (443 if u.scheme == "https" else 80)
    try:
        t = time.monotonic()
        await asyncio.to_thread(socket.getaddrinfo, host, port)
        say(f"✔ DNS {host} ({time.monotonic() - t:.1f}s)")
        t = time.monotonic()
        r, w = await asyncio.wait_for(asyncio.open_connection(host, port, ssl=(u.scheme == "https") or None), 15)
        w.close()
        say(f"✔ connexion {'TLS ' if u.scheme == 'https' else ''}{host}:{port} ({time.monotonic() - t:.1f}s)")
    except Exception as e:
        say(f"✖ réseau vers {host}:{port} : {type(e).__name__} {e}")
        return
    if cfg.mode != "local" and cfg.api_key_env and not cfg.api_key():
        say(f"✖ clé absente : exporte {cfg.api_key_env}=… (ou /preset ollama pour un modèle local)")
        return
    say("… test du flux")
    await _probe(cfg, say, "Réponds uniquement : OK", None, "flux")
    if cfg.native_tools:
        say("… test des outils natifs")
        await _probe(cfg, say, "Appelle l'outil list_files avec path='.'.", TOOL_SPECS, "outils natifs")
        say("(si les outils natifs échouent : /conf set native_tools false)")
