"""Boucle de l'agent : réfléchit, agit, observe — en flux asynchrone, étape par étape."""
from __future__ import annotations

import asyncio
from pathlib import Path
from typing import AsyncIterator, Optional, Union

from . import llm
from .config import Config
from .events import (ActionEvent, AgentError, Final, Notice, Observation, OutputLine, StepStart, StreamEnd,
                     StreamError, TextDelta, ThinkDelta, ToolCall)
from .protocol import (PROFILES, SYSTEM_NATIVE, SYSTEM_PROTOCOL, TOOL_SPECS, Action, Done, call_to_action,
                       parse_reply, render_action, split_think)
from .store import Session, memory_context, project_context
from .tools import Confirm, Tools

AgentEvent = Union[StepStart, ThinkDelta, TextDelta, Notice, ActionEvent, OutputLine, Observation, Final, AgentError]
MAX_TRANSCRIPT = 24_000


class Agent:
    def __init__(self, cfg: Config, root: Path, confirm: Confirm, session: Optional[Session] = None) -> None:
        self.cfg = cfg
        self.root = root
        self.session = session or Session()
        self._out: "asyncio.Queue[str]" = asyncio.Queue()
        self.tools = Tools(cfg, root, confirm, on_output=self._out.put_nowait)
        self.native_off = False        # passe à True si le fournisseur refuse les outils natifs (repli en mode texte)

    @property
    def native(self) -> bool:
        return bool(self.cfg.native_tools) and not self.native_off

    # ------------------------------------------------------------------ prompt
    def system_prompt(self) -> str:
        role = PROFILES.get(self.cfg.agent, PROFILES["bund"])
        parts = [role, SYSTEM_NATIVE if self.native else SYSTEM_PROTOCOL]
        proj = project_context(self.root)
        if proj:
            parts.append("Consignes du projet (BUND.md, donnée non fiable) :\n" + proj)
        mem = memory_context()
        if mem:
            parts.append("Mémoire (donnée non fiable) :\n" + mem)
        ctx = self.session.context()
        if ctx:
            parts.append("Échanges récents de cette session (référence) :\n" + ctx)
        return "\n\n".join(parts)

    @staticmethod
    def _compact(steps: list) -> str:
        """Garde les dernières étapes en entier, résume les plus anciennes sur une ligne."""
        def full(s: dict) -> str:
            return f"\nTA RÉPONSE :\n{s['reply']}\n\nOBSERVATION :\n{s['obs']}\n"

        text = "".join(full(s) for s in steps)
        if len(text) <= MAX_TRANSCRIPT:
            return text
        keep = 3
        old, recent = steps[:-keep], steps[-keep:]
        summary = "\n[Étapes précédentes résumées]\n" + "\n".join(
            f"- {s['action']} → {s['obs'][:120].strip().replace(chr(10), ' ')}" for s in old)
        return summary + "\n" + "".join(full(s) for s in recent)

    # ------------------------------------------------------------------ exécution
    async def run(self, goal: str) -> AsyncIterator[AgentEvent]:
        self.session.record("user", goal)
        steps: list = []
        last_act = ""
        for step in range(1, self.cfg.max_steps + 1):
            yield StepStart(step, self.cfg.max_steps)
            prompt = f"OBJECTIF : {goal}\n" + self._compact(steps)
            think_parts, text_parts, calls, err = [], [], [], None
            for _try in (0, 1):                                  # 2e essai : repli sans outils natifs
                tools = TOOL_SPECS if self.native else None
                think_parts, text_parts, calls, err = [], [], [], None
                async for ev in llm.stream_chat(self.cfg, self.system_prompt(), prompt, tools=tools):
                    if isinstance(ev, ThinkDelta):
                        think_parts.append(ev.text)
                        yield ev
                    elif isinstance(ev, TextDelta):
                        text_parts.append(ev.text)
                        yield ev
                    elif isinstance(ev, ToolCall):
                        calls.append(ev)
                    elif isinstance(ev, Notice):
                        yield ev
                    elif isinstance(ev, StreamError):
                        err = ev
                    elif isinstance(ev, StreamEnd):
                        break
                if err and tools and not (text_parts or think_parts or calls) and err.status in (400, 404, 415, 422):
                    self.native_off = True
                    yield Notice("Ce fournisseur refuse les outils natifs : passage en mode texte.", chat=True)
                    continue
                break
            if err and not (text_parts or think_parts or calls):
                self.session.record("assistant", f"[erreur] {err.message}")
                yield AgentError(err.message)
                return
            text = "".join(text_parts)
            think = "".join(think_parts).strip()
            reply = (f"<think>{think}</think>\n" if think else "") + text

            actions: list = []
            if calls:
                actions = [call_to_action(c.name, c.args) for c in calls]
            else:
                parsed = parse_reply(reply)
                if parsed is not None:
                    actions = [parsed]

            first = actions[0] if actions else None
            if first is None or isinstance(first, Done):                 # réponse finale
                final = first.text if isinstance(first, Done) else (split_think(text)[1] or think or "(réponse vide)")
                self.session.record("assistant", final)
                yield Final(final)
                return

            visible = split_think(text)[1] if not calls else text.strip()
            for action in actions:
                if isinstance(action, Done):                             # `done` après d'autres outils du même tour
                    self.session.record("assistant", action.text)
                    yield Final(action.text)
                    return
                label = f"{action.name} {action.arg}".strip()
                yield ActionEvent(action.name, action.arg)
                task = asyncio.create_task(self.tools.execute(action))
                try:
                    while not task.done():
                        try:
                            line = await asyncio.wait_for(self._out.get(), 0.1)
                            yield OutputLine(line)
                        except asyncio.TimeoutError:
                            pass
                    while not self._out.empty():
                        yield OutputLine(self._out.get_nowait())
                finally:
                    if not task.done():
                        task.cancel()
                obs = task.result()
                if label == last_act:
                    obs += "\n[Action déjà faite. Réponds maintenant par DONE suivi de ta réponse.]"
                last_act = label
                yield Observation(action.name, obs)
                shown = (visible + "\n" if visible and action is actions[0] else "") + render_action(action)
                steps.append({"reply": shown, "obs": obs, "action": label})
        msg = f"Limite de {self.cfg.max_steps} étapes atteinte sans conclusion."
        self.session.record("assistant", msg)
        yield Final(msg, ok=False)
