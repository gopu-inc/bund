"""Serveurs MCP configurés (format `mcpServers`, identique à Claude Desktop / Claude Code) : liste, ajout, retrait."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Optional

from .config import bund_home


def path() -> Path:
    return bund_home() / "mcp.json"


def load() -> Dict[str, dict]:
    try:
        data = json.loads(path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    servers = data.get("mcpServers") or data.get("servers") or {}
    return {k: v for k, v in servers.items() if isinstance(v, dict)} if isinstance(servers, dict) else {}


def _save(servers: Dict[str, dict]) -> None:
    p = path()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"mcpServers": servers}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def add(name: str, target: str, args: Optional[List[str]] = None) -> None:
    servers = load()
    servers[name] = {"url": target} if target.startswith(("http://", "https://")) else {"command": target, "args": args or []}
    _save(servers)


def remove(name: str) -> bool:
    servers = load()
    if name not in servers:
        return False
    del servers[name]
    _save(servers)
    return True


def describe() -> str:
    servers = load()
    out = ["**Serveurs MCP**", ""]
    if not servers:
        out.append("_Aucun serveur configuré._")
    for name, s in servers.items():
        if "url" in s:
            out.append(f"- **{name}** · http · `{s['url']}`")
        else:
            out.append(f"- **{name}** · stdio · `{' '.join([s.get('command', '?')] + [str(a) for a in s.get('args', [])])}`")
    out += ["", "`/mcp add NOM COMMANDE [ARGS…]` ou `/mcp add NOM https://url` · `/mcp rm NOM`",
            f"Fichier : `{path()}`", "",
            "_Pour l'instant BUND enregistre et affiche vos serveurs ; l'appel de leurs outils par l'agent n'est pas encore branché._"]
    return "\n".join(out)
