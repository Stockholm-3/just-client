/**
 * @file tui.c
 * @brief Terminal User Interface implementation using ncurses.
 *
 * Layout (4 fixed windows):
 *
 *   row 0          : HEADER  — app name + server address
 *   rows 1..LINES-4: CONTENT — menus / input forms / scrollable results
 *   row LINES-2    : HELP    — context-sensitive key hints
 *   row LINES-1    : STATUS  — "Ready" / "Fetching..." / error text
 *
 * State machine:
 *   MAIN_MENU → INPUT → (dispatch) → RESULT → MAIN_MENU
 *   MAIN_MENU → CONFIRM → MAIN_MENU
 */
#include "tui.h"

#include "../api/weather_client.h"
#include "../utils/utils.h"

#include <ctype.h>
#include <jansson.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ======================================================================
 * Constants
 * ====================================================================== */
#define MAX_INPUT_LEN 127
#define LINE_BUF_SIZE 512
#define MIN_LINES 10
#define MIN_COLS 50
#define MAX_SAVED_CITIES 64

/* Color pairs */
#define CP_NORMAL 1
#define CP_WORKING 2
#define CP_ERROR 3
#define CP_HEADER 4
#define CP_SELECTED 5
#define CP_HELP 6

/* ======================================================================
 * Internal types
 * ====================================================================== */
typedef enum {
    TUI_SCREEN_MAIN_MENU,
    TUI_SCREEN_CITY_MENU,
    TUI_SCREEN_CITY_SELECT,
    TUI_SCREEN_CITY_SEARCH,
    TUI_SCREEN_INPUT,
    TUI_SCREEN_RESULT,
    TUI_SCREEN_CONFIRM,
    TUI_SCREEN_SAVE_CITY,
} TuiScreen;

typedef struct {
    char city[MAX_INPUT_LEN + 1];
    char price[MAX_INPUT_LEN + 1];
} SavedCity;

typedef enum {
    TUI_CMD_NONE = 0,
    TUI_CMD_CURRENT,
    TUI_CMD_WEATHER,
    TUI_CMD_CITIES,
    TUI_CMD_PLAN,
    TUI_CMD_CLEAR_CACHE,
    TUI_CMD_SERVER,
} TuiCommand;

struct TuiContext {
    WeatherClient* client;

    WINDOW* header_win;
    WINDOW* content_win;
    WINDOW* help_win;
    WINDOW* status_win;

    TuiScreen  screen;
    TuiCommand pending_cmd;

    int menu_selected;

    int  input_field;
    int  input_nfields;
    char input_bufs[4][MAX_INPUT_LEN + 1];

    char** result_lines;
    int    result_nlines;
    int    result_scroll;
    char   result_title[64]; /* shown on top border of result window */

    SavedCity saved_cities[MAX_SAVED_CITIES]; /* quick-search city list */
    int       saved_cities_count;
    int       city_menu_selected;            /* highlighted row in city menu */
    int       city_select_idx;               /* highlighted row in load list */
    char      search_buf[MAX_INPUT_LEN + 1]; /* live filter query */
    int       city_search_idx; /* highlighted row in search results */

    int running;
};

/* ======================================================================
 * Menu data
 * ====================================================================== */
static const char* g_city_menu_items[] = {
    "Load",
    "Search",
    "< Back",
};
#define CITY_MENU_COUNT 3

static const char* g_menu_items[] = {
    "Weather by Coordinates",
    "Weather by City",
    "Search Cities",
    "Energy Plan",
    "Clear Cache",
    "Server Settings",
    "Quit",
};
#define MENU_COUNT 7

/* ======================================================================
 * Forward declarations
 * ====================================================================== */
static void tui_create_windows(TuiContext* ctx);
static void tui_destroy_windows(TuiContext* ctx);
static void tui_recreate_windows(TuiContext* ctx);
static void tui_draw_header(TuiContext* ctx);
static void tui_set_status(TuiContext* ctx, const char* msg, int cp);
static void tui_set_help(TuiContext* ctx, const char* msg);
static void tui_free_result(TuiContext* ctx);

static void tui_handle_main_menu(TuiContext* ctx);
static void tui_handle_city_menu(TuiContext* ctx);
static void tui_handle_city_select(TuiContext* ctx);
static void tui_handle_city_search(TuiContext* ctx);
static void tui_handle_input(TuiContext* ctx);
static void tui_handle_result(TuiContext* ctx);
static void tui_handle_confirm(TuiContext* ctx);
static void tui_handle_save_city(TuiContext* ctx);
static void tui_activate_menu_item(TuiContext* ctx);
static void tui_dispatch_command(TuiContext* ctx);

static void tui_draw_main_menu(TuiContext* ctx);
static void tui_draw_city_menu(TuiContext* ctx);
static void tui_draw_city_select(TuiContext* ctx);
static void tui_draw_city_search(TuiContext* ctx);
static void tui_draw_input_screen(TuiContext* ctx);
static void tui_draw_result_screen(TuiContext* ctx);
static void tui_draw_confirm_screen(TuiContext* ctx);
static void tui_draw_save_city_screen(TuiContext* ctx);

static const char* tui_input_title(TuiCommand cmd);
static const char* tui_input_label(TuiCommand cmd, int field);
static int         tui_validate_input(TuiContext* ctx);

static int    lb_append(char*** lines, int* nlines, int* cap, const char* text);
static char** tui_build_weather_lines(json_t* data, int* out_nlines);
static char** tui_build_cities_lines(json_t* data, int* out_nlines);
static char** tui_build_plan_lines(json_t* data, int* out_nlines);
static char** tui_build_json_lines(json_t* data, int* out_nlines);
static char** tui_build_error_lines(const char* msg, int* out_nlines);

/* ======================================================================
 * Saved cities persistence (~/.just-weather-cities.json)
 * JSON format: [{"city":"Stockholm","price":"SE3"}, ...]
 * ====================================================================== */
static const char* tui_cities_file_path(char* buf, size_t bufsz) {
    snprintf(buf, bufsz, "src/config/saved-cities.json");
    return buf;
}

static void tui_cities_load(TuiContext* ctx) {
    char path[512];
    if (!tui_cities_file_path(path, sizeof(path))) {
        return;
    }

    FILE* f = fopen(path, "r");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return;
    }

    char* buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';

    json_error_t err;
    json_t*      arr = json_loads(buf, 0, &err);
    free(buf);
    if (!arr || !json_is_array(arr)) {
        json_decref(arr);
        return;
    }

    size_t  i;
    json_t* obj;
    json_array_foreach(arr, i, obj) {
        if (ctx->saved_cities_count >= MAX_SAVED_CITIES) {
            break;
        }
        json_t* jcity  = json_object_get(obj, "city");
        json_t* jprice = json_object_get(obj, "price");
        if (!json_is_string(jcity) || !json_is_string(jprice)) {
            continue;
        }

        SavedCity* sc = &ctx->saved_cities[ctx->saved_cities_count];
        strncpy(sc->city, json_string_value(jcity), MAX_INPUT_LEN);
        strncpy(sc->price, json_string_value(jprice), MAX_INPUT_LEN);
        sc->city[MAX_INPUT_LEN]  = '\0';
        sc->price[MAX_INPUT_LEN] = '\0';
        ctx->saved_cities_count++;
    }
    json_decref(arr);
}

static void tui_cities_save(TuiContext* ctx) {
    char path[512];
    if (!tui_cities_file_path(path, sizeof(path))) {
        return;
    }
    mkdir("src/config", 0755); /* no-op if already exists */

    json_t* arr = json_array();
    if (!arr) {
        return;
    }
    for (int i = 0; i < ctx->saved_cities_count; i++) {
        json_t* obj = json_object();
        json_object_set_new(obj, "city",
                            json_string(ctx->saved_cities[i].city));
        json_object_set_new(obj, "price",
                            json_string(ctx->saved_cities[i].price));
        json_array_append_new(arr, obj);
    }
    char* out = json_dumps(arr, JSON_INDENT(2));
    json_decref(arr);
    if (!out) {
        return;
    }

    FILE* f = fopen(path, "w");
    if (f) {
        fputs(out, f);
        fclose(f);
    }
    free(out);
}

/* Normalize city: trim whitespace, then Title Case each word.
 * "  new york  " → "New York",  "STOCKHOLM" → "Stockholm" */
static void str_normalize_city(const char* src, char* dst, size_t dstsz) {
    /* skip leading whitespace */
    while (*src && isspace((unsigned char)*src)) {
        src++;
    }

    size_t di       = 0;
    int    new_word = 1;
    while (*src && di + 1 < dstsz) {
        unsigned char c = (unsigned char)*src++;
        if (isspace(c)) {
            /* collapse multiple spaces into one, but only if we already
             * wrote something */
            if (di > 0) {
                dst[di++] = ' ';
            }
            new_word = 1;
            /* skip remaining whitespace */
            while (*src && isspace((unsigned char)*src)) {
                src++;
            }
            continue;
        }
        dst[di++] = new_word ? (char)toupper(c) : (char)tolower(c);
        new_word  = 0;
    }
    /* strip trailing space that may have been added */
    while (di > 0 && dst[di - 1] == ' ') {
        di--;
    }
    dst[di] = '\0';
}

/* Normalize price: trim + uppercase.  "se3" → "SE3" */
static void str_normalize_price(const char* src, char* dst, size_t dstsz) {
    while (*src && isspace((unsigned char)*src)) {
        src++;
    }
    size_t di = 0;
    while (*src && di + 1 < dstsz) {
        unsigned char c = (unsigned char)*src++;
        if (isspace(c)) {
            break;
        }
        dst[di++] = (char)toupper(c);
    }
    dst[di] = '\0';
}

static int tui_cities_find(TuiContext* ctx, const char* city,
                           const char* price) {
    char nc[MAX_INPUT_LEN + 1], np[MAX_INPUT_LEN + 1];
    str_normalize_city(city, nc, sizeof(nc));
    str_normalize_price(price, np, sizeof(np));
    for (int i = 0; i < ctx->saved_cities_count; i++) {
        if (strcmp(ctx->saved_cities[i].city, nc) == 0 &&
            strcmp(ctx->saved_cities[i].price, np) == 0) {
            return i;
        }
    }
    return -1;
}

static void tui_cities_add(TuiContext* ctx, const char* city,
                           const char* price) {
    if (ctx->saved_cities_count >= MAX_SAVED_CITIES) {
        return;
    }
    SavedCity* sc = &ctx->saved_cities[ctx->saved_cities_count++];
    str_normalize_city(city, sc->city, sizeof(sc->city));
    str_normalize_price(price, sc->price, sizeof(sc->price));
}

static void tui_cities_remove(TuiContext* ctx, int idx) {
    if (idx < 0 || idx >= ctx->saved_cities_count) {
        return;
    }
    for (int i = idx; i < ctx->saved_cities_count - 1; i++) {
        ctx->saved_cities[i] = ctx->saved_cities[i + 1];
    }
    ctx->saved_cities_count--;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

TuiContext* tui_create(WeatherClient* client) {
    if (!client) {
        return NULL;
    }

    initscr();

    if (LINES < MIN_LINES || COLS < MIN_COLS) {
        endwin();
        fprintf(stderr, "Terminal too small (need %dx%d, got %dx%d)\n",
                MIN_COLS, MIN_LINES, COLS, LINES);
        return NULL;
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_NORMAL, COLOR_WHITE, -1);
        init_pair(CP_WORKING, COLOR_YELLOW, -1);
        init_pair(CP_ERROR, COLOR_RED, -1);
        init_pair(CP_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(CP_SELECTED, COLOR_BLACK, COLOR_WHITE);
        init_pair(CP_HELP, COLOR_CYAN, -1);
    }

    TuiContext* ctx = calloc(1, sizeof(TuiContext));
    if (!ctx) {
        endwin();
        return NULL;
    }

    ctx->client  = client;
    ctx->screen  = TUI_SCREEN_MAIN_MENU;
    ctx->running = 1;

    tui_cities_load(ctx);
    tui_create_windows(ctx);
    return ctx;
}

void tui_destroy(TuiContext* ctx) {
    if (!ctx) {
        return;
    }
    tui_free_result(ctx);
    tui_destroy_windows(ctx);
    endwin();
    free(ctx);
}

void tui_run(TuiContext* ctx) {
    if (!ctx) {
        return;
    }

    tui_draw_header(ctx);
    tui_set_status(ctx, "Ready", CP_NORMAL);

    while (ctx->running) {
        switch (ctx->screen) {
        case TUI_SCREEN_MAIN_MENU:
            tui_handle_main_menu(ctx);
            break;
        case TUI_SCREEN_CITY_MENU:
            tui_handle_city_menu(ctx);
            break;
        case TUI_SCREEN_CITY_SELECT:
            tui_handle_city_select(ctx);
            break;
        case TUI_SCREEN_CITY_SEARCH:
            tui_handle_city_search(ctx);
            break;
        case TUI_SCREEN_INPUT:
            tui_handle_input(ctx);
            break;
        case TUI_SCREEN_RESULT:
            tui_handle_result(ctx);
            break;
        case TUI_SCREEN_CONFIRM:
            tui_handle_confirm(ctx);
            break;
        case TUI_SCREEN_SAVE_CITY:
            tui_handle_save_city(ctx);
            break;
        }
    }
}

/* ======================================================================
 * Window management
 * ====================================================================== */
static void tui_create_windows(TuiContext* ctx) {
    ctx->header_win  = newwin(1, COLS, 0, 0);
    ctx->content_win = newwin(LINES - 3, COLS, 1, 0);
    ctx->help_win    = newwin(1, COLS, LINES - 2, 0);
    ctx->status_win  = newwin(1, COLS, LINES - 1, 0);
    keypad(ctx->content_win, TRUE);
}

static void tui_destroy_windows(TuiContext* ctx) {
    if (ctx->header_win) {
        delwin(ctx->header_win);
        ctx->header_win = NULL;
    }
    if (ctx->content_win) {
        delwin(ctx->content_win);
        ctx->content_win = NULL;
    }
    if (ctx->help_win) {
        delwin(ctx->help_win);
        ctx->help_win = NULL;
    }
    if (ctx->status_win) {
        delwin(ctx->status_win);
        ctx->status_win = NULL;
    }
}

static void tui_recreate_windows(TuiContext* ctx) {
    tui_destroy_windows(ctx);
    endwin();
    refresh();
    clear();
    tui_create_windows(ctx);
    tui_draw_header(ctx);
    tui_set_status(ctx, "Ready", CP_NORMAL);
}

static void tui_draw_header(TuiContext* ctx) {
    WINDOW* win = ctx->header_win;
    wbkgd(win, COLOR_PAIR(CP_HEADER));
    werase(win);
    wattron(win, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(win, 0, 1,
              " Just Weather Client  |  localhost:10680  |  [q] Quit ");
    wattroff(win, COLOR_PAIR(CP_HEADER) | A_BOLD);
    wrefresh(win);
}

static void tui_set_status(TuiContext* ctx, const char* msg, int cp) {
    WINDOW* win = ctx->status_win;
    werase(win);
    wattron(win, COLOR_PAIR(cp) | A_BOLD);
    mvwprintw(win, 0, 1, "%s", msg);
    wattroff(win, COLOR_PAIR(cp) | A_BOLD);
    wrefresh(win);
}

static void tui_set_help(TuiContext* ctx, const char* msg) {
    WINDOW* win = ctx->help_win;
    werase(win);
    wattron(win, COLOR_PAIR(CP_HELP));
    mvwprintw(win, 0, 1, "%s", msg);
    wattroff(win, COLOR_PAIR(CP_HELP));
    wrefresh(win);
}

static void tui_free_result(TuiContext* ctx) {
    if (ctx->result_lines) {
        for (int i = 0; i < ctx->result_nlines; i++) {
            free(ctx->result_lines[i]);
        }
        free(ctx->result_lines);
        ctx->result_lines  = NULL;
        ctx->result_nlines = 0;
        ctx->result_scroll = 0;
    }
}

/* ======================================================================
 * Generic menu drawing helper
 * ====================================================================== */
static void draw_menu(TuiContext* ctx, const char* title, const char** items,
                      int count, int selected) {
    WINDOW* win = ctx->content_win;
    werase(win);
    box(win, 0, 0);

    int height = getmaxy(win);
    int width  = getmaxx(win);

    int title_x = (width - (int)strlen(title)) / 2;
    if (title_x < 1) {
        title_x = 1;
    }
    mvwprintw(win, 0, title_x, "%s", title);

    int start_y = (height - count) / 2;
    if (start_y < 2) {
        start_y = 2;
    }

    for (int i = 0; i < count; i++) {
        int y = start_y + i;
        if (y >= height - 1) {
            break;
        }

        char label[80];
        snprintf(label, sizeof(label), "  %s  ", items[i]);
        int x = (width - (int)strlen(label)) / 2;
        if (x < 2) {
            x = 2;
        }

        if (i == selected) {
            wattron(win, COLOR_PAIR(CP_SELECTED) | A_BOLD);
            mvwprintw(win, y, x, "%s", label);
            wattroff(win, COLOR_PAIR(CP_SELECTED) | A_BOLD);
        } else {
            mvwprintw(win, y, x, "%s", label);
        }
    }

    wrefresh(win);
}

/* ======================================================================
 * Main menu
 * ====================================================================== */
static void tui_draw_main_menu(TuiContext* ctx) {
    draw_menu(ctx, " Main Menu ", g_menu_items, MENU_COUNT, ctx->menu_selected);
}

static void tui_activate_menu_item(TuiContext* ctx) {
    memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
    ctx->input_field = 0;

    switch (ctx->menu_selected) {
    case 0:
        ctx->pending_cmd   = TUI_CMD_CURRENT;
        ctx->input_nfields = 2;
        ctx->screen        = TUI_SCREEN_INPUT;
        break;
    case 1:
        ctx->pending_cmd   = TUI_CMD_WEATHER;
        ctx->input_nfields = 3;
        ctx->screen        = TUI_SCREEN_INPUT;
        break;
    case 2:
        ctx->pending_cmd   = TUI_CMD_CITIES;
        ctx->input_nfields = 1;
        ctx->screen        = TUI_SCREEN_INPUT;
        break;
    case 3:
        ctx->pending_cmd   = TUI_CMD_PLAN;
        ctx->input_nfields = 2;
        if (ctx->saved_cities_count > 0) {
            ctx->city_menu_selected = 0;
            ctx->screen             = TUI_SCREEN_CITY_MENU;
        } else {
            ctx->screen = TUI_SCREEN_INPUT;
        }
        break;
    case 4:
        ctx->screen = TUI_SCREEN_CONFIRM;
        break;
    case 5: {
        /* Server Settings — pre-fill with current host:port */
        ctx->pending_cmd   = TUI_CMD_SERVER;
        ctx->input_nfields = 2;
        snprintf(ctx->input_bufs[0], MAX_INPUT_LEN + 1, "%s",
                 weather_client_get_host(ctx->client));
        snprintf(ctx->input_bufs[1], MAX_INPUT_LEN + 1, "%d",
                 weather_client_get_port(ctx->client));
        ctx->input_field = 0;
        ctx->screen      = TUI_SCREEN_INPUT;
        break;
    }
    case 6:
        ctx->running = 0;
        break;
    default:
        break;
    }
}

static void tui_handle_main_menu(TuiContext* ctx) {
    tui_draw_main_menu(ctx);
    tui_set_help(
        ctx, "[Up/Down] Navigate  [Enter] Select  [1-7] Shortcut  [q] Quit");

    int ch = wgetch(ctx->content_win);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    switch (ch) {
    case KEY_UP:
        if (ctx->menu_selected > 0) {
            ctx->menu_selected--;
        }
        break;
    case KEY_DOWN:
        if (ctx->menu_selected < MENU_COUNT - 1) {
            ctx->menu_selected++;
        }
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        tui_activate_menu_item(ctx);
        break;
    case 'q':
    case 27: /* ESC */
        ctx->running = 0;
        break;
    default:
        if (ch >= '1' && ch <= '0' + MENU_COUNT) {
            ctx->menu_selected = ch - '1';
            tui_activate_menu_item(ctx);
        }
        break;
    }
}

/* ======================================================================
 * City menu (Load / Search / Back)
 * ====================================================================== */
static void tui_draw_city_menu(TuiContext* ctx) {
    draw_menu(ctx, " Energy Plan - Quick Search ", g_city_menu_items,
              CITY_MENU_COUNT, ctx->city_menu_selected);
}

static void tui_handle_city_menu(TuiContext* ctx) {
    tui_draw_city_menu(ctx);
    tui_set_help(ctx, "[Up/Down] Navigate  [Enter] Select  [q/ESC] Back");

    int ch = wgetch(ctx->content_win);
    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    switch (ch) {
    case KEY_UP:
        if (ctx->city_menu_selected > 0) {
            ctx->city_menu_selected--;
        }
        break;
    case KEY_DOWN:
        if (ctx->city_menu_selected < CITY_MENU_COUNT - 1) {
            ctx->city_menu_selected++;
        }
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (ctx->city_menu_selected == 0) { /* Load */
            ctx->city_select_idx = 0;
            ctx->screen          = TUI_SCREEN_CITY_SELECT;
        } else if (ctx->city_menu_selected ==
                   1) { /* Search — manual input form */
            memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
            ctx->input_field = 0;
            ctx->screen      = TUI_SCREEN_INPUT;
        } else { /* Back */
            tui_set_status(ctx, "Ready", CP_NORMAL);
            ctx->screen = TUI_SCREEN_MAIN_MENU;
        }
        break;
    case 'q':
    case 27: /* ESC */
        tui_set_status(ctx, "Ready", CP_NORMAL);
        ctx->screen = TUI_SCREEN_MAIN_MENU;
        break;
    default:
        break;
    }
}

/* ======================================================================
 * City search screen
 * ====================================================================== */
static int str_icontains(const char* hay, const char* needle) {
    if (!needle || !needle[0]) {
        return 1;
    }
    char   h[MAX_INPUT_LEN + 1], n[MAX_INPUT_LEN + 1];
    size_t i;
    for (i = 0; hay[i] && i < MAX_INPUT_LEN; i++) {
        h[i] = (char)tolower((unsigned char)hay[i]);
    }
    h[i] = '\0';
    for (i = 0; needle[i] && i < MAX_INPUT_LEN; i++) {
        n[i] = (char)tolower((unsigned char)needle[i]);
    }
    n[i] = '\0';
    return strstr(h, n) != NULL;
}

static int tui_city_filter(TuiContext* ctx, int* out, int max) {
    int n = 0;
    for (int i = 0; i < ctx->saved_cities_count && n < max; i++) {
        if (str_icontains(ctx->saved_cities[i].city, ctx->search_buf) ||
            str_icontains(ctx->saved_cities[i].price, ctx->search_buf)) {
            out[n++] = i;
        }
    }
    return n;
}

static void tui_draw_city_search(TuiContext* ctx) {
    WINDOW* win     = ctx->content_win;
    int     height  = getmaxy(win);
    int     width   = getmaxx(win);
    int     inner_w = width - 4;
    int     inner_h = height - 2;

    werase(win);
    box(win, 0, 0);

    const char* title = " Search Saved Cities ";
    int         tx    = (width - (int)strlen(title)) / 2;
    if (tx < 1) {
        tx = 1;
    }
    mvwprintw(win, 0, tx, "%s", title);

    /* Search input row */
    mvwprintw(win, 2, 2, "Search: [%-*.*s]", inner_w - 10, inner_w - 10,
              ctx->search_buf);

    /* Filtered results */
    int matches[MAX_SAVED_CITIES];
    int nmatches = tui_city_filter(ctx, matches, MAX_SAVED_CITIES);
    int list_h   = inner_h - 4; /* rows available below input */
    if (list_h < 1) {
        list_h = 1;
    }

    int scroll = ctx->city_search_idx - list_h / 2;
    if (scroll < 0) {
        scroll = 0;
    }
    if (scroll > nmatches - list_h) {
        scroll = nmatches - list_h;
    }
    if (scroll < 0) {
        scroll = 0;
    }

    for (int r = 0; r < list_h && (r + scroll) < nmatches; r++) {
        int  si  = matches[r + scroll];
        int  row = 4 + r;
        char label[MAX_INPUT_LEN * 2 + 8];
        snprintf(label, sizeof(label), "  %-*s  %s", MAX_INPUT_LEN,
                 ctx->saved_cities[si].city, ctx->saved_cities[si].price);
        int iw = width - 2;
        if (iw < 1) {
            iw = 1;
        }

        if ((r + scroll) == ctx->city_search_idx) {
            wattron(win, COLOR_PAIR(CP_SELECTED));
            mvwprintw(win, row, 1, "%-*.*s", iw, iw, label);
            wattroff(win, COLOR_PAIR(CP_SELECTED));
        } else {
            mvwprintw(win, row, 1, "%-*.*s", iw, iw, label);
        }
    }

    if (nmatches == 0) {
        const char* none = "No matches";
        mvwprintw(win, 4, (width - (int)strlen(none)) / 2, "%s", none);
    }

    wrefresh(win);
}

static void tui_handle_city_search(TuiContext* ctx) {
    tui_draw_city_search(ctx);
    tui_set_help(ctx, "[Type] Filter  [Up/Down] Navigate  [Enter] Select  [d] "
                      "Delete  [ESC] Back");

    /* Show cursor on the search field */
    curs_set(1);
    int ch = wgetch(ctx->content_win);
    curs_set(0);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    int matches[MAX_SAVED_CITIES];
    int nmatches = tui_city_filter(ctx, matches, MAX_SAVED_CITIES);

    switch (ch) {
    case KEY_UP:
        if (ctx->city_search_idx > 0) {
            ctx->city_search_idx--;
        }
        break;
    case KEY_DOWN:
        if (ctx->city_search_idx < nmatches - 1) {
            ctx->city_search_idx++;
        }
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (nmatches > 0) {
            int si = matches[ctx->city_search_idx];
            memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
            ctx->input_field = 0;
            strncpy(ctx->input_bufs[0], ctx->saved_cities[si].city,
                    MAX_INPUT_LEN);
            strncpy(ctx->input_bufs[1], ctx->saved_cities[si].price,
                    MAX_INPUT_LEN);
            ctx->input_bufs[0][MAX_INPUT_LEN] = '\0';
            ctx->input_bufs[1][MAX_INPUT_LEN] = '\0';
            ctx->screen                       = TUI_SCREEN_INPUT;
        }
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b': {
        int len = (int)strlen(ctx->search_buf);
        if (len > 0) {
            ctx->search_buf[len - 1] = '\0';
        }
        ctx->city_search_idx = 0;
        break;
    }
    case 'd':
    case 'D':
        if (nmatches > 0) {
            int si = matches[ctx->city_search_idx];
            tui_cities_remove(ctx, si);
            tui_cities_save(ctx);
            tui_set_status(ctx, "City removed", CP_NORMAL);
            /* Recompute and clamp index */
            nmatches = tui_city_filter(ctx, matches, MAX_SAVED_CITIES);
            if (ctx->city_search_idx >= nmatches && ctx->city_search_idx > 0) {
                ctx->city_search_idx--;
            }
            if (ctx->saved_cities_count == 0) {
                memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
                ctx->input_field = 0;
                ctx->screen      = TUI_SCREEN_INPUT;
            }
        }
        break;
    case 'q':
    case 27: /* ESC */
        ctx->screen = TUI_SCREEN_CITY_MENU;
        break;
    default:
        if (ch >= 32 && ch < 127) {
            int len = (int)strlen(ctx->search_buf);
            if (len < MAX_INPUT_LEN) {
                ctx->search_buf[len]     = (char)ch;
                ctx->search_buf[len + 1] = '\0';
            }
            ctx->city_search_idx = 0;
        }
        break;
    }
}

/* ======================================================================
 * City select screen
 * ====================================================================== */
static void tui_draw_city_select(TuiContext* ctx) {
    WINDOW* win     = ctx->content_win;
    int     height  = getmaxy(win);
    int     width   = getmaxx(win);
    int     inner_h = height - 2;

    werase(win);
    box(win, 0, 0);

    const char* title = " Quick Search - Select City ";
    int         tx    = (width - (int)strlen(title)) / 2;
    if (tx < 1) {
        tx = 1;
    }
    mvwprintw(win, 0, tx, "%s", title);

    int count = ctx->saved_cities_count;
    /* Simple scroll: keep selected row visible */
    int scroll = ctx->city_select_idx - inner_h / 2;
    if (scroll < 0) {
        scroll = 0;
    }
    if (scroll > count - inner_h) {
        scroll = count - inner_h;
    }
    if (scroll < 0) {
        scroll = 0;
    }

    for (int i = scroll; i < count && (i - scroll) < inner_h; i++) {
        int  row = i - scroll + 1;
        char label[MAX_INPUT_LEN * 2 + 8];
        snprintf(label, sizeof(label), "  %-*s  %s", MAX_INPUT_LEN,
                 ctx->saved_cities[i].city, ctx->saved_cities[i].price);
        /* Trim label to inner width */
        int iw = width - 2;
        if (iw < 1) {
            iw = 1;
        }

        if (i == ctx->city_select_idx) {
            wattron(win, COLOR_PAIR(CP_SELECTED));
            mvwprintw(win, row, 1, "%-*.*s", iw, iw, label);
            wattroff(win, COLOR_PAIR(CP_SELECTED));
        } else {
            mvwprintw(win, row, 1, "%-*.*s", iw, iw, label);
        }
    }

    wrefresh(win);
}

static void tui_handle_city_select(TuiContext* ctx) {
    tui_draw_city_select(ctx);
    tui_set_help(ctx, "[Up/Down] Navigate  [Enter] Select  [n] New  [d] Delete "
                      " [q/ESC] Back");

    int ch = wgetch(ctx->content_win);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    int count = ctx->saved_cities_count;

    switch (ch) {
    case KEY_UP:
        if (ctx->city_select_idx > 0) {
            ctx->city_select_idx--;
        }
        break;
    case KEY_DOWN:
        if (ctx->city_select_idx < count - 1) {
            ctx->city_select_idx++;
        }
        break;
    case '\n':
    case '\r':
    case KEY_ENTER: {
        /* Pre-fill input bufs from selected city and go to input form */
        int idx = ctx->city_select_idx;
        memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
        ctx->input_field = 0;
        strncpy(ctx->input_bufs[0], ctx->saved_cities[idx].city, MAX_INPUT_LEN);
        strncpy(ctx->input_bufs[1], ctx->saved_cities[idx].price,
                MAX_INPUT_LEN);
        ctx->input_bufs[0][MAX_INPUT_LEN] = '\0';
        ctx->input_bufs[1][MAX_INPUT_LEN] = '\0';
        ctx->screen                       = TUI_SCREEN_INPUT;
        break;
    }
    case 'n':
    case 'N':
        /* Enter a completely new city */
        memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
        ctx->input_field = 0;
        ctx->screen      = TUI_SCREEN_INPUT;
        break;
    case 'd':
    case 'D':
        if (count > 0) {
            tui_cities_remove(ctx, ctx->city_select_idx);
            tui_cities_save(ctx);
            if (ctx->city_select_idx >= ctx->saved_cities_count &&
                ctx->city_select_idx > 0) {
                ctx->city_select_idx--;
            }
            tui_set_status(ctx, "City removed", CP_NORMAL);
            /* If list is now empty, go straight to input */
            if (ctx->saved_cities_count == 0) {
                memset(ctx->input_bufs, 0, sizeof(ctx->input_bufs));
                ctx->input_field = 0;
                ctx->screen      = TUI_SCREEN_INPUT;
            }
        }
        break;
    case 'q':
    case 27: /* ESC */
        ctx->screen = TUI_SCREEN_CITY_MENU;
        break;
    default:
        break;
    }
}

/* ======================================================================
 * Input screen
 * ====================================================================== */
static const char* tui_input_title(TuiCommand cmd) {
    switch (cmd) {
    case TUI_CMD_CURRENT:
        return " Weather by Coordinates ";
    case TUI_CMD_WEATHER:
        return " Weather by City ";
    case TUI_CMD_CITIES:
        return " Search Cities ";
    case TUI_CMD_PLAN:
        return " Energy Plan ";
    case TUI_CMD_SERVER:
        return " Server Settings ";
    default:
        return " Input ";
    }
}

static const char* tui_input_label(TuiCommand cmd, int field) {
    switch (cmd) {
    case TUI_CMD_CURRENT:
        return field == 0 ? "Latitude:" : "Longitude:";
    case TUI_CMD_WEATHER:
        if (field == 0) {
            return "City:";
        }
        if (field == 1) {
            return "Country (optional, e.g. SE):";
        }
        return "Region (optional):";
    case TUI_CMD_CITIES:
        return "Search query (min 2 chars):";
    case TUI_CMD_PLAN:
        return field == 0 ? "City:" : "Price zone (SE1, SE2, SE3 or SE4):";
    case TUI_CMD_SERVER:
        return field == 0 ? "Host:" : "Port:";
    default:
        return "Value:";
    }
}

static void tui_draw_input_screen(TuiContext* ctx) {
    WINDOW* win    = ctx->content_win;
    int     height = getmaxy(win);
    int     width  = getmaxx(win);

    werase(win);
    box(win, 0, 0);

    const char* title   = tui_input_title(ctx->pending_cmd);
    int         title_x = (width - (int)strlen(title)) / 2;
    if (title_x < 1) {
        title_x = 1;
    }
    mvwprintw(win, 0, title_x, "%s", title);

    int field_w = width - 8;
    if (field_w < 20) {
        field_w = 20;
    }
    if (field_w > LINE_BUF_SIZE - 1) {
        field_w = LINE_BUF_SIZE - 1;
    }

    int total_h = ctx->input_nfields * 4;
    int start_y = (height - total_h) / 2;
    if (start_y < 2) {
        start_y = 2;
    }

    for (int i = 0; i < ctx->input_nfields; i++) {
        int y = start_y + i * 4;
        if (y + 2 >= height - 1) {
            break;
        }

        const char* label = tui_input_label(ctx->pending_cmd, i);
        mvwprintw(win, y, 3, "%s", label);

        /* Box border — bold for active field */
        if (i == ctx->input_field) {
            wattron(win, A_BOLD);
        }
        mvwprintw(win, y + 1, 3, "[");
        for (int j = 0; j < field_w; j++) {
            waddch(win, ' ');
        }
        mvwprintw(win, y + 1, 3 + field_w + 1, "]");

        /* Content */
        mvwprintw(win, y + 1, 4, "%.*s", field_w - 1, ctx->input_bufs[i]);

        /* Cursor indicator for active field */
        if (i == ctx->input_field) {
            int cur_x = 4 + (int)strlen(ctx->input_bufs[i]);
            if (cur_x < 3 + field_w) {
                mvwaddch(win, y + 1, cur_x, '_');
            }
            wattroff(win, A_BOLD);
        }
    }

    wrefresh(win);
}

static int tui_validate_input(TuiContext* ctx) {
    switch (ctx->pending_cmd) {
    case TUI_CMD_CURRENT: {
        char*  endp;
        double lat = strtod(ctx->input_bufs[0], &endp);
        if (endp == ctx->input_bufs[0] || *endp != '\0') {
            tui_set_status(ctx, "Error: invalid latitude value", CP_ERROR);
            return 0;
        }
        double lon = strtod(ctx->input_bufs[1], &endp);
        if (endp == ctx->input_bufs[1] || *endp != '\0') {
            tui_set_status(ctx, "Error: invalid longitude value", CP_ERROR);
            return 0;
        }
        if (!validate_latitude(lat)) {
            tui_set_status(ctx, "Error: latitude must be in [-90, +90]",
                           CP_ERROR);
            return 0;
        }
        if (!validate_longitude(lon)) {
            tui_set_status(ctx, "Error: longitude must be in [-180, +180]",
                           CP_ERROR);
            return 0;
        }
        break;
    }
    case TUI_CMD_WEATHER:
        if (!validate_city_name(ctx->input_bufs[0])) {
            tui_set_status(ctx, "Error: city name is required", CP_ERROR);
            return 0;
        }
        break;
    case TUI_CMD_CITIES:
        if (strlen(ctx->input_bufs[0]) < 2) {
            tui_set_status(ctx, "Error: query must be at least 2 characters",
                           CP_ERROR);
            return 0;
        }
        break;
    case TUI_CMD_PLAN:
        if (!validate_city_name(ctx->input_bufs[0])) {
            tui_set_status(ctx, "Error: city name is required", CP_ERROR);
            return 0;
        }
        {
            /* Normalize price in-place before validation */
            char norm[MAX_INPUT_LEN + 1];
            str_normalize_price(ctx->input_bufs[1], norm, sizeof(norm));
            strncpy(ctx->input_bufs[1], norm, MAX_INPUT_LEN);
            ctx->input_bufs[1][MAX_INPUT_LEN] = '\0';

            const char* z = ctx->input_bufs[1];
            if (strcmp(z, "SE1") != 0 && strcmp(z, "SE2") != 0 &&
                strcmp(z, "SE3") != 0 && strcmp(z, "SE4") != 0) {
                tui_set_status(ctx, "Error: zone must be SE1, SE2, SE3 or SE4",
                               CP_ERROR);
                return 0;
            }
        }
        break;
    case TUI_CMD_SERVER: {
        if (strlen(ctx->input_bufs[0]) == 0) {
            tui_set_status(ctx, "Error: host is required", CP_ERROR);
            return 0;
        }
        char* endp;
        long  port = strtol(ctx->input_bufs[1], &endp, 10);
        if (endp == ctx->input_bufs[1] || *endp != '\0' || port < 1 ||
            port > 65535) {
            tui_set_status(ctx, "Error: port must be 1-65535", CP_ERROR);
            return 0;
        }
        break;
    }
    default:
        break;
    }
    return 1;
}

static void tui_handle_input(TuiContext* ctx) {
    tui_draw_input_screen(ctx);
    tui_set_help(
        ctx, "[Tab/Down] Next field  [Up] Prev  [Enter] Submit  [ESC] Back");

    int ch = wgetch(ctx->content_win);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    switch (ch) {
    case 27: /* ESC — go back without submitting */
        ctx->screen = TUI_SCREEN_MAIN_MENU;
        tui_set_status(ctx, "Ready", CP_NORMAL);
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b': {
        int len = (int)strlen(ctx->input_bufs[ctx->input_field]);
        if (len > 0) {
            ctx->input_bufs[ctx->input_field][len - 1] = '\0';
        }
        tui_set_status(ctx, "Ready", CP_NORMAL);
        break;
    }
    case '\t':
    case KEY_DOWN:
        ctx->input_field = (ctx->input_field + 1) % ctx->input_nfields;
        tui_set_status(ctx, "Ready", CP_NORMAL);
        break;
    case KEY_UP:
        ctx->input_field =
            (ctx->input_field - 1 + ctx->input_nfields) % ctx->input_nfields;
        tui_set_status(ctx, "Ready", CP_NORMAL);
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (tui_validate_input(ctx)) {
            tui_dispatch_command(ctx);
        }
        break;
    default:
        if (ch >= 32 && ch <= 126) {
            int len = (int)strlen(ctx->input_bufs[ctx->input_field]);
            if (len < MAX_INPUT_LEN) {
                ctx->input_bufs[ctx->input_field][len]     = (char)ch;
                ctx->input_bufs[ctx->input_field][len + 1] = '\0';
            }
            tui_set_status(ctx, "Ready", CP_NORMAL);
        }
        break;
    }
}

/* ======================================================================
 * API dispatch
 * ====================================================================== */
static void tui_dispatch_command(TuiContext* ctx) {
    /* Server settings: no API call — update client and return to main menu */
    if (ctx->pending_cmd == TUI_CMD_SERVER) {
        int port = (int)strtol(ctx->input_bufs[1], NULL, 10);
        weather_client_set_server(ctx->client, ctx->input_bufs[0], port);
        char msg[MAX_INPUT_LEN + 32];
        snprintf(msg, sizeof(msg), "Server set to %s:%d", ctx->input_bufs[0],
                 port);
        tui_set_status(ctx, msg, CP_NORMAL);
        ctx->screen = TUI_SCREEN_MAIN_MENU;
        return;
    }

    tui_set_status(ctx, "Fetching...", CP_WORKING);
    wrefresh(ctx->status_win);

    json_t* result       = NULL;
    char*   error        = NULL;
    char**  lines        = NULL;
    int     nlines       = 0;
    int     plan_success = 0;

    switch (ctx->pending_cmd) {
    case TUI_CMD_CURRENT: {
        double lat = strtod(ctx->input_bufs[0], NULL);
        double lon = strtod(ctx->input_bufs[1], NULL);
        result     = weather_client_get_current(ctx->client, lat, lon, &error);
        if (result) {
            lines = tui_build_weather_lines(result, &nlines);
        }
        break;
    }
    case TUI_CMD_WEATHER: {
        const char* city = ctx->input_bufs[0];
        const char* country =
            strlen(ctx->input_bufs[1]) > 0 ? ctx->input_bufs[1] : NULL;
        const char* region =
            strlen(ctx->input_bufs[2]) > 0 ? ctx->input_bufs[2] : NULL;
        result = weather_client_get_weather_by_city(ctx->client, city, country,
                                                    region, &error);
        if (result) {
            lines = tui_build_weather_lines(result, &nlines);
        }
        break;
    }
    case TUI_CMD_CITIES:
        result = weather_client_search_cities(ctx->client, ctx->input_bufs[0],
                                              &error);
        if (result) {
            lines = tui_build_cities_lines(result, &nlines);
        }
        break;
    case TUI_CMD_PLAN:
        result = weather_client_get_plan(ctx->client, ctx->input_bufs[0],
                                         ctx->input_bufs[1], &error);
        if (result) {
            lines        = tui_build_plan_lines(result, &nlines);
            plan_success = 1;
        }
        break;
    default:
        break;
    }

    if (!result) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Error: %s",
                 error ? error : "Unknown error");
        lines = tui_build_error_lines(msg, &nlines);
        tui_set_status(ctx, msg, CP_ERROR);
    } else {
        if (!lines) {
            lines = tui_build_error_lines("Failed to format result", &nlines);
            tui_set_status(ctx, "Format error", CP_ERROR);
        } else {
            tui_set_status(ctx, "OK", CP_NORMAL);
        }
        json_decref(result);
    }
    free(error);

    /* Set result title based on command */
    switch (ctx->pending_cmd) {
    case TUI_CMD_CURRENT: /* fall-through */
    case TUI_CMD_WEATHER:
        snprintf(ctx->result_title, sizeof(ctx->result_title),
                 " Weather Result ");
        break;
    case TUI_CMD_CITIES:
        snprintf(ctx->result_title, sizeof(ctx->result_title), " City Search ");
        break;
    case TUI_CMD_PLAN:
        snprintf(ctx->result_title, sizeof(ctx->result_title), " Energy Plan ");
        break;
    default:
        snprintf(ctx->result_title, sizeof(ctx->result_title), " Result ");
        break;
    }

    tui_free_result(ctx);
    ctx->result_lines  = lines;
    ctx->result_nlines = nlines;
    ctx->result_scroll = 0;

    /* For a successful plan fetch, offer to save city (unless already saved) */
    if (plan_success &&
        tui_cities_find(ctx, ctx->input_bufs[0], ctx->input_bufs[1]) < 0) {
        ctx->screen = TUI_SCREEN_SAVE_CITY;
    } else {
        ctx->screen = TUI_SCREEN_RESULT;
    }
}

/* ======================================================================
 * Result screen
 * ====================================================================== */
static void tui_draw_result_screen(TuiContext* ctx) {
    WINDOW* win     = ctx->content_win;
    int     height  = getmaxy(win);
    int     width   = getmaxx(win);
    int     inner_h = height - 2;
    int     inner_w = width - 2;

    werase(win);
    box(win, 0, 0);

    if (ctx->result_title[0] != '\0') {
        int tx = (width - (int)strlen(ctx->result_title)) / 2;
        if (tx < 1) {
            tx = 1;
        }
        mvwprintw(win, 0, tx, "%s", ctx->result_title);
    }

    /* Scroll indicator */
    if (ctx->result_nlines > inner_h) {
        char ind[32];
        snprintf(ind, sizeof(ind), "[%d/%d] ", ctx->result_scroll + 1,
                 ctx->result_nlines);
        int ind_x = width - (int)strlen(ind) - 1;
        if (ind_x > 0) {
            mvwprintw(win, 0, ind_x, "%s", ind);
        }
    }

    int end = ctx->result_scroll + inner_h;
    if (end > ctx->result_nlines) {
        end = ctx->result_nlines;
    }

    for (int i = ctx->result_scroll; i < end; i++) {
        int y = i - ctx->result_scroll + 1;
        if (ctx->result_lines[i]) {
            mvwprintw(win, y, 1, "%.*s", inner_w, ctx->result_lines[i]);
        }
    }

    wrefresh(win);
}

static void tui_handle_result(TuiContext* ctx) {
    tui_draw_result_screen(ctx);
    tui_set_help(ctx, "[Up/Down] Scroll  [PgUp/PgDn] Page  [g] Bottom  [G] Top "
                      " [q/ESC] Back");

    int ch = wgetch(ctx->content_win);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    int inner_h    = getmaxy(ctx->content_win) - 2;
    int max_scroll = ctx->result_nlines - inner_h;
    if (max_scroll < 0) {
        max_scroll = 0;
    }

    switch (ch) {
    case KEY_UP:
        if (ctx->result_scroll > 0) {
            ctx->result_scroll--;
        }
        break;
    case KEY_DOWN:
        if (ctx->result_scroll < max_scroll) {
            ctx->result_scroll++;
        }
        break;
    case KEY_PPAGE:
        ctx->result_scroll -= inner_h;
        if (ctx->result_scroll < 0) {
            ctx->result_scroll = 0;
        }
        break;
    case KEY_NPAGE:
        ctx->result_scroll += inner_h;
        if (ctx->result_scroll > max_scroll) {
            ctx->result_scroll = max_scroll;
        }
        break;
    case 'g':
        ctx->result_scroll = max_scroll;
        break;
    case 'G':
        ctx->result_scroll = 0;
        break;
    case 'q':
    case 27: /* ESC */
        tui_free_result(ctx);
        ctx->screen = TUI_SCREEN_MAIN_MENU;
        tui_set_status(ctx, "Ready", CP_NORMAL);
        break;
    default:
        break;
    }
}

/* ======================================================================
 * Confirm dialog (clear cache)
 * ====================================================================== */
static void tui_draw_confirm_screen(TuiContext* ctx) {
    WINDOW* win    = ctx->content_win;
    int     height = getmaxy(win);
    int     width  = getmaxx(win);

    werase(win);
    box(win, 0, 0);

    const char* title   = " Confirm ";
    int         title_x = (width - (int)strlen(title)) / 2;
    if (title_x < 1) {
        title_x = 1;
    }
    mvwprintw(win, 0, title_x, "%s", title);

    int cy = height / 2 - 1;
    if (cy < 2) {
        cy = 2;
    }

    const char* msg   = "Clear all cached weather data?";
    int         msg_x = (width - (int)strlen(msg)) / 2;
    if (msg_x < 2) {
        msg_x = 2;
    }
    mvwprintw(win, cy, msg_x, "%s", msg);

    const char* hint   = "[Y] Yes, clear cache     [N / ESC] Cancel";
    int         hint_x = (width - (int)strlen(hint)) / 2;
    if (hint_x < 2) {
        hint_x = 2;
    }
    mvwprintw(win, cy + 2, hint_x, "%s", hint);

    wrefresh(win);
}

static void tui_handle_confirm(TuiContext* ctx) {
    tui_draw_confirm_screen(ctx);
    tui_set_help(ctx, "[Y] Confirm clear cache  [N/ESC] Cancel");

    int ch = wgetch(ctx->content_win);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    switch (ch) {
    case 'y':
    case 'Y':
        weather_client_clear_cache(ctx->client);
        tui_set_status(ctx, "Cache cleared successfully", CP_NORMAL);
        ctx->screen = TUI_SCREEN_MAIN_MENU;
        break;
    default:
        tui_set_status(ctx, "Ready", CP_NORMAL);
        ctx->screen = TUI_SCREEN_MAIN_MENU;
        break;
    }
}

/* ======================================================================
 * Save city popup
 * ====================================================================== */
static void tui_draw_save_city_screen(TuiContext* ctx) {
    WINDOW* win    = ctx->content_win;
    int     height = getmaxy(win);
    int     width  = getmaxx(win);

    werase(win);
    box(win, 0, 0);

    const char* title   = " Save for Quick Search ";
    int         title_x = (width - (int)strlen(title)) / 2;
    if (title_x < 1) {
        title_x = 1;
    }
    mvwprintw(win, 0, title_x, "%s", title);

    int cy = height / 2 - 2;
    if (cy < 2) {
        cy = 2;
    }

    /* city up to 127 chars + price up to 127 chars + fixed text ~30 */
    char line1[MAX_INPUT_LEN * 2 + 48];
    snprintf(line1, sizeof(line1), "Save '%s (%s)' for quick search?",
             ctx->input_bufs[0], ctx->input_bufs[1]);
    int x1 = (width - (int)strlen(line1)) / 2;
    if (x1 < 2) {
        x1 = 2;
    }
    mvwprintw(win, cy, x1, "%s", line1);

    {
        char line2[64];
        snprintf(line2, sizeof(line2), "(%d cities already saved)",
                 ctx->saved_cities_count);
        int x2 = (width - (int)strlen(line2)) / 2;
        if (x2 < 2) {
            x2 = 2;
        }
        mvwprintw(win, cy + 1, x2, "%s", line2);
    }

    const char* hint   = "[Y] Save     [N / ESC] Skip";
    int         hint_x = (width - (int)strlen(hint)) / 2;
    if (hint_x < 2) {
        hint_x = 2;
    }
    mvwprintw(win, cy + 3, hint_x, "%s", hint);

    wrefresh(win);
}

static void tui_handle_save_city(TuiContext* ctx) {
    tui_draw_save_city_screen(ctx);
    tui_set_help(ctx, "[Y] Save city  [N/ESC] Skip");

    int ch = wgetch(ctx->content_win);

    if (ch == KEY_RESIZE) {
        tui_recreate_windows(ctx);
        return;
    }

    switch (ch) {
    case 'y':
    case 'Y':
        tui_cities_add(ctx, ctx->input_bufs[0], ctx->input_bufs[1]);
        tui_cities_save(ctx);
        tui_set_status(ctx, "City saved for quick search", CP_NORMAL);
        ctx->screen = TUI_SCREEN_RESULT;
        break;
    default:
        ctx->screen = TUI_SCREEN_RESULT;
        break;
    }
}

/* ======================================================================
 * Line buffer helper
 * ====================================================================== */
static int lb_append(char*** lines, int* nlines, int* cap, const char* text) {
    if (*nlines >= *cap) {
        int    new_cap   = (*cap == 0) ? 32 : *cap * 2;
        char** new_lines = realloc(*lines, (size_t)new_cap * sizeof(char*));
        if (!new_lines) {
            return 0;
        }
        *lines = new_lines;
        *cap   = new_cap;
    }
    (*lines)[*nlines] = strdup(text ? text : "");
    if (!(*lines)[*nlines]) {
        return 0;
    }
    (*nlines)++;
    return 1;
}

/* ======================================================================
 * Error / JSON fallback line builders
 * ====================================================================== */
static char** tui_build_error_lines(const char* msg, int* out_nlines) {
    char** lines  = NULL;
    int    nlines = 0, cap = 0;

    lb_append(&lines, &nlines, &cap, "");
    lb_append(&lines, &nlines, &cap, msg ? msg : "Unknown error");
    lb_append(&lines, &nlines, &cap, "");
    lb_append(&lines, &nlines, &cap, "Press q or ESC to go back.");

    *out_nlines = nlines;
    return lines;
}

static char** tui_build_json_lines(json_t* data, int* out_nlines) {
    char** lines  = NULL;
    int    nlines = 0, cap = 0;

    char* json_str = json_dumps(data, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    if (!json_str) {
        lb_append(&lines, &nlines, &cap, "(empty response)");
        *out_nlines = nlines;
        return lines;
    }

    char* dup = strdup(json_str);
    free(json_str);
    if (!dup) {
        *out_nlines = 0;
        return NULL;
    }

    char* p          = dup;
    char* line_start = p;
    while (*p) {
        if (*p == '\n') {
            *p = '\0';
            lb_append(&lines, &nlines, &cap, line_start);
            line_start = p + 1;
        }
        p++;
    }
    if (*line_start) {
        lb_append(&lines, &nlines, &cap, line_start);
    }
    free(dup);

    *out_nlines = nlines;
    return lines;
}

/* ======================================================================
 * Weather line builder
 * ====================================================================== */
static char** tui_build_weather_lines(json_t* data, int* out_nlines) {
    char** lines  = NULL;
    int    nlines = 0, cap = 0;
    char   buf[LINE_BUF_SIZE];

    json_t* d = json_object_get(data, "data");
    if (!d || !json_is_object(d)) {
        return tui_build_json_lines(data, out_nlines);
    }

    json_t* loc = json_object_get(d, "location");
    json_t* cw  = json_object_get(d, "current_weather");
    if (!cw) {
        return tui_build_json_lines(data, out_nlines);
    }

    lb_append(&lines, &nlines, &cap, "");

    if (loc) {
        json_t* name_j    = json_object_get(loc, "name");
        json_t* country_j = json_object_get(loc, "country");
        json_t* cc_j      = json_object_get(loc, "country_code");
        json_t* region_j  = json_object_get(loc, "region");
        json_t* lat_j     = json_object_get(loc, "latitude");
        json_t* lon_j     = json_object_get(loc, "longitude");
        json_t* pop_j     = json_object_get(loc, "population");
        json_t* tz_j      = json_object_get(loc, "timezone");

        const char* name    = (name_j && json_is_string(name_j))
                                  ? json_string_value(name_j)
                                  : "?";
        const char* country = (country_j && json_is_string(country_j))
                                  ? json_string_value(country_j)
                                  : "";
        const char* cc =
            (cc_j && json_is_string(cc_j)) ? json_string_value(cc_j) : "";
        const char* region = (region_j && json_is_string(region_j))
                                 ? json_string_value(region_j)
                                 : "";

        snprintf(buf, sizeof(buf), "Location: %s, %s (%s)  Region: %s", name,
                 country, cc, region);
        lb_append(&lines, &nlines, &cap, buf);

        if (lat_j && lon_j) {
            snprintf(buf, sizeof(buf), "Lat: %.4f  Lon: %.4f",
                     json_real_value(lat_j), json_real_value(lon_j));
            if (pop_j && json_integer_value(pop_j) > 0) {
                size_t blen = strlen(buf);
                snprintf(buf + blen, sizeof(buf) - blen, "  Population: %lld",
                         (long long)json_integer_value(pop_j));
            }
            if (tz_j && json_is_string(tz_j)) {
                size_t blen = strlen(buf);
                snprintf(buf + blen, sizeof(buf) - blen, "  Timezone: %s",
                         json_string_value(tz_j));
            }
            lb_append(&lines, &nlines, &cap, buf);
        }
    }

    lb_append(&lines, &nlines, &cap, "");

    json_t* time_j = json_object_get(cw, "time");
    if (time_j && json_is_string(time_j)) {
        snprintf(buf, sizeof(buf), "Time: %s", json_string_value(time_j));
        lb_append(&lines, &nlines, &cap, buf);
    }

    lb_append(&lines, &nlines, &cap, "");
    lb_append(&lines, &nlines, &cap, "---------- Current Weather ----------");
    lb_append(&lines, &nlines, &cap, "");

    json_t* temp_j     = json_object_get(cw, "temperature");
    json_t* temp_u_j   = json_object_get(cw, "temperature_unit");
    json_t* humid_j    = json_object_get(cw, "humidity");
    json_t* press_j    = json_object_get(cw, "pressure");
    json_t* wspeed_j   = json_object_get(cw, "windspeed");
    json_t* wspeed_u_j = json_object_get(cw, "windspeed_unit");
    json_t* wdir_j     = json_object_get(cw, "wind_direction_name");
    json_t* precip_j   = json_object_get(cw, "precipitation");
    json_t* precip_u_j = json_object_get(cw, "precipitation_unit");
    json_t* desc_j     = json_object_get(cw, "weather_description");

    const char* temp_u   = (temp_u_j && json_is_string(temp_u_j))
                               ? json_string_value(temp_u_j)
                               : "?";
    const char* speed_u  = (wspeed_u_j && json_is_string(wspeed_u_j))
                               ? json_string_value(wspeed_u_j)
                               : "?";
    const char* precip_u = (precip_u_j && json_is_string(precip_u_j))
                               ? json_string_value(precip_u_j)
                               : "?";

    snprintf(buf, sizeof(buf), "Temperature:    %.1f %s",
             temp_j ? json_real_value(temp_j) : 0.0, temp_u);
    lb_append(&lines, &nlines, &cap, buf);

    snprintf(buf, sizeof(buf), "Humidity:       %.0f%%",
             humid_j ? json_real_value(humid_j) : 0.0);
    lb_append(&lines, &nlines, &cap, buf);

    snprintf(buf, sizeof(buf), "Pressure:       %.0f hPa",
             press_j ? json_real_value(press_j) : 0.0);
    lb_append(&lines, &nlines, &cap, buf);

    snprintf(buf, sizeof(buf), "Wind Speed:     %.1f %s",
             wspeed_j ? json_real_value(wspeed_j) : 0.0, speed_u);
    lb_append(&lines, &nlines, &cap, buf);

    snprintf(buf, sizeof(buf), "Wind Direction: %s",
             (wdir_j && json_is_string(wdir_j)) ? json_string_value(wdir_j)
                                                : "-");
    lb_append(&lines, &nlines, &cap, buf);

    snprintf(buf, sizeof(buf), "Precipitation:  %.1f %s",
             precip_j ? json_real_value(precip_j) : 0.0, precip_u);
    lb_append(&lines, &nlines, &cap, buf);

    snprintf(buf, sizeof(buf), "Condition:      %s",
             (desc_j && json_is_string(desc_j)) ? json_string_value(desc_j)
                                                : "-");
    lb_append(&lines, &nlines, &cap, buf);

    lb_append(&lines, &nlines, &cap, "");

    *out_nlines = nlines;
    return lines;
}

/* ======================================================================
 * Cities line builder
 * ====================================================================== */
static char** tui_build_cities_lines(json_t* data, int* out_nlines) {
    char** lines  = NULL;
    int    nlines = 0, cap = 0;
    char   buf[LINE_BUF_SIZE];

    json_t* d = json_object_get(data, "data");
    if (!d || !json_is_object(d)) {
        return tui_build_json_lines(data, out_nlines);
    }

    json_t* query_j  = json_object_get(d, "query");
    json_t* count_j  = json_object_get(d, "count");
    json_t* cities_j = json_object_get(d, "cities");
    if (!cities_j || !json_is_array(cities_j)) {
        return tui_build_json_lines(data, out_nlines);
    }

    lb_append(&lines, &nlines, &cap, "");
    snprintf(buf, sizeof(buf), "Search: \"%s\"  Results: %lld",
             (query_j && json_is_string(query_j)) ? json_string_value(query_j)
                                                  : "",
             count_j ? (long long)json_integer_value(count_j)
                     : (long long)json_array_size(cities_j));
    lb_append(&lines, &nlines, &cap, buf);
    lb_append(&lines, &nlines, &cap, "");

    lb_append(&lines, &nlines, &cap,
              " #  | City                 | Country              | CC | Region "
              "              |    Lat |    Lon | Population");
    lb_append(&lines, &nlines, &cap,
              "----+----------------------+----------------------+----+--------"
              "--------------+--------+--------+-----------");

    size_t n = json_array_size(cities_j);
    for (size_t i = 0; i < n; i++) {
        json_t* c      = json_array_get(cities_j, i);
        json_t* name_j = json_object_get(c, "name");
        json_t* ctr_j  = json_object_get(c, "country");
        json_t* cc_j   = json_object_get(c, "country_code");
        json_t* reg_j  = json_object_get(c, "region");
        json_t* lat_j  = json_object_get(c, "latitude");
        json_t* lon_j  = json_object_get(c, "longitude");
        json_t* pop_j  = json_object_get(c, "population");

        snprintf(
            buf, sizeof(buf),
            "%3zu | %-20s | %-20s | %-2s | %-20s | %6.2f | %6.2f | %lld", i + 1,
            (name_j && json_is_string(name_j)) ? json_string_value(name_j)
                                               : "-",
            (ctr_j && json_is_string(ctr_j)) ? json_string_value(ctr_j) : "-",
            (cc_j && json_is_string(cc_j)) ? json_string_value(cc_j) : "-",
            (reg_j && json_is_string(reg_j)) ? json_string_value(reg_j) : "-",
            lat_j ? json_real_value(lat_j) : 0.0,
            lon_j ? json_real_value(lon_j) : 0.0,
            pop_j ? (long long)json_integer_value(pop_j) : 0LL);
        lb_append(&lines, &nlines, &cap, buf);
    }

    lb_append(&lines, &nlines, &cap, "");
    *out_nlines = nlines;
    return lines;
}

/* ======================================================================
 * Energy plan line builder — helpers
 * ====================================================================== */

/* Parse start time string ("HH:MM" or ISO "...THH:MM:SS...") into minutes */
static int plan_parse_start_minutes(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    const char* t = strchr(s, 'T');
    int         h = 0, m = 0;
    if (t) {
        sscanf(t + 1, "%d:%d", &h, &m);
    } else {
        sscanf(s, "%d:%d", &h, &m);
    }
    return h * 60 + m;
}

/* Fill dst with bar_len '#' chars, pad to max_len with spaces */
static void plan_fill_bar(char* dst, int bar_len, int max_len) {
    int i;
    for (i = 0; i < bar_len && i < max_len; i++) {
        dst[i] = '#';
    }
    for (; i < max_len; i++) {
        dst[i] = ' ';
    }
    dst[max_len] = '\0';
}

/* ======================================================================
 * Energy plan line builder
 * ====================================================================== */
static char** tui_build_plan_lines(json_t* data, int* out_nlines) {
    char** lines  = NULL;
    int    nlines = 0, cap = 0;
    char   buf[LINE_BUF_SIZE];

    json_t* city_j      = json_object_get(data, "city");
    json_t* zone_j      = json_object_get(data, "price_zone");
    json_t* lat_j       = json_object_get(data, "latitude");
    json_t* lon_j       = json_object_get(data, "longitude");
    json_t* start_j     = json_object_get(data, "start_time");
    json_t* slots_j     = json_object_get(data, "slots_total");
    json_t* summary_j   = json_object_get(data, "summary");
    json_t* decisions_j = json_object_get(data, "decisions");

    if (!city_j || !zone_j || !summary_j || !decisions_j) {
        return tui_build_json_lines(data, out_nlines);
    }

    /* ---- header ---- */
    lb_append(&lines, &nlines, &cap, "");
    snprintf(buf, sizeof(buf), "City: %-20s  Zone: %s",
             json_string_value(city_j), json_string_value(zone_j));
    lb_append(&lines, &nlines, &cap, buf);

    if (lat_j && lon_j) {
        snprintf(buf, sizeof(buf), "Lat: %.4f  Lon: %.4f",
                 json_real_value(lat_j), json_real_value(lon_j));
        lb_append(&lines, &nlines, &cap, buf);
    }

    int start_min = 0;
    if (start_j && json_is_string(start_j)) {
        start_min = plan_parse_start_minutes(json_string_value(start_j));
        snprintf(buf, sizeof(buf), "Start: %s", json_string_value(start_j));
        if (slots_j) {
            size_t blen = strlen(buf);
            snprintf(buf + blen, sizeof(buf) - blen, "    Slots: %lld",
                     (long long)json_integer_value(slots_j));
        }
        lb_append(&lines, &nlines, &cap, buf);
    }

    /* ---- pre-compute min/max price and collect buy-slots ---- */
    size_t n     = json_array_size(decisions_j);
    double min_p = 1e9, max_p = -1e9;

    /* buy_slots[i] = slot index where buy_electricity > 0, sorted by price */
    int    buy_idx[96];
    double buy_price[96];
    int    buy_count = 0;

    for (size_t i = 0; i < n; i++) {
        json_t* slot = json_array_get(decisions_j, i);
        json_t* inv  = json_object_get(slot, "input_variables");
        json_t* out  = json_object_get(slot, "output");
        if (!inv || !out) {
            continue;
        }
        double p = json_real_value(json_object_get(inv, "elpris"));
        if (p < min_p) {
            min_p = p;
        }
        if (p > max_p) {
            max_p = p;
        }
        if (json_real_value(json_object_get(out, "buy_electricity")) > 0.0 &&
            buy_count < 96) {
            /* insertion sort by price */
            int pos = buy_count;
            while (pos > 0 && (buy_price[pos - 1] > p + 5e-4 ||
                               (buy_price[pos - 1] <= p + 5e-4 &&
                                buy_price[pos - 1] >= p - 5e-4 &&
                                buy_idx[pos - 1] > (int)i))) {
                buy_price[pos] = buy_price[pos - 1];
                buy_idx[pos]   = buy_idx[pos - 1];
                pos--;
            }
            buy_price[pos] = p;
            buy_idx[pos]   = (int)i;
            buy_count++;
        }
    }
    if (max_p <= min_p) {
        max_p = min_p + 1.0; /* avoid division by zero */
    }

#define BAR_W 20

    /* ---- Best Buy Windows ---- */
    lb_append(&lines, &nlines, &cap, "");
    lb_append(&lines, &nlines, &cap, "=== Best Buy Windows ===");
    if (buy_count == 0) {
        lb_append(&lines, &nlines, &cap,
                  "  (no buy recommendations in this plan)");
    } else {
        lb_append(&lines, &nlines, &cap,
                  "  # | Time  | Price SEK | Bar (cheap->exp)     | Rec");
        lb_append(&lines, &nlines, &cap,
                  "----+-------+-----------+----------------------+----");
        int show = buy_count < 10 ? buy_count : 10;
        for (int k = 0; k < show; k++) {
            int    si      = buy_idx[k];
            double p       = buy_price[k];
            int    tot     = start_min + si * 15;
            int    hh      = (tot / 60) % 24;
            int    mm      = tot % 60;
            int    bar_len = (int)((p - min_p) / (max_p - min_p) * BAR_W);
            char   bar[BAR_W + 1];
            plan_fill_bar(bar, bar_len, BAR_W);
            snprintf(buf, sizeof(buf), " %2d | %02d:%02d | %9.3f | %s | BUY",
                     k + 1, hh, mm, p, bar);
            lb_append(&lines, &nlines, &cap, buf);
        }
    }

    /* ---- Peak Hours (most expensive slots) ---- */
    {
        /* insertion sort descending by price, tiebreak by earlier slot */
        int    peak_idx[96];
        double peak_price[96];
        int    peak_count = 0;

        for (size_t i = 0; i < n; i++) {
            json_t* slot = json_array_get(decisions_j, i);
            json_t* inv  = json_object_get(slot, "input_variables");
            if (!inv) {
                continue;
            }
            double p   = json_real_value(json_object_get(inv, "elpris"));
            int    pos = peak_count;
            while (pos > 0 && (peak_price[pos - 1] < p - 5e-4 ||
                               (peak_price[pos - 1] <= p + 5e-4 &&
                                peak_price[pos - 1] >= p - 5e-4 &&
                                peak_idx[pos - 1] > (int)i))) {
                peak_price[pos] = peak_price[pos - 1];
                peak_idx[pos]   = peak_idx[pos - 1];
                pos--;
            }
            peak_price[pos] = p;
            peak_idx[pos]   = (int)i;
            if (peak_count < 96) {
                peak_count++;
            }
        }

        lb_append(&lines, &nlines, &cap, "");
        lb_append(&lines, &nlines, &cap,
                  "=== Peak Hours - Use Less Electricity ===");
        lb_append(&lines, &nlines, &cap,
                  "  # | Time  | Price SEK | Bar (cheap->exp)     | Rec");
        lb_append(&lines, &nlines, &cap,
                  "----+-------+-----------+----------------------+------");
        int show_peak = peak_count < 10 ? peak_count : 10;
        for (int k = 0; k < show_peak; k++) {
            int    si      = peak_idx[k];
            double p       = peak_price[k];
            int    tot     = start_min + si * 15;
            int    hh      = (tot / 60) % 24;
            int    mm      = tot % 60;
            int    bar_len = (int)((p - min_p) / (max_p - min_p) * BAR_W);
            char   bar[BAR_W + 1];
            plan_fill_bar(bar, bar_len, BAR_W);
            snprintf(buf, sizeof(buf), " %2d | %02d:%02d | %9.3f | %s | SAVE",
                     k + 1, hh, mm, p, bar);
            lb_append(&lines, &nlines, &cap, buf);
        }
    }

    /* ---- Summary ---- */
    lb_append(&lines, &nlines, &cap, "");
    lb_append(&lines, &nlines, &cap, "=== Summary ===");
    lb_append(&lines, &nlines, &cap,
              " Buy Elec (kWh)  | Direct Use (kWh) | Charge Bat (kWh) | Sell "
              "Excess (kWh)");
    lb_append(&lines, &nlines, &cap,
              "-----------------+------------------+------------------+--------"
              "----------");
    snprintf(buf, sizeof(buf), "%16.3f | %16.3f | %16.3f | %16.3f",
             json_real_value(json_object_get(summary_j, "buy_electricity")),
             json_real_value(json_object_get(summary_j, "direct_use")),
             json_real_value(json_object_get(summary_j, "charge_battery")),
             json_real_value(json_object_get(summary_j, "sell_excess")));
    lb_append(&lines, &nlines, &cap, buf);

    /* ---- Price Timeline ---- */
    lb_append(&lines, &nlines, &cap, "");
    lb_append(&lines, &nlines, &cap, "=== Price Timeline (SEK/kWh) ===");
    lb_append(&lines, &nlines, &cap,
              " Time  | Price  | Chart                | Act");
    lb_append(&lines, &nlines, &cap,
              "-------+--------+----------------------+----");

    for (size_t i = 0; i < n; i++) {
        json_t* slot = json_array_get(decisions_j, i);
        json_t* inv  = json_object_get(slot, "input_variables");
        json_t* out  = json_object_get(slot, "output");
        if (!inv || !out) {
            continue;
        }

        double p = json_real_value(json_object_get(inv, "elpris"));
        double buy_kw =
            json_real_value(json_object_get(out, "buy_electricity"));
        int  tot     = start_min + (int)i * 15;
        int  hh      = (tot / 60) % 24;
        int  mm      = tot % 60;
        int  bar_len = (int)((p - min_p) / (max_p - min_p) * BAR_W);
        char bar[BAR_W + 1];
        plan_fill_bar(bar, bar_len, BAR_W);
        const char* act = buy_kw > 0.0 ? " *  " : "    ";
        snprintf(buf, sizeof(buf), " %02d:%02d | %6.3f | %s |%s", hh, mm, p,
                 bar, act);
        lb_append(&lines, &nlines, &cap, buf);
    }

    /* ---- Decisions table (extended) ---- */
    lb_append(&lines, &nlines, &cap, "");
    snprintf(buf, sizeof(buf), "=== Decisions (%zu x 15 min) ===", n);
    lb_append(&lines, &nlines, &cap, buf);
    lb_append(&lines, &nlines, &cap,
              " Rec | Time  | Price(SEK) | Temp(C)  | Sun    | Buy     | "
              "Direct  | Charge  | Sell");
    lb_append(&lines, &nlines, &cap,
              "-----+-------+------------+----------+--------+---------+-------"
              "--+---------+---------");

    for (size_t i = 0; i < n; i++) {
        json_t* slot = json_array_get(decisions_j, i);
        json_t* inv  = json_object_get(slot, "input_variables");
        json_t* out  = json_object_get(slot, "output");
        if (!inv || !out) {
            continue;
        }

        double buy_kw =
            json_real_value(json_object_get(out, "buy_electricity"));
        int tot = start_min + (int)i * 15;
        int hh  = (tot / 60) % 24;
        int mm  = tot % 60;
        snprintf(buf, sizeof(buf),
                 "%s | %02d:%02d | %10.3f | %8.2f | %6.2f | %7.3f | %7.3f | "
                 "%7.3f | %7.3f",
                 buy_kw > 0.0 ? " BUY" : "SAVE", hh, mm,
                 json_real_value(json_object_get(inv, "elpris")),
                 json_real_value(json_object_get(inv, "temperature")),
                 json_real_value(json_object_get(inv, "sun_intensity")), buy_kw,
                 json_real_value(json_object_get(out, "direct_use")),
                 json_real_value(json_object_get(out, "charge_battery")),
                 json_real_value(json_object_get(out, "sell_excess")));
        lb_append(&lines, &nlines, &cap, buf);
    }

#undef BAR_W

    lb_append(&lines, &nlines, &cap, "");
    *out_nlines = nlines;
    return lines;
}
