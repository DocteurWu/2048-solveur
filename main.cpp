#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
typedef int SOCKET;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Board = uint64_t;
using Clock = std::chrono::steady_clock;

static constexpr int DIR_LEFT = 0;
static constexpr int DIR_RIGHT = 1;
static constexpr int DIR_UP = 2;
static constexpr int DIR_DOWN = 3;

static constexpr double TERMINAL = -1.0e9;
static constexpr uint64_t NODE_LIMIT = 4000000000ULL;

#ifndef CFG_W_EMPTY_BASE
#define CFG_W_EMPTY_BASE 120.0
#endif
#ifndef CFG_W_EMPTY
#define CFG_W_EMPTY 45.0
#endif
#ifndef CFG_W_MONO
#define CFG_W_MONO 55.0
#endif
#ifndef CFG_W_SMOOTH
#define CFG_W_SMOOTH 22.0
#endif
#ifndef CFG_W_SNAKE
#define CFG_W_SNAKE 20.0
#endif
#ifndef CFG_W_CORNER
#define CFG_W_CORNER 380.0
#endif

static constexpr double W_EMPTY_BASE = CFG_W_EMPTY_BASE;
static constexpr double W_EMPTY = CFG_W_EMPTY;
static constexpr double W_MONO = CFG_W_MONO;
static constexpr double W_SMOOTH = CFG_W_SMOOTH;
static constexpr double W_SNAKE = CFG_W_SNAKE;
static constexpr double W_CORNER = CFG_W_CORNER;

static const int SNAKE_W[16] = {16, 15, 14, 13,
                                 9, 10, 11, 12,
                                 8,  7,  6,  5,
                                 1,  2,  3,  4};

static uint16_t row_left[65536];
static uint16_t row_right[65536];
static uint32_t score_left[65536];
static uint32_t score_right[65536];

struct RowRes {
    uint16_t packed;
    uint32_t score;
};

static RowRes build_row(uint16_t row, bool reverse) {
    uint8_t a[4];
    for (int i = 0; i < 4; i++) {
        int src = reverse ? (3 - i) : i;
        a[i] = (uint8_t)((row >> (4 * src)) & 0xF);
    }
    uint8_t out[4] = {0, 0, 0, 0};
    bool merged[4] = {false, false, false, false};
    int n = 0;
    int sc = 0;
    for (int i = 0; i < 4; i++) {
        if (!a[i]) continue;
        if (n > 0 && out[n - 1] == a[i] && !merged[n - 1] && a[i] < 15) {
            out[n - 1]++;
            merged[n - 1] = true;
            sc += 1 << out[n - 1];
        } else {
            out[n] = a[i];
            merged[n] = false;
            n++;
        }
    }
    uint16_t packed = 0;
    for (int i = 0; i < n; i++) {
        int dst = reverse ? (3 - i) : i;
        packed |= (uint16_t)((uint16_t)out[i] << (4 * dst));
    }
    return {packed, (uint32_t)sc};
}

static void init_lut() {
    for (int i = 0; i < 65536; i++) {
        RowRes l = build_row((uint16_t)i, false);
        row_left[i] = l.packed;
        score_left[i] = l.score;
        RowRes r = build_row((uint16_t)i, true);
        row_right[i] = r.packed;
        score_right[i] = r.score;
    }
}

// LUT d'evaluation par ligne : mono(8) | smooth(8) | empt(4) | mx(4)
static uint32_t g_line[65536];
// contribution snake de chaque rangee (poids specifiques par position)
static uint16_t g_snake_row[4][65536];

static void init_eval_lut() {
    for (int i = 0; i < 65536; i++) {
        int e[4] = {i & 0xF, (i >> 4) & 0xF, (i >> 8) & 0xF, (i >> 12) & 0xF};
        int pos = 0, neg = 0, sm = 0, empt = 0, mx = 0;
        for (int k = 0; k < 4; k++) {
            if (!e[k]) empt++;
            if (e[k] > mx) mx = e[k];
            if (k < 3 && e[k] && e[k + 1]) {
                int d = e[k] - e[k + 1];
                sm += d < 0 ? -d : d;
                if (d > 0) pos += d;
                else neg -= d;
            }
        }
        int mono = pos < neg ? pos : neg;
        g_line[i] = (uint32_t)mono | ((uint32_t)sm << 8) |
                    ((uint32_t)empt << 16) | ((uint32_t)mx << 20);
    }
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 65536; i++) {
            int v = 0;
            for (int k = 0; k < 4; k++)
                v += ((i >> (4 * k)) & 0xF) * SNAKE_W[4 * r + k];
            g_snake_row[r][i] = (uint16_t)v;
        }
    }
}

static inline Board transpose(Board x) {
    Board t = (x ^ (x >> 12)) & 0x0000F0000F0000F0ULL;
    x ^= t ^ (t << 12);
    t = (x ^ (x >> 24)) & 0x00000000F0000F00ULL;
    x ^= t ^ (t << 24);
    t = (x ^ (x >> 36)) & 0x000000000000F000ULL;
    x ^= t ^ (t << 36);
    return x;
}

struct MoveRes {
    Board b;
    int pts;
};

static inline MoveRes apply_move(Board b, int dir) {
    Board out = 0;
    int pts = 0;
    if (dir == DIR_LEFT || dir == DIR_RIGHT) {
        for (int r = 0; r < 4; r++) {
            uint16_t row = (uint16_t)((b >> (16 * r)) & 0xFFFF);
            if (dir == DIR_LEFT) {
                out |= (Board)row_left[row] << (16 * r);
                pts += score_left[row];
            } else {
                out |= (Board)row_right[row] << (16 * r);
                pts += score_right[row];
            }
        }
    } else {
        Board t = transpose(b);
        Board o = 0;
        for (int r = 0; r < 4; r++) {
            uint16_t row = (uint16_t)((t >> (16 * r)) & 0xFFFF);
            if (dir == DIR_UP) {
                o |= (Board)row_left[row] << (16 * r);
                pts += score_left[row];
            } else {
                o |= (Board)row_right[row] << (16 * r);
                pts += score_right[row];
            }
        }
        out = transpose(o);
    }
    return {out, pts};
}

static inline uint64_t empties_mask(Board b) {
    uint64_t x = b | (b >> 1) | (b >> 2) | (b >> 3);
    return ~x & 0x1111111111111111ULL;
}

static inline int empties_count(Board b) {
    return std::popcount(empties_mask(b));
}

static inline int max_exp(Board b) {
    int m = 0;
    for (int i = 0; i < 16; i++) {
        int v = (int)((b >> (4 * i)) & 0xF);
        if (v > m) m = v;
    }
    return m;
}

// reference lente (boucles d'origine) - utilisee par le selftest pour
// verifier l'equivalence exacte avec la version LUT
static double evaluate_ref(Board b) {
    int e[16];
    int empt = 0;
    int mx = 0;
    for (int i = 0; i < 16; i++) {
        int v = (int)((b >> (4 * i)) & 0xF);
        e[i] = v;
        if (!v) empt++;
        if (v > mx) mx = v;
    }
    int mono = 0;
    int smooth = 0;
    for (int r = 0; r < 4; r++) {
        int pos = 0, neg = 0;
        for (int c = 0; c < 3; c++) {
            int i = 4 * r + c, j = i + 1;
            if (e[i] && e[j]) {
                int d = e[i] - e[j];
                smooth += d < 0 ? -d : d;
                if (d > 0) pos += d;
                else neg -= d;
            }
        }
        mono += pos < neg ? pos : neg;
    }
    for (int c = 0; c < 4; c++) {
        int pos = 0, neg = 0;
        for (int r = 0; r < 3; r++) {
            int i = 4 * r + c, j = i + 4;
            if (e[i] && e[j]) {
                int d = e[i] - e[j];
                smooth += d < 0 ? -d : d;
                if (d > 0) pos += d;
                else neg -= d;
            }
        }
        mono += pos < neg ? pos : neg;
    }
    int snake = 0;
    for (int i = 0; i < 16; i++) snake += e[i] * SNAKE_W[i];
    int corner = (mx > 0 && e[0] == mx) ? mx : 0;
    return empt * (W_EMPTY_BASE + W_EMPTY * mx)
         - (double)mono * W_MONO
         - (double)smooth * W_SMOOTH
         + (double)snake * W_SNAKE
         + (double)corner * W_CORNER;
}

static double evaluate(Board b) {
    int empt = 0;
    int mx = 0;
    int mono = 0;
    int smooth = 0;
    int snake = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t li = g_line[(uint16_t)((b >> (16 * r)) & 0xFFFF)];
        empt += (int)((li >> 16) & 0xF);
        int lmx = (int)((li >> 20) & 0xF);
        if (lmx > mx) mx = lmx;
        mono += (int)(li & 0xFF);
        smooth += (int)((li >> 8) & 0xFF);
        snake += (int)g_snake_row[r][(uint16_t)((b >> (16 * r)) & 0xFFFF)];
    }
    Board t = transpose(b);
    for (int c = 0; c < 4; c++) {
        uint32_t li = g_line[(uint16_t)((t >> (16 * c)) & 0xFFFF)];
        mono += (int)(li & 0xFF);
        smooth += (int)((li >> 8) & 0xFF);
    }
    int corner = (mx > 0 && (int)(b & 0xF) == mx) ? mx : 0;
    return empt * (W_EMPTY_BASE + W_EMPTY * mx)
         - (double)mono * W_MONO
         - (double)smooth * W_SMOOTH
         + (double)snake * W_SNAKE
         + (double)corner * W_CORNER;
}

struct TTEntry {
    Board key;
    double val;
    int32_t depth;
    int32_t kind;
};

static inline uint64_t tt_mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static constexpr size_t TT_SIZE = (size_t)1 << 20;
static thread_local TTEntry *t_tt = nullptr;
static inline TTEntry *my_tt() {
    if (!t_tt) t_tt = (TTEntry *)calloc(TT_SIZE, sizeof(TTEntry));
    return t_tt;
}

static bool tt_lookup(Board k, int d, int kind, double &out) {
    if (d <= 0) return false;
    TTEntry *tt = my_tt();
    uint64_t h = tt_mix(k);
    size_t base = (size_t)(h & ((1u << 19) - 1)) * 2;
    for (int i = 0; i < 2; i++) {
        TTEntry &e = tt[base + i];
        if (e.depth == d && e.kind == kind && e.key == k) {
            out = e.val;
            return true;
        }
    }
    return false;
}

static void tt_store(Board k, int d, int kind, double v) {
    if (d <= 0) return;
    TTEntry *tt = my_tt();
    uint64_t h = tt_mix(k);
    size_t base = (size_t)(h & ((1u << 19) - 1)) * 2;
    TTEntry *slot = nullptr;
    for (int i = 0; i < 2; i++) {
        TTEntry &e = tt[base + i];
        if (e.key == k && e.kind == kind) {
            if (e.depth >= d) return;
            slot = &e;
            break;
        }
    }
    if (!slot) {
        slot = &tt[base];
        if (tt[base + 1].depth < slot->depth) slot = &tt[base + 1];
    }
    slot->key = k;
    slot->val = v;
    slot->depth = d;
    slot->kind = kind;
}

static thread_local uint64_t t_nodes = 0;
static std::atomic<uint64_t> g_nodes{0};
static std::atomic<bool> g_abort{false};
static Clock::time_point g_deadline;
static uint64_t g_node_limit;

static inline void tick() {
    uint64_t n = ++t_nodes;
    if ((n & 0x3FFF) == 0) {
        g_nodes.fetch_add(0x4000, std::memory_order_relaxed);
        if (g_nodes.load(std::memory_order_relaxed) >= g_node_limit)
            g_abort.store(true, std::memory_order_relaxed);
        else if (Clock::now() >= g_deadline)
            g_abort.store(true, std::memory_order_relaxed);
    }
}

static double max_node(Board b, int d);
static double chance_node(Board b, int d);

static double max_node(Board b, int d) {
    if (g_abort) return 0.0;
    tick();
    double tv;
    if (tt_lookup(b, d, 0, tv)) return tv;
    if (d == 0 && empties_count(b) > 0) return evaluate(b);
    Board boards[4];
    int pts[4];
    int nm = 0;
    for (int dir = 0; dir < 4; dir++) {
        MoveRes m = apply_move(b, dir);
        if (m.b != b) {
            boards[nm] = m.b;
            pts[nm] = m.pts;
            nm++;
        }
    }
    if (nm == 0) return TERMINAL;
    if (d == 0) {
        double ev = evaluate(b);
        return ev;
    }
    double best = -1.0e18;
    for (int i = 0; i < nm; i++) {
        double v = (double)pts[i] + chance_node(boards[i], d);
        if (g_abort) return 0.0;
        if (v > best) best = v;
    }
    tt_store(b, d, 0, best);
    return best;
}

static double chance_node(Board b, int d) {
    if (g_abort) return 0.0;
    tick();
    double tv;
    if (tt_lookup(b, d, 1, tv)) return tv;
    uint64_t em = empties_mask(b);
    int n = std::popcount(em);
    if (n == 0) return max_node(b, d - 1);
    static const double P[2] = {0.9, 0.1};
    static const unsigned V[2] = {1, 2};
    double sum = 0.0;
    double mass = 0.0;
    uint64_t m = em;
    while (m) {
        unsigned bit = (unsigned)std::countr_zero(m);
        m &= m - 1;
        unsigned cell = bit >> 2;
        Board base = b | (1ULL << (4 * cell));
        for (int k = 0; k < 2; k++) {
            double p = P[k] / n;
            sum += p * max_node(base | ((Board)V[k] << (4 * cell)), d - 1);
            if (g_abort) return 0.0;
            mass += p;
        }
    }
    double v = mass > 0.0 ? sum / mass : 0.0;
    tt_store(b, d, 1, v);
    return v;
}

// --- Pool de workers : split racine, une direction par tache ---------------
// Affinite STICKY : le worker wid traite les directions wid, wid+size, ...
// (claim deterministe, sans mutex dans la boucle). Chaque direction reste
// ainsi sur UNE seule table de transposition (TLS) d'un coup a l'autre et
// d'un niveau iteratif a l'autre -> taux de reussite TT maximal et
// determinisme des comptes de noeuds (un claim aleatoire fragmentait la TT
// et faisait varier les noeuds x2 a x10 selon la planification).
static int g_nthreads = 1;
static std::mutex g_pool_mu;
static std::condition_variable g_pool_cv_start;
static std::condition_variable g_pool_cv_end;
static bool g_pool_stop = false;
static bool g_pool_ready = false;
static int g_pool_size = 0;
static int g_round = 0;           // numero de manche (monotone)
static int g_round_nm = 0;        // nombre de directions a evaluer
static int g_round_depth = 0;
static Board g_round_boards[4];
static int g_finished = 0;
static double g_dir_val[4];
static std::vector<std::thread> g_workers;

static void worker_main(int wid) {
    int seen = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(g_pool_mu);
            g_pool_cv_start.wait(lk, [seen] { return g_pool_stop || g_round != seen; });
            if (g_pool_stop) return;
            seen = g_round;
        }
        for (int i = wid; i < g_round_nm; i += g_pool_size)
            g_dir_val[i] = chance_node(g_round_boards[i], g_round_depth);
        {
            std::lock_guard<std::mutex> lk(g_pool_mu);
            g_finished++;
            if (g_finished == g_pool_size) g_pool_cv_end.notify_one();
        }
    }
}

static void pool_ensure(int n) {
    std::lock_guard<std::mutex> lk(g_pool_mu);
    if (g_pool_ready) return;
    g_pool_size = n;
    for (int i = 0; i < n; i++) g_workers.emplace_back(worker_main, i);
    g_pool_ready = true;
}

static void pool_round(const Board *boards, int nm, int d) {
    {
        std::lock_guard<std::mutex> lk(g_pool_mu);
        for (int i = 0; i < nm; i++) g_round_boards[i] = boards[i];
        g_round_nm = nm;
        g_round_depth = d;
        g_finished = 0;
        g_round++;
    }
    g_pool_cv_start.notify_all();
    std::unique_lock<std::mutex> lk(g_pool_mu);
    g_pool_cv_end.wait(lk, [] { return g_finished == g_pool_size; });
}

static void pool_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_pool_mu);
        if (!g_pool_ready) return;
        g_pool_stop = true;
    }
    g_pool_cv_start.notify_all();
    for (std::thread &t : g_workers)
        if (t.joinable()) t.join();
    g_pool_ready = false;
}

struct PoolGuard {
    ~PoolGuard() { pool_shutdown(); }
};
static PoolGuard g_pool_guard;

struct RootRes {
    int dir;
    int depth;
    uint64_t nodes;
    double value;
    double ms;
    bool no_move;
};

static RootRes solve(Board b, int max_depth, double max_ms, uint64_t node_limit) {
    RootRes res{};
    res.dir = -1;
    res.depth = 0;
    res.nodes = 0;
    res.value = 0.0;
    res.ms = 0.0;
    res.no_move = false;
    g_nodes = 0;
    g_abort = false;
    g_node_limit = node_limit;
    g_deadline = Clock::now() + std::chrono::milliseconds((long long)max_ms);
    Clock::time_point t0 = Clock::now();

    int dirs[4];
    Board boards[4];
    int pts[4];
    int nm = 0;
    for (int dir = 0; dir < 4; dir++) {
        MoveRes m = apply_move(b, dir);
        if (m.b != b) {
            dirs[nm] = dir;
            boards[nm] = m.b;
            pts[nm] = m.pts;
            nm++;
        }
    }
    if (nm == 0) {
        res.no_move = true;
        res.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        res.nodes = g_nodes.load(std::memory_order_relaxed);
        return res;
    }
    res.dir = dirs[0];
    bool parallel = g_nthreads > 1 && nm > 1;
    if (parallel) pool_ensure(g_nthreads);
    for (int d = 1; d <= max_depth; d++) {
        if (parallel) {
            pool_round(boards, nm, d);
        } else {
            for (int i = 0; i < nm; i++) g_dir_val[i] = chance_node(boards[i], d);
        }
        if (g_abort) break;
        double best = -1.0e18;
        int best_dir = dirs[0];
        double best_val = 0.0;
        for (int i = 0; i < nm; i++) {
            double v = (double)pts[i] + g_dir_val[i];
            if (v > best) {
                best = v;
                best_dir = dirs[i];
                best_val = v;
            }
        }
        res.dir = best_dir;
        res.value = best_val;
        res.depth = d;
        if (g_abort) break;
    }
    res.nodes = g_nodes.load(std::memory_order_relaxed);
    res.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return res;
}

static int policy_depth(Board b, int cap) {
    // Table remontee de +2 : le vrai garde-fou est le BUDGET TEMPS (arret au
    // dernier niveau complet). L'ancien plafond gaspillait le budget restant
    // sur les plateaux bon marche (ex. e=9 : L5 fini en ~0,25 s, niveaux
    // 6-7 jamais tentes).
    int e = empties_count(b);
    int d = 7;
    if (e <= 8) d = 8;
    if (e <= 6) d = 9;
    if (e <= 4) d = 10;
    if (e <= 2) d = 11;
    if (e <= 1) d = 12;
    return d < cap ? d : cap;
}

static bool spawn(Board &b, std::mt19937_64 &rng) {
    uint64_t em = empties_mask(b);
    int n = std::popcount(em);
    if (!n) return false;
    int pick = (int)(rng() % (uint64_t)n);
    uint64_t m = em;
    while (pick--) m &= m - 1;
    unsigned bit = (unsigned)std::countr_zero(m);
    unsigned cell = bit >> 2;
    unsigned v = (rng() % 10 == 0) ? 2u : 1u;
    b |= (Board)v << (4 * cell);
    return true;
}

static void print_board(Board b) {
    printf("+------+------+------+------+\n");
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int v = (int)((b >> (4 * (4 * r + c))) & 0xF);
            if (v) printf("|%6d", 1 << v);
            else printf("|%6s", "-");
        }
        printf("|\n");
        printf("+------+------+------+------+\n");
    }
}

static const char *dir_name(int d) {
    switch (d) {
        case DIR_LEFT: return "gauche";
        case DIR_RIGHT: return "droite";
        case DIR_UP: return "haut";
        case DIR_DOWN: return "bas";
    }
    return "?";
}

static const char *dir_arrow(int d) {
    switch (d) {
        case DIR_LEFT: return "ArrowLeft";
        case DIR_RIGHT: return "ArrowRight";
        case DIR_UP: return "ArrowUp";
        case DIR_DOWN: return "ArrowDown";
    }
    return "";
}

static void play_game(uint64_t seed, double ms, int cap_depth, bool verbose) {
    std::mt19937_64 rng(seed);
    Board b = 0;
    spawn(b, rng);
    spawn(b, rng);
    long long score = 0;
    int moves = 0;
    uint64_t total_nodes = 0;
    double total_ms = 0.0;
    while (true) {
        int d = policy_depth(b, cap_depth);
        RootRes r = solve(b, d, ms, NODE_LIMIT);
        if (r.no_move) break;
        MoveRes m = apply_move(b, r.dir);
        b = m.b;
        score += m.pts;
        moves++;
        spawn(b, rng);
        total_nodes += r.nodes;
        total_ms += r.ms;
        if (verbose || moves % 50 == 0) {
            printf("--- coups=%d score=%lld tuile=%d prof=%d noeuds=%llu\n",
                   moves, score, 1 << max_exp(b), r.depth,
                   (unsigned long long)r.nodes);
            print_board(b);
        }
    }
    printf("\n=== Partie terminee ===\n");
    print_board(b);
    printf("Score      : %lld\n", score);
    printf("Tuile max  : %d\n", 1 << max_exp(b));
    printf("Coups      : %d\n", moves);
    printf("Noeuds     : %llu\n", (unsigned long long)total_nodes);
    printf("Temps      : %.1f ms (moy %.1f ms/coup)\n", total_ms,
           moves ? total_ms / moves : 0.0);
    if (total_ms > 0.0)
        printf("NPS moyen  : %.0f\n", (double)total_nodes / (total_ms / 1000.0));
}

static void naive_slide(int line[4], bool reverse, int &pts) {
    int a[4];
    for (int i = 0; i < 4; i++) a[i] = line[reverse ? 3 - i : i];
    int out[4] = {0, 0, 0, 0};
    bool mg[4] = {false, false, false, false};
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (!a[i]) continue;
        if (n > 0 && out[n - 1] == a[i] && !mg[n - 1] && a[i] < 15) {
            out[n - 1]++;
            mg[n - 1] = true;
            pts += 1 << out[n - 1];
        } else {
            out[n] = a[i];
            mg[n] = false;
            n++;
        }
    }
    for (int i = 0; i < 4; i++) line[reverse ? 3 - i : i] = (i < n) ? out[i] : 0;
}

static Board naive_move(Board b, int dir, int &pts) {
    int g[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) g[r][c] = (int)((b >> (4 * (4 * r + c))) & 0xF);
    pts = 0;
    if (dir == DIR_LEFT || dir == DIR_RIGHT) {
        bool rev = (dir == DIR_RIGHT);
        for (int r = 0; r < 4; r++) {
            int line[4] = {g[r][0], g[r][1], g[r][2], g[r][3]};
            naive_slide(line, rev, pts);
            for (int c = 0; c < 4; c++) g[r][c] = line[c];
        }
    } else {
        bool rev = (dir == DIR_DOWN);
        for (int c = 0; c < 4; c++) {
            int line[4] = {g[0][c], g[1][c], g[2][c], g[3][c]};
            naive_slide(line, rev, pts);
            for (int r = 0; r < 4; r++) g[r][c] = line[r];
        }
    }
    Board out = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out |= (Board)g[r][c] << (4 * (4 * r + c));
    return out;
}

static Board naive_transpose(Board b) {
    Board out = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            Board v = (b >> (4 * (4 * r + c))) & 0xF;
            out |= v << (4 * (4 * c + r));
        }
    return out;
}

static int run_selftest() {
    int fails = 0;
    {
        std::mt19937_64 rr(0xC0FFEEULL);
        for (int t = 0; t < 50000; t++) {
            Board b = rr();
            if (evaluate(b) != evaluate_ref(b)) {
                if (fails < 3)
                    printf("eval LUT FAIL board=%016llx lut=%.6f ref=%.6f\n",
                           (unsigned long long)b, evaluate(b), evaluate_ref(b));
                fails++;
                break;
            }
        }
    }
    for (int i = 0; i < 65536; i++) {
        int line[4] = {(i) & 0xF, (i >> 4) & 0xF, (i >> 8) & 0xF, (i >> 12) & 0xF};
        int pts = 0;
        int tmp[4] = {line[0], line[1], line[2], line[3]};
        naive_slide(tmp, false, pts);
        uint16_t packed = (uint16_t)(tmp[0] | (tmp[1] << 4) | (tmp[2] << 8) | (tmp[3] << 12));
        if (packed != row_left[i] || pts != (int)score_left[i]) {
            if (fails < 5)
                printf("LUT left FAIL row=%04X got=%04X/%d want=%04X/%d\n", i, packed,
                       pts, row_left[i], score_left[i]);
            fails++;
        }
        pts = 0;
        int rev[4] = {line[3], line[2], line[1], line[0]};
        naive_slide(rev, false, pts);
        packed = (uint16_t)(rev[0] | (rev[1] << 4) | (rev[2] << 8) | (rev[3] << 12));
        int back[4] = {packed & 0xF, (packed >> 4) & 0xF, (packed >> 8) & 0xF,
                       (packed >> 12) & 0xF};
        int backpacked = back[3] | (back[2] << 4) | (back[1] << 8) | (back[0] << 12);
        if ((uint16_t)backpacked != row_right[i] || pts != (int)score_right[i]) {
            if (fails < 5)
                printf("LUT right FAIL row=%04X got=%04X/%d want=%04X/%d\n", i,
                       backpacked, pts, row_right[i], score_right[i]);
            fails++;
        }
    }
    std::mt19937_64 rng(12345);
    for (int it = 0; it < 20000; it++) {
        Board b = 0;
        for (int i = 0; i < 16; i++) {
            unsigned v = (unsigned)(rng() % 7);
            if (v >= 4) v = 0;
            b |= (Board)v << (4 * i);
        }
        Board nt = naive_transpose(b);
        if (nt != transpose(b) || transpose(nt) != b) {
            if (fails < 10) printf("transpose FAIL b=%016llX\n", (unsigned long long)b);
            fails++;
        }
        for (int dir = 0; dir < 4; dir++) {
            int pts1 = apply_move(b, dir).pts;
            Board m1 = apply_move(b, dir).b;
            int pts2 = 0;
            Board m2 = naive_move(b, dir, pts2);
            if (m1 != m2 || pts1 != pts2) {
                if (fails < 10)
                    printf("move FAIL dir=%d b=%016llX got=%016llX/%d want=%016llX/%d\n",
                           dir, (unsigned long long)b, (unsigned long long)m1, pts1,
                           (unsigned long long)m2, pts2);
                fails++;
            }
        }
        int naive_empty = 0;
        int naive_max = 0;
        for (int i = 0; i < 16; i++) {
            int v = (int)((b >> (4 * i)) & 0xF);
            if (!v) naive_empty++;
            if (v > naive_max) naive_max = v;
        }
        if (naive_empty != empties_count(b) || naive_max != max_exp(b)) {
            if (fails < 10) printf("count FAIL b=%016llX\n", (unsigned long long)b);
            fails++;
        }
    }
    if (fails) {
        printf("SELFTEST: %d echec(s)\n", fails);
        return 1;
    }
    printf("SELFTEST: OK (65536 lignes + 20000 plateaux x 4 coups + transposition)\n");
    return 0;
}

static void run_bench(int cap_depth) {
    struct Case {
        const char *name;
        int cells[16];
    };
    static const Case cases[] = {
        {"debut", {2, 4, 2, 0, 0, 2, 0, 4, 0, 0, 2, 0, 0, 0, 0, 0}},
        {"milieu", {16, 8, 4, 2, 128, 64, 32, 0, 256, 16, 8, 4, 0, 2, 2, 0}},
        {"serre", {1024, 512, 256, 128, 8, 16, 32, 64, 4, 8, 16, 0, 2, 4, 2, 0}},
        {"fin", {2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 0, 2, 0, 0, 0}},
    };
    printf("=== Benchmark expectimax (profondeur fixe %d) ===\n", 4);
    uint64_t total_nodes = 0;
    double total_ms = 0.0;
    const int reps = 3;
    for (const Case &c : cases) {
        Board b = 0;
        for (int i = 0; i < 16; i++) {
            int v = c.cells[i];
            if (v <= 0) continue;
            int e = 0;
            while (v > 1) {
                v >>= 1;
                e++;
            }
            b |= (Board)e << (4 * i);
        }
        double case_ms = 0.0;
        uint64_t case_nodes = 0;
        RootRes last{};
        for (int r = 0; r < reps; r++) {
            last = solve(b, std::min(4, cap_depth), 1000000.0, 50000000ULL);
            case_nodes += last.nodes;
            case_ms += last.ms;
        }
        total_nodes += case_nodes;
        total_ms += case_ms;
        printf("  %-8s prof=%d coup=%-7s noeuds=%-10llu %8.1f ms  %10.0f nps\n",
               c.name, last.depth, dir_name(last.dir),
               (unsigned long long)case_nodes, case_ms,
               case_ms > 0.0 ? (double)case_nodes / (case_ms / 1000.0) : 0.0);
    }
    printf("  TOTAL    noeuds=%llu  %.1f ms  NPS=%.0f\n",
           (unsigned long long)total_nodes, total_ms,
           total_ms > 0.0 ? (double)total_nodes / (total_ms / 1000.0) : 0.0);
}

#ifdef _WIN32
struct WinsockApi {
    HMODULE mod;
    int(WSAAPI *WSAStartup)(WORD, LPWSADATA);
    SOCKET(WSAAPI *socket)(int, int, int);
    int(WSAAPI *bind)(SOCKET, const sockaddr *, int);
    int(WSAAPI *listen)(SOCKET, int);
    SOCKET(WSAAPI *accept)(SOCKET, sockaddr *, int *);
    int(WSAAPI *recv)(SOCKET, char *, int, int);
    int(WSAAPI *send)(SOCKET, const char *, int, int);
    int(WSAAPI *closesocket)(SOCKET);
    int(WSAAPI *setsockopt)(SOCKET, int, int, const char *, int);
};

static WinsockApi g_ws;

template <typename T>
static bool load_fn(T &fn, const char *name) {
    FARPROC raw = GetProcAddress(g_ws.mod, name);
    fn = reinterpret_cast<T>(reinterpret_cast<void *>(raw));
    if (!fn) {
        printf("GetProcAddress echec: %s\n", name);
        return false;
    }
    return true;
}

static bool winsock_init() {
    g_ws.mod = LoadLibraryA("ws2_32.dll");
    if (!g_ws.mod) {
        printf("ws2_32.dll indisponible\n");
        return false;
    }
    bool ok = true;
    ok &= load_fn(g_ws.WSAStartup, "WSAStartup");
    ok &= load_fn(g_ws.socket, "socket");
    ok &= load_fn(g_ws.bind, "bind");
    ok &= load_fn(g_ws.listen, "listen");
    ok &= load_fn(g_ws.accept, "accept");
    ok &= load_fn(g_ws.recv, "recv");
    ok &= load_fn(g_ws.send, "send");
    ok &= load_fn(g_ws.closesocket, "closesocket");
    ok &= load_fn(g_ws.setsockopt, "setsockopt");
    if (!ok) return false;
    WSADATA wsa;
    if (g_ws.WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup echec\n");
        return false;
    }
    return true;
}
#else
struct WinsockApi {
    SOCKET socket(int domain, int type, int protocol) {
        return ::socket(domain, type, protocol);
    }
    int bind(SOCKET s, const sockaddr *addr, int len) {
        return ::bind(s, addr, (socklen_t)len);
    }
    int listen(SOCKET s, int backlog) { return ::listen(s, backlog); }
    SOCKET accept(SOCKET s, sockaddr *addr, int *len) {
        socklen_t l = len ? (socklen_t)*len : 0;
        SOCKET r = ::accept(s, addr, len ? &l : nullptr);
        if (len) *len = (int)l;
        return r;
    }
    int recv(SOCKET s, char *buf, int len, int flags) {
        return (int)::recv(s, buf, (size_t)len, flags);
    }
    int send(SOCKET s, const char *buf, int len, int flags) {
        return (int)::send(s, buf, (size_t)len, flags);
    }
    int closesocket(SOCKET s) { return ::close(s); }
    int setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen) {
        return ::setsockopt(s, level, optname, optval, (socklen_t)optlen);
    }
};
static WinsockApi g_ws;
static bool winsock_init() {
    signal(SIGPIPE, SIG_IGN);
    return true;
}
#endif

static bool send_all(SOCKET s, const char *p, int len) {
    while (len > 0) {
        int n = g_ws.send(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static std::string recv_request(SOCKET s) {
    std::string buf;
    char tmp[4096];
    size_t header_end = std::string::npos;
    while (true) {
        header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        int n = g_ws.recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return std::string();
        buf.append(tmp, (size_t)n);
        if (buf.size() > 65536) return std::string();
    }
    int clen = 0;
    std::string lower;
    lower.reserve(buf.size());
    for (size_t i = 0; i < buf.size(); i++)
        lower.push_back((char)tolower((unsigned char)buf[i]));
    size_t p = lower.find("content-length:");
    if (p != std::string::npos) {
        p += 15;
        while (p < lower.size() && lower[p] == ' ') p++;
        clen = atoi(lower.c_str() + p);
    }
    if (clen < 0) clen = 0;
    if (clen > 65536) return std::string();
    size_t body_start = header_end + 4;
    while (buf.size() < body_start + (size_t)clen) {
        int n = g_ws.recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        if (buf.size() > 131072) return std::string();
    }
    if (buf.size() > body_start + (size_t)clen)
        buf.resize(body_start + (size_t)clen);
    return buf;
}

static void http_send(SOCKET s, const char *status, const std::string &body) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: content-type\r\n"
                     "Access-Control-Allow-Private-Network: true\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, body.size());
    if (n <= 0) return;
    if (!send_all(s, hdr, n)) return;
    if (!body.empty()) send_all(s, body.data(), (int)body.size());
}

static bool parse_cells(const std::string &s, int out[16]) {
    size_t p = s.find("\"cells\"");
    if (p == std::string::npos) return false;
    size_t lb = s.find('[', p);
    if (lb == std::string::npos) return false;
    size_t rb = s.find(']', lb);
    if (rb == std::string::npos) return false;
    size_t i = lb + 1;
    int idx = 0;
    while (i < rb && idx < 16) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == ',' || ch == '\n' || ch == '\r') {
            i++;
            continue;
        }
        char *end = nullptr;
        long long v = strtoll(s.c_str() + i, &end, 10);
        if (end == s.c_str() + i) return false;
        out[idx++] = (int)v;
        i = (size_t)(end - s.c_str());
    }
    return idx == 16;
}

static bool json_int(const std::string &s, const char *key, long long &out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    char *end = nullptr;
    long long v = strtoll(s.c_str() + p, &end, 10);
    if (end == s.c_str() + p) return false;
    out = v;
    return true;
}

static Board board_from_cells(const int cells[16]) {
    Board b = 0;
    for (int i = 0; i < 16; i++) {
        long long v = cells[i];
        if (v <= 0) continue;
        if (v > 32768) v = 32768;
        int e = 0;
        while (v > 1) {
            v >>= 1;
            e++;
        }
        b |= (Board)e << (4 * i);
    }
    return b;
}

static void handle_solve(SOCKET s, const std::string &body, double default_ms,
                         int cap_depth) {
    int cells[16];
    long long ms = -1;
    long long depth = -1;
    if (!parse_cells(body, cells)) {
        http_send(s, "400 Bad Request", "{\"error\":\"cells manquantes\"}");
        printf("  requete invalide\n");
        return;
    }
    json_int(body, "ms", ms);
    json_int(body, "depth", depth);
    Board b = board_from_cells(cells);
    double budget = ms > 0 ? (double)ms : default_ms;
    int cap = depth > 0 ? (int)depth : cap_depth;
    int d = policy_depth(b, cap);
    RootRes r = solve(b, d, budget, NODE_LIMIT);
    double nps = r.ms > 0.0 ? (double)r.nodes / (r.ms / 1000.0) : 0.0;
    std::string resp;
    char buf[512];
    if (r.no_move) {
        snprintf(buf, sizeof(buf),
                 "{\"move\":null,\"key\":null,\"depth\":0,\"nodes\":%llu,"
                 "\"nps\":0,\"time_ms\":%.1f,\"eval\":0}",
                 (unsigned long long)r.nodes, r.ms);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"move\":\"%s\",\"key\":\"%s\",\"depth\":%d,\"nodes\":%llu,"
                 "\"nps\":%.0f,\"time_ms\":%.1f,\"eval\":%.1f}",
                 dir_name(r.dir), dir_arrow(r.dir), r.depth,
                 (unsigned long long)r.nodes, nps, r.ms, r.value);
    }
    resp = buf;
    http_send(s, "200 OK", resp);
    printf("  move=%-7s prof=%d noeuds=%llu %.1fms %.0fnps\n",
           r.no_move ? "null" : dir_name(r.dir), r.depth,
           (unsigned long long)r.nodes, r.ms, nps);
}

static int run_server(int port, double default_ms, int cap_depth) {
    if (!winsock_init()) return 1;
    SOCKET srv = g_ws.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        printf("socket() echec\n");
        return 1;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // swap d'octets manuel (hote little-endian : x86 et ARM64) : evite de
    // lier htons/htonl (winsock charge dynamiquement sur Windows)
    addr.sin_port = (uint16_t)(((uint16_t)port << 8) | ((uint16_t)port >> 8));
    addr.sin_addr.s_addr = 0;  // INADDR_ANY : toutes interfaces
    if (g_ws.bind(srv, (const sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("bind() echec sur 127.0.0.1:%d (port occupe ?)\n", port);
        g_ws.closesocket(srv);
        return 1;
    }
    if (g_ws.listen(srv, 8) != 0) {
        printf("listen() echec\n");
        g_ws.closesocket(srv);
        return 1;
    }
    printf("Serveur pret: http://127.0.0.1:%d (toutes interfaces 0.0.0.0:%d)\n", port, port);
    printf("POST /solve, GET /health\n");
    printf("LUT, heuristique et expectimax initialises. En attente...\n");
    while (true) {
        SOCKET c = g_ws.accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
#ifdef _WIN32
        DWORD timeout = 5000;
        g_ws.setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                        sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        g_ws.setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif
        std::string req = recv_request(c);
        if (!req.empty()) {
            size_t line_end = req.find("\r\n");
            std::string line = req.substr(0, line_end == std::string::npos ? req.size() : line_end);
            size_t sp1 = line.find(' ');
            size_t sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            std::string method =
                (sp1 == std::string::npos) ? "" : line.substr(0, sp1);
            std::string path = (sp1 == std::string::npos || sp2 == std::string::npos)
                                   ? ""
                                   : line.substr(sp1 + 1, sp2 - sp1 - 1);
            size_t q = path.find('?');
            if (q != std::string::npos) path.resize(q);
            if (method == "OPTIONS") {
                http_send(c, "204 No Content", "");
            } else if (method == "GET" && path == "/health") {
                http_send(c, "200 OK", "{\"ok\":true}");
            } else if (method == "POST" && path == "/solve") {
                size_t he = req.find("\r\n\r\n");
                std::string body =
                    (he == std::string::npos) ? "" : req.substr(he + 4);
                handle_solve(c, body, default_ms, cap_depth);
            } else {
                http_send(c, "404 Not Found", "{\"error\":\"inconnu\"}");
            }
        }
        g_ws.closesocket(c);
    }
    return 0;
}

static void usage() {
    printf("Usage: solver2048 [options]\n");
    printf("  (sans argument)   selftest + partie automatique + benchmark NPS\n");
    printf("  --selftest        verifie LUT, transposition et coups\n");
    printf("  --play            joue une partie complete en console\n");
    printf("  --bench           benchmark NPS\n");
    printf("  --serve           serveur HTTP local pour le navigateur\n");
    printf("  --games N         nombre de parties (--play)\n");
    printf("  --seed N          graine aleatoire de depart\n");
    printf("  --ms N            budget de recherche par coup (defaut 150)\n");
    printf("  --depth N         profondeur max (defaut 13)\n");
    printf("  --threads N       threads de recherche (defaut auto, max utile 4)\n");
    printf("  --port N          port du serveur (defaut 8765)\n");
    printf("  --verbose         affiche le plateau a chaque coup\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool f_self = false, f_play = false, f_bench = false, f_serve = false;
    bool verbose = false;
    uint64_t seed = 2048;
    double ms = 150.0;
    int cap_depth = 13;
    int games = 1;
    int port = 8765;
    int threads = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--selftest") f_self = true;
        else if (a == "--play") f_play = true;
        else if (a == "--bench") f_bench = true;
        else if (a == "--serve") f_serve = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 10);
        else if (a == "--ms" && i + 1 < argc) ms = atof(argv[++i]);
        else if (a == "--games" && i + 1 < argc) games = atoi(argv[++i]);
        else if (a == "--depth" && i + 1 < argc) cap_depth = atoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            printf("Argument inconnu: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (!f_self && !f_play && !f_bench && !f_serve) {
        f_self = true;
        f_play = true;
        f_bench = true;
    }

    auto t0 = Clock::now();
    init_lut();
    init_eval_lut();
    {
        unsigned hc = std::thread::hardware_concurrency();
        int auto_t = hc ? (int)hc : 4;
        if (auto_t > 4) auto_t = 4;
        if (auto_t < 1) auto_t = 1;
        g_nthreads = (threads > 0) ? threads : auto_t;
        if (g_nthreads > 16) g_nthreads = 16;
    }
    double lut_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    printf("2048 solver - bitboard 64 bits + expectimax + table de transposition\n");
    printf("LUT initialisees en %.2f ms\n", lut_ms);
    printf("poids: empty_base=%.0f empty=%.0f mono=%.0f smooth=%.0f snake=%.0f corner=%.0f\n",
           W_EMPTY_BASE, W_EMPTY, W_MONO, W_SMOOTH, W_SNAKE, W_CORNER);
    printf("threads: %d\n\n", g_nthreads);

    int rc = 0;
    if (f_self) rc |= run_selftest();
    if (f_play) {
        if (games < 1) games = 1;
        for (int g = 0; g < games; g++) {
            uint64_t s = seed + (uint64_t)g * 0x9E3779B97F4A7C15ULL;
            printf("=== Partie %d/%d (seed=%llu, budget=%.0f ms/coup) ===\n", g + 1,
                   games, (unsigned long long)s, ms);
            play_game(s, ms, cap_depth, verbose);
            if (g + 1 < games) printf("\n");
        }
    }
    if (f_bench) run_bench(cap_depth);
    if (f_serve) rc |= run_server(port, ms, cap_depth);
    return rc;
}
