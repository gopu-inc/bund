"""Contrôle d'import de la TUI SANS Textual installé (faux modules) : attrape les NameError/SyntaxError.
Ce n'est PAS un test d'affichage : pour cela, lancer `bund` avec Textual installé."""
import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


class _Any:
    def __init__(self, *a, **k): pass
    def __class_getitem__(cls, item): return cls
    def __getattr__(self, n): return _Any()
    def __call__(self, *a, **k): return _Any()


def mod(name, **attrs):
    m = types.ModuleType(name)
    m.__dict__.update(attrs)
    sys.modules[name] = m
    return m


def deco(*a, **k):
    return lambda f: f


class _Meta(type):
    def __getattr__(cls, name):          # ListView.Selected, Input.Submitted, DirectoryTree.FileSelected…
        return _Any

    def __getitem__(cls, item):
        return cls


class Base(metaclass=_Meta):
    def __init__(self, *a, **k): pass


mod("rich"); mod("rich.text", Text=_Any)
mod("textual", on=deco)
mod("textual.app", App=Base, ComposeResult=object)
mod("textual.binding", Binding=_Any)
mod("textual.containers", Horizontal=Base, Vertical=Base, VerticalScroll=Base)
mod("textual.screen", ModalScreen=Base)
mod("textual.suggester", SuggestFromList=_Any)
mod("textual.widgets", DirectoryTree=Base, Footer=Base, Input=Base, Label=Base, ListItem=Base, ListView=Base,
    Markdown=Base, Static=Base, TextArea=Base)
import bund.tui.app as app  # noqa: E402

assert hasattr(app, "BundApp") and hasattr(app, "ConfirmScreen")
print("import TUI OK (faux Textual)")
