"""Interface web optionnelle (Streamlit) : même agent, même flux mot par mot.

Lancement :  bund web      (nécessite :  pip install "bund[web]")
"""
from __future__ import annotations

import time
from pathlib import Path

import streamlit as st

from bund import __version__
from bund.agent import Agent
from bund.config import PRESETS, Config
from bund.events import (ActionEvent, AgentError, Final, Observation, OutputLine, StepStart, TextDelta, ThinkDelta)
from bund.store import Session
from bund.syncbridge import iter_sync

st.set_page_config(page_title="BUND", page_icon="🤖", layout="wide")

if "cfg" not in st.session_state:
    st.session_state.cfg = Config.load()
    st.session_state.session = Session()
    st.session_state.messages = []
cfg: Config = st.session_state.cfg

with st.sidebar:
    st.title("BUND")
    st.caption(f"v{__version__}")
    preset = st.selectbox("Fournisseur", list(PRESETS), index=list(PRESETS).index("openai"))
    if st.button("Appliquer le preset"):
        cfg.apply_preset(preset)
    cfg.model = st.text_input("Modèle", cfg.model)
    show_think = st.checkbox("Afficher la réflexion", True)
    auto_ok = st.checkbox("Approuver automatiquement les actions (écriture, commandes)", False)
    if st.button("Nouvelle session"):
        st.session_state.session = Session()
        st.session_state.messages = []
        st.rerun()

st.subheader("Agent de développement")
for m in st.session_state.messages:
    with st.chat_message(m["role"]):
        st.markdown(m["text"])

prompt = st.chat_input("Dites-moi ce que vous voulez faire…")
if prompt:
    st.session_state.messages.append({"role": "user", "text": prompt})
    with st.chat_message("user"):
        st.markdown(prompt)

    async def confirm(question: str) -> bool:
        return bool(auto_ok)

    agent = Agent(cfg, Path.cwd(), confirm, st.session_state.session)
    with st.chat_message("assistant"):
        log = st.status("L'agent travaille…", expanded=True)
        think_ph, live_ph = st.empty(), st.empty()
        think, text, shell, last = "", "", "", 0.0
        final = ""
        for ev in iter_sync(agent.run(prompt)):
            if isinstance(ev, StepStart):
                think, text, shell = "", "", ""
            elif isinstance(ev, ThinkDelta):
                think += ev.text
            elif isinstance(ev, TextDelta):
                text += ev.text
            elif isinstance(ev, ActionEvent):
                log.write(f"→ `{ev.name} {ev.arg}`")
                think, text = "", ""
                think_ph.empty()
                live_ph.empty()
            elif isinstance(ev, OutputLine):
                shell += ev.text
            elif isinstance(ev, Observation):
                log.write(f"```\n{ev.text[:600]}\n```")
                shell = ""
            elif isinstance(ev, Final):
                final = ev.text
            elif isinstance(ev, AgentError):
                final = f"✖ {ev.message}"
            now = time.monotonic()
            if now - last > 0.08:                     # ~12 mises à jour/s
                last = now
                if show_think and think:
                    think_ph.markdown("*◌ réflexion* — " + think[-500:])
                if text or shell:
                    live_ph.markdown(text + ("\n```\n" + shell[-1200:] + "\n```" if shell else ""))
        think_ph.empty()
        live_ph.markdown(final or text)
        log.update(label="Terminé", state="complete", expanded=False)
    st.session_state.messages.append({"role": "assistant", "text": final or text})
