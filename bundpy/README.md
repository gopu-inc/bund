# BUND 1.0 — agent de code en Python (Textual + asyncio)

Réécriture complète de BUND (C → Python). Dépôt : https://github.com/gopu-inc/bund

## Installation (iSH / Alpine, Linux, macOS)

```sh
apk add python3 py3-pip git        # Alpine / iSH
pip install textual                # seule dépendance obligatoire
pip install -e .                   # installe la commande « bund »
# interface web facultative :  pip install -e ".[web]"
```

## Utilisation

```sh
bund                       # interface Textual
bund run "corrige le bug"  # sans interface, jetons écrits au fil de l'eau (-y : tout approuver)
bund web                   # interface Streamlit (facultative)
bund conf presets          # fournisseurs : openai, claude, google, groq, deepseek, ollama…
bund conf preset ollama
```

Clé API : variable d'environnement (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`…) ou fichier `~/.bund/secrets/NOM`.
La configuration `~/.bund/bund.env.conf` garde le même format que la version C.

## Ce qui change

- **Streaming asyncio** : lecture réseau dans un thread démon → `asyncio.Queue` ; l'interface ne se fige jamais.
  Réflexion (`reasoning_content`, `<think>`, `thinking`) puis réponse s'écrivent mot par mot.
  OpenAI-compatible (Groq, DeepSeek, Gemini, Ollama…) et Anthropic natif ; relance auto sur 429/5xx.
- **Textual** : repeint par différences (plus de clear d'écran), ~12 images/s, redimensionnement natif,
  modales de confirmation, sessions (Ctrl+R), arbre de fichiers (Ctrl+B), historique ↑/↓, suggestions de commandes.
- **Nouvel outil `edit_file`** (blocs SEARCH/REPLACE ciblés) en plus de `write_file`.
- **Cœur sans dépendance** (`config`, `llm`, `agent`, `tools`, `store`) : testable sans Textual.
- Les manifestes écrits par l'agent (pyproject.toml, package.json…) reçoivent toujours le contributeur Bund + l'URL du dépôt.

## Variables utiles

`BUND_STREAM=0` coupe le flux · `BUND_THINK_BUDGET=2048` réflexion étendue Anthropic (min. 1024) · `BUND_HOME` change `~/.bund`.

## Tests

```sh
python3 -m unittest discover -s tests        # 23 tests, serveur LLM factice inclus (aucune clé requise)
python3 tests/check_tui_import.py            # contrôle d'import de la TUI (faux Textual)
python3 tests/fake_server.py 8765            # serveur de démo : BUND_API_BASE_URL=http://127.0.0.1:8765 BUND_MODEL=agent
```

## Pas encore porté depuis la version C

MCP (`mcp_*`), mode `deep` et sous-agents parallèles, connexion GitHub OAuth (`/login`), `/bot`, `/job`, sélecteur de modèles.
Pour pousser, `git push` utilise vos identifiants git habituels.


## Nouveautés 1.1

- **Interface type IDE** : arbre de fichiers · **éditeur de code** (Ctrl+S ou toucher la barre pour enregistrer) · agent.
  Sur petit écran, trois onglets (Agent / Fichiers / Éditeur). Thème sombre « Replit ».
- **Outils natifs** (function calling) pour OpenAI-compatibles, Anthropic et Ollama, avec repli automatique sur le
  protocole texte si le fournisseur les refuse. Désactivable : `/conf set native_tools false`.
- **Réflexion en direct** : détectée dans les champs `reasoning_content`, `reasoning`, `thinking` (ou balises `<think>`),
  y compris Ollama (`/v1` ou API native avec une URL de base en `/api`). Un toucher déplie la réflexion terminée.
- `/model` (liste du fournisseur), `/agents`, `/mcp`, `/login github JETON [bot [nom]]`, `/doctor`.
- `bund doctor` : diagnostic réseau / clé / flux / outils.
- Les données sont écrites dans un dossier réellement inscriptible (iOS, a-Shell, iSH) ; voir `BUND_HOME`.
