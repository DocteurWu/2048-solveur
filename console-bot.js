(async () => {
    'use strict';
    const SERVER = 'http://127.0.0.1:8766/solve';
    const POLL_MS = 40;
    const WAIT_MS = 5000;
    let running = true;
    let runMoves = 0;
    let games = 0;
    let hud;

    const ABANDON = [
        { at: 200, minTile: 256 },
        { at: 600, minTile: 1024 },
        { at: 1200, minTile: 2048 },
    ];
    const budgetFor = (e) =>
        e >= 13 ? 5000 : e >= 9 ? 2000 : e >= 6 ? 4000 : e >= 4 ? 6000 : e >= 2 ? 8000 : 10000;

    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const readBoard = () => {
        const cells = document.querySelectorAll('.jeux-2048-cell');
        if (cells.length !== 16) return null;
        return [...cells].map((c) => {
            const t = [...c.classList].find((x) => x.startsWith('tile-'));
            return t ? parseInt(t.slice(5), 10) || 0 : 0;
        });
    };
    const readScore = () => {
        const b = document.querySelectorAll('.score-box .score-value');
        return b.length > 1 ? parseInt(b[1].textContent, 10) || 0 : 0;
    };
    const gameOver = () => document.body.innerText.includes('Game Over');
    const maxTile = (c) => c.reduce((a, b) => (b > a ? b : a), 0);
    const findButton = (labels) => {
        for (const t of labels) {
            const b = [...document.querySelectorAll('button')].find(
                (x) => x.textContent.trim() === t
            );
            if (b) return b;
        }
        return null;
    };
    const show = (msg) => {
        if (!hud) {
            hud = document.createElement('div');
            hud.style.cssText =
                'position:fixed;right:12px;bottom:12px;z-index:99999;background:rgba(20,20,20,.93);color:#eee;font:12px/1.5 Consolas,monospace;padding:10px 12px;border-radius:8px;min-width:240px;box-shadow:0 4px 14px rgba(0,0,0,.4);white-space:pre;';
            document.body.appendChild(hud);
        }
        hud.textContent =
            '2048 SOLVER (console)\n' +
            'etat  : ' + msg + '\n' +
            'coups : ' + runMoves + '  parties: ' + games + '\n' +
            'score : ' + readScore();
    };
    const ask = async (cells, budget) => {
        const r = await fetch(SERVER, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ cells, ms: budget }),
        });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
    };
    const restart = async (cause) => {
        games++;
        show('restart (' + cause + ')');
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
    };

    show('demarrage...');
    if (window.__solver2048Bound) {
        show('bot deja actif (S pour arreter)');
        return;
    }
    window.__solver2048Bound = true;
    addEventListener('keydown', (e) => {
        if (e.key === 's' || e.key === 'S') {
            if (e.target && /INPUT|TEXTAREA/.test(e.target.tagName)) return;
            running = !running;
            show(running ? 'joue' : 'arrete');
            if (running) loop();
        }
    });

    async function loop() {
        let fails = 0;
        while (running) {
            if (gameOver()) { await restart('game over'); continue; }
            let cells = readBoard();
            if (!cells) {
                const btn = findButton(['Commencer']);
                if (btn) btn.click();
                show('attente de partie');
                await sleep(500);
                continue;
            }
            const budget = budgetFor(cells.filter((v) => v === 0).length);
            let res;
            try {
                res = await ask(cells, budget);
                fails = 0;
            } catch (e) {
                fails++;
                show('erreur: ' + e.message);
                await sleep(fails >= 3 ? 3000 : 800);
                continue;
            }
            if (!res.key) { await restart('aucun coup'); continue; }
            document.dispatchEvent(
                new KeyboardEvent('keydown', { key: res.key, bubbles: true, cancelable: true })
            );
            const t0 = Date.now();
            let after = null;
            while (Date.now() - t0 < WAIT_MS) {
                await sleep(POLL_MS);
                after = readBoard();
                if (after && after.join() !== cells.join()) break;
                if (gameOver()) break;
            }
            if (!after || after.join() === cells.join()) {
                if (gameOver()) { await restart('game over'); continue; }
                if (++fails >= 5) { running = false; return; }
                show('coup non applique (retry)');
                await sleep(300);
                continue;
            }
            runMoves++;
            show('joue (p=' + res.depth + ', ' + Math.round(res.nps / 1000) + 'k nps)');
            const mt = maxTile(after);
            if (ABANDON.some((r) => runMoves >= r.at && mt < r.minTile)) {
                await restart('cadence faible');
                continue;
            }
            await sleep(30 + Math.random() * 300);
        }
        show('arrete');
    }
    loop();
    return 'bot lance - S pour stopper';
})();
