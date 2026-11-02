"""Point d'entrée : `bund` (TUI), `bund run "objectif"`, `bund web`, `bund conf …`."""
from __future__ import annotations

import argparse
import asyncio
import subprocess
import sys
from pathlib import Path

from .config import PRESETS, VERSION, Config
from .events import (ActionEvent, AgentError, Final, Notice, Observation, OutputLine, StepStart, TextDelta, ThinkDelta)

DIM, RST = "\033[2m", "\033[0m"


async def run_headless(goal: str, yes: bool) -> int:
    """Mode sans interface : jetons écrits dès qu'ils arrivent (comme Ollama)."""
    from .agent import Agent

    cfg = Config.load()
    tty = sys.stdout.isatty()

    async def confirm(q: str) -> bool:
        if yes:
            return True
        if not sys.stdin.isatty():
            return False
        print(f"\n? {q} [o/N] ", end="", flush=True)
        ans = await asyncio.to_thread(sys.stdin.readline)
        return ans.strip().lower() in {"o", "oui", "y", "yes"}

    agent = Agent(cfg, Path.cwd(), confirm)
    mode = None            # None | "think" | "text"
    printed = False
    rc = 1
    async for ev in agent.run(goal):
        if isinstance(ev, ThinkDelta):
            if mode != "think":
                sys.stdout.write((DIM if tty else "") + "\n◌ ")
                mode = "think"
            sys.stdout.write(ev.text)
        elif isinstance(ev, TextDelta):
            if mode != "text":
                sys.stdout.write((RST if tty else "") + "\n")
                mode = "text"
            sys.stdout.write(ev.text)
            printed = True
        else:
            if mode:
                sys.stdout.write((RST if tty else "") + "\n")
                mode = None
            if isinstance(ev, StepStart):
                printed = False
                print(f"· étape {ev.step}/{ev.total}")
            elif isinstance(ev, Notice):
                if ev.chat:
                    print(f"  ⚠ {ev.text}", file=sys.stderr)
            elif isinstance(ev, ActionEvent):
                print(f"  → {ev.name} {ev.arg}")
            elif isinstance(ev, OutputLine):
                sys.stdout.write(ev.text)
            elif isinstance(ev, Observation):
                o = ev.text if len(ev.text) < 300 else ev.text[:300] + " …"
                print(f"  ← {o}")
            elif isinstance(ev, Final):
                print("✔" if printed else f"✔ {ev.text}")
                rc = 0 if ev.ok else 1
            elif isinstance(ev, AgentError):
                print(f"✖ {ev.message}", file=sys.stderr)
        sys.stdout.flush()
    return rc


def conf_cmd(args: list) -> int:
    cfg = Config.load()
    if not args:
        print(cfg.summary())
    elif args[0] == "presets":
        for p in PRESETS.values():
            print(f"{p.name:13} {p.model:34} {p.desc}")
    elif args[0] == "preset" and len(args) > 1:
        print("ok" if cfg.apply_preset(args[1]) else "preset inconnu")
    elif args[0] == "set" and len(args) > 2:
        print("ok" if cfg.set(args[1], " ".join(args[2:])) else "clé ou valeur invalide")
    else:
        print("usage : bund conf [presets | preset NOM | set CLÉ VALEUR]")
        return 2
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="bund", description="BUND — agent de code (Python / Textual / asyncio)")
    ap.add_argument("--version", action="version", version=f"bund {VERSION}")
    sub = ap.add_subparsers(dest="cmd")
    r = sub.add_parser("run", help="exécuter un objectif sans interface")
    r.add_argument("goal", nargs="+")
    r.add_argument("-y", "--yes", action="store_true", help="approuver automatiquement les actions")
    sub.add_parser("web", help="interface web (Streamlit)")
    sub.add_parser("doctor", help="diagnostic : réseau, clé, flux, outils natifs")
    c = sub.add_parser("conf", help="configuration")
    c.add_argument("args", nargs="*")
    ns = ap.parse_args(argv)

    if ns.cmd == "run":
        try:
            return asyncio.run(run_headless(" ".join(ns.goal), ns.yes))
        except KeyboardInterrupt:
            print("\n■ interrompu")
            return 130
    if ns.cmd == "conf":
        return conf_cmd(ns.args)
    if ns.cmd == "doctor":
        from . import doctor
        try:
            asyncio.run(doctor.run(Config.load(), print))
        except KeyboardInterrupt:
            pass
        return 0
    if ns.cmd == "web":
        try:
            import streamlit  # noqa: F401
        except ImportError:
            print('Streamlit absent : pip install "bund[web]"', file=sys.stderr)
            return 1
        return subprocess.call([sys.executable, "-m", "streamlit", "run", str(Path(__file__).with_name("web.py"))])
    try:
        from .tui import run
    except ImportError as e:
        print(f"Textual est requis pour l'interface : pip install textual ({e})", file=sys.stderr)
        return 1
    run()
    return 0
