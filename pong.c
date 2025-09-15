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