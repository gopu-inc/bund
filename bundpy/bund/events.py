"""Événements émis par le flux LLM et par l'agent (tous asynchrones)."""
from __future__ import annotations

from dataclasses import dataclass


# ---- flux LLM ----
@dataclass
class ThinkDelta:
    text: str


@dataclass
class TextDelta:
    text: str


@dataclass
class StreamError:
    message: str
    status: int = 0
    retryable: bool = False


@dataclass
class StreamEnd:
    pass


@dataclass
class ToolCall:
    """Appel d'outil natif (function calling) renvoyé par le fournisseur."""
    name: str
    args: dict
    id: str = ""


@dataclass
class Notice:
    """Information passagère (connexion, nouvelle tentative…). chat=True : à écrire dans le fil."""
    text: str
    chat: bool = False


# ---- agent ----
@dataclass
class StepStart:
    step: int
    total: int


@dataclass
class ActionEvent:
    name: str
    arg: str


@dataclass
class Observation:
    name: str
    text: str


@dataclass
class OutputLine:
    """Sortie en direct d'une commande shell."""
    text: str


@dataclass
class Final:
    text: str
    ok: bool = True


@dataclass
class AgentError:
    message: str
