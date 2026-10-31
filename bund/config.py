"""Configuration : presets de fournisseurs, fichier ~/.bund/bund.env.conf (même format que la version C)."""
from __future__ import annotations

import os
import tempfile
from dataclasses import dataclass, fields
from pathlib import Path
from typing import Dict, Optional

VERSION = "1.1.0"
REPO_URL = "https://github.com/gopu-inc/bund"
CONTRIB = f"Bund <{REPO_URL}>"


_HOME_CACHE: Optional[tuple] = None   # (valeur de $BUND_HOME, chemin retenu)


def _writable(d: Path) -> bool:
    """Vrai si on peut créer le dossier d et y écrire (testé pour de vrai, pas seulement os.access)."""
    try:
        d.mkdir(parents=True, exist_ok=True)
        probe = d / ".w"
        probe.write_text("1", encoding="utf-8")
        probe.unlink()
        return True
    except OSError:
        return False


def bund_home() -> Path:
    """Dossier de données de BUND.

    Ordre : $BUND_HOME, ~/.bund, puis des emplacements autorisés sur iOS (a-Shell : ~/Documents et
    ~/Library sont inscriptibles, mais pas la racine du conteneur), enfin le dossier temporaire.
    """
    global _HOME_CACHE
    env = os.environ.get("BUND_HOME", "")
    if _HOME_CACHE is not None and _HOME_CACHE[0] == env:
        return _HOME_CACHE[1]
    home = Path(os.environ.get("HOME") or os.path.expanduser("~") or "/tmp")
    cands = []
    if os.environ.get("BUND_HOME"):
        cands.append(Path(os.environ["BUND_HOME"]))
    cands += [home / ".bund", home / "Documents" / ".bund", home / "Library" / ".bund",
              Path(tempfile.gettempdir()) / "bund"]
    for c in cands:
        if _writable(c):
            _HOME_CACHE = (env, c)
            return c
    _HOME_CACHE = (env, cands[-1])
    return cands[-1]


@dataclass(frozen=True)
class Preset:
    name: str
    provider: str
    base_url: str
    model: str
    api_key_env: str
    mode: str
    desc: str


PRESETS: Dict[str, Preset] = {p.name: p for p in [
    Preset("openai", "OPENAI", "https://api.openai.com/v1", "gpt-4.1-mini", "OPENAI_API_KEY", "cloud", "OpenAI"),
    Preset("claude", "CLAUDE", "https://api.anthropic.com/v1", "claude-sonnet-4-5", "ANTHROPIC_API_KEY", "cloud",
           "Anthropic Claude (adaptateur natif)"),
    Preset("google", "GOOGLE", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.0-flash",
           "GEMINI_API_KEY", "cloud", "Google Gemini (endpoint OpenAI)"),
    Preset("deepseek", "DEEPSEEK", "https://api.deepseek.com/v1", "deepseek-chat", "DEEPSEEK_API_KEY", "cloud", "DeepSeek"),
    Preset("groq", "GROQ", "https://api.groq.com/openai/v1", "openai/gpt-oss-120b", "GROQ_API_KEY", "cloud", "Groq"),
    Preset("together", "TOGETHER", "https://api.together.xyz/v1", "Qwen/Qwen2.5-Coder-32B-Instruct",
           "TOGETHER_API_KEY", "cloud", "Together AI"),
    Preset("openrouter", "OPENROUTER", "https://openrouter.ai/api/v1", "openai/gpt-4.1-mini", "OPENROUTER_API_KEY",
           "cloud", "OpenRouter (multi-modèles)"),
    Preset("mistral", "MISTRAL", "https://api.mistral.ai/v1", "codestral-latest", "MISTRAL_API_KEY", "cloud", "Mistral AI"),
    Preset("ollama-coder", "OLLAMA", "http://127.0.0.1:11434/v1", "qwen2.5-coder:0.5b", "", "local",
           "Ollama : qwen2.5-coder 0.5b (très léger)"),
    Preset("ollama", "OLLAMA", "http://127.0.0.1:11434/v1", "qwen3:0.6b", "", "local", "Ollama local"),
    Preset("compatible", "OPENAI_COMPATIBLE", "http://127.0.0.1:8000/v1", "mon-modele", "BUND_API_KEY", "cloud",
           "Tout serveur compatible OpenAI"),
]}

# clé du fichier de config -> (attribut, type)
KEYMAP = {
    "BUND_PROVIDER": ("provider", str), "BUND_API_BASE_URL": ("base_url", str), "BUND_MODEL": ("model", str),
    "BUND_API_KEY_ENV": ("api_key_env", str), "BUND_MODE": ("mode", str), "BUND_AGENT": ("agent", str),
    "BUND_GIT_NAME": ("git_name", str), "BUND_GIT_EMAIL": ("git_email", str), "BUND_THEME": ("theme", str),
    "BUND_MAX_STEPS": ("max_steps", int), "BUND_MAX_RETRIES": ("max_retries", int),
    "BUND_TIMEOUT_SECONDS": ("timeout", int), "BUND_AUTO_EDIT": ("auto_edit", bool),
    "BUND_AUTO_TEST": ("auto_test", bool), "BUND_AUTO_PUSH": ("auto_push", bool),
    "BUND_STREAM": ("stream", bool), "BUND_THINK_BUDGET": ("think_budget", int),
    "BUND_NATIVE_TOOLS": ("native_tools", bool), "BUND_GITHUB_USER": ("github_user", str),
}
# paramètres que l'agent a le droit de lire / modifier (jamais de clés ni de secrets)
AGENT_KEYS = {"provider", "model", "agent", "theme", "max_steps", "max_retries", "timeout", "auto_test", "stream",
              "think_budget"}


def _truthy(v: str) -> bool:
    return v.strip().lower() in {"1", "true", "yes", "on", "oui", "o"}


@dataclass
class Config:
    provider: str = "OPENAI"
    base_url: str = "https://api.openai.com/v1"
    model: str = "gpt-4.1-mini"
    api_key_env: str = "OPENAI_API_KEY"
    mode: str = "cloud"
    agent: str = "bund"
    git_name: str = "Bund"
    git_email: str = ""
    theme: str = "dark"
    max_steps: int = 100
    max_retries: int = 2
    timeout: int = 120
    auto_edit: bool = False
    auto_test: bool = True
    auto_push: bool = False
    stream: bool = True
    think_budget: int = 0      # >= 1024 : réflexion étendue Anthropic
    native_tools: bool = True  # appels d'outils natifs (function calling) ; repli automatique en mode texte
    github_user: str = ""

    # ---- persistance ----
    @staticmethod
    def path() -> Path:
        return bund_home() / "bund.env.conf"

    @classmethod
    def load(cls) -> "Config":
        c = cls()
        p = cls.path()
        if p.exists():
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                c._apply(k.strip(), v.strip())
        else:
            try:
                c.save()
            except OSError:
                pass
        env_stream = os.environ.get("BUND_STREAM")
        if env_stream is not None:
            c.stream = _truthy(env_stream)
        env_think = os.environ.get("BUND_THINK_BUDGET")
        if env_think and env_think.isdigit():
            c.think_budget = int(env_think)
        return c

    def save(self) -> None:
        p = self.path()
        lines = ["# BUND — configuration (modifiable avec /conf)"]
        for key, (attr, typ) in KEYMAP.items():
            v = getattr(self, attr)
            lines.append(f"{key}={'true' if v else 'false'}" if typ is bool else f"{key}={v}")
        try:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        except OSError:
            pass          # dossier non inscriptible : on garde la config en mémoire, sans planter

    # ---- modification ----
    def _apply(self, key: str, value: str) -> bool:
        key = key.strip()
        if key.upper() in KEYMAP:
            attr, typ = KEYMAP[key.upper()]
        else:
            names = {a: t for a, t in KEYMAP.values()}
            if key.lower() not in names:
                return False
            attr, typ = key.lower(), names[key.lower()]
        try:
            val = _truthy(value) if typ is bool else (int(value) if typ is int else value)
        except ValueError:
            return False
        setattr(self, attr, val)
        return True

    def set(self, key: str, value: str) -> bool:
        ok = self._apply(key, value)
        if ok:
            self.save()
        return ok

    def apply_preset(self, name: str) -> bool:
        p = PRESETS.get(name.lower())
        if not p:
            return False
        self.provider, self.base_url, self.model = p.provider, p.base_url, p.model
        self.api_key_env, self.mode = p.api_key_env, p.mode
        self.save()
        return True

    # ---- clé API ----
    def api_key(self) -> Optional[str]:
        if not self.api_key_env:
            return None
        v = os.environ.get(self.api_key_env, "").strip()
        if v:
            return v
        f = bund_home() / "secrets" / self.api_key_env
        try:
            t = f.read_text(encoding="utf-8").strip()
            return t or None
        except OSError:
            return None

    def github_token(self) -> Optional[str]:
        for k in ("GITHUB_TOKEN", "GH_TOKEN"):
            v = os.environ.get(k, "").strip()
            if v:
                return v
        try:
            return (bund_home() / "secrets" / "GITHUB_TOKEN").read_text(encoding="utf-8").strip() or None
        except OSError:
            return None

    @property
    def is_anthropic(self) -> bool:
        return self.provider.upper() == "CLAUDE"

    @property
    def is_ollama_native(self) -> bool:
        """URL de base en /api (et non /v1) : API native d'Ollama (champ `thinking` séparé)."""
        return self.base_url.rstrip("/").endswith("/api")

    def summary(self) -> str:
        key = "présente" if self.api_key() else ("inutile (local)" if self.mode == "local" else "ABSENTE")
        return (f"fournisseur : {self.provider}\nURL : {self.base_url}\nmodèle : {self.model}\n"
                f"clé ({self.api_key_env or '—'}) : {key}\nflux : {'oui' if self.stream else 'non'}\n"
                f"réflexion étendue : {self.think_budget or 'non'}\noutils natifs : {'oui' if self.native_tools else 'non'}\n"
                f"github : {self.github_user or ('jeton présent' if self.github_token() else 'non connecté')}\n"
                f"étapes max : {self.max_steps}")


CONFIG_FIELDS = [f.name for f in fields(Config)]
