#!/bin/bash
# Test du serveur POSIX : health + solve, puis arret.
cd "$(dirname "$0")"
./solver2048-linux --serve --port 8767 --ms 400 --threads 4 > /tmp/ws.log 2>&1 &
SPID=$!
sleep 2
echo "--- health ---"
curl -s -m 5 http://127.0.0.1:8767/health
echo
echo "--- solve ---"
curl -s -m 20 -X POST http://127.0.0.1:8767/solve \
  -H "Content-Type: application/json" \
  -d '{"cells":[4,2,0,0,0,2,0,0,0,0,0,0,0,0,0,0],"ms":400}'
echo
kill $SPID 2>/dev/null
wait $SPID 2>/dev/null
echo "--- log serveur ---"
cat /tmp/ws.log
