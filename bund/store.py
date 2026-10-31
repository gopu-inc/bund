"""Sessions (JSON) et mémoire (notes Markdown) sous ~/.bund — logique simple et sans dépendance."""
from __future__ import annotations

import json
import re
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from .config import bund_home


# ----------------------------------------------------------------------------- sessions
@dataclass
class SessionInfo:
    id: str
    title: str
    updated: float


class Session:
    def __init__(self, sid: Optional[str] = None, title: str = "") -> None:
        self.id = sid or time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:4]
        self.title = title
        self.messages: List[dict] = []

    @staticmethod
    def dir() -> Path:
        return bund_home() / "sessions"

    @property
    def path(self) -> Path:
        return self.dir() / f"{self.id}.json"

    def record(self, role: str, text: str) -> None:
        if not self.title and role == "user":
            self.title = text.strip().splitlines()[0][:80] if text.strip() else ""
        self.messages.append({"role": role, "text": text, "ts": time.time()})
        self.save()

    def save(self) -> None:
        try:
            self.dir().mkdir(parents=True, exist_ok=True)
            self.path.write_text(json.dumps({"id": self.id, "title": self.title, "messages": self.messages},
                                            ensure_ascii=False), encoding="utf-8")
        except OSError:
            pass

    @classmethod
    def load(cls, sid: str) -> Optional["Session"]:
        try:
            d = json.loads((cls.dir() / f"{sid}.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        s = cls(d.get("id", sid), d.get("title", ""))
        s.messages = d.get("messages", [])
        return s

    @classmethod
    def list(cls, limit: int = 30) -> List[SessionInfo]:
        out: List[SessionInfo] = []
        d = cls.dir()
        if not d.exists():
            return out
        for p in d.glob("*.json"):
            try:
                j = json.loads(p.read_text(encoding="utf-8"))
                out.append(SessionInfo(j.get("id", p.stem), j.get("title") or "(sans titre)", p.stat().st_mtime))
            except (OSError, ValueError):
                continue
        out.sort(key=lambda s: s.updated, reverse=True)
        return out[:limit]

    def context(self, max_chars: int = 3000) -> str:
        """Résumé des derniers échanges, à titre de référence (donnée non fiable)."""
        lines = []
        for m in self.messages[-8:]:
            t = m["text"].strip().replace("\n", " ")
            lines.append(f"{m['role']}: {t[:400]}")
        s = "\n".join(lines)
        return s[-max_chars:]


# ----------------------------------------------------------------------------- mémoire
def memory_dir() -> Path:
    return bund_home() / "memory"


def _slug(name: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_-]+", "-", name.strip().lower()).strip("-")
    return s[:48] or "note"


def memory_add(slug: str, text: str) -> Path:
    d = memory_dir()
    d.mkdir(parents=True, exist_ok=True)
    p = d / f"{_slug(slug)}.md"
    with p.open("a", encoding="utf-8") as f:
        f.write(text.strip() + "\n")
    return p


def memory_list() -> List[str]:
    d = memory_dir()
    return sorted(p.stem for p in d.glob("*.md")) if d.exists() else []


def memory_context(max_chars: int = 2500) -> str:
    d = memory_dir()
    if not d.exists():
        return ""
    chunks = []
    for p in sorted(d.glob("*.md")):
        try:
            chunks.append(f"[{p.stem}] " + p.read_text(encoding="utf-8").strip().replace("\n", " "))
        except OSError:
            pass
    return "\n".join(chunks)[:max_chars]


def project_context(root: Path, max_chars: int = 3000) -> str:
    """BUND.md à la racine du projet (consignes du projet)."""
    p = root / "BUND.md"
    try:
        return p.read_text(encoding="utf-8")[:max_chars]
    except OSError:
        return ""
