// ==UserScript==
// @name         2048 Solver - Pont local
// @namespace    2048-solver-local
// @version      2.0.0
// @description  Joue au 2048 via le solveur C++ local (solver2048 --serve)
// @match        https://eleves.rezal-mdm.com/jeux/2048*
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// @connect      localhost
// @connect      *
// @run-at       document-idle
// ==/UserScript==

(function () {
    'use strict';

    // URL du solveur : surchargeable sans editer le script :
    //   localStorage.setItem('solver2048_url', 'http://IP_ORPI:8766/solve')
    const SERVER =
        (typeof localStorage !== 'undefined' && localStorage.getItem('solver2048_url')) ||
        'http://127.0.0.1:8766/solve';
    const POST_DELAY_MS = 30;
    const JITTER_MS = 300;           // aleatoire entre les coups (rythme naturel)
    const WAIT_TIMEOUT_MS = 5000;
    const POLL_MS = 40;
    const AUTO_START = true;

    // Early-abandon : partie sans avenir -> restart (plus de parties = plus de cartons)
    const ABANDON = [
        { at: 200, minTile: 256 },
        { at: 600, minTile: 1024 },
        { at: 1200, minTile: 2048 },
    ];

    // Budget de recherche par coup selon le nombre de cases vides.
    // Attention : un plateau quasi VIDE est le plus cher (28 branches de
    // spawn par niveau -> niveau 4 ~ 85M noeuds) -> dotation superieure.
    function budgetFor(empt) {
        if (empt >= 13) return 5000;
        if (empt >= 9) return 2000;
        if (empt >= 6) return 4000;
        if (empt >= 4) return 6000;
        if (empt >= 2) return 8000;
        return 10000;
    }

    let running = false;
    let runMoves = 0;
    let status = 'arrete';
    let hud = null;
    let stats = { games: 0, bestScore: 0, bestTile: 0 };
    let sess = { depth: 0, nodes: 0, nps: 0, ms: 0 };

    function readBoard() {
        const cells = document.querySelectorAll('.jeux-2048-cell');
        if (cells.length !== 16) return null;
        const values = [];
        for (const cell of cells) {
            let v = 0;
            for (const cls of cell.classList) {
                if (cls.startsWith('tile-')) {
                    v = parseInt(cls.slice(5), 10) || 0;
                    break;
                }
            }
            values.push(v);
        }
        return values;
    }

    function readScore() {
        const boxes = document.querySelectorAll('.score-box .score-value');
        if (boxes.length < 2) return 0;
        return parseInt(boxes[1].textContent, 10) || 0;
    }

    function isGameOver() {
        return document.body.innerText.includes('Game Over');
    }

    function maxTile(cells) {
        return cells.reduce((a, b) => (b > a ? b : a), 0);
    }

    function countEmpty(cells) {
        let n = 0;
        for (const v of cells) if (v === 0) n++;
        return n;
    }

    function isTyping(e) {
        const t = e.target;
        return t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable);
    }

    function requestMove(cells, budget) {
        const body = JSON.stringify({ cells: cells, ms: budget });
        return new Promise((resolve, reject) => {
            if (typeof GM_xmlhttpRequest === 'function') {
                GM_xmlhttpRequest({
                    method: 'POST',
                    url: SERVER,
                    headers: { 'Content-Type': 'application/json' },
                    data: body,
                    timeout: budget + 15000,
                    onload: (r) => {
                        if (r.status === 200) {
                            try { resolve(JSON.parse(r.responseText)); }
                            catch (e) { reject(new Error('JSON invalide')); }
                        } else {
                            reject(new Error('HTTP ' + r.status));
                        }
                    },
                    onerror: () => reject(new Error('solveur injoignable')),
                    ontimeout: () => reject(new Error('timeout solveur')),
                });
            } else {
                fetch(SERVER, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: body,
                })
                    .then((r) => {
                        if (!r.ok) throw new Error('HTTP ' + r.status);
                        return r.json();
                    })
                    .then(resolve)
                    .catch(() => reject(new Error('solveur injoignable (CORS/LNA)')));
            }
        });
    }

    function dispatchMove(key) {
        document.dispatchEvent(
            new KeyboardEvent('keydown', { key: key, bubbles: true, cancelable: true })
        );
    }

    function sleep(ms) {
        return new Promise((r) => setTimeout(r, ms));
    }

    async function waitBoardChange(before) {
        const t0 = Date.now();
        while (Date.now() - t0 < WAIT_TIMEOUT_MS) {
            await sleep(POLL_MS);
            const now = readBoard();
            if (now && now.join() !== before.join()) return now;
            if (isGameOver()) return now;
        }
        return readBoard();
    }

    function findButton(labels) {
        for (const t of labels) {
            const b = [...document.querySelectorAll('button')].find(
                (x) => x.textContent.trim() === t
            );
            if (b) return b;
        }
        return null;
    }

    function ensureHud() {
        if (hud) return;
        hud = document.createElement('div');
        hud.style.cssText =
            'position:fixed;right:12px;bottom:12px;z-index:99999;background:rgba(20,20,20,.92);' +
            'color:#eee;font:12px/1.5 Consolas,monospace;padding:10px 12px;border-radius:8px;' +
            'min-width:250px;box-shadow:0 4px 14px rgba(0,0,0,.4);white-space:pre;';
        document.body.appendChild(hud);
    }

    function updateHud(extra) {
        ensureHud();
        const score = readScore();
        const cells = readBoard();
        const mt = cells ? maxTile(cells) : 0;
        if (score > stats.bestScore) stats.bestScore = score;
        if (mt > stats.bestTile) stats.bestTile = mt;
        hud.textContent =
            '2048 SOLVER v2 (local)\n' +
            'etat   : ' + status + '\n' +
            'coups  : ' + runMoves + '\n' +
            'score  : ' + score + '   tuile: ' + mt + '\n' +
            'prof   : ' + sess.depth +
            '   nps: ' + (sess.nps >= 1000 ? Math.round(sess.nps / 1000) + 'k' : sess.nps) + '\n' +
            'noeuds : ' + sess.nodes + '  (' + sess.ms.toFixed(0) + ' ms)\n' +
            'parties: ' + stats.games +
            '  best: ' + stats.bestTile + '/' + stats.bestScore + '\n' +
            (extra ? extra + '\n' : '') +
            'S: demarrer/stopper';
    }

    async function restartFlow(cause) {
        stats.games++;
        status = 'restart (' + cause + ')';
        updateHud();
        await sleep(2000);
        const btn = findButton(['Nouvelle Partie', 'Recommencer', 'Rejouer', 'Commencer']);
        if (btn) btn.click();
        const t0 = Date.now();
        while (Date.now() - t0 < 10000) {
            await sleep(400);
            const c = readBoard();
            if (c && c.filter((x) => x > 0).length === 2 && maxTile(c) <= 4) break;
        }
        runMoves = 0;
        status = 'joue';
        updateHud();
    }

    function shouldAbandon(cells, moves) {
        const mt = maxTile(cells);
        for (const rule of ABANDON) {
            if (moves >= rule.at && mt < rule.minTile) return true;
        }
        return false;
    }

    async function loop() {
        let failures = 0;
        while (running) {
            if (isGameOver()) {
                await restartFlow('game over');
                continue;
            }
            let cells = readBoard();
            if (!cells) {
                status = 'attente de partie';
                updateHud('cliquez "Commencer"');
                if (AUTO_START) {
                    const btn = findButton(['Commencer']);
                    if (btn) btn.click();
                }
                await sleep(500);
                continue;
            }
            const budget = budgetFor(countEmpty(cells));
            status = 'analyse (' + budget + ' ms)';
            let result;
            try {
                result = await requestMove(cells, budget);
                failures = 0;
            } catch (e) {
                failures++;
                status = 'erreur: ' + e.message;
                updateHud();
                if (failures >= 8) {
                    running = false;
                    status = 'arrete (serveur KO)';
                    updateHud();
                    return;
                }
                await sleep(failures >= 3 ? 3000 : 800);
                continue;
            }
            sess = {
                depth: result.depth || 0,
                nodes: result.nodes || 0,
                nps: result.nps || 0,
                ms: result.time_ms || 0,
            };
            if (!result.key || !result.move) {
                await restartFlow('aucun coup');
                continue;
            }
            dispatchMove(result.key);
            const after = await waitBoardChange(cells);
            if (!after || after.join() === cells.join()) {
                if (isGameOver()) {
                    await restartFlow('game over');
                    continue;
                }
                failures++;
                status = 'coup non applique (retry)';
                updateHud();
                if (failures >= 5) {
                    running = false;
                    status = 'arrete (coup ignore)';
                    updateHud();
                    return;
                }
                await sleep(300);
                continue;
            }
            runMoves++;
            status = 'joue';
            updateHud();
            if (shouldAbandon(after, runMoves)) {
                await restartFlow('cadence faible');
                continue;
            }
            await sleep(POST_DELAY_MS + Math.random() * JITTER_MS);
        }
        updateHud();
    }

    function toggle() {
        running = !running;
        status = running ? 'demarrage...' : 'arrete';
        if (running) loop();
        else updateHud();
    }

    document.addEventListener('keydown', (e) => {
        if (isTyping(e)) return;
        if (e.key === 's' || e.key === 'S') {
            e.preventDefault();
            toggle();
        }
    });

    updateHud();
})();
