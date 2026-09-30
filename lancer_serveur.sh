#!/bin/bash
# Lancement du serveur solveur.
#   PORT=8766 MS=5000 THREADS=4 ./lancer_serveur.sh
cd "$(dirname "$0")"
PORT=${PORT:-8766}
MS=${MS:-5000}
THREADS=${THREADS:-4}
DEPTH=${DEPTH:-13}
exec ./solver2048 --serve --port "$PORT" --ms "$MS" --threads "$THREADS" --depth "$DEPTH"
