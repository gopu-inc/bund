# BUND 0.7.0 — Notcurses TUI

Le TUI utilise l'API Notcurses Core. Le Makefile la détecte avec
`pkg-config notcurses-core`. Si la bibliothèque n'est pas présente, BUND
compile tout de même et démarre en mode texte.

## Installation dans iSH (Alpine)

```sh
apk update
apk add build-base cmake pkgconf ncurses-dev notcurses-dev curl git unzip
unzip BUND-0.6.0-notcurses.zip
cd bund
make clean
make
./bund
```

Les noms de paquets Notcurses peuvent dépendre de la version/architecture des dépôts Alpine actifs. Vérifier avec `apk search notcurses` si `notcurses-dev` est introuvable. Notcurses n'est pas un remplacement source-compatible de ncurses et nécessite ses propres en-têtes et bibliothèques.

## Saisie

- `Ctrl+O` active/désactive le composeur multiligne.
- En mode multiligne, Entrée ajoute une nouvelle ligne et `Ctrl+S` envoie.
- Si `Ctrl+S` est intercepté par le terminal, `Ctrl+O` désactive le composeur
  puis Entrée envoie le texte.
- `/multi` active le mode multiligne pour la prochaine saisie.
- Les flèches, Home/End, Backspace/Delete et Tab servent à éditer le prompt.
- En mode texte, une ligne seule `.` termine un message multiligne.

## Clé API

Ne place pas ta clé API dans le code. Exemple :

```sh
export GROQ_API_KEY='TA_NOUVELLE_CLE'
./bund
```

Si tu as déjà partagé une clé dans une conversation, révoque-la et crée-en une nouvelle.
