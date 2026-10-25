# BUND 0.8.0 — TUI, saisie et GitHub

## Interface

BUND utilise Notcurses Core quand il est disponible, sinon il passe en mode texte.
La mise en page s'adapte à la largeur du terminal :

- **< 100 colonnes** (téléphone, iSH) : une seule colonne — en-tête, sortie, ligne d'état,
  boîte de saisie, aide.
- **≥ 100 colonnes** : un panneau latéral s'ajoute (GitHub, IA, Git, fichiers du dossier).
  Les statuts affichés sont réels : GitHub vient du jeton local, IA indique si la clé API est
  présente, Git donne la branche et le nombre de fichiers modifiés. Rien n'est simulé
  (pas de faux « MCP connecté »).

La sortie des commandes et de l'agent s'affiche **au fil des étapes**, et les confirmations
(écriture de fichier, commande, commit, push) apparaissent dans une boîte au centre.

## Saisie

Le texte passe à la ligne automatiquement au bord de l'écran (coupe au caractère, jamais au
milieu d'un caractère accentué ou large). La boîte de saisie grandit jusqu'à 2 à 6 lignes selon
la hauteur du terminal, puis défile (indicateur `↑ n`).

| Touche | Action |
|---|---|
| Entrée | envoie (en mode multiligne : nouvelle ligne) |
| Ctrl+O | active/désactive le mode multiligne ; Ctrl+S envoie |
| ← → | déplace le curseur |
| ↑ ↓ | ligne visuelle précédente/suivante, puis historique en bord de texte ; choisit dans la fenêtre d'autocomplétion si elle est ouverte |
| Home / End (Ctrl+A / Ctrl+E) | début / fin de la ligne logique |
| Tab | accepte la suggestion choisie ; sur saisie vide, insère un prompt prêt à l'emploi |
| Ctrl+C | vide la saisie |
| Ctrl+L | efface l'écran |
| Ctrl+D | quitte (saisie vide) |

Autocomplétion : `/…` (commandes), `git …` / `/git …` (add, commit -m, push, status, log…),
`/conf preset …`, `/agent use …`. Tu peux taper `git add .` directement, sans `/`.

## GitHub et commits « Bund »

1. `/login` — l'ID client OAuth est intégré (`Ov23lig…`, public, sans secret). BUND affiche
   l'URL `https://github.com/login/device` et un code ; autorise dans le navigateur.
   Si la CLI `gh` est déjà connectée, elle est utilisée.
2. `/git add .`, `/git commit -m "message"`, `/git push` — ou `git add .` directement.
   `git push` sans upstream pousse vers `origin` et mémorise la branche.
3. Les commits faits par BUND (`/git commit` et l'action agent `git_commit`) ont pour
   **auteur et committer « Bund »**. L'e-mail est le `ID+login@users.noreply.github.com` du
   compte connecté, pour que GitHub rattache le commit à ton compte (contributions, avatar).
   Ta configuration git globale n'est jamais modifiée.

Réglages : `/conf set GIT_NAME "Bund"`, `/conf set GIT_EMAIL moi@exemple.fr`
(vide = e-mail noreply du compte connecté).

Important : un OAuth App agit **au nom de l'utilisateur connecté** — le push est autorisé par
ton compte, pas par un compte « bund[bot] » distinct. Pour que GitHub affiche un compte Bund
séparé, connecte-toi avec un compte GitHub dédié à Bund, ou utilise une GitHub App (clé privée
côté serveur, donc non distribuable dans cette CLI).

Garde-fous de l'agent : `git_commit` et `git_push` demandent toujours une confirmation (sauf
`BUND_AUTO_COMMIT` / `BUND_AUTO_PUSH`), et un commit est refusé si l'index contient un fichier
`.env`, une clé, un certificat ou un dossier `secrets/`.

Le contributeur `Bund <https://github.com/gopu-inc/bund>` est ajouté aux manifestes
(`pyproject.toml`, `setup.py`, `package.json`, …) à chaque commit.

## Tests

```sh
make test       # noyau, éditeur (UTF-8, retour à la ligne, fuzz), autocomplétion
make tui-sim    # compile la TUI contre un faux Notcurses (tests/nc_stub)
STUB_ROWS=30 STUB_COLS=40 ./tests/tui_sim 'git {SNAP 1}'
```

Le simulateur rejoue des touches et affiche l'écran en texte : utile pour vérifier la mise en
page et le comportement de la saisie. Il **ne remplace pas** un essai sur un vrai terminal.

## Installation iSH

```sh
apk add build-base curl git pkgconf notcurses-dev   # notcurses-dev facultatif
make clean && make && ./bund
```
