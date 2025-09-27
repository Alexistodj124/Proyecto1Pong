// PONG
// CC3086 - Programación de microprocesadores
// Requiere: ncurses y pthreads
// Compilar: gcc pong.c -o pong -lncurses -lpthread

#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

// CONFIGURACIÓN
#define TARGET_FPS_PLAY 60
#define FRAME_USEC_PLAY (1000000 / TARGET_FPS_PLAY)

#define PADDLE_LEN 5
#define PADDLE_SPEED 1
#define BALL_SPEED_X 0.8f
#define BALL_SPEED_Y 0.4f
#define SCORE_TO_WIN 7

#define NAME_MAXLEN 24
#define LEADERBOARD_FILE "pong_scores.txt"
#define MAX_LEADER_ENTRIES 200

// ESTADO GLOBAL
typedef struct {
    float x, y;
    float vx, vy;
} Ball;

typedef struct {
    int x;
    float y;
} Paddle;

typedef struct {
    int p1, p2;
} Score;

typedef struct {
    char winner[NAME_MAXLEN+1];
    char loser[NAME_MAXLEN+1];
    int  winScore;
    int  loseScore;
    time_t ts;
} Entry;

// Estados
typedef enum {
    SC_MENU = 0,
    SC_INSTR,
    SC_PLAYING,
    SC_LEADER
} Scene;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_threads_should_run = false;
static volatile bool g_paused = false;
static volatile bool g_exit_requested = false;

// Entradas
static volatile int g_p1_up = 0, g_p1_down = 0;
static volatile int g_p2_up = 0, g_p2_down = 0;

// Elementos del juego
static Ball   g_ball;
static Paddle g_pad1, g_pad2;
static Score  g_score;
static int    g_top, g_bottom, g_left, g_right;
static int    g_midX;

static pthread_t th_ball, th_p1, th_p2;

// Nombres
static char g_name1[NAME_MAXLEN+1] = "Jugador 1";
static char g_name2[NAME_MAXLEN+1] = "Jugador 2";

// UTILIDADES
static void clamp_float(float* v, float mn, float mx) {
    if (*v < mn) *v = mn;
    if (*v > mx) *v = mx;
}

static void reset_world() {
    int H, W;
    getmaxyx(stdscr, H, W);
    
    // Score e instrucciones
    g_top = 2;
    g_bottom = H - 2;
    g_left = 2;
    g_right = W - 3;
    g_midX = W / 2;

    // Paletas
    g_pad1.x = g_left + 2;
    g_pad2.x = g_right - 2;
    g_pad1.y = (g_top + g_bottom) / 2;
    g_pad2.y = (g_top + g_bottom) / 2;

    // Pelota
    g_ball.x = (float)((g_left + g_right) / 2);
    g_ball.y = (float)((g_top + g_bottom) / 2);
    g_ball.vx = ((rand() % 2) ? 1.0f : -1.0f) * BALL_SPEED_X;
    g_ball.vy = ((rand() % 2) ? 1.0f : -1.0f) * BALL_SPEED_Y;
    g_score.p1 = 0; g_score.p2 = 0;
    g_paused = false;
}

static void draw_borders_and_center() {
    for (int x = g_left; x <= g_right; ++x) {
        mvaddch(g_top, x, '-');
        mvaddch(g_bottom, x, '-');
    }
    for (int y = g_top; y <= g_bottom; ++y) {
        mvaddch(y, g_left, '|');
        mvaddch(y, g_right, '|');
    }
    for (int y = g_top + 1; y < g_bottom; y += 2) {
        mvaddch(y, g_midX, ':');
    }
}

static void draw_score() {
    mvprintw(0, 2, "%s: %d   %s: %d   (P: pausa, Q: menu)",
             g_name1, g_score.p1, g_name2, g_score.p2);
}

static void draw_paddles_and_ball() {
    int y1 = (int)g_pad1.y;
    for (int k = -PADDLE_LEN/2; k <= PADDLE_LEN/2; ++k) {
        int yy = y1 + k;
        if (yy > g_top && yy < g_bottom) mvaddch(yy, g_pad1.x, '|');
    }
    int y2 = (int)g_pad2.y;
    for (int k = -PADDLE_LEN/2; k <= PADDLE_LEN/2; ++k) {
        int yy = y2 + k;
        if (yy > g_top && yy < g_bottom) mvaddch(yy, g_pad2.x, '|');
    }
    mvaddch((int)g_ball.y, (int)g_ball.x, 'O');
}

// LEADERBOARD
static void ensure_file_exists() {
    FILE* f = fopen(LEADERBOARD_FILE, "a");
    if (f) fclose(f);
}

static int cmp_entry(const void* a, const void* b) {
    const Entry* ea = (const Entry*)a;
    const Entry* eb = (const Entry*)b;
    if (eb->winScore != ea->winScore) return eb->winScore - ea->winScore;
    if (eb->ts > ea->ts) return 1;
    if (eb->ts < ea->ts) return -1;
    return 0;
}

static int load_entries(Entry* arr, int maxn) {
    ensure_file_exists();
    FILE* f = fopen(LEADERBOARD_FILE, "r");
    if (!f) return 0;
    int n = 0;
    while (n < maxn) {
        Entry e;
        int scanned = fscanf(f, "%24[^,],%24[^,],%d,%d,%ld\n",
                             e.winner, e.loser, &e.winScore, &e.loseScore, &e.ts);
        if (scanned != 5) break;
        arr[n++] = e;
    }
    fclose(f);
    return n;
}

static void append_entry(const Entry* e) {
    ensure_file_exists();
    FILE* f = fopen(LEADERBOARD_FILE, "a");
    if (!f) return;
    fprintf(f, "%s,%s,%d,%d,%ld\n", e->winner, e->loser, e->winScore, e->loseScore, (long)e->ts);
    fclose(f);
}

static void leaderboard_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    Entry entries[MAX_LEADER_ENTRIES];
    int n = load_entries(entries, MAX_LEADER_ENTRIES);
    qsort(entries, n, sizeof(Entry), cmp_entry);

    int topN = (n < 10) ? n : 10;

    while (1) {
        clear();
        mvprintw(0, 2, "PUNTAJES DESTACADOS (Top %d)  -  ENTER para volver", topN);
        mvprintw(2, 2, "%-3s %-24s %-6s %-24s %-10s", "#", "Ganador", "Marcador", "Perdedor", "Fecha");
        mvhline(3, 2, '-', 70);
        for (int i = 0; i < topN; ++i) {
            char datebuf[20];
            struct tm* tmv = localtime(&entries[i].ts);
            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", tmv);
            mvprintw(4 + i, 2, "%-3d %-24s %2d-%-3d %-24s %-10s",
                     i+1, entries[i].winner, entries[i].winScore, entries[i].loseScore, entries[i].loser, datebuf);
        }
        if (n == 0) mvprintw(5, 2, "Aun no hay partidas registradas. Juega una y se guardara aqui.");
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
    }
}

// INPUT DE TEXTO
static void read_line_ncurses(char* out, int maxlen, int y, int x) {
    int len = 0;
    out[0] = '\0';
    move(y, x);
    curs_set(1);
    while (1) {
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                out[len] = '\0';
                mvaddch(y, x+len, ' ');
                move(y, x+len);
            }
        } else if (ch >= 32 && ch <= 126) {
            if (len < maxlen) {
                out[len++] = (char)ch;
                out[len] = '\0';
                mvaddch(y, x+len-1, ch);
            }
        }
        refresh();
    }
    curs_set(0);
    if (len == 0) strncpy(out, "Jugador", maxlen);
}

// LÓGICA
static void* thread_ball_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            g_ball.x += g_ball.vx;
            g_ball.y += g_ball.vy;
            if (g_ball.y <= g_top + 1) { g_ball.y = g_top + 1; g_ball.vy *= -1.0f; }
            if (g_ball.y >= g_bottom - 1) { g_ball.y = g_bottom - 1; g_ball.vy *= -1.0f; }
            int y1 = (int)g_pad1.y;
            if ((int)g_ball.x == g_pad1.x + 1 && g_ball.vx < 0) {
                if ((int)g_ball.y >= y1 - PADDLE_LEN/2 && (int)g_ball.y <= y1 + PADDLE_LEN/2) {
                    g_ball.vx *= -1.0f;
                    int dy = (int)g_ball.y - y1;
                    g_ball.vy += 0.15f * dy;
                }
            }
            int y2 = (int)g_pad2.y;
            if ((int)g_ball.x == g_pad2.x - 1 && g_ball.vx > 0) {
                if ((int)g_ball.y >= y2 - PADDLE_LEN/2 && (int)g_ball.y <= y2 + PADDLE_LEN/2) {
                    g_ball.vx *= -1.0f;
                    int dy = (int)g_ball.y - y2;
                    g_ball.vy += 0.15f * dy;
                }
            }
            if ((int)g_ball.x <= g_left) {
                g_score.p2++;
                g_ball.x = (float)((g_left + g_right) / 2);
                g_ball.y = (float)((g_top + g_bottom) / 2);
                g_ball.vx = +BALL_SPEED_X;
                g_ball.vy = ((rand() % 2) ? 1 : -1) * BALL_SPEED_Y;
            } else if ((int)g_ball.x >= g_right) {
                g_score.p1++;
                g_ball.x = (float)((g_left + g_right) / 2);
                g_ball.y = (float)((g_top + g_bottom) / 2);
                g_ball.vx = -BALL_SPEED_X;
                g_ball.vy = ((rand() % 2) ? 1 : -1) * BALL_SPEED_Y;
            }
            pthread_mutex_unlock(&g_lock);
        }
        usleep(FRAME_USEC_PLAY);
    }
    return NULL;
}

static void move_paddle(Paddle* p, int dir) {
    if (dir == -1) p->y -= PADDLE_SPEED;
    else if (dir == 1) p->y += PADDLE_SPEED;
    clamp_float(&p->y, g_top + 1 + PADDLE_LEN/2, g_bottom - 1 - PADDLE_LEN/2);
}

static void* thread_p1_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            int dir = 0;
            if (g_p1_up && !g_p1_down) dir = -1;
            else if (g_p1_down && !g_p1_up) dir = 1;
            move_paddle(&g_pad1, dir);
            pthread_mutex_unlock(&g_lock);
        }
        usleep(FRAME_USEC_PLAY);
    }
    return NULL;
}

static void* thread_p2_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            int dir = 0;
            if (g_p2_up && !g_p2_down) dir = -1;
            else if (g_p2_down && !g_p2_up) dir = 1;
            move_paddle(&g_pad2, dir);
            pthread_mutex_unlock(&g_lock);
        }
        usleep(FRAME_USEC_PLAY);
    }
    return NULL;
}

// PANTALLAS
static void input_names_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    clear();
    mvprintw(0, 2, "NOMBRES DE JUGADORES");
    mvprintw(2, 2, "Ingresa nombre para Jugador 1 (W/S):");
    mvprintw(3, 4, "> ");
    refresh();
    read_line_ncurses(g_name1, NAME_MAXLEN, 3, 6);
    mvprintw(5, 2, "Ingresa nombre para Jugador 2 (Flechas):");
    mvprintw(6, 4, "> ");
    refresh();
    read_line_ncurses(g_name2, NAME_MAXLEN, 6, 6);
}

static int menu_screen() {
    const char* items[] = {
        "Iniciar partida",
        "Instrucciones",
        "Puntajes destacados",
        "Salir"
    };
    const int N = sizeof(items) / sizeof(items[0]);
    int sel = 0;

    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        int H, W; getmaxyx(stdscr, H, W);
        mvprintw(0, 2, "PONG - FASE 02  (Usa Flechas y ENTER)");
        for (int i = 0; i < N; ++i) {
            if (i == sel) attron(A_REVERSE);
            mvprintw(3 + i, 4, "%s", items[i]);
            if (i == sel) attroff(A_REVERSE);
        }
        mvprintw(H-2, 2, "Q para salir rapido");
        refresh();

        int ch = getch();
        if (ch == KEY_UP) { sel = (sel - 1 + N) % N; }
        else if (ch == KEY_DOWN) { sel = (sel + 1) % N; }
        else if (ch == '\n' || ch == KEY_ENTER) { return sel; }
        else if (ch == 'q' || ch == 'Q') { return 3; }
    }
}

//selección de modo
static int mode_screen() {
    const char* items[] = {
        "Jugador vs Jugador",
        "Jugador vs Computadora",
        "Computadora vs Computadora",
    };
    const int N = sizeof(items) / sizeof(items[0]);
    int sel = 0;

    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        int H, W; getmaxyx(stdscr, H, W);
        mvprintw(0, 2, "Selecciona un modo de juego  (Usa Flechas y ENTER)");
        for (int i = 0; i < N; ++i) {
            if (i == sel) attron(A_REVERSE);
            mvprintw(3 + i, 4, "%s", items[i]);
            if (i == sel) attroff(A_REVERSE);
        }
        mvprintw(H-2, 2, "Q para salir rapido");
        refresh();

        int ch = getch();
        if (ch == KEY_UP) { sel = (sel - 1 + N) % N; }
        else if (ch == KEY_DOWN) { sel = (sel + 1) % N; }
        else if (ch == '\n' || ch == KEY_ENTER) { return sel; }
        else if (ch == 'q' || ch == 'Q') { return 3; }
    }
}

static void instructions_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        mvprintw(0, 2, "INSTRUCCIONES");
        mvprintw(2, 2, "Objetivo: Evita que la pelota pase tu borde. Gana quien llegue a %d.", SCORE_TO_WIN);
        mvprintw(4, 2, "Controles:");
        mvprintw(5, 4, "%s: W (arriba), S (abajo)", g_name1);
        mvprintw(6, 4, "%s: Flecha UP (arriba), Flecha DOWN (abajo)", g_name2);
        mvprintw(7, 4, "Globales en juego: P (pausa), Q (menu)");
        mvprintw(9, 2, "Presiona ENTER para volver al menu.");
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
    }
}

static void announce_winner_and_wait(const char* who) {
    int H, W; getmaxyx(stdscr, H, W);
    int cx = W/2;
    attron(A_BOLD);
    mvprintw((H/2)-1, cx - (int)strlen(who)/2, "%s", who);
    attroff(A_BOLD);
    const char* msg = "(ENTER) Reiniciar   (Q) Menu";
    mvprintw((H/2)+1, cx - (int)strlen(msg)/2, "%s", msg);
    refresh();
}

static Scene play_screen() {
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    timeout(0);
    reset_world();
    g_threads_should_run = true;
    pthread_create(&th_ball, NULL, thread_ball_func, NULL);
    pthread_create(&th_p1, NULL, thread_p1_func, NULL);
    pthread_create(&th_p2, NULL, thread_p2_func, NULL);

    Scene next = SC_MENU;
    while (!g_exit_requested) {
        int ch;
        while ((ch = getch()) != ERR) {
            if (ch == 'q' || ch == 'Q') { next = SC_MENU; goto END_PLAY; }
            if (ch == 'p' || ch == 'P') { g_paused = !g_paused; }
            if (ch == 'w' || ch == 'W') g_p1_up = 1;
            if (ch == 's' || ch == 'S') g_p1_down = 1;
            if (ch == KEY_UP) g_p2_up = 1;
            if (ch == KEY_DOWN) g_p2_down = 1;
            if (ch == 'w' || ch == 'W') g_p1_down = 0;
            if (ch == 's' || ch == 'S') g_p1_up = 0;
            if (ch == KEY_UP) g_p2_down = 0;
            if (ch == KEY_DOWN) g_p2_up = 0;
        }

        pthread_mutex_lock(&g_lock);
        clear();
        draw_score();
        draw_borders_and_center();
        draw_paddles_and_ball();

        if (g_paused) {
            attron(A_BOLD);
            mvprintw((g_top + g_bottom)/2, g_midX - 2, "PAUSA");
            attroff(A_BOLD);
        }

        if (g_score.p1 >= SCORE_TO_WIN || g_score.p2 >= SCORE_TO_WIN) {
            const bool p1win = g_score.p1 > g_score.p2;
            const char* who = p1win ? "Gana JUGADOR 1" : "Gana JUGADOR 2";
            announce_winner_and_wait(who);
            pthread_mutex_unlock(&g_lock);

            // Guardar en leaderboard
            Entry e = {0};
            strncpy(e.winner, p1win ? g_name1 : g_name2, NAME_MAXLEN);
            strncpy(e.loser,  p1win ? g_name2 : g_name1, NAME_MAXLEN);
            e.winScore = p1win ? g_score.p1 : g_score.p2;
            e.loseScore = p1win ? g_score.p2 : g_score.p1;
            e.ts = time(NULL);
            append_entry(&e);

            nodelay(stdscr, FALSE);
            int c;
            while ((c = getch())) {
                if (c == 'q' || c == 'Q') { next = SC_MENU; goto END_PLAY; }
                if (c == '\n' || c == KEY_ENTER) {
                    nodelay(stdscr, TRUE);
                    reset_world();
                    break;
                }
            }
        } else {
            refresh();
            pthread_mutex_unlock(&g_lock);
        }
        usleep(FRAME_USEC_PLAY);
    }

END_PLAY:
    g_threads_should_run = false;
    usleep(2 * FRAME_USEC_PLAY);
    pthread_join(th_ball, NULL);
    pthread_join(th_p1, NULL);
    pthread_join(th_p2, NULL);
    return next;
}

// MAIN
int main(void) {
    srand((unsigned int)time(NULL));
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    Scene scene = SC_MENU;
    while (!g_exit_requested) {
        if (scene == SC_MENU) {
            int sel = menu_screen();
            if (sel == 0) {
                int mode = mode_screen();
                if (mode == 0) { // Jugador vs Jugador
                    scene = SC_PLAYING;
                } else if (mode == 1) { // Jugador vs Computadora
                    input_names_screen();
                    scene = SC_PLAYING;
                } else if (mode == 2) { // Computadora vs Computadora
                    scene = SC_PLAYING;
                } else if (mode == 3) { // Q para salir rapido
                    scene = SC_MENU;
                }
            }
            else if (sel == 1) { instructions_screen(); scene = SC_MENU; }
            else if (sel == 2) { leaderboard_screen(); scene = SC_MENU; }
            else if (sel == 3) { g_exit_requested = true; }
        } else if (scene == SC_PLAYING) {
            scene = play_screen();
        } else if (scene == SC_INSTR) {
            instructions_screen();
            scene = SC_MENU;
        } else if (scene == SC_LEADER) {
            leaderboard_screen();
            scene = SC_MENU;
        }
    }
    endwin();
    return 0;
}

