#ifndef FICLI_DASHBOARD_LIST_H
#define FICLI_DASHBOARD_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct dashboard_list_state dashboard_list_state_t;

dashboard_list_state_t *dashboard_list_create(sqlite3 *db);
void                    dashboard_list_destroy(dashboard_list_state_t *ls);
void                    dashboard_list_draw(dashboard_list_state_t *ls,
                                            WINDOW *win, bool focused);
void                    dashboard_list_mark_dirty(dashboard_list_state_t *ls);

#endif
