"""Connexion GitHub : jeton stocké (0600), identité de commit (utilisateur ou « nom[bot] »), vérification via l'API."""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional, Tuple

from .config import Config, bund_home


def token_file() -> Path:
    return bund_home() / "secrets" / "GITHUB_TOKEN"


def save_token(token: str) -> None:
    f = token_file()
    f.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(f), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as h:
        h.write(token.strip())


def clear_token() -> bool:
    try:
        token_file().unlink()
        return True
    except OSError:
        return False


def verify(token: str, timeout: int = 15) -> dict:
    """Renvoie {"login", "id", "name"}. Un jeton d'application GitHub (bot) ne peut pas lire /user : on le tolère."""
    req = urllib.request.Request("https://api.github.com/user", headers={
        "Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json", "User-Agent": "bund"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            obj = json.loads(r.read().decode("utf-8", "replace"))
        return {"login": obj.get("login", ""), "id": obj.get("id", 0), "name": obj.get("name") or ""}
    except urllib.error.HTTPError as e:
        if e.code == 401:
            raise RuntimeError("jeton refusé par GitHub (401) : vérifie qu'il est valide et non expiré")
        if e.code == 403:                                   # jeton d'installation d'une GitHub App
            return {"login": "", "id": 0, "name": ""}
        raise RuntimeError(f"GitHub a répondu HTTP {e.code}")
    except Exception as e:
        raise RuntimeError(f"connexion à GitHub impossible : {e}")


def identity(user: dict, bot: Optional[str] = None) -> Tuple[str, str]:
    """(nom, email) pour les commits : `nom[bot]` si bot est donné, sinon le compte GitHub."""
    if bot:
        n = bot.strip().removesuffix("[bot]")
        return f"{n}[bot]", f"{n}[bot]@users.noreply.github.com"
    login = user.get("login") or "bund"
    uid = user.get("id") or 0
    return login, (f"{uid}+{login}@users.noreply.github.com" if uid else f"{login}@users.noreply.github.com")


def login(cfg: Config, token: str, bot: Optional[str] = None) -> str:
    """Vérifie, enregistre le jeton et règle l'identité git. Renvoie un message pour l'utilisateur."""
    token = token.strip()
    if len(token) < 20 or " " in token:
        raise RuntimeError("ce n'est pas un jeton GitHub (voir github.com/settings/tokens)")
    user = verify(token)
    save_token(token)
    name, email = identity(user, bot)
    cfg.set("git_name", name)
    cfg.set("git_email", email)
    cfg.set("github_user", user.get("login") or name)
    return f"connecté à GitHub en tant que {name}"


def logout(cfg: Config) -> str:
    cfg.set("github_user", "")
    return "jeton GitHub supprimé" if clear_token() else "aucun jeton enregistré"
