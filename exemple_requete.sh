#!/bin/bash
# Exemple d'appel du solveur.
#   ./exemple_requete.sh [host]   (defaut 127.0.0.1)
# Reponse JSON: {"move","key","depth","nodes","nps","time_ms","eval"}
HOST=${1:-127.0.0.1}
PORT=${2:-8766}

echo "== /health =="
curl -s -m 5 "http://$HOST:$PORT/health"
echo

echo "== /solve (plateau exemple) =="
curl -s -m 30 -X POST "http://$HOST:$PORT/solve" \
  -H "Content-Type: application/json" \
  -d '{"cells":[4,2,0,0,0,2,0,0,0,0,0,0,0,0,0,0],"ms":5000}'
echo
