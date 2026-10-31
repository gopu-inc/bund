"""Interface Textual de BUND — style IDE : fichiers · éditeur · agent, réflexion et réponse en direct."""
from __future__ import annotations

import asyncio
import re
import shlex
import time
from pathlib import Path
from typing import Callable, List, Optional, Tuple

from rich.text import Text
from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.suggester import SuggestFromList
from textual.widgets import (DirectoryTree, Footer, Input, Label, ListItem, ListView, Markdown, Static, TextArea)

from .. import __version__, doctor, github, llm, mcp
from ..agent import Agent
from ..config import PRESETS, Config
from ..events import (ActionEvent, AgentError, Final, Notice, Observation, OutputLine, StepStart, TextDelta,
                      ThinkDelta)
from ..protocol import PROFILES, Action
from ..store import Session, memory_list

COMMANDS = ["/help", "/clear", "/new", "/sessions", "/presets", "/preset ", "/model", "/model ", "/agents", "/agent ",
            "/mcp", "/mcp add ", "/mcp rm ", "/login github ", "/logout github", "/files", "/chat", "/edit ", "/save",
            "/ide", "/conf", "/conf set ", "/git status", "/git diff", "/git add", "/git commit ", "/git push",
            "/memory", "/think on", "/think off", "/theme replit", "/theme classic", "/theme light", "/doctor",
            "/quit"]

HELP = """**Vues** (onglets en haut sur petit écran)
- `/chat` agent · `/files` fichiers · `/edit CHEMIN` éditeur · `/save` enregistre (ou touche la barre de l'éditeur) · `/ide` panneaux côte à côte

**Agent et modèles**
- `/model` choisir un modèle (liste du fournisseur) · `/presets` · `/agents` · `/mcp` · `/think on|off`
- `/conf` · `/conf set CLÉ VALEUR` · `/doctor` (diagnostic réseau, clé, flux, outils)

**GitHub** : `/login github JETON [bot [nom]]` · `/logout github` · `/git status|diff|add|commit MSG|push`

**Autres** : `/new` · `/sessions` · `/clear` · `/memory` · `/theme replit|classic|light` · `/quit`

**Touches** : Entrée envoie · Ctrl+C interrompt · Ctrl+S enregistre · Ctrl+B panneaux · Échap retour à l'agent · Ctrl+Q quitte
"""

LOGO = "[b #F26207]◆ BUND[/] [dim]agent de code · /help[/]"
IGNORED = {".git", "__pycache__", "node_modules", ".venv", "venv", ".mypy_cache", ".pytest_cache"}
LANGS = {".py": "python", ".js": "javascript", ".mjs": "javascript", ".ts": "javascript", ".json": "json",
         ".md": "markdown", ".html": "html", ".css": "css", ".sh": "bash", ".toml": "toml", ".yml": "yaml",
         ".yaml": "yaml", ".rs": "rust", ".go": "go", ".java": "java", ".sql": "sql", ".xml": "xml"}
ICONS = {"list_files": "▤", "read_file": "≡", "search": "⌕", "write_file": "✎", "edit_file": "✎", "run": "▶",
         "run_tests": "✓", "git_diff": "±", "git_commit": "⎇", "git_push": "⇪", "browse_web": "◍", "remember": "✦",
         "env_get": "⚙", "env_set": "⚙", "skills_list": "✦", "skill_read": "✦"}


def _ok(obs: str) -> bool:
    if obs.strip().lower().startswith(("erreur", "refusé", "action inconnue")):
        return False
    m = re.search(r"\[exit (-?\d+)\]\s*$", obs)
    return not (m and m.group(1) != "0")


def _visible(text: str) -> str:
    """Texte à montrer pendant le flux : sans le protocole texte (ACTION …, DONE)."""
    text = re.sub(r"^\s*DONE\b[:\s]*", "", text)
    m = re.search(r"^\s*ACTION\b", text, re.M)
    return (text[:m.start()] if m else text)[-3000:]


def _think_text(full: str, secs: int, opened: bool) -> Text:
    one = " ".join(full.split())
    head = f"✻ Réflexion · {secs}s {'▾' if opened else '▸'}"
    if opened:
        return Text(head + "\n" + full.strip(), style="italic dim")
    return Text(head + " · " + (one[:90] + (" …" if len(one) > 90 else "")), style="italic dim")


# ====================================================================== petits widgets tactiles
class Tap(Static):
    """Texte cliquable (utile sur téléphone, où Ctrl+… est pénible)."""

    def __init__(self, label, fn: Callable[[], None], **kw) -> None:
        super().__init__(label, **kw)
        self._tap_fn = fn

    def on_click(self) -> None:
        self._tap_fn()


class TabBtn(Static):
    def __init__(self, label: str, view: str, **kw) -> None:
        super().__init__(label, classes="tabbtn", **kw)
        self._target_view = view

    def on_click(self) -> None:
        self.app.set_view(self._target_view)


class ThinkCard(Static):
    """Réflexion terminée : une ligne repliée ; un toucher la déplie."""

    def __init__(self, full: str, secs: int) -> None:
        super().__init__(_think_text(full, secs, False), classes="thinkdone")
        self._tc_full, self._tc_secs, self._tc_open = full, secs, False

    def on_click(self) -> None:
        self._tc_open = not self._tc_open
        self.update(_think_text(self._tc_full, self._tc_secs, self._tc_open))


class ProjectTree(DirectoryTree):
    def filter_paths(self, paths):
        return [p for p in paths if p.name not in IGNORED and not p.name.endswith(".pyc")]


# ====================================================================== écrans modaux
class ConfirmScreen(ModalScreen[bool]):
    BINDINGS = [Binding("y", "yes", "Oui"), Binding("o", "yes", "Oui", show=False),
                Binding("n", "no", "Non"), Binding("escape", "no", "Non", show=False)]
    DEFAULT_CSS = """
    ConfirmScreen { align: center middle; }
    #box { width: 90%; max-width: 90; height: auto; max-height: 80%; border: round $warning; background: $surface; padding: 1 2; }
    #q { margin-bottom: 1; }
    #btns { height: 1; }
    #btns Tap { width: 1fr; text-align: center; background: $panel; }
    #btns .yes { background: $success 40%; }
    #btns .no { background: $error 40%; }
    """

    def __init__(self, question: str) -> None:
        super().__init__()
        self.question = question

    def compose(self) -> ComposeResult:
        with Vertical(id="box"):
            yield Label("Confirmation requise", id="title")
            yield Static(Text(self.question), id="q")
            with Horizontal(id="btns"):
                yield Tap("✔ Oui (Y)", self.action_yes, classes="yes")
                yield Tap("✖ Non (N)", self.action_no, classes="no")

    def action_yes(self) -> None:
        self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)


class PickScreen(ModalScreen[Optional[str]]):
    """Liste à toucher : choisir un modèle, un agent, une session…"""
    BINDINGS = [Binding("escape", "close", "Fermer")]
    DEFAULT_CSS = """
    PickScreen { align: center middle; }
    #pbox { width: 92%; max-width: 100; height: 75%; border: round $primary; background: $surface; padding: 0 1; }
    #pbox ListView { height: 1fr; }
    #pclose { height: 1; text-align: center; background: $panel; }
    """

    def __init__(self, title: str, items: List[Tuple[str, str]], current: str = "") -> None:
        super().__init__()
        self.title_text, self.items, self.current = title, items, current

    def compose(self) -> ComposeResult:
        rows = [ListItem(Label(Text(("● " if v == self.current else "  ") + lab)), name=v) for lab, v in self.items]
        with Vertical(id="pbox"):
            yield Label(Text(self.title_text, style="bold"))
            yield ListView(*rows) if rows else Label("[dim]rien à afficher[/]")
            yield Tap("Fermer", self.action_close, id="pclose")

    @on(ListView.Selected)
    def picked(self, ev: ListView.Selected) -> None:
        self.dismiss(ev.item.name)

    def action_close(self) -> None:
        self.dismiss(None)


class InfoScreen(ModalScreen[None]):
    BINDINGS = [Binding("escape", "close", "Fermer")]
    DEFAULT_CSS = """
    InfoScreen { align: center middle; }
    #ibox { width: 92%; max-width: 100; height: 75%; border: round $primary; background: $surface; padding: 0 1; }
    #iclose { height: 1; text-align: center; background: $panel; }
    """

    def __init__(self, md: str) -> None:
        super().__init__()
        self.md = md

    def compose(self) -> ComposeResult:
        with Vertical(id="ibox"):
            with VerticalScroll():
                yield Markdown(self.md)
            yield Tap("Fermer", self.action_close, id="iclose")

    def action_close(self) -> None:
        self.dismiss(None)


# ====================================================================== application
class BundApp(App):
    TITLE = "BUND"
    CSS = """
    Screen { background: $background; }
    #titlebar { height: 1; background: $primary; color: $text; padding: 0 1; text-style: bold; }
    #tabs { height: 1; display: none; }
    .tabbtn { width: 1fr; height: 1; text-align: center; background: $panel; color: $text-muted; }
    .tabbtn.active { background: $surface; color: $secondary; text-style: bold; }
    #body { height: 1fr; }
    #tree { width: 30; border-right: tall $panel; scrollbar-size: 1 1; }
    #editor-pane { width: 3fr; }
    #editor-title { height: 1; background: $panel; color: $text; padding: 0 1; }
    #editor { height: 1fr; border: none; }
    #chat { width: 2fr; padding: 0 1; border-left: tall $panel; scrollbar-size-vertical: 1; }
    Screen.narrow #tree { width: 1fr; border-right: none; }
    Screen.narrow #editor-pane { width: 1fr; }
    Screen.narrow #chat { width: 1fr; border-left: none; }
    #chat > Static, #chat > Markdown { width: 100%; height: auto; margin: 0; padding: 0; }
    .user { color: $text; background: $boost; border-left: thick $secondary; padding: 0 1; margin-top: 1; }
    .card { margin-top: 0; }
    .err { color: $error; text-style: bold; }
    .note { color: $text-muted; text-style: italic; }
    .thinkdone { color: $text-muted; }
    #think { color: $text-muted; text-style: italic; display: none; max-height: 6; }
    #status { height: 1; padding: 0 1; color: $secondary; }
    Markdown { margin: 0; padding: 0; background: transparent; }
    MarkdownH1, MarkdownH2, MarkdownH3, MarkdownH4, MarkdownH5, MarkdownH6 {
        margin: 1 0 0 0; padding: 0; background: transparent; border: none; text-style: bold; color: $secondary; }
    MarkdownParagraph { margin: 0 0 1 0; }
    MarkdownFence { margin: 0 0 1 0; max-height: 18; }
    MarkdownBulletList, MarkdownOrderedList { margin: 0 0 1 0; }
    #prompt { height: 1; border: none; padding: 0 1; background: $boost; }
    #prompt:focus { border: none; background: $boost; }
    Footer { height: 1; }
    Screen.narrow Footer { display: none; }
    """
    BINDINGS = [
        Binding("ctrl+c", "interrupt", "Stop", priority=True),
        Binding("ctrl+s", "save_file", "Enregistrer"),
        Binding("ctrl+b", "toggle_ide", "Panneaux"),
        Binding("ctrl+q", "quit", "Quitter"),
        Binding("ctrl+l", "clear", "Effacer", show=False),
        Binding("ctrl+r", "sessions", "Sessions", show=False),
        Binding("escape", "to_chat", "Agent", show=False),
        Binding("up", "history(-1)", show=False),
        Binding("down", "history(1)", show=False),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.cfg = Config.load()
        self.root = Path.cwd().resolve()
        self.session = Session()
        self.history: List[str] = []
        self.hpos = 0
        self.show_think = True
        self.current_view = "chat"
        self._ide: Optional[bool] = None
        self._last_w = -1
        self._worker = None
        self._running = False
        self._phase = ""
        self._t0 = 0.0
        self._chars = 0
        self._think = ""
        self._think_t0 = 0.0
        self._text = ""
        self._shell: List[str] = []
        self._dirty = False
        self._step = (0, 0)
        self._status_shown = False
        self._notice = ""
        self._card: Optional[Static] = None
        self._card_head: Optional[Text] = None
        self._open_path: Optional[Path] = None
        self._saved_text = ""
        self._edit_dirty = False
        self._models: List[str] = []

    # ------------------------------------------------------------------ accès sûrs
    @property
    def _scr(self):
        """Écran principal : les widgets restent accessibles même quand une fenêtre modale est ouverte."""
        return self.screen_stack[0]

    @property
    def chat(self) -> VerticalScroll:
        return self._scr.query_one("#chat", VerticalScroll)

    def _ed(self) -> TextArea:
        return self._scr.query_one("#editor", TextArea)

    # ------------------------------------------------------------------ composition
    def compose(self) -> ComposeResult:
        yield Static(id="titlebar")
        with Horizontal(id="tabs"):
            yield TabBtn("Agent", "chat", id="tab-chat")
            yield TabBtn("Fichiers", "files", id="tab-files")
            yield TabBtn("Éditeur", "editor", id="tab-editor")
        with Horizontal(id="body"):
            yield ProjectTree(str(self.root), id="tree")
            with Vertical(id="editor-pane"):
                yield Tap("", self.action_save_file, id="editor-title")
                try:
                    yield TextArea.code_editor("", id="editor")
                except Exception:
                    yield TextArea("", id="editor")
            with VerticalScroll(id="chat"):
                yield Static(LOGO, id="logo")
                yield Static(id="think")
                yield Static(id="live")
        yield Static(id="status")
        yield Input(placeholder="❯ Votre demande… (/help)", id="prompt",
                    suggester=SuggestFromList(COMMANDS, case_sensitive=False))
        yield Footer()

    def on_mount(self) -> None:
        try:
            from textual.theme import Theme
            self.register_theme(Theme(
                name="replit", primary="#0079F2", secondary="#F26207", accent="#F26207", warning="#F5A524",
                error="#FF6166", success="#0CCE6B", foreground="#F5F9FC", background="#0E1525",
                surface="#1C2333", panel="#2B3245", dark=True))
        except Exception:
            pass
        self._apply_theme(self.cfg.theme)
        self.set_interval(1 / 12, self._tick)
        self._refresh_titlebar()
        self._refresh_editor_title()
        self._scr.query_one("#prompt", Input).focus()
        self._say(Markdown("Je suis **Bund**. Décrivez la tâche, ou tapez `/help`."))
        if self.cfg.mode != "local" and self.cfg.api_key_env and not self.cfg.api_key():
            self._say(Static(Text(f"⚠ clé absente : exporte {self.cfg.api_key_env} ou utilise /preset ollama",
                                  style="bold yellow")))

    def _apply_theme(self, name: str) -> None:
        try:
            self.theme = {"light": "textual-light", "classic": "textual-dark"}.get(name, "replit")
        except Exception:
            self.dark = name != "light"
        try:
            self._ed().theme = "github_light" if name == "light" else "vscode_dark"
        except Exception:
            pass

    # ------------------------------------------------------------------ disposition (3 panneaux / onglets)
    def _layout(self) -> None:
        s = self._scr
        w = self.size.width or 80
        wide = w >= 100
        if self._ide is None:
            self._ide = wide
        tabs, tree, pane, chat = (s.query_one(x) for x in ("#tabs", "#tree", "#editor-pane", "#chat"))
        if wide:
            tabs.display = False
            tree.display = pane.display = bool(self._ide)
            chat.display = True
        else:
            tabs.display = True
            tree.display = self.current_view == "files"
            pane.display = self.current_view == "editor"
            chat.display = self.current_view == "chat"
        for v in ("chat", "files", "editor"):
            s.query_one(f"#tab-{v}").set_class(v == self.current_view, "active")
        s.set_class(not wide, "narrow")
        self._refresh_titlebar()

    def set_view(self, v: str) -> None:
        self.current_view = v
        self._layout()
        target = {"chat": "#prompt", "files": "#tree", "editor": "#editor"}[v]
        try:
            self._scr.query_one(target).focus()
        except Exception:
            pass

    def action_toggle_ide(self) -> None:
        if (self.size.width or 80) >= 100:
            self._ide = not self._ide
            self._layout()
        else:
            self.set_view("chat" if self.current_view != "chat" else "files")

    def action_to_chat(self) -> None:
        if (self.size.width or 80) < 100:
            self.set_view("chat")
        else:
            self._scr.query_one("#prompt", Input).focus()

    # ------------------------------------------------------------------ affichage
    def _say(self, widget):
        """Ajoute un élément dans le fil, avant les zones « en direct »."""
        self.chat.mount(widget, before=self._scr.query_one("#think"))
        self.chat.scroll_end(animate=False)
        return widget

    def _note(self, text: str, style: str = "") -> None:
        self._say(Static(Text(text, style=style or "dim")))

    def _refresh_titlebar(self) -> None:
        narrow = (self.size.width or 80) < 60
        gh = f" · {self.cfg.github_user}" if self.cfg.github_user else ""
        self._scr.query_one("#titlebar", Static).update(
            f"◆ BUND · {self.cfg.model}" if narrow
            else f"◆ BUND v{__version__} · {self.cfg.provider}/{self.cfg.model} · {self.root.name}{gh}")

    def _tick(self) -> None:
        """Repeint les zones vivantes ~12 fois par seconde (pas à chaque jeton)."""
        if self.size.width != self._last_w:
            self._last_w = self.size.width
            try:
                self._layout()
            except Exception:
                pass
        s = self._scr
        status = s.query_one("#status", Static)
        if self._running:
            frames = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
            sp = frames[int(time.monotonic() * 10) % len(frames)]
            el = int(time.monotonic() - self._t0)
            phase = {"wait": self._notice or "Connexion…", "think": "Réflexion…", "write": "Rédaction…",
                     "tool": "Outil en cours…"}.get(self._phase, "")
            st, n = self._step
            if self.size.width < 70:
                status.update(f"{sp} {phase} {el}s · {st}/{n}")
            else:
                status.update(f"{sp} {phase} {el}s · étape {st}/{n} · {self._chars} car. · Ctrl+C stoppe")
            self._status_shown = True
        elif self._status_shown:
            self._status_shown = False
            status.update("")
        if not self._dirty:
            return
        self._dirty = False
        think_w, live_w = s.query_one("#think", Static), s.query_one("#live", Static)
        if self.show_think and self._think:
            think_w.display = True
            tail = "\n".join(self._think.strip().splitlines()[-4:])[-400:]
            think_w.update(Text("✻ Réflexion…\n" + tail, style="italic dim"))
        else:
            think_w.display = False
        extra = ("\n" + "".join(self._shell[-6:])) if self._shell else ""
        body = _visible(self._text)
        live_w.update(Text(body + extra) if (body or extra) else "")
        self.chat.scroll_end(animate=False)

    def _reset_live(self) -> None:
        self._think, self._text, self._shell = "", "", []
        self._dirty = True

    # ------------------------------------------------------------------ entrée utilisateur
    @on(Input.Submitted, "#prompt")
    async def submit(self, ev: Input.Submitted) -> None:
        line = ev.value.strip()
        ev.input.value = ""
        if not line:
            return
        if not line.startswith("/login"):               # le jeton ne doit pas rester dans l'historique
            self.history.append(line)
            self.hpos = len(self.history)
        if line.startswith("/"):
            await self.command(line)
            return
        if self._running:
            self._note("L'agent travaille encore (Ctrl+C pour l'interrompre).", "yellow")
            return
        if self.current_view != "chat" and (self.size.width or 80) < 100:
            self.set_view("chat")
        self._say(Static(Text(line), classes="user"))
        self._worker = self.run_worker(self._run_goal(line), exclusive=True, name="agent", exit_on_error=False)

    def action_history(self, d: int) -> None:
        inp = self._scr.query_one("#prompt", Input)
        if not self.history or not inp.has_focus:
            return
        self.hpos = max(0, min(len(self.history), self.hpos + d))
        inp.value = self.history[self.hpos] if self.hpos < len(self.history) else ""
        inp.cursor_position = len(inp.value)

    # ------------------------------------------------------------------ agent
    async def _confirm(self, question: str) -> bool:
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self.push_screen(ConfirmScreen(question), lambda r: fut.set_result(bool(r)) if not fut.done() else None)
        prev, self._phase = self._phase, "tool"
        try:
            return await fut
        finally:
            self._phase = prev

    def _card_new(self, name: str, arg: str) -> None:
        head = Text.assemble((f"{ICONS.get(name, '•')} ", "bold #F26207"), (name, "bold"), ("  " + arg[:200], "dim"))
        self._card_head = head
        self._card = self._say(Static(head, classes="card"))

    def _card_done(self, obs: str) -> None:
        if self._card is None or self._card_head is None:
            return
        ok = _ok(obs)
        line = " ".join(obs.split())
        line = line if len(line) <= 160 else line[:160] + " …"
        t = self._card_head.copy()
        t.append("\n  " + ("✓ " if ok else "✗ "), style="bold green" if ok else "bold red")
        t.append(line, style="dim")
        self._card.update(t)
        self._card = None

    async def _run_goal(self, goal: str) -> None:
        agent = Agent(self.cfg, self.root, self._confirm, self.session)
        self._running, self._phase, self._t0, self._chars, self._notice = True, "wait", time.monotonic(), 0, ""
        self._reset_live()
        last = Action("")
        try:
            async for ev in agent.run(goal):
                if isinstance(ev, StepStart):
                    self._step, self._phase, self._notice = (ev.step, ev.total), "wait", ""
                    self._flush_think()
                    self._reset_live()
                elif isinstance(ev, Notice):
                    if ev.chat:
                        self._note("⚠ " + ev.text, "yellow")
                    else:
                        self._notice = ev.text
                elif isinstance(ev, ThinkDelta):
                    if not self._think:
                        self._think_t0 = time.monotonic()
                    self._phase = "think"
                    self._think += ev.text
                    self._chars += len(ev.text)
                    self._dirty = True
                elif isinstance(ev, TextDelta):
                    self._phase = "write"
                    self._text += ev.text
                    self._chars += len(ev.text)
                    self._dirty = True
                elif isinstance(ev, ActionEvent):
                    self._phase = "tool"
                    self._flush_think()
                    self._reset_live()
                    last = Action(ev.name, ev.arg)
                    self._card_new(ev.name, ev.arg)
                elif isinstance(ev, OutputLine):
                    self._shell.append(ev.text)
                    self._dirty = True
                elif isinstance(ev, Observation):
                    self._card_done(ev.text)
                    self._after_tool(last.name, last.arg, ev.text)
                    self._reset_live()
                elif isinstance(ev, Final):
                    self._flush_think()
                    self._reset_live()
                    self._say(Markdown(ev.text))
                elif isinstance(ev, AgentError):
                    self._reset_live()
                    self._say(Static(Text("✖ " + ev.message), classes="err"))
        except asyncio.CancelledError:
            self._reset_live()
            self._note("■ interrompu", "bold yellow")
            raise
        except Exception as e:                                   # jamais de silence : on montre l'erreur
            self._reset_live()
            self._say(Static(Text(f"✖ erreur interne : {type(e).__name__}: {e}"), classes="err"))
        finally:
            self._running, self._phase, self._notice = False, "", ""
            self._reset_live()

    def _flush_think(self) -> None:
        if self._think.strip() and self.show_think:
            secs = int(time.monotonic() - self._think_t0) if self._think_t0 else 0
            self._say(ThinkCard(self._think, secs))
        self._think_t0 = 0.0

    def action_interrupt(self) -> None:
        if self._running and self._worker is not None:
            self._worker.cancel()
            return
        f = self.focused
        if isinstance(f, TextArea):                              # Ctrl+C = copier dans l'éditeur
            try:
                f.action_copy()
                return
            except Exception:
                pass
        self.notify("Rien à interrompre — /quit pour quitter.", timeout=2)

    # ------------------------------------------------------------------ éditeur
    def _resolve(self, arg: str) -> Optional[Path]:
        arg = (arg or "").strip().strip("`\"'")
        if not arg:
            return None
        p = Path(arg)
        p = p if p.is_absolute() else self.root / p
        try:
            p = p.resolve()
            p.relative_to(self.root)
        except (ValueError, OSError):
            return None
        return p

    def _refresh_editor_title(self) -> None:
        t = self._scr.query_one("#editor-title", Static)
        if self._open_path is None:
            t.update("✎ aucun fichier — ouvre-en un dans Fichiers, ou /edit chemin")
            return
        rel = str(self._open_path.relative_to(self.root))
        t.update(Text(f"✎ {rel}{' ●' if self._edit_dirty else ''}  · toucher = enregistrer"))

    def open_file(self, p: Path, switch: bool = True) -> bool:
        if p.is_dir():
            self._note("C'est un dossier.", "yellow")
            return False
        text = ""
        if p.exists():
            try:
                if p.stat().st_size > 400_000:
                    self._note("Fichier trop gros pour l'éditeur (> 400 Ko).", "yellow")
                    return False
                text = p.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                self._note("Fichier binaire : ouverture impossible.", "yellow")
                return False
            except OSError as e:
                self._note(f"Lecture impossible : {e}", "yellow")
                return False
        ed = self._ed()
        try:
            ed.load_text(text)
        except AttributeError:
            ed.text = text
        try:
            ed.language = LANGS.get(p.suffix.lower())
        except Exception:
            pass
        self._open_path, self._saved_text, self._edit_dirty = p, ed.text, False
        self._refresh_editor_title()
        if switch:
            self.set_view("editor")
        return True

    @on(TextArea.Changed, "#editor")
    def editor_changed(self, ev) -> None:
        if self._open_path is None:
            return
        d = self._ed().text != self._saved_text
        if d != self._edit_dirty:
            self._edit_dirty = d
            self._refresh_editor_title()

    def action_save_file(self) -> None:
        if self._open_path is None:
            self.notify("Aucun fichier ouvert (/edit chemin).", timeout=3)
            return
        try:
            self._open_path.parent.mkdir(parents=True, exist_ok=True)
            text = self._ed().text
            self._open_path.write_text(text, encoding="utf-8")
        except OSError as e:
            self.notify(f"Échec de l'enregistrement : {e}", severity="error")
            return
        self._saved_text, self._edit_dirty = text, False
        self._refresh_editor_title()
        self.notify(f"Enregistré : {self._open_path.name}", timeout=2)
        try:
            self._scr.query_one("#tree", DirectoryTree).reload()
        except Exception:
            pass

    def _after_tool(self, name: str, arg: str, obs: str) -> None:
        """Quand l'agent écrit un fichier : rafraîchir l'arbre et montrer le fichier dans l'éditeur."""
        if name not in ("write_file", "edit_file") or not obs.startswith(("écrit", "modifié")):
            return
        p = self._resolve(arg)
        if p is None:
            return
        try:
            self._scr.query_one("#tree", DirectoryTree).reload()
        except Exception:
            pass
        if self._open_path == p and self._edit_dirty:
            self.notify("L'agent a modifié ce fichier : vos changements non enregistrés sont conservés.", timeout=4)
        elif self._open_path in (None, p) or not self._edit_dirty:
            self.open_file(p, switch=False)

    @on(DirectoryTree.FileSelected)
    def file_selected(self, ev: DirectoryTree.FileSelected) -> None:
        p = self._resolve(str(ev.path))
        if p is not None:
            self.open_file(p, switch=True)

    # ------------------------------------------------------------------ actions
    def action_clear(self) -> None:
        for w in list(self.chat.children):
            if w.id not in ("think", "live"):
                w.remove()
        self._reset_live()

    def action_sessions(self) -> None:
        items = [(f"{s.title}  {s.id}", s.id) for s in Session.list()]

        def done(sid: Optional[str]) -> None:
            if not sid:
                return
            s = Session.load(sid)
            if s:
                self.session = s
                self.action_clear()
                for m in s.messages[-20:]:
                    self._say(Static(Text(m["text"]), classes="user") if m["role"] == "user" else Markdown(m["text"]))
        self.push_screen(PickScreen("Sessions récentes", items), done)

    # ------------------------------------------------------------------ commandes slash
    async def command(self, line: str) -> None:
        cmd, _, arg = line.partition(" ")
        arg = arg.strip()
        if cmd == "/help":
            self._say(Markdown(HELP))
        elif cmd == "/clear":
            self.action_clear()
        elif cmd == "/new":
            self.session = Session()
            self.action_clear()
            self._note("Nouvelle session.")
        elif cmd == "/sessions":
            self.action_sessions()
        elif cmd == "/presets":
            self.push_screen(PickScreen("Fournisseurs (toucher pour appliquer)",
                                        [(f"{p.name} · {p.model}", p.name) for p in PRESETS.values()],
                                        self.cfg.provider.lower()), self._preset_picked)
        elif cmd == "/preset":
            self._preset_picked(arg)
        elif cmd == "/model":
            self.run_worker(self._cmd_model(arg), exit_on_error=False)
        elif cmd == "/agents":
            self.push_screen(PickScreen("Agents (toucher pour activer)",
                                        [(f"{n} — {d}", n) for n, d in PROFILES.items()], self.cfg.agent),
                             self._agent_picked)
        elif cmd == "/agent":
            self._agent_picked(arg)
        elif cmd == "/mcp":
            self._cmd_mcp(arg)
        elif cmd == "/login":
            self.run_worker(self._cmd_login(arg), exit_on_error=False)
        elif cmd == "/logout":
            self._note(github.logout(self.cfg))
            self._refresh_titlebar()
        elif cmd == "/conf":
            if arg.startswith("set "):
                k, _, v = arg[4:].partition(" ")
                self._note("ok" if self.cfg.set(k, v.strip()) else "clé ou valeur invalide")
                self._refresh_titlebar()
            else:
                self._say(Static(Text(self.cfg.summary())))
        elif cmd == "/think":
            self.show_think = arg != "off"
            self._note(f"Réflexion en direct : {'activée' if self.show_think else 'masquée'}")
        elif cmd in ("/files", "/tree"):
            self.set_view("files") if (self.size.width or 80) < 100 else self.action_toggle_ide()
        elif cmd == "/chat":
            self.set_view("chat")
        elif cmd == "/ide":
            self.action_toggle_ide()
        elif cmd in ("/edit", "/open"):
            p = self._resolve(arg)
            if p is None:
                self._note("Usage : /edit chemin/du/fichier (dans le projet)", "yellow")
            else:
                self.open_file(p, switch=True)
        elif cmd == "/save":
            self.action_save_file()
        elif cmd == "/theme":
            self.cfg.set("theme", arg or "replit")
            self._apply_theme(arg or "replit")
        elif cmd == "/memory":
            self._note("Notes : " + (", ".join(memory_list()) or "aucune"))
        elif cmd == "/doctor":
            self.run_worker(doctor.run(self.cfg, lambda t: self._note(t, "")), exit_on_error=False)
        elif cmd == "/git":
            await self._git(arg)
        elif cmd in ("/quit", "/exit"):
            self.exit()
        else:
            self._note("Commande inconnue — /help")

    def _preset_picked(self, name: Optional[str]) -> None:
        if not name:
            return
        if self.cfg.apply_preset(name):
            self._refresh_titlebar()
            self._note(f"Fournisseur : {self.cfg.provider}/{self.cfg.model}")
        else:
            self._note("Preset inconnu (voir /presets)", "yellow")

    def _agent_picked(self, name: Optional[str]) -> None:
        if not name:
            return
        if name in PROFILES:
            self.cfg.set("agent", name)
            self._note(f"Agent actif : {name}")
        else:
            self._note("Agent inconnu (voir /agents)", "yellow")

    async def _cmd_model(self, arg: str) -> None:
        if arg and not arg.isdigit():
            self.cfg.set("model", arg)
            self._refresh_titlebar()
            self._note(f"Modèle : {self.cfg.model}")
            return
        self._note("Chargement de la liste des modèles…")
        try:
            self._models = await asyncio.to_thread(llm.list_models, self.cfg)
        except RuntimeError as e:
            self._note(f"Liste indisponible ({e}). Modèle actuel : {self.cfg.model} — change-le avec /model NOM", "yellow")
            return
        if arg.isdigit():
            i = int(arg) - 1
            if 0 <= i < len(self._models):
                self.cfg.set("model", self._models[i])
                self._refresh_titlebar()
                self._note(f"Modèle : {self.cfg.model}")
            return

        def picked(m: Optional[str]) -> None:
            if m:
                self.cfg.set("model", m)
                self._refresh_titlebar()
                self._note(f"Modèle : {self.cfg.model}")
        self.push_screen(PickScreen(f"Modèles · {self.cfg.provider} (toucher pour choisir)",
                                    [(m, m) for m in self._models], self.cfg.model), picked)

    def _cmd_mcp(self, arg: str) -> None:
        sub, _, rest = arg.partition(" ")
        try:
            if sub == "add":
                parts = shlex.split(rest)
                if len(parts) < 2:
                    self._note("Usage : /mcp add NOM COMMANDE [ARGS…]  ou  /mcp add NOM https://url", "yellow")
                    return
                mcp.add(parts[0], parts[1], parts[2:])
                self._note(f"Serveur MCP « {parts[0]} » ajouté.")
                return
            if sub in ("rm", "remove", "del"):
                self._note("Supprimé." if mcp.remove(rest.strip()) else "Serveur introuvable.")
                return
        except (OSError, ValueError) as e:
            self._note(f"Échec : {e}", "yellow")
            return
        self.push_screen(InfoScreen(mcp.describe()))

    async def _cmd_login(self, arg: str) -> None:
        parts = arg.split()
        if not parts or parts[0] != "github" or len(parts) == 1:
            etat = self.cfg.github_user or ("jeton présent" if self.cfg.github_token() else "non connecté")
            self._say(Markdown(
                f"GitHub : **{etat}**\n\n`/login github JETON` · `/login github JETON bot [nom]` (commits signés `nom[bot]`) "
                "· `/logout github`\n\nCrée un jeton sur github.com/settings/tokens avec le droit *Contents : écriture*."))
            return
        bot = (parts[3] if len(parts) > 3 else "bund") if len(parts) > 2 and parts[2] == "bot" else None
        try:
            msg = await asyncio.to_thread(github.login, self.cfg, parts[1], bot)
        except RuntimeError as e:
            self._note(f"✖ {e}", "bold red")
            return
        self._refresh_titlebar()
        self._note(f"✔ {msg}", "bold green")

    async def _git(self, arg: str) -> None:
        agent = Agent(self.cfg, self.root, self._confirm, self.session)
        sub, _, rest = arg.partition(" ")
        if sub == "status":
            out = await agent.tools._git("status", "-sb")
        elif sub == "diff":
            out = await agent.tools.execute(Action("git_diff"))
        elif sub == "add":
            out = await agent.tools._git("add", "-A")
        elif sub == "commit":
            out = await agent.tools.execute(Action("git_commit", rest.strip()))
        elif sub == "push":
            out = await agent.tools.execute(Action("git_push"))
        else:
            out = "usage : /git status|diff|add|commit MSG|push"
        self._say(Static(Text(out.strip())))
