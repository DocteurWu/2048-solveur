# Solveur 2048 — C++20 (bitboard + expectimax + table de transposition)

Solveur complet pour <https://eleves.rezal-mdm.com/jeux/2048> :

- **Moteur** : bitboard 64 bits, LUT d'évaluation, table de transposition par
  thread, expectimax avec découpe itérative, pool de workers *sticky*
  (affinité direction → worker, déterministe), ~10-17 M nps.
- **Serveur HTTP** interrogé à chaque coup par le navigateur (ou un agent IA).
- **Clients** : userscript Violentmonkey + fallback console.
- **Tuning** : harnais de campagne d'heuristique (Windows, poids `-DCFG_W_*`).

## Orange Pi 3B (Linux)

### Build

```bash
./build.sh          # g++ -O3 -march=native -std=c++20 -pthread + selftest
```

### Lancer le serveur (port 8766, toutes interfaces)

```bash
./lancer_serveur.sh                 # PORT=8766 MS=5000 THREADS=4
```

ou en service systemd :

```bash
sudo cp 2048-solver.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now 2048-solver
systemctl status 2048-solver
```

### API

```
GET  /health            -> {"ok":true}
POST /solve             -> {"move":"haut","key":"ArrowUp","depth":5,
                            "nodes":...,"nps":...,"time_ms":...,"eval":...}
```

Corps de la requête :

```json
{"cells": [16 entiers, ligne par ligne, tuiles réelles (0 = vide)],
 "ms": 5000,            // budget en ms (optionnel, défaut serveur)
 "depth": 11}           // plafond de profondeur (optionnel)
```

Test depuis la carte :

```bash
./exemple_requete.sh 127.0.0.1
```

**Budget conseillé par nombre de cases vides** (les plateaux quasi vides sont
les plus chers : 28 branches de spawn par niveau → niveau 4 ≈ 85 M nœuds) :

| cases vides | ≥13 | 9-12 | 6-8 | 4-5 | 2-3 | ≤1 |
|---|---|---|---|---|---|---|
| budget | 5000 ms | 2000 | 4000 | 6000 | 8000 | 10000 |

### Automatisation navigateur

1. Installer Violentmonkey, importer `solver2048.user.js`.
2. Sur la page du jeu, définir l'URL de la carte :
   `localStorage.setItem('solver2048_url', 'http://IP_DE_LA_CARTE:8766/solve')`
3. Touche **S** : start/stop. Le script gère l'auto-start, le auto-restart
   après Game Over, les budgets par phase, un HUD de session et un léger
   jitter entre les coups.

Fallback sans extension : coller `console-bot.js` dans la console DevTools.

**Early-abandon** (pour farmer) : le script redémarre la partie si
tuile max < 256 au coup 200, < 1024 au coup 600, ou < 2048 au coup 1200.

## Windows

```powershell
# build (MinGW/g++ -Wall -Wextra sans warning)
g++ -O3 -march=native -std=c++20 -Wall -Wextra main.cpp -o solver2048.exe

.\solver2048.exe --selftest                      # 65536 lignes + 20000 plateaux
.\solver2048.exe --bench --threads 4             # NPS
.\solver2048.exe --play --ms 1000 --threads 4    # partie en console
.\lancer_serveur.bat                             # serveur sur 0.0.0.0:8766
```

> **Piège PowerShell** : les guillemets du JSON sont mangés par `curl.exe`
> si on les met dans la ligne de commande. Passer par un fichier :
> `curl -X POST ... --data-binary @req.json`.

## Tuning heuristique (Windows)

```powershell
.\tuner.ps1 -Ms 400 -Parallel 4
```

Compile une variante par config (`-DCFG_W_EMPTY_BASE`, `CFG_W_MONO`,
`CFG_W_SNAKE`, `CFG_W_CORNER`, `CFG_W_SMOOTH`, `CFG_W_EMPTY`), joue 4 seeds
appariées en parallèle (`--threads 1` par jeu) et écrit
`tuning_summary.txt` / `tuning_results.csv`.

Historique dans `tuning_summary.txt` : **Round 1** → les poids `base`
actuels gagnent face à toutes les hausses/baisses testées ; **Round 2**
(moteur sticky) confirme ou infirme.
