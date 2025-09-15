// PONG
// CC3086 - Programación de microprocesadores
// Requiere: ncurses y pthreads
// Compilar: gcc pong_phase2.c -o pong_phase2 -lncurses -lpthread

#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>

// CONFIGURACIÓN 
#define TARGET_FPS_PLAY 60
#define FRAME_USEC_PLAY (1000000 / TARGET_FPS_PLAY)

#define PADDLE_LEN 5
#define PADDLE_SPEED 1         // celdas por frame de render
#define BALL_SPEED_X 0.8f
#define BALL_SPEED_Y 0.4f
#define SCORE_TO_WIN 7

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

// Escena
typedef enum {
    SC_MENU = 0,
    SC_INSTR,
    SC_PLAYING
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
static int    g_top, g_bottom, g_left, g_right; // límites del campo útil (sin la línea de score)
static int    g_midX;

static pthread_t th_ball, th_p1, th_p2;

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

    g_score.p1 = 0;
    g_score.p2 = 0;

    g_paused = false;
}
tic void draw_borders_and_center() {
    // Marco
    for (int x = g_left; x <= g_right; ++x) {
        mvaddch(g_top, x, '-');
        mvaddch(g_bottom, x, '-');
    }
    for (int y = g_top; y <= g_bottom; ++y) {
        mvaddch(y, g_left, '|');
        mvaddch(y, g_right, '|');
    }
    // Línea central
    for (int y = g_top + 1; y < g_bottom; y += 2) {
        mvaddch(y, g_midX, ':');
    }
}

static void draw_score() {
    mvprintw(0, 2, "P1: %d   P2: %d   (P: pausa, Q: menu)", g_score.p1, g_score.p2);
}

static void draw_paddles_and_ball() {
    // Paleta 1
    int y1 = (int)g_pad1.y;
    for (int k = -PADDLE_LEN/2; k <= PADDLE_LEN/2; ++k) {
        int yy = y1 + k;
        if (yy > g_top && yy < g_bottom) {
            mvaddch(yy, g_pad1.x, '|');
        }
    }
    // Paleta 2
    int y2 = (int)g_pad2.y;
    for (int k = -PADDLE_LEN/2; k <= PADDLE_LEN/2; ++k) {
        int yy = y2 + k;
        if (yy > g_top && yy < g_bottom) {
            mvaddch(yy, g_pad2.x, '|');
        }
    }
    // Pelota
    mvaddch((int)g_ball.y, (int)g_ball.x, 'O');
}

static void announce_winner_and_wait(const char* who) {
    const char* msg = "(ENTER) Reiniciar   (Q) Menu";
    int H, W; getmaxyx(stdscr, H, W);
    int cx = W/2;
    attron(A_BOLD);
    mvprintw((H/2)-1, cx - (int)strlen(who)/2, "%s", who);
    attroff(A_BOLD);
    mvprintw((H/2)+1, cx - (int)strlen(msg)/2, "%s", msg);
    refresh();
}

// LÓGICA EN HILOS
static void* thread_ball_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            // Avanzar
            g_ball.x += g_ball.vx;
            g_ball.y += g_ball.vy;

            // Rebotes con techo/piso
            if (g_ball.y <= g_top + 1) {
                g_ball.y = g_top + 1;
                g_ball.vy *= -1.0f;
            }
            if (g_ball.y >= g_bottom - 1) {
                g_ball.y = g_bottom - 1;
                g_ball.vy *= -1.0f;
            }

            // Colisión con paleta izquierda
            int y1 = (int)g_pad1.y;
            if ((int)g_ball.x == g_pad1.x + 1 && g_ball.vx < 0) {
                if ((int)g_ball.y >= y1 - PADDLE_LEN/2 && (int)g_ball.y <= y1 + PADDLE_LEN/2) {
                    g_ball.vx *= -1.0f;
                    // "Efecto" simple según zona de impacto
                    int dy = (int)g_ball.y - y1;
                    g_ball.vy += 0.15f * dy;
                }
            }
            // Colisión con paleta derecha
            int y2 = (int)g_pad2.y;
            if ((int)g_ball.x == g_pad2.x - 1 && g_ball.vx > 0) {
                if ((int)g_ball.y >= y2 - PADDLE_LEN/2 && (int)g_ball.y <= y2 + PADDLE_LEN/2) {
                    g_ball.vx *= -1.0f;
                    int dy = (int)g_ball.y - y2;
                    g_ball.vy += 0.15f * dy;
                }
            }

            // Puntos
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
static int menu_screen() {
    const char* items[] = {
        "Iniciar partida (J1: W/S, J2: Flechas)",
        "Instrucciones",
        "Puntajes destacados (WIP)",
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

static void instructions_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        mvprintw(0, 2, "INSTRUCCIONES");
        mvprintw(2, 2, "Objetivo: Evita que la pelota pase tu borde. Gana quien llegue a %d.", SCORE_TO_WIN);
        mvprintw(4, 2, "Controles:");
        mvprintw(5, 4, "Jugador 1: W (arriba), S (abajo)");
        mvprintw(6, 4, "Jugador 2: Flecha UP (arriba), Flecha DOWN (abajo)");
        mvprintw(7, 4, "Globales en juego: P (pausa), Q (menu)");
        mvprintw(9, 2, "Presiona ENTER para volver al menu.");
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
    }
}

static Scene play_screen() {
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    timeout(0);

    // Resetear
    reset_world();
    g_threads_should_run = true;
    pthread_create(&th_ball, NULL, thread_ball_func, NULL);
    pthread_create(&th_p1, NULL, thread_p1_func, NULL);
    pthread_create(&th_p2, NULL, thread_p2_func, NULL);

    Scene next = SC_MENU;
    while (!g_exit_requested) {
        // INPUT
        int ch;
        while ((ch = getch()) != ERR) {
            if (ch == 'q' || ch == 'Q') { next = SC_MENU; goto END_PLAY; }
            if (ch == 'p' || ch == 'P') { g_paused = !g_paused; }
            // J1
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
        // Ganador
        if (g_score.p1 >= SCORE_TO_WIN || g_score.p2 >= SCORE_TO_WIN) {
            const char* who = (g_score.p1 > g_score.p2) ? "Gana JUGADOR 1" : "Gana JUGADOR 2";
            announce_winner_and_wait(who);
            pthread_mutex_unlock(&g_lock);
            // Esperar acción
            nodelay(stdscr, FALSE);
            int c;
            while ((c = getch())) {
                if (c == 'q' || c == 'Q') { next = SC_MENU; goto END_PLAY; }
                if (c == '\n' || c == KEY_ENTER) {
                    // Reiniciar partida
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
            if      (sel == 0) scene = SC_PLAYING;
            else if (sel == 1) { instructions_screen(); scene = SC_MENU; }
            else if (sel == 2) { /* Placeholder puntajes */ scene = SC_MENU; }
            else if (sel == 3) { g_exit_requested = true; }
        }
        else if (scene == SC_INSTR) {
            instructions_screen();
            scene = SC_MENU;
        }
        else if (scene == SC_PLAYING) {
            scene = play_screen();
        }
    }

    endwin();
    return 0;
}
