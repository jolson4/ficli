#include "ui/dashboard_list.h"

#include "db/query.h"
#include "ui/colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DASHBOARD_TOP_EXPENSE_COUNT 3

typedef struct {
    char label[128];
    int64_t expense_cents;
} top_expense_row_t;

struct dashboard_list_state {
    sqlite3 *db;

    int64_t net_worth_cents;

    int64_t mtd_income_cents;
    int64_t mtd_expense_cents;
    int64_t mtd_net_cents;

    int64_t ytd_income_cents;
    int64_t ytd_expense_cents;
    int64_t ytd_net_cents;

    int64_t trailing_income_cents;
    int64_t trailing_expense_cents;
    int64_t trailing_net_cents;

    top_expense_row_t top_expense[DASHBOARD_TOP_EXPENSE_COUNT];
    int top_expense_count;

    char message[128];
    bool dirty;
};

static void format_cents(int64_t cents, bool show_plus, char *buf, int n) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;

    char raw[32];
    snprintf(raw, sizeof(raw), "%ld", (long)whole);
    int raw_len = (int)strlen(raw);

    char grouped[48];
    int gi = 0;
    for (int i = 0; i < raw_len; i++) {
        if (i > 0 && (raw_len - i) % 3 == 0)
            grouped[gi++] = ',';
        grouped[gi++] = raw[i];
    }
    grouped[gi] = '\0';

    if (cents < 0)
        snprintf(buf, n, "-%s.%02ld", grouped, (long)frac);
    else if (show_plus)
        snprintf(buf, n, "+%s.%02ld", grouped, (long)frac);
    else
        snprintf(buf, n, "%s.%02ld", grouped, (long)frac);
}

static int draw_clipped_text(WINDOW *win, int row, int col, int max_col,
                             const char *text) {
    if (!win || !text || col >= max_col)
        return col;
    int len = (int)strlen(text);
    int avail = max_col - col;
    if (avail <= 0)
        return col;
    if (len > avail)
        len = avail;
    mvwprintw(win, row, col, "%.*s", len, text);
    return col + len;
}

static int draw_clipped_colored_text(WINDOW *win, int row, int col, int max_col,
                                     const char *text, int color_pair) {
    if (col >= max_col)
        return col;
    wattron(win, COLOR_PAIR(color_pair));
    int next_col = draw_clipped_text(win, row, col, max_col, text);
    wattroff(win, COLOR_PAIR(color_pair));
    return next_col;
}

static void accumulate_flow_totals(const report_row_t *rows, int row_count,
                                   int64_t *out_income_cents,
                                   int64_t *out_expense_cents,
                                   int64_t *out_net_cents) {
    if (!out_income_cents || !out_expense_cents || !out_net_cents)
        return;
    *out_income_cents = 0;
    *out_expense_cents = 0;
    *out_net_cents = 0;
    if (!rows || row_count <= 0)
        return;

    for (int i = 0; i < row_count; i++) {
        *out_income_cents += rows[i].income_cents;
        *out_expense_cents += rows[i].expense_cents;
        *out_net_cents += rows[i].net_cents;
    }
}

static bool is_uncategorized_label(const char *label) {
    return label && strcmp(label, "Uncategorized") == 0;
}

static void consider_top_expense(dashboard_list_state_t *ls, const char *label,
                                 int64_t expense_cents) {
    if (!ls || !label || expense_cents <= 0)
        return;
    if (is_uncategorized_label(label))
        return;

    int insert_at = ls->top_expense_count;
    for (int i = 0; i < ls->top_expense_count; i++) {
        if (expense_cents > ls->top_expense[i].expense_cents) {
            insert_at = i;
            break;
        }
    }

    if (insert_at >= DASHBOARD_TOP_EXPENSE_COUNT)
        return;

    int limit = ls->top_expense_count;
    if (limit >= DASHBOARD_TOP_EXPENSE_COUNT)
        limit = DASHBOARD_TOP_EXPENSE_COUNT - 1;

    for (int i = limit; i > insert_at; i--)
        ls->top_expense[i] = ls->top_expense[i - 1];

    snprintf(ls->top_expense[insert_at].label,
             sizeof(ls->top_expense[insert_at].label), "%s", label);
    ls->top_expense[insert_at].expense_cents = expense_cents;

    if (ls->top_expense_count < DASHBOARD_TOP_EXPENSE_COUNT)
        ls->top_expense_count++;
}

static void reload(dashboard_list_state_t *ls) {
    if (!ls)
        return;

    ls->message[0] = '\0';
    ls->top_expense_count = 0;

    bool had_error = false;

    account_t *accounts = NULL;
    int account_count = db_get_accounts(ls->db, &accounts);
    if (account_count < 0) {
        account_count = 0;
        had_error = true;
    }

    int64_t net_worth = 0;
    for (int i = 0; i < account_count; i++) {
        int64_t account_balance = 0;
        if (db_get_account_balance_cents(ls->db, accounts[i].id, &account_balance) <
            0) {
            had_error = true;
            continue;
        }
        net_worth += account_balance;
    }
    ls->net_worth_cents = net_worth;
    free(accounts);

    report_row_t *mtd_rows = NULL;
    int mtd_count =
        db_get_report_rows(ls->db, REPORT_GROUP_CATEGORY, REPORT_PERIOD_THIS_MONTH,
                           &mtd_rows);
    if (mtd_count < 0) {
        mtd_count = 0;
        had_error = true;
    }
    accumulate_flow_totals(mtd_rows, mtd_count, &ls->mtd_income_cents,
                           &ls->mtd_expense_cents, &ls->mtd_net_cents);
    free(mtd_rows);

    report_row_t *ytd_rows = NULL;
    int ytd_count =
        db_get_report_rows(ls->db, REPORT_GROUP_CATEGORY, REPORT_PERIOD_YTD,
                           &ytd_rows);
    if (ytd_count < 0) {
        ytd_count = 0;
        had_error = true;
    }
    accumulate_flow_totals(ytd_rows, ytd_count, &ls->ytd_income_cents,
                           &ls->ytd_expense_cents, &ls->ytd_net_cents);
    for (int i = 0; i < ytd_count; i++)
        consider_top_expense(ls, ytd_rows[i].label, ytd_rows[i].expense_cents);
    free(ytd_rows);

    if (db_get_flow_totals_last_days(ls->db, 365, &ls->trailing_income_cents,
                                     &ls->trailing_expense_cents,
                                     &ls->trailing_net_cents) < 0) {
        ls->trailing_income_cents = 0;
        ls->trailing_expense_cents = 0;
        ls->trailing_net_cents = 0;
        had_error = true;
    }

    if (had_error) {
        snprintf(ls->message, sizeof(ls->message),
                 "Some dashboard metrics could not be loaded");
    }

    ls->dirty = false;
}

static void draw_totals_line(WINDOW *win, int row, int width, const char *title,
                             int64_t income_cents, int64_t expense_cents,
                             int64_t net_cents) {
    int col = 2;
    int max_col = width - 2;

    char income[24];
    char expense[24];
    char net[24];
    format_cents(income_cents, false, income, sizeof(income));
    format_cents(expense_cents, false, expense, sizeof(expense));
    format_cents(net_cents, true, net, sizeof(net));

    wattron(win, A_BOLD);
    col = draw_clipped_text(win, row, col, max_col, title);
    wattroff(win, A_BOLD);
    col = draw_clipped_text(win, row, col, max_col, "  Income ");
    col = draw_clipped_colored_text(win, row, col, max_col, income, COLOR_INCOME);
    col = draw_clipped_text(win, row, col, max_col, "  Expenses ");
    col = draw_clipped_colored_text(win, row, col, max_col, expense, COLOR_EXPENSE);
    col = draw_clipped_text(win, row, col, max_col, "  Net ");
    draw_clipped_colored_text(win, row, col, max_col, net,
                              net_cents < 0 ? COLOR_EXPENSE : COLOR_INCOME);
}

dashboard_list_state_t *dashboard_list_create(sqlite3 *db) {
    dashboard_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->dirty = true;
    return ls;
}

void dashboard_list_destroy(dashboard_list_state_t *ls) {
    if (!ls)
        return;
    free(ls);
}

void dashboard_list_draw(dashboard_list_state_t *ls, WINDOW *win, bool focused) {
    (void)focused;
    if (!ls || !win)
        return;
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    if (h < 14 || w < 56) {
        mvwprintw(win, 1, 2, "Window too small for Dashboard");
        return;
    }

    int row = 1;
    wattron(win, A_BOLD);
    mvwprintw(win, row, 2, "Dashboard");
    wattroff(win, A_BOLD);
    row++;

    mvwprintw(win, row, 2, "%-*s", w - 4, "");
    if (ls->message[0] != '\0')
        mvwprintw(win, row, 2, "%s", ls->message);
    row += 2;

    if (row >= h - 1)
        return;
    char net_worth[24];
    format_cents(ls->net_worth_cents, true, net_worth, sizeof(net_worth));
    wattron(win, A_BOLD);
    mvwprintw(win, row, 2, "Current Net Worth");
    wattroff(win, A_BOLD);
    mvwprintw(win, row, 22, " ");
    wattron(win,
            COLOR_PAIR(ls->net_worth_cents < 0 ? COLOR_EXPENSE : COLOR_INCOME));
    mvwprintw(win, row, 23, "%s", net_worth);
    wattroff(win,
             COLOR_PAIR(ls->net_worth_cents < 0 ? COLOR_EXPENSE : COLOR_INCOME));
    row += 2;

    if (row >= h - 1)
        return;
    draw_totals_line(win, row, w, "MTD", ls->mtd_income_cents,
                     ls->mtd_expense_cents, ls->mtd_net_cents);
    row += 2;

    if (row >= h - 1)
        return;
    wattron(win, A_BOLD);
    mvwprintw(win, row, 2, "Top 3 Expense Categories (YTD)");
    wattroff(win, A_BOLD);
    row++;

    int left = 2;
    int right = w - 2;
    int avail = right - left;
    for (int i = 0; i < DASHBOARD_TOP_EXPENSE_COUNT && row < h - 1; i++, row++) {
        mvwprintw(win, row, left, "%*s", avail, "");
        if (i < ls->top_expense_count) {
            char amount[24];
            format_cents(ls->top_expense[i].expense_cents, false, amount,
                         sizeof(amount));
            int amount_len = (int)strlen(amount);
            int amount_col = right - amount_len;
            if (amount_col < left + 8)
                amount_col = left + 8;

            char rank_prefix[8];
            snprintf(rank_prefix, sizeof(rank_prefix), "%d) ", i + 1);
            mvwprintw(win, row, left, "%s", rank_prefix);
            int label_col = left + (int)strlen(rank_prefix);
            int label_w = amount_col - label_col - 1;
            if (label_w < 1)
                label_w = 1;
            mvwprintw(win, row, label_col, "%-*.*s", label_w, label_w,
                      ls->top_expense[i].label);

            wattron(win, COLOR_PAIR(COLOR_EXPENSE));
            mvwprintw(win, row, amount_col, "%s", amount);
            wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
        } else if (i == 0) {
            wattron(win, A_DIM);
            mvwprintw(win, row, left, "No categorized expenses this year");
            wattroff(win, A_DIM);
        }
    }

    if (row < h - 1) {
        row++;
        draw_totals_line(win, row, w, "YTD", ls->ytd_income_cents,
                         ls->ytd_expense_cents, ls->ytd_net_cents);
    }

    if (row + 2 < h - 1) {
        row += 2;
        draw_totals_line(win, row, w, "Last 365 Days", ls->trailing_income_cents,
                         ls->trailing_expense_cents, ls->trailing_net_cents);
    }
}

void dashboard_list_mark_dirty(dashboard_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}
