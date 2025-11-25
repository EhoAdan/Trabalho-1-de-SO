#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <cstring>
#include <string>
#include <cmath>

using namespace std::chrono_literals;

// ---------- Config / Tipos ----------
enum Aim { AIM_UP, AIM_UPLEFT, AIM_UPRIGHT, AIM_LEFT, AIM_RIGHT };

struct Enemy {
    int id;
    int x, y;
    bool alive;
    pthread_t tid;
};

struct Rocket {
    int id;
    int x, y;
    Aim aim;
    pthread_t tid;
    bool active;
};

struct DifficultySettings {
    int k_launchers;           // número de lançadores (k)
    int m_enemies;             // quantidade total m
    int enemy_step_ms;         // velocidade (ms por passo)
    int reload_time_ms;        // tempo de recarga por lançador (ms)
    int spawn_interval_ms;     // intervalo de spawn (ms)
};

static DifficultySettings EASY   = {3, 12, 700, 1200, 900};
static DifficultySettings MEDIUM = {5, 18, 450, 800, 600};
static DifficultySettings HARD   = {8, 25, 250, 350, 300};

// ---------- Globals de jogo ----------
int SCREEN_H = 24, SCREEN_W = 80;

std::vector<Enemy> enemies;
std::vector<Rocket> rockets;

pthread_mutex_t enemyListMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rocketListMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t batteryMutex    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t screenMutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  batteryNotFull  = PTHREAD_COND_INITIALIZER;

// battery state
std::vector<bool> launchers; // true = contém foguete
int k_launchers_global = 0;

// counters
std::atomic<int> destroyedEnemies{0};
std::atomic<int> groundHits{0};
std::atomic<int> spawnedEnemies{0};

// threads control
std::atomic<bool> gameRunning{true};
std::atomic<bool> spawnDone{false};

// settings in use
DifficultySettings settings;

// ncurses window
WINDOW* gamewin = nullptr;

// aim state (protected by batteryMutex or separate mutex)
Aim currentAim = AIM_UP;

// random
std::mt19937 rng;

// ids
std::atomic<int> nextEnemyId{1};
std::atomic<int> nextRocketId{1};

// ---------- Helper functions ----------
void drawScreen() {
    pthread_mutex_lock(&screenMutex);
    werase(gamewin);

    // header
    mvwprintw(gamewin, 0, 1, "Antiaereo - Fogo em massa!  Press Q para sair");
    mvwprintw(gamewin, 1, 1, "Destroyed: %d    Ground hits: %d    Spawned: %d/%d",
              destroyedEnemies.load(), groundHits.load(), spawnedEnemies.load(), settings.m_enemies);

    // battery display top-right
    int bx = SCREEN_W - 28;
    mvwprintw(gamewin, 2, bx, "Battery (k=%d):", k_launchers_global);
    pthread_mutex_lock(&batteryMutex);
    for (int i = 0; i < k_launchers_global; ++i) {
        mvwprintw(gamewin, 3 + i/8, bx + (i%8)*3, "%c", launchers[i] ? 'O' : '.');
    }
    pthread_mutex_unlock(&batteryMutex);

    // show aim
    const char* aimText = "";
    switch (currentAim) {
        case AIM_UP: aimText = "90° (|)"; break;
        case AIM_UPLEFT: aimText = "45° (\\)"; break;
        case AIM_UPRIGHT: aimText = "45° (/)"; break;
        case AIM_LEFT: aimText = "180° left (--)"; break;
        case AIM_RIGHT: aimText = "180° right (--)"; break;
    }
    mvwprintw(gamewin, 6, bx, "Aim: %s", aimText);

    // draw border and ground
    for (int x = 0; x < SCREEN_W-1; ++x) {
        mvwprintw(gamewin, SCREEN_H-2, x, "="); // ground line
    }

    // enemies
    pthread_mutex_lock(&enemyListMutex);
    for (const auto& e : enemies) {
        if (!e.alive) continue;
        if (e.y >= 0 && e.y < SCREEN_H-2 && e.x >= 0 && e.x < SCREEN_W-1) {
            mvwprintw(gamewin, e.y, e.x, "V"); // enemy glyph
        }
    }
    pthread_mutex_unlock(&enemyListMutex);

    // rockets
    pthread_mutex_lock(&rocketListMutex);
    for (const auto& r : rockets) {
        if (!r.active) continue;
        if (r.y >= 0 && r.y < SCREEN_H-2 && r.x >= 0 && r.x < SCREEN_W-1) {
            mvwprintw(gamewin, r.y, r.x, "*");
        }
    }
    pthread_mutex_unlock(&rocketListMutex);

    // footer
    mvwprintw(gamewin, SCREEN_H-1, 1, "Objective: shoot at least 50%% of enemies to win.");

    box(gamewin, 0, 0);
    wrefresh(gamewin);
    pthread_mutex_unlock(&screenMutex);
}

// convert aim to step dx, dy per rocket tick
void aimToStep(Aim a, int &dx, int &dy) {
    // dy negative = up (since enemies come from top to bottom)
    switch (a) {
        case AIM_UP:     dx = 0; dy = -1; break;
        case AIM_UPLEFT: dx = -1; dy = -1; break;
        case AIM_UPRIGHT:dx = 1; dy = -1; break;
        case AIM_LEFT:   dx = -1; dy = 0; break;
        case AIM_RIGHT:  dx = 1; dy = 0; break;
    }
}

// safe remove rocket by id
void removeRocketById(int id) {
    pthread_mutex_lock(&rocketListMutex);
    rockets.erase(std::remove_if(rockets.begin(), rockets.end(), [&](const Rocket& r){ return r.id == id; }), rockets.end());
    pthread_mutex_unlock(&rocketListMutex);
}

// ---------- Thread functions ----------

// rocketThread: move rocket until offscreen or hit
void* rocketThreadFn(void* arg) {
    Rocket r = *(Rocket*)arg;
    delete (Rocket*)arg;

    int dx, dy;
    aimToStep(r.aim, dx, dy);

    while (gameRunning && r.active) {
        // move
        r.x += dx;
        r.y += dy;

        // update global rocket position
        pthread_mutex_lock(&rocketListMutex);
        for (auto &gr : rockets) {
            if (gr.id == r.id) {
                gr.x = r.x; gr.y = r.y;
                break;
            }
        }
        pthread_mutex_unlock(&rocketListMutex);

        // collision check with enemies
        bool hit = false;
        pthread_mutex_lock(&enemyListMutex);
        for (auto &e : enemies) {
            if (e.alive && e.x == r.x && e.y == r.y) {
                e.alive = false;
                hit = true;
                destroyedEnemies++;
                break;
            }
        }
        pthread_mutex_unlock(&enemyListMutex);

        if (hit) {
            // rocket ends
            break;
        }

        // offscreen?
        if (r.x < 1 || r.x >= SCREEN_W-1 || r.y < 1 || r.y >= SCREEN_H-2) {
            break;
        }

        std::this_thread::sleep_for(70ms);
    }

    // mark rocket inactive and remove from list
    pthread_mutex_lock(&rocketListMutex);
    for (auto &gr : rockets) {
        if (gr.id == r.id) {
            gr.active = false;
            break;
        }
    }
    pthread_mutex_unlock(&rocketListMutex);

    // notify reload thread there's space (a launcher is logically empty already at firing time)
    pthread_cond_signal(&batteryNotFull);
    return nullptr;
}

// enemyThread: each enemy descends until ground or destroyed
void* enemyThreadFn(void* arg) {
    Enemy e = *(Enemy*)arg;
    delete (Enemy*)arg;

    while (gameRunning && e.alive) {
        std::this_thread::sleep_for(std::chrono::milliseconds(settings.enemy_step_ms));

        // move down
        e.y += 1;

        pthread_mutex_lock(&enemyListMutex);
        for (auto &ge : enemies) {
            if (ge.id == e.id) {
                ge.y = e.y;
                break;
            }
        }
        pthread_mutex_unlock(&enemyListMutex);

        // reached ground?
        if (e.y >= SCREEN_H-2) {
            pthread_mutex_lock(&enemyListMutex);
            for (auto &ge : enemies) {
                if (ge.id == e.id && ge.alive) {
                    ge.alive = false;
                    groundHits++;
                    break;
                }
            }
            pthread_mutex_unlock(&enemyListMutex);
            break;
        }
    }

    return nullptr;
}

// enemySpawnerThread: spawns m enemies at random x positions
void* enemySpawnerFn(void* arg) {
    std::uniform_int_distribution<int> distX(2, SCREEN_W - 4);
    int m = settings.m_enemies;

    for (int i = 0; i < m && gameRunning; ++i) {
        Enemy e;
        e.id = nextEnemyId++;
        e.x = distX(rng);
        e.y = 1;
        e.alive = true;

        pthread_mutex_lock(&enemyListMutex);
        enemies.push_back(e);
        pthread_mutex_unlock(&enemyListMutex);

        // spawn thread for enemy
        Enemy* earg = new Enemy;
        *earg = e;
        pthread_t tid;
        pthread_create(&tid, nullptr, enemyThreadFn, earg);

        // store tid inside list
        pthread_mutex_lock(&enemyListMutex);
        for (auto &ge : enemies) if (ge.id == e.id) ge.tid = tid;
        pthread_mutex_unlock(&enemyListMutex);

        spawnedEnemies++;
        std::this_thread::sleep_for(std::chrono::milliseconds(settings.spawn_interval_ms));
    }

    spawnDone = true;
    return nullptr;
}

// reloadThread: refills launchers from infinite loader
void* reloadThreadFn(void* arg) {
    while (gameRunning) {
        pthread_mutex_lock(&batteryMutex);
        // check if all launchers full -> sleep on cond
        bool allFull = true;
        for (bool b : launchers) if (!b) { allFull = false; break; }
        if (allFull) {
            // wait until a launcher becomes empty (signal fired when rocket removed/fired)
            pthread_cond_wait(&batteryNotFull, &batteryMutex);
            pthread_mutex_unlock(&batteryMutex);
            continue;
        }
        // find first empty and reload one by one with delay
        for (int i = 0; i < k_launchers_global && gameRunning; ++i) {
            if (!launchers[i]) {
                // simulate travel/time to load single launcher
                pthread_mutex_unlock(&batteryMutex);
                std::this_thread::sleep_for(std::chrono::milliseconds(settings.reload_time_ms));
                pthread_mutex_lock(&batteryMutex);
                if (!launchers[i]) {
                    launchers[i] = true;
                    // after filling one, update draw etc.
                    pthread_cond_signal(&batteryNotFull); // maybe others waiting
                }
            }
        }
        pthread_mutex_unlock(&batteryMutex);
    }
    return nullptr;
}

// player controller thread: reads keys and does firing
void* playerControllerFn(void* arg) {
    // We'll read keys from gamewin with getch in non-blocking mode
    nodelay(gamewin, TRUE);
    keypad(gamewin, TRUE);

    int ch;
    while (gameRunning) {
        pthread_mutex_lock(&screenMutex);
        ch = wgetch(gamewin);
        pthread_mutex_unlock(&screenMutex);

        if (ch == ERR) {
            std::this_thread::sleep_for(30ms);
            continue;
        }

        if (ch == 'q' || ch == 'Q') {
            gameRunning = false;
            pthread_cond_broadcast(&batteryNotFull); // wake reload thread
            break;
        }

        bool fired = false;
        if (ch == KEY_UP || ch == 'w' || ch == 'W') {
            pthread_mutex_lock(&batteryMutex); currentAim = AIM_UP; pthread_mutex_unlock(&batteryMutex);
        } else if (ch == KEY_LEFT || ch == 'a' || ch == 'A') {
            pthread_mutex_lock(&batteryMutex); currentAim = AIM_LEFT; pthread_mutex_unlock(&batteryMutex);
        } else if (ch == KEY_RIGHT || ch == 'd' || ch == 'D') {
            pthread_mutex_lock(&batteryMutex); currentAim = AIM_RIGHT; pthread_mutex_unlock(&batteryMutex);
        } else if (ch == 'z' || ch == 'Z') {
            pthread_mutex_lock(&batteryMutex); currentAim = AIM_UPLEFT; pthread_mutex_unlock(&batteryMutex);
        } else if (ch == 'c' || ch == 'C') {
            pthread_mutex_lock(&batteryMutex); currentAim = AIM_UPRIGHT; pthread_mutex_unlock(&batteryMutex);
        } else if (ch == ' ' ) {
            // attempt fire: consume first launcher that contains a rocket
            pthread_mutex_lock(&batteryMutex);
            int chosen = -1;
            for (int i = 0; i < k_launchers_global; ++i) {
                if (launchers[i]) { chosen = i; break; }
            }
            if (chosen != -1) {
                launchers[chosen] = false; // consume rocket
                fired = true;
            }
            // if after consumption there's any empty launcher, wake reloadThread
            pthread_cond_signal(&batteryNotFull);
            pthread_mutex_unlock(&batteryMutex);

            if (fired) {
                // create rocket at bottom center-ish
                Rocket rr;
                rr.id = nextRocketId++;
                rr.aim = currentAim;
                rr.active = true;

                // starting position: center-bottom above ground
                rr.x = SCREEN_W / 2;
                rr.y = SCREEN_H - 3;

                // push and start thread
                pthread_mutex_lock(&rocketListMutex);
                rockets.push_back(rr);
                pthread_mutex_unlock(&rocketListMutex);

                Rocket* rarg = new Rocket;
                *rarg = rr;
                pthread_t rtid;
                pthread_create(&rtid, nullptr, rocketThreadFn, rarg);

                // store tid
                pthread_mutex_lock(&rocketListMutex);
                for (auto &gr : rockets) if (gr.id == rr.id) gr.tid = rtid;
                pthread_mutex_unlock(&rocketListMutex);
            } else {
                // optional: beep or message (no rockets available)
                pthread_mutex_lock(&screenMutex);
                mvwprintw(gamewin, SCREEN_H-3, 2, "No rockets available!");
                wrefresh(gamewin);
                pthread_mutex_unlock(&screenMutex);
                std::this_thread::sleep_for(300ms);
            }
        }
        // redraw
        drawScreen();
        std::this_thread::sleep_for(30ms);
    }
    return nullptr;
}

// ---------- Helpers for end-of-game and cleanup ----------
void waitForAllThreadsAndCleanup() {
    // Wait for enemy threads to finish
    pthread_mutex_lock(&enemyListMutex);
    for (auto &e : enemies) {
        if (e.tid) pthread_join(e.tid, nullptr);
    }
    pthread_mutex_unlock(&enemyListMutex);

    // Wait for rocket threads
    pthread_mutex_lock(&rocketListMutex);
    for (auto &r : rockets) {
        if (r.tid) pthread_join(r.tid, nullptr);
    }
    pthread_mutex_unlock(&rocketListMutex);
}

// ---------- Main ----------
int main() {
    // seed rng
    rng.seed((unsigned)time(nullptr));

    // init ncurses
    initscr();
    noecho();
    curs_set(FALSE);
    keypad(stdscr, TRUE);

    // adapt screen size constants
    getmaxyx(stdscr, SCREEN_H, SCREEN_W);
    if (SCREEN_H < 20) SCREEN_H = 20;
    if (SCREEN_W < 60) SCREEN_W = 60;

    // choose difficulty
    WINDOW* menu = newwin(10, 36, (SCREEN_H-10)/2, (SCREEN_W-36)/2);
    box(menu, 0, 0);
    mvwprintw(menu, 1, 2, "Choose difficulty:");
    mvwprintw(menu, 3, 4, "1 - Easy");
    mvwprintw(menu, 4, 4, "2 - Medium");
    mvwprintw(menu, 5, 4, "3 - Hard");
    mvwprintw(menu, 7, 2, "Use keys 1/2/3 then Enter");
    wrefresh(menu);

    int choice = 2;
    int c;
    keypad(menu, TRUE);
    nodelay(menu, FALSE);
    while (true) {
        c = wgetch(menu);
        if (c == '1') { choice = 1; break; }
        if (c == '2') { choice = 2; break; }
        if (c == '3') { choice = 3; break; }
        if (c == 10) break;
    }
    delwin(menu);

    if (choice == 1) settings = EASY;
    else if (choice == 2) settings = MEDIUM;
    else settings = HARD;

    k_launchers_global = settings.k_launchers;
    launchers.assign(k_launchers_global, true);

    // create main game window
    gamewin = newwin(SCREEN_H, SCREEN_W, 0, 0);

    // start threads: spawner, reload, player controller
    pthread_t spawnerTid, reloadTid, playerTid;

    pthread_create(&spawnerTid, nullptr, enemySpawnerFn, nullptr);
    pthread_create(&reloadTid, nullptr, reloadThreadFn, nullptr);
    pthread_create(&playerTid, nullptr, playerControllerFn, nullptr);

    // main loop: draw screen and check end conditions
    while (gameRunning) {
        drawScreen();

        // termination conditions
        int m = settings.m_enemies;
        int destroyed = destroyedEnemies.load();
        int ground = groundHits.load();

        if (destroyed >= (m + 1)/2) {
            // victory
            pthread_mutex_lock(&screenMutex);
            mvwprintw(gamewin, SCREEN_H/2, SCREEN_W/2 - 8, "YOU WIN! (%d/%d)", destroyed, m);
            wrefresh(gamewin);
            pthread_mutex_unlock(&screenMutex);
            gameRunning = false;
            break;
        }
        if (ground > m/2) {
            pthread_mutex_lock(&screenMutex);
            mvwprintw(gamewin, SCREEN_H/2, SCREEN_W/2 - 8, "YOU LOSE! (%d/%d)", ground, m);
            wrefresh(gamewin);
            pthread_mutex_unlock(&screenMutex);
            gameRunning = false;
            break;
        }
        // if spawn finished and all enemies are either destroyed or grounded, end and evaluate counts
        if (spawnDone) {
            bool anyAlive = false;
            pthread_mutex_lock(&enemyListMutex);
            for (const auto &e : enemies) if (e.alive) { anyAlive = true; break; }
            pthread_mutex_unlock(&enemyListMutex);
            if (!anyAlive) {
                // all finished, check counts
                if (destroyed >= (m+1)/2) {
                    pthread_mutex_lock(&screenMutex);
                    mvwprintw(gamewin, SCREEN_H/2, SCREEN_W/2 - 8, "YOU WIN! (%d/%d)", destroyed, m);
                    wrefresh(gamewin);
                    pthread_mutex_unlock(&screenMutex);
                } else {
                    pthread_mutex_lock(&screenMutex);
                    mvwprintw(gamewin, SCREEN_H/2, SCREEN_W/2 - 8, "YOU LOSE! (%d/%d)", ground, m);
                    wrefresh(gamewin);
                    pthread_mutex_unlock(&screenMutex);
                }
                gameRunning = false;
                break;
            }
        }

        std::this_thread::sleep_for(120ms);
    }

    // notify threads to stop
    gameRunning = false;
    pthread_cond_broadcast(&batteryNotFull);

    // wait joins
    pthread_join(spawnerTid, nullptr);
    pthread_join(reloadTid, nullptr);
    pthread_join(playerTid, nullptr);

    waitForAllThreadsAndCleanup();

    // final pause to show result
    pthread_mutex_lock(&screenMutex);
    mvwprintw(gamewin, SCREEN_H-4, 2, "Press any key to exit...");
    wrefresh(gamewin);
    pthread_mutex_unlock(&screenMutex);

    nodelay(gamewin, FALSE);
    wgetch(gamewin);

    // cleanup ncurses
    delwin(gamewin);
    endwin();

    return 0;
}