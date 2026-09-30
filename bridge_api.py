#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Client « API » du solveur 2048 : joue la vraie partie du portail sans navigateur.

Le userscript lit le plateau dans le DOM et envoie les touches ; ce client-ci
fait la même chose mais en parlant directement à l'API du portail :

    portail (source de vérité)  <--->  moteur C++ local (port 8766)

Boucle par coup :
    1. GET  /api/jeux/partie/2048            -> plateau + score courants
    2. POST http://127.0.0.1:8766/solve      -> {move, depth, nodes, nps, eval}
    3. PUT  /api/jeux/partie/2048 {coup, score}
    4. vérification : le plateau renvoyé doit être exactement la prévision
       locale augmentée d'une seule tuile 2 ou 4, et le score doit coller au
       gain calculé (sinon avertissement).

Reprend les mêmes règles que le userscript : budget de recherche selon les
cases vides, jitter entre les coups, early-abandon sur partie sans avenir,
auto-restart après Game Over.

    python3 bridge_api.py                    # 1 partie, reprend la partie en cours
    python3 bridge_api.py --games 5          # enchaîne 5 parties
    python3 bridge_api.py --new              # force une nouvelle partie
    python3 bridge_api.py --dry 10           # 10 coups, diagnostics détaillés
"""

import argparse
import http.cookiejar
import json
import os
import random
import sys
import time
import urllib.error
import urllib.request

PORTAL = "https://eleves.rezal-mdm.com/api"
CREDS = os.path.expanduser("~/.hermes/credentials/rezal.env")
JEU = "2048"

ABANDON = [(200, 256), (600, 1024), (1200, 2048)]  # (coups, tuile mini)
JITTER_MS = 300


def budget_for(empt, scale=1.0):
    """Dotation de recherche selon le nombre de cases vides (comme le userscript).

    scale < 1 reduit les budgets : utile sur la carte, ~3x plus lente que le PC
    de reference pour lequel les valeurs du userscript ont ete calibrees.
    """
    if empt >= 13:
        ms = 5000
    elif empt >= 9:
        ms = 2000
    elif empt >= 6:
        ms = 4000
    elif empt >= 4:
        ms = 6000
    elif empt >= 2:
        ms = 8000
    else:
        ms = 10000
    return int(ms * scale)


def log(msg):
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


# --------------------------------------------------------------- moteur local
def slide_left(row):
    out = [v for v in row if v]
    res, gained, i = [], 0, 0
    while i < len(out):
        if i + 1 < len(out) and out[i] == out[i + 1]:
            v = out[i] * 2
            res.append(v)
            gained += v
            i += 2
        else:
            res.append(out[i])
            i += 1
    res += [0] * (4 - len(res))
    return res, gained


def apply_move(cells, move):
    """cells : liste de 16 valeurs row-major. Renvoie (nouvelles cells, gain)."""
    grid = [list(cells[4 * r:4 * r + 4]) for r in range(4)]
    total = 0
    if move in ("gauche", "droite"):
        for r in range(4):
            row, g = slide_left(grid[r] if move == "gauche" else grid[r][::-1])
            if move == "droite":
                row = row[::-1]
            grid[r] = row
            total += g
    else:
        for c in range(4):
            col = [grid[r][c] for r in range(4)]
            new, g = slide_left(col if move == "haut" else col[::-1])
            if move == "bas":
                new = new[::-1]
            for r in range(4):
                grid[r][c] = new[r]
            total += g
    return [v for row in grid for v in row], total


def verifie(avant, move, apres):
    """Le plateau serveur doit valoir la prévision locale + une seule tuile 2/4."""
    pred, _ = apply_move(avant, move)
    diff = [i for i in range(16) if pred[i] != apres[i]]
    if len(diff) != 1:
        return False, "attendu 1 tuile ajoutee, %d case(s) differente(s)" % len(diff)
    i = diff[0]
    if pred[i] != 0 or apres[i] not in (2, 4):
        return False, "case %d : %s -> %s (pas une tuile 2/4 sur une case vide)" % (i, pred[i], apres[i])
    return True, ""


# ------------------------------------------------------------------- clients
class Portail:
    def __init__(self, timeout=30, retries=12):
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar))
        self.timeout = timeout
        self.retries = retries

    def _call(self, method, path, payload=None, base=PORTAL):
        """Appel API, avec nouvelles tentatives tant que le reseau est coupe.

        Une coupure wifi d'une minute ne doit pas tuer la partie : on retente
        avec un backoff progressif (2, 4, 8... 30 s max).
        """
        data = json.dumps(payload).encode() if payload is not None else None
        derniere = None
        for essai in range(self.retries):
            req = urllib.request.Request(base + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
            try:
                with self.opener.open(req, timeout=self.timeout) as r:
                    body = r.read().decode()
                return json.loads(body) if body.strip() else {}
            except urllib.error.HTTPError as e:
                detail = e.read().decode()[:200]
                if 500 <= e.code < 600 and essai < self.retries - 1:
                    derniere = "HTTP %s %s" % (e.code, detail)
                else:
                    raise RuntimeError("%s %s -> HTTP %s %s" % (method, path, e.code, detail))
            except (urllib.error.URLError, OSError) as e:
                derniere = str(getattr(e, "reason", e))
            attente = min(30, 2 ** (essai + 1))
            log("portail injoignable (%s), nouvel essai dans %d s [%d/%d]"
                % (derniere, attente, essai + 1, self.retries))
            time.sleep(attente)
        raise RuntimeError("%s %s -> portail injoignable (%s)" % (method, path, derniere))

    def login(self, user, password):
        res = self._call("POST", "/login/connexion", {"username": user, "password": password})
        if not res.get("connecte"):
            raise RuntimeError("connexion refusee : %s" % res)
        return res

    def partie(self):
        return self._call("GET", "/jeux/partie/%s" % JEU)

    def nouvelle_partie(self):
        return self._call("POST", "/jeux/partie", {"jeu": JEU})

    def jouer(self, coup, score):
        return self._call("PUT", "/jeux/partie/%s" % JEU, {"coup": coup, "score": score})

    def leaderboard(self):
        return self._call("GET", "/jeux/leaderboard/%s" % JEU)


def load_credentials():
    user, pwd = os.environ.get("REZAL_USER"), os.environ.get("REZAL_PASS")
    if (user and pwd) or not os.path.exists(CREDS):
        return user, pwd
    vals = {}
    with open(CREDS) as f:
        for line in f:
            if "=" in line and not line.strip().startswith("#"):
                k, v = line.split("=", 1)
                vals[k.strip()] = v.strip().strip('"').strip("'")
    return user or vals.get("REZAL_USER"), pwd or vals.get("REZAL_PASS")


class Moteur:
    """Moteurs de recherche : le premier joignable gagne.

    Les URLs sont ordonnees par preference, ex. --solver
    http://PC:8766/solve,http://127.0.0.1:8766/solve : le PC (rapide) d'abord,
    la carte en secours si le PC dort ou quitte le reseau.
    """

    def __init__(self, urls):
        if isinstance(urls, str):
            urls = [u.strip() for u in urls.split(",") if u.strip()]
        self.urls = urls
        self.quarantaine = {}

    def _post(self, url, cells, ms, depth):
        payload = {"cells": cells, "ms": ms}
        if depth:
            payload["depth"] = depth
        req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                     method="POST",
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=ms / 1000.0 + 60) as r:
            return json.loads(r.read().decode())

    def solve(self, cells, ms, depth=None):
        derniere = None
        for url in self.urls:
            if self.quarantaine.get(url) and time.time() - self.quarantaine[url] < 60:
                continue
            try:
                rep = self._post(url, cells, ms, depth)
                if url != self.urls[0]:
                    log("recherche sur le moteur de secours %s" % url)
                self.quarantaine.pop(url, None)
                return rep
            except Exception as e:
                derniere = e
                if url == self.urls[0] and len(self.urls) > 1:
                    log("moteur principal injoignable (%s), bascule sur le secours" % e)
                self.quarantaine[url] = time.time()
        raise RuntimeError("aucun moteur joignable (%s)" % derniere)


# --------------------------------------------------------------------- partie
class Partie:
    def __init__(self, portail, moteur, args):
        self.p, self.m, self.args = portail, moteur, args
        self.erreurs = 0
        self.avertissements = 0
        self.coups = 0

    def jouer(self, etat, limite=None):
        self.coups = 0
        score = int(etat.get("score") or 0)
        abandon = None
        while True:
            if etat.get("terminee"):
                break
            cells = [v for row in etat["etat"]["plateau"] for v in row]
            vides = cells.count(0)

            ms = self.args.ms or budget_for(vides, self.args.budget_scale)
            try:
                rep = self.m.solve(cells, ms, self.args.depth)
            except Exception as e:
                self.erreurs += 1
                log("moteur injoignable (%s), erreur %d/8" % (e, self.erreurs))
                if self.erreurs >= 8:
                    raise
                time.sleep(1)
                continue
            self.erreurs = 0
            move = rep.get("move")

            if not move:
                log("le moteur ne voit aucun coup : fin de partie")
                break

            pred, gain = apply_move(cells, move)
            rep_portail = self.p.jouer(move, score)
            if not rep_portail:
                log("reponse vide du portail, arret")
                break
            apres = [v for row in rep_portail["etat"]["plateau"] for v in row]
            score_srv = int(rep_portail["score"])

            if apres == cells:
                log("coup %s refuse par le serveur (partie desynchronisee ?), arret" % move)
                break
            ok, why = verifie(cells, move, apres)
            if not ok:
                self.avertissements += 1
                log("ATTENTION synchronisation : %s" % why)
            if score_srv != score + gain:
                self.avertissements += 1
                log("ATTENTION score : serveur %d, attendu %d" % (score_srv, score + gain))

            self.coups += 1
            score, etat = score_srv, rep_portail
            tuile = max(apres)

            if self.args.verbose or self.coups % 25 == 0:
                log("coup %4d  score %7d  tuile %5d  prof %2s  %s nps  %4.0f ms"
                    % (self.coups, score, tuile, rep.get("depth"),
                       ("%.2fM" % (rep.get("nps", 0) / 1e6)), rep.get("time_ms", 0)))

            for coups_min, tuile_min in ABANDON:
                if self.coups == coups_min and tuile < tuile_min:
                    abandon = "cadence faible (%d <= %d au coup %d)" % (tuile, tuile_min, coups_min)
            if abandon:
                log("abandon : %s" % abandon)
                break
            if limite and self.coups >= limite:
                break

            time.sleep(random.random() * JITTER_MS / 1000.0)

        tuile = max(max(row) for row in etat["etat"]["plateau"]) if etat.get("etat") else 0
        return score, tuile, abandon

    def run(self):
        args = self.args
        for i in range(args.games):
            etat = None if (args.new or i) else self.p.partie()
            if not etat or etat.get("terminee"):
                log("nouvelle partie")
                etat = self.p.nouvelle_partie()
            else:
                log("reprise de la partie en cours (score %s)" % etat.get("score"))
            score, tuile, abandon = self.jouer(etat, args.dry)
            log("PARTIE %d : score %d, tuile max %d, %d coups, %d avertissement(s)%s"
                % (i + 1, score, tuile, self.coups, self.avertissements,
                   " [abandon : %s]" % abandon if abandon else ""))
        lb = self.p.leaderboard()
        if lb:
            log("classement : " + " | ".join("%s %s" % (e["nom"], e["score"]) for e in lb[:5]))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Joue le 2048 du portail avec le moteur local")
    ap.add_argument("--solver", default="http://127.0.0.1:8766/solve",
                    help="URL(s) de moteur, separees par des virgules : le premier joignable gagne")
    ap.add_argument("--games", type=int, default=1)
    ap.add_argument("--new", action="store_true", help="forcer une nouvelle partie")
    ap.add_argument("--dry", type=int, metavar="N", help="s'arreter apres N coups")
    ap.add_argument("--ms", type=int, help="budget fixe par coup (sinon budget selon les cases vides)")
    ap.add_argument("--budget-scale", type=float, default=1.0,
                    help="facteur applique aux budgets du userscript (0.4 conseille sur la carte)")
    ap.add_argument("--depth", type=int, help="plafond de profondeur")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--user")
    ap.add_argument("--password")
    args = ap.parse_args(argv)

    if args.seed is not None:
        random.seed(args.seed)

    user, password = args.user, args.password
    if not (user and password):
        u, p = load_credentials()
        user, password = user or u, password or p
    if not (user and password):
        log("identifiants absents (--user/--password, REZAL_USER/REZAL_PASS, %s)" % CREDS)
        return 2

    portail = Portail()
    portail.login(user, password)
    log("connecte au portail (%s)" % user)

    moteur = Moteur(args.solver)
    try:
        moteur.solve([0] * 16, 200)
    except Exception as e:
        log("serveur solveur injoignable sur %s : %s" % (args.solver, e))
        return 1
    log("moteur OK (%s)" % args.solver)

    try:
        Partie(portail, moteur, args).run()
    except KeyboardInterrupt:
        log("interrompu")
    return 0


if __name__ == "__main__":
    sys.exit(main())
