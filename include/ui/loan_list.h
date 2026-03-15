#ifndef FICLI_LOAN_LIST_H
#define FICLI_LOAN_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct loan_list_state loan_list_state_t;

loan_list_state_t *loan_list_create(sqlite3 *db);
void               loan_list_destroy(loan_list_state_t *ls);
void               loan_list_draw(loan_list_state_t *ls, WINDOW *win, bool focused);
bool               loan_list_handle_input(loan_list_state_t *ls, WINDOW *parent,
                                          int ch);
const char        *loan_list_status_hint(const loan_list_state_t *ls);
void               loan_list_mark_dirty(loan_list_state_t *ls);
bool               loan_list_consume_changed(loan_list_state_t *ls);
void               loan_list_focus_add_button(loan_list_state_t *ls);

#endif
