# Solveur 2048 — C++20 (bitboard + expectimax + LUT précalculées)

> **Guide pour agent IA.** Tu n'as aucun contexte préalable : ce document
> explique ce que fait le projet, comment il marche (dont les **précalculs**),
> et comment le lancer de zéro sur un **Orange Pi 3B** puis le piloter depuis
> un navigateur. Si tu as la main sur la machine, suis la section
> [Recette agent](#7-recette-agent-ia--démarrage-de-a-à-z).

## 1. Mission du projet

Le projet joue **automatiquement** au jeu web 2048
(<https://eleves.rezal-mdm.com/jeux/2048>) et vise le haut du classement
(tuiles 8192/16384). Il se compose de 4 morceaux :

| Morceau | Fichier | Rôle |
|---|---|---|
| Moteur de jeu | `main.cpp` | choisit le meilleur coup d'un plateau (C++20, ~10-17 M nœuds/s) |
| Serveur HTTP | idem, `--serve` | expose le moteur sur le port **8766** pour le navigateur |
| Clients navigateur | `solver2048.user.js`, `console-bot.js` | lisent le plateau dans le DOM, appellent le serveur, envoient la touche |
| Tuning heuristique | `tuner.ps1` (Windows) | campagne de mesures pour comparer des pondérations |

**Boucle de fonctionnement** (un « coup » = un tour de 2048) :

```
DOM du jeu (.jeux-2048-cell ×16) ──cells──▶ POST /solve ──▶ moteur C++
        ▲                                         │ move/key
        └──── KeyboardEvent("ArrowUp") ◀──────────┘
 puis : attendre le changement de plateau → coup suivant
 (après Game Over : cliquer « Nouvelle Partie », ou abandonner si trop faible)
```

Le serveur tourne **en fond** (idéalement sur l'Orange Pi, accessible du
réseau local) ; le navigateur ne fait que des allers-retours JSON.

## 2. Architecture du moteur (ce que fait `main.cpp`)

- **Bitboard 64 bits** : les 16 cases du plateau tiennent dans un `uint64_t`
  (4 bits par case, valeur = exposant de 2 : 0=vide, 1=2, 2=4, … 15=32768).
  Toutes les opérations sont des bit-twiddling sans allocation.
- **Coups** : glisser une ligne = table `row_left[65536]` / `row_right[65536]`
  (voir précalculs ci-dessous). Un plateau = 4 lignes + transposition.
- **Expectimax** : l'aléa de 2048 (apparition de 2 ou 4) est modélisé par des
  nœuds *chance* (0,9 / 0,1 sur chaque case vide) ; les choix du joueur par
  des nœuds *max*. `max_node(d)` / `chance_node(d)` alternent, `d` décroît.
- **Évaluation** `evaluate(b)` en feuille : pondération de — cases vides,
  mono-tonicité, douceur (smooth), max, snake (poids par position), coin.
- **Table de transposition (TT)** : 2²⁰ entrées **par thread** (calloc à la
  première utilisation), clé = bitboard, profondeur exacte (`depth == d`),
  2-ways. Évite d'explorer deux fois la même position au même profondeur.
- **Découpe itérative** : `solve()` essaie le niveau `d = 1, 2, 3 … max_depth`,
  arrêt au budget (temps `g_deadline` / limite de nœuds) via `tick()`
  (contrôle toutes les 16 384 NODES PAR THREAD). Dernier niveau **complet**
  = profondeur retournée (`depth`).
- **`policy_depth`** : plafond de profondeur selon les cases vides `e` :

  | e | ≥9 | ≤8 | ≤6 | ≤4 | ≤2 | ≤1 |
  |---|---|---|---|---|---|---|
   | prof. max | 7 | 8 | 9 | 10 | 11 | 12 |

  (plafond global `--depth 13` par défaut ; le vrai garde-fou est le budget
  temps — seul le dernier niveau **complet** est retourné).
- **Pool de workers *sticky*** : les 4 threads de recherche évaluent **toujours
  les mêmes directions racine** (`worker wid` → directions `wid, wid+4…`).
  Chaque direction reste donc sur **une seule TT** d'un niveau à l'autre et
  d'un coup à l'autre → déterminisme (les comptes de nœuds sont reproductibles
  au nœud près) et taux de hits TT maximal. ⚠️ Ne « désynchronise » pas :
  l'ancien `claim++` aléatoire faisait varier les nœuds ×2 à ×10.

### 2.1 Les précalculs (LUT) — comment ils marchent et les utiliser

**Tout est recalculé automatiquement au démarrage du binaire** (≈ 2 ms) :
aucun fichier de données à télécharger, rien à régénérer manuellement.
Ligne d'attente attendue au lancement :

```
2048 solver - bitboard 64 bits + expectimax + table de transposition
LUT initialisees en 1.78 ms
poids: empty_base=120 empty=45 mono=55 smooth=22 snake=20 corner=380
threads: 4
```

Deux fonctions construisent tout, appelées dans `main()` avant le moindre
traitement (`init_lut()` puis `init_eval_lut()`, main.cpp:1264-1266) :

1. **`init_lut()` — précalcul des coups** (main.cpp:120)
   - Pour les 65 536 motifs de ligne possibles (4 cases × 4 bits) :
     `row_left[i]` / `row_right[i]` (uint16 : ligne après glissement) et
     `score_left[i]` / `score_right[i]` (uint32 : points du glissement).
   - Construit par `build_row()`, version de référence non optimisée
     `naive_slide()` conservée pour les tests.
2. **`init_eval_lut()` — précalcul de l'évaluation** (main.cpp:136)
   - `g_line[65536]` : par ligne, encode en un seul uint32
     `mono:8 | smooth:8 | empt:4 | mx:4` (monotonicité, douceur, cases vides,
     valeur max). L'évaluation d'un plateau = 4 lectures ligne + 4 colonne.
   - `g_snake_row[4][65536]` : contribution snake de chaque rangée selon la
     position (poids `SNAKE_W[]` compilés **à la construction** de la LUT).

**Utiliser les précalculs concrètement :**

- **Agent / utilisateur final** : rien à faire. Lancer le binaire suffit —
  les LUT sont en mémoire dès l'affiche `LUT initialisees en … ms`. Si cette
  ligne est absente ou que le temps est aberrant, le binaire est corrompu :
  recompiler.
- **Vérification** : `./solver2048 --selftest` recalcule tout et compare à la
  référence brute → doit afficher
  `SELFTEST: OK (65536 lignes + 20000 plateaux x 4 coups + transposition)`.
  Il vérifie aussi `evaluate()` contre `evaluate_ref()` (équivalence
  bit-exacte de la version LUT).
- **Si tu changes les poids** (recompilation `-DCFG_W_SNAKE=…` ou édition des
  `W_*`), **il faut recompiler** : `g_snake_row` et `g_line` sont des constantes
  de compilation. Aucun autre précalcul à toucher.
- **La TT** (à ne pas confondre avec les LUT) n'est PAS un précalcul : elle est
  allouée à la volée (1 Mo/thread), persiste **entre les coups** d'une même
  partie (chaque appel de `solve()` la conserve) et n'est pas persistée sur
  disque.
- Attendus de performance (i5-7300U) : 10,4 M nps (1 thread), 17,6 M nps
  (4 threads) ; sur A55 attendre ~40-60 % de ça. Le **selftest** est la
  seule validation obligatoire après build.

## 3. Lancer le projet

### 3.1 Orange Pi 3B (Linux — cible principale)

Prérequis : `g++` (≥ 10, C++20 : `std::popcount`, `std::countr_zero`,
`std::thread`) — sur Debian/Armbian : `sudo apt install build-essential`.

```bash
git clone https://github.com/DocteurWu/2048-solveur.git
cd 2048-solveur
 ./build.sh        # = g++ -O3 -march=native -flto -std=c++20 -pthread -Wall -Wextra main.cpp -o solver2048
                  # + ./solver2048 --selftest   (doit dire OK)
```

**Lancer en service (recommandé, redémarrage auto) :**

```bash
sudo cp 2048-solver.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now 2048-solver
systemctl status 2048-solver     # doit dire "active (running)"
journalctl -u 2048-solver -f     # logs en direct
```

**…ou en avant-plan (débogage)** :

```bash
./lancer_serveur.sh              # PORT=8766 MS=5000 THREADS=4 (surchargeables)
```

**Vérifications, dans l'ordre :**

```bash
# 1. le serveur répond (sortie attendue : {"ok":true})
curl -s http://127.0.0.1:8766/health

# 2. il joue (sortie attendue : JSON avec "move":"haut", "depth":5, …)
./exemple_requete.sh 127.0.0.1

# 3. il est joignable depuis l'extérieur (depuis un autre ordinateur)
curl -s http://IP_DE_LA_CARTE:8766/health
```

Récupérer l'IP : `hostname -I`. Si le pare-feu est actif :
`sudo ufw allow 8766/tcp`. Le serveur écoute sur **0.0.0.0:8766** (toutes
interfaces) — c'est voulu pour le LAN.

### 3.2 Windows (poste de dev / tuning)

```powershell
# MinGW-w64 (exemple WinLibs via winget) puis :
g++ -O3 -march=native -flto -std=c++20 -Wall -Wextra main.cpp -o solver2048.exe

.\solver2048.exe --selftest     # vérif
.\solver2048.exe --bench        # NPS
.\solver2048.exe --play --ms 1000 --threads 4    # partie console
.\lancer_serveur.bat            # serveur 0.0.0.0:8766, ms=5000, threads=4
```

> **Piège PowerShell** : ne mets JAMAIS le JSON directement dans la ligne
> `curl.exe` — les guillemets sont mangés et le serveur répond
> `{"error":"cells manquantes"}`. Passe par un fichier :
> `curl -X POST ... --data-binary @req.json`.

### 3.3 Modes du binaire (référence)

| Démarrage | Fait |
|---|---|
| aucun argument | selftest + partie + bench |
| `--selftest` | vérifie LUT, évaluation, glissements, TT |
| `--play [--seed N] [--ms N] [--games N]` | partie en console (imprime plateau/`prof`/nœuds) |
| `--bench` | NPS sur 4 plateaux types |
| `--serve [--port N]` | serveur HTTP (GET `/health`, POST `/solve`) |
| `--ms N` | budget par coup (défaut 150 ; serveur scripts : 5000) |
 | `--depth N` | plafond de profondeur (défaut 13) |
| `--threads N` | threads de recherche (défaut auto = min(4, cœurs)) |
| `--verbose` | plateau à chaque coup en mode `--play` |

## 4. API du serveur

### `GET /health` → `{"ok":true}`

### `POST /solve`

Requête :

```json
{"cells": [4,2,0,0, 0,2,0,0, 0,0,0,0, 0,0,0,0],
 "ms": 5000,
  "depth": 13}
```

- `cells` : **obligatoire**. 16 entiers, **ligne par ligne (row-major)**,
  tuiles en valeur réelle (0 = vide, 2, 4, … 32768). Même ordre que le DOM :
  index `4*r+c`.
- `ms` : budget en millisecondes (défaut = `--ms` du serveur).
- `depth` : plafond de profondeur (défaut = `--depth`).

Réponse :

```json
{"move":"haut","key":"ArrowUp","depth":5,"nodes":8175616,
 "nps":14591448,"time_ms":1000.2,"eval":6338.6}
```

| champ | signification |
|---|---|
| `move` | direction en français : `gauche`/`droite`/`haut`/`bas`, ou `null` (partie finie) |
| `key` | touche à dispatcher : `ArrowLeft`/`ArrowRight`/`ArrowUp`/`ArrowDown` |
| `depth` | profondeur **complétée** (niveau iteratif abouti) |
| `nodes` / `nps` / `time_ms` | nœuds explorés, nœuds/s, temps réellement utilisé |
| `eval` | valeur expectimax du coup choisi (comparaison entre coups possible) |

Erreurs : `{"error":"cells manquantes"}` (400) si `cells` absent/invalide ;
`{"error":"inconnu"}` (404) pour une autre route.

**Budgets recommandés par cases vides** (les plateaux quasi vides sont les
PLUS chers : 28 branches de spawn par niveau, niveau 4 ≈ 85 M nœuds ≈ 5 s —
un plateau à e=9, lui, fait 5 niveaux en < 1 s) :

| cases vides | ≥13 | 9-12 | 6-8 | 4-5 | 2-3 | ≤1 |
|---|---|---|---|---|---|---|
| `ms` | 5000 | 2000 | 4000 | 6000 | 8000 | 10000 |

Ces valeurs sont **déjà codées** dans les clients (`budgetFor`).

## 5. Pilotage du navigateur

### Option A — userscript (recommandé)

1. Installer Violentmonkey (ou Tampermonkey) dans le navigateur.
2. Importer `solver2048.user.js` (Glisser-déposer ou « Créer une nouvelle
   script » → coller).
3. Sur la page du jeu, définir l'URL du serveur **si ce n'est pas la machine
   locale** (DevTools → Console) :

   ```js
   localStorage.setItem('solver2048_url', 'http://IP_ORPI:8766/solve');
   ```

4. Charger la page, appuyer sur **S** (start/stop).

Ce que le script automatise :

- lit les 16 tuiles (`.jeux-2048-cell`, classes `tile-N`) et le score ;
- calcule le budget selon les cases vides, appelle `/solve` (timeout adapté) ;
- dispatch la touche, **attend le changement de plateau** (sinon retry) ;
- **auto-start** (clic « Commencer »), **auto-restart** après Game Over
  (clic « Nouvelle Partie » + attend un plateau frais à 2 tuiles) ;
- **early-abandon** pour farmer : relance si tuile < 256 au coup 200,
  < 1024 au coup 600, < 2048 au coup 1200 ;
- HUD en bas à droite (état, coups, score, prof/nps, stats de session) ;
- jitter aléatoire ≤ 300 ms entre les coups (rythme naturel) ;
- coupe après 8 erreurs consécutives (serveur injoignable).

Garde l'onglet du jeu **visible** : les navigateurs ralentissent les timers
des onglets en arrière-plan.

### Option B — console DevTools (sans extension)

Ouvrir la console sur la page du jeu, coller le contenu de `console-bot.js`
(même logique, `SERVER` en tête de fichier à adapter). **S** pour stopper.

### Option C — agent IA externe (pilotage complet)

Un agent qui a accès au navigateur (CDP/Playwright/type de script) peut tout
faire lui-même sans userscript :

1. `document.querySelectorAll('.jeux-2048-cell')` → 16 valeurs
   (classe `tile-N`, sinon 0), index `4*r+c` ;
2. `POST /solve` avec `cells` + `ms` (table de la section 4) ;
3. dispatcher `key` en `KeyboardEvent('keydown', {key, bubbles:true})`
   sur `document` ;
4. attendre ~0,3-1 s que le DOM change ; si `document.body.innerText`
   contient `Game Over` → cliquer le bouton dont le texte est
   `Nouvelle Partie` (sinon `Commencer`), attendre 2 tuiles → reprise ;
5. appliquer les règles d'early-abandon ; répéter.

Contraintes : ne JAMAIS envoyer de JSON avec guillemets échappés à la
volée (fichier ou `JSON.stringify`), respecter `ms` (un coup à budget
épuisé retourne le dernier niveau complété, souvent `depth` 3-5), et ne pas
chevaucher deux requêtes (le serveur traite séquentiellement).

## 6. Tuning heuristique (Windows uniquement)

```powershell
.\tuner.ps1 -Ms 400 -Parallel 4
```

- Compile une variante par config (`-DCFG_W_EMPTY_BASE=…`, `CFG_W_MONO`,
  `CFG_W_SNAKE`, `CFG_W_CORNER`, `CFG_W_SMOOTH`, `CFG_W_EMPTY`) dans `tune/`.
- Joue 4 seeds appariées (2048, 99991, 777001, 424242) **en parallèle** avec
  `--threads 1` par jeu (comparaison appariée propre).
- Agrège → `tuning_summary.txt` (classement) + `tuning_results.csv`.

Historique (détail dans `tuning_summary.txt`, données brutes
`tuning_results_r1.csv` / `tuning_results_r2.csv`) :

- **Round 1** (ancien moteur, 200 ms) : `base` 1ᵉʳ (avg tile 3072) devant
  mono75, corner600, empty160… toutes les hausses testées ont perdu.
- **Round 2** (moteur sticky, 400 ms, threads=1) : **`base` et `snake12`
  égalité** (avg tile 3072 ; avg score 53 269 vs 53 270 = bruit), puis
  mono45 (2560), mono75/corner600 (2304), corner250 (2048), empty160
  (1792), empty90 (1408).

**Conclusion : les poids par défaut (120/45/55/22/20/380) sont conservés** —
ils gagnent ou égalent tout sur deux rounds et deux moteurs ; baisser snake
20→12 ne change rien (bruit). Le tuner reprend là où il s'est arrêté si la
machine redémarre (CSV déjà rempli = jeux sautés).

## 7. Recette agent IA — démarrage de A à Z

```bash
# 0. matériel : Orange Pi 3B sur le même réseau que le navigateur, SSH ouvert
git clone https://github.com/DocteurWu/2048-solveur.git && cd 2048-solveur

# 1. build + validation des précalculs (MUST : "SELFTEST: OK")
./build.sh

# 2. démarrage permanent du serveur
sudo cp 2048-solver.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo systemctl enable --now 2048-solver

# 3. tests ({"ok":true} puis un JSON "move")
curl -s http://127.0.0.1:8766/health
./exemple_requete.sh 127.0.0.1

# 4. brancher le navigateur
#    - importer solver2048.user.js (Violentmonkey)
#    - page du jeu : localStorage.setItem('solver2048_url','http://<IP>:8766/solve')
#    - recharger, touche S, vérifier le HUD ("etat : joue")

# 5. supervision
watch -n5 curl -s http://127.0.0.1:8766/health
journalctl -u 2048-solver -f          # requêtes : "move=... prof=... nps"
#   scores/tuiles : HUD navigateur ; résultats de tuning : tuning_summary.txt
```

Points de vigilance pour un agent : **ne pas** publier le port 8766 sur
Internet (LAN uniquement), **ne pas** lancer deux instances (port déjà
utilisé → `systemctl restart 2048-solver`), et **ne pas** modifier les
scripts en CRLF (le `.gitattributes` force LF pour `*.sh`).

### 7.1 Checklist agent — valider une optimisation du moteur

Toute modification de `main.cpp` qui touche la recherche ou l'évaluation
doit passer cette grille **dans l'ordre** :

```bash
# 1. selftest (LUT + eval bit-exact + TT) — MUST "SELFTEST: OK"
./solver2048 --selftest

# 2. bench déterministe — les NŒUDS doivent rester identiques :
#    t4 = 2080768, t1 = 1835008. Un écart = l'évaluation a bougé → STOP.
./solver2048 --bench --threads 4 --ms 5000

# 3. A/B apparié (si la recherche/heuristique change) — MÊMES conditions
#    des deux côtés, 4 jeux en parallèle, ~15 min :
ancien binaire : cmd /c "git show HEAD:main.cpp > old_main.cpp"   # PAS de > PowerShell (UTF-16 !)
                  g++ -O3 -march=native -std=c++20 old_main.cpp -o solver_old.exe
nouveau binaire : g++ -O3 -march=native -flto -std=c++20 main.cpp -o solver2048.exe
les 2 : --play --seed S --ms 400 --threads 1   pour S ∈ {2048, 99991, 777001, 424242}
comparer le "Score :" final des 4 logs ; garder seulement si total ≥ baseline.

# 4. rebuild du serveur : tuer l'ancien process, recompiler (sinon
#    "Permission denied"), relancer, curl /health → {"ok":true}

# 5. commit + push
```

Pièges déjà rencontrés (ne pas retomber) :

- **PowerShell `>` ré-encode en UTF-16** → un `main.cpp` corrompu
  (`'i' does not name a type` en pagaille). Toujours `cmd /c "git show … > f"`.
- **`solver2048.exe` verrouillé** tant que le serveur tourne → tuer le
  process avant de relancer `g++`.
- **Comparaison à un ancien run isolé** : fausse — l'arrêt au budget dépend
  de l'horloge (charge CPU), 2 runs d'un même binaire donnent des parties
  différentes. Seul le A/B apparié (ancien vs nouveau, même session) prouve
  quelque chose.
- **`-flto`** est dans tous les builds (gain ~+5 % NPS vérifié en A/B
  intercalé ; inutile de le retirer, inutile de le « retrouver »).

## 8. Dépannage

| Symptôme | Cause / solution |
|---|---|
| `{"error":"cells manquantes"}` | JSON mangé (PowerShell) → passer par `@fichier` |
| `connection refused` sur /health | service pas démarré → `systemctl status 2048-solver` |
| `bind() echec … (port occupe ?)` | instance déjà là → `systemctl restart` ou tuer le process |
| fetch bloqué depuis le navigateur (CORS/LNA) | userscript (GM_xmlhttpRequest) plutôt que `fetch` ; sinon vérifier l'IP |
| `Permission denied` à la compilation | binaire encore lancé → arrêter le serveur avant rebuild |
| scripts `.sh` refusés (`CRLF: command not found`) | `sed -i 's/\r$//' *.sh` (le repo force déjà LF) |
| `depth` toujours 3-5 avec gros `ms` | normal : les plateaux vides coûtent cher (cf. budgets) ; le niveau supérieur revient souvent quasi gratuit après coup (TT) |
| NPS faible | charge CPU concurrente (navigateur, autres services) ; normal sur A55 (~40-60 % du PC) |
| partie qui s'arrête vite | early-abandon volontaire (HUD : `restart (cadence faible)`) |

## 9. Fichiers du dépôt

| Fichier | Rôle |
|---|---|
| `main.cpp` | moteur + serveur (tout-en-un) |
| `build.sh` | build Linux + selftest |
| `lancer_serveur.sh` / `2048-solver.service` | lancement Manuel / systemd (port 8766) |
| `exemple_requete.sh` / `test_serve.sh` | tests API |
| `solver2048.user.js` | clients navigateur (budgets, restart, abandon, HUD) |
| `console-bot.js` | fallback DevTools |
| `lancer_serveur.bat` | serveur Windows |
| `tuner.ps1` + `tuning_summary.txt`, `tuning_results_r1.csv`, `tuning_results_r2.csv` | campagnes de poids |
| `.gitignore` / `.gitattributes` | hors binaires ; LF imposé pour les `*.sh` |
