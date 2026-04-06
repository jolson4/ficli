#include "csv/csv_import.h"
#include "db/query.h"
#include "models/account.h"
#include "models/transaction.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_COLS 32
#define LINE_BUF 4096
#define FIELD_BUF 4096
static const int transfer_match_date_window_days = 3;

typedef struct {
    uint64_t hash;
    char *key;
    int count;
} dedup_entry_t;

typedef struct {
    dedup_entry_t *entries;
    int capacity;
    int size;
} dedup_map_t;

static uint64_t dedup_hash_key(const char *s) {
    uint64_t hash = 1469598103934665603ull;
    while (s && *s) {
        hash ^= (unsigned char)*s++;
        hash *= 1099511628211ull;
    }
    return hash;
}

static int dedup_next_pow2(int v) {
    int n = 16;
    while (n < v && n > 0)
        n <<= 1;
    return n > 0 ? n : 16;
}

static void dedup_map_free(dedup_map_t *map) {
    if (!map || !map->entries)
        return;
    for (int i = 0; i < map->capacity; i++) {
        free(map->entries[i].key);
        map->entries[i].key = NULL;
    }
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0;
    map->size = 0;
}

static bool dedup_map_init(dedup_map_t *map, int desired_capacity) {
    if (!map)
        return false;
    map->capacity = dedup_next_pow2(desired_capacity);
    map->size = 0;
    map->entries = calloc((size_t)map->capacity, sizeof(dedup_entry_t));
    return map->entries != NULL;
}

static bool dedup_map_grow(dedup_map_t *map) {
    int old_cap = map->capacity;
    dedup_entry_t *old_entries = map->entries;
    if (!dedup_map_init(map, old_cap * 2))
        return false;

    for (int i = 0; i < old_cap; i++) {
        if (!old_entries[i].key)
            continue;
        uint64_t hash = old_entries[i].hash;
        int idx = (int)(hash & (uint64_t)(map->capacity - 1));
        while (map->entries[idx].key)
            idx = (idx + 1) & (map->capacity - 1);
        map->entries[idx] = old_entries[i];
        map->size++;
    }
    free(old_entries);
    return true;
}

static bool dedup_map_add(dedup_map_t *map, const char *key) {
    if (!map || !key)
        return false;
    if (!map->entries && !dedup_map_init(map, 32))
        return false;
    if ((map->size + 1) * 10 >= map->capacity * 7) {
        if (!dedup_map_grow(map))
            return false;
    }

    uint64_t hash = dedup_hash_key(key);
    int idx = (int)(hash & (uint64_t)(map->capacity - 1));
    while (map->entries[idx].key) {
        if (map->entries[idx].hash == hash &&
            strcmp(map->entries[idx].key, key) == 0) {
            map->entries[idx].count++;
            return true;
        }
        idx = (idx + 1) & (map->capacity - 1);
    }

    size_t len = strlen(key);
    char *copy = malloc(len + 1);
    if (!copy)
        return false;
    memcpy(copy, key, len + 1);
    map->entries[idx].hash = hash;
    map->entries[idx].key = copy;
    map->entries[idx].count = 1;
    map->size++;
    return true;
}

static bool dedup_map_take(dedup_map_t *map, const char *key) {
    if (!map || !map->entries || !key)
        return false;
    uint64_t hash = dedup_hash_key(key);
    int idx = (int)(hash & (uint64_t)(map->capacity - 1));
    while (map->entries[idx].key) {
        if (map->entries[idx].hash == hash &&
            strcmp(map->entries[idx].key, key) == 0) {
            if (map->entries[idx].count > 0) {
                map->entries[idx].count--;
                return true;
            }
            return false;
        }
        idx = (idx + 1) & (map->capacity - 1);
    }
    return false;
}

// Parse one CSV line into fields[]. Each fields[i] points into buf.
// Returns number of fields parsed.
static int csv_parse_line(const char *line, char **fields, int max_fields,
                          char *buf, int buflen) {
    int nfields = 0;
    int bi = 0;
    const char *p = line;

    while (*p && nfields < max_fields) {
        if (bi >= buflen - 1)
            break;
        fields[nfields++] = buf + bi;

        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"') {
                    if (*(p + 1) == '"') {
                        if (bi < buflen - 1)
                            buf[bi++] = '"';
                        p += 2;
                    } else {
                        p++;
                        break;
                    }
                } else {
                    if (bi < buflen - 1)
                        buf[bi++] = *p;
                    p++;
                }
            }
            while (*p && *p != ',')
                p++;
        } else {
            while (*p && *p != ',') {
                if (bi < buflen - 1)
                    buf[bi++] = *p;
                p++;
            }
        }

        if (bi < buflen)
            buf[bi++] = '\0';
        if (*p == ',')
            p++;
    }

    return nfields;
}

// Lowercase + trim whitespace for column-name matching.
static void normalize_col(const char *src, char *dst, int dstlen) {
    while (*src == ' ' || *src == '\t')
        src++;
    int di = 0;
    while (*src && di < dstlen - 1) {
        char c = *src++;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        dst[di++] = c;
    }
    while (di > 0 && (dst[di - 1] == ' ' || dst[di - 1] == '\t'))
        di--;
    dst[di] = '\0';
}

// Infer transaction direction from textual type labels used by some
// checking/savings exports (e.g., Debit/Credit).
// Returns -1 for expense/outflow, +1 for income/inflow, 0 if unknown.
static int direction_from_txn_type(const char *src) {
    if (!src)
        return 0;

    char norm[64];
    normalize_col(src, norm, sizeof(norm));
    if (norm[0] == '\0')
        return 0;

    if (strcmp(norm, "debit") == 0 || strstr(norm, "withdraw") != NULL)
        return -1;
    if (strcmp(norm, "credit") == 0 || strstr(norm, "deposit") != NULL)
        return 1;
    return 0;
}

// Normalize date to YYYY-MM-DD. Handles MM/DD/YYYY, MM/DD/YY, and
// YYYY-MM-DD input, and also QIF style M/D'YY.
// Returns true on success, false if format unrecognized.
static bool normalize_date(const char *src, char *dst) {
    if (!src || !dst)
        return false;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s", src);

    // Trim surrounding whitespace
    char *start = buf;
    while (*start && isspace((unsigned char)*start))
        start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1)))
        *(--end) = '\0';

    int y = 0, m = 0, d = 0;
    int len = (int)strlen(start);
    if (len == 10 && start[4] == '-' && start[7] == '-') {
        if (sscanf(start, "%4d-%2d-%2d", &y, &m, &d) != 3)
            return false;
    } else {
        char trailing = '\0';
        if (sscanf(start, "%d/%d/%d%c", &m, &d, &y, &trailing) != 3 &&
            sscanf(start, "%d/%d'%d%c", &m, &d, &y, &trailing) != 3) {
            return false;
        }
        if (y < 100)
            y += (y >= 70) ? 1900 : 2000;
    }

    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900)
        return false;

    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    if (mktime(&tmv) == (time_t)-1)
        return false;
    if (tmv.tm_year != y - 1900 || tmv.tm_mon != m - 1 || tmv.tm_mday != d)
        return false;

    return strftime(dst, 11, "%Y-%m-%d", &tmv) == 10;
}

// Parse a dollar amount string into cents. Strips $, commas, spaces.
// Handles negatives via leading '-' or parentheses notation.
// Returns true if a non-zero value was parsed.
static bool parse_csv_amount(const char *src, int64_t *out) {
    char buf[64];
    int bi = 0;
    bool negative = false;

    for (const char *p = src; *p && bi < (int)sizeof(buf) - 1; p++) {
        if (*p == '(') { negative = true; continue; }
        if (*p == ')') continue;
        if (*p == '-' && bi == 0) { negative = true; continue; }
        if (*p == '$' || *p == ',' || *p == ' ') continue;
        buf[bi++] = *p;
    }
    buf[bi] = '\0';

    if (bi == 0) {
        *out = 0;
        return false;
    }

    char *dot = strchr(buf, '.');
    int64_t whole = 0, frac = 0;
    if (dot) {
        *dot = '\0';
        if (buf[0] != '\0')
            whole = (int64_t)atoll(buf);
        const char *dp = dot + 1;
        int dplen = (int)strlen(dp);
        if (dplen >= 2) {
            char fb[3] = {dp[0], dp[1], '\0'};
            frac = (int64_t)atoll(fb);
        } else if (dplen == 1) {
            frac = (int64_t)(dp[0] - '0') * 10;
        }
    } else {
        whole = (int64_t)atoll(buf);
    }

    *out = whole * 100 + frac;
    if (negative)
        *out = -(*out);
    return (*out != 0);
}

// Extract the last 4 digits from a card number string.
static void extract_last4(const char *card_str, char *out) {
    int len = (int)strlen(card_str);
    char digits[5];
    int count = 0;
    for (int i = len - 1; i >= 0 && count < 4; i--) {
        if (card_str[i] >= '0' && card_str[i] <= '9')
            digits[count++] = card_str[i];
    }
    if (count == 4) {
        out[0] = digits[3];
        out[1] = digits[2];
        out[2] = digits[1];
        out[3] = digits[0];
        out[4] = '\0';
    } else {
        out[0] = '\0';
    }
}

static void strip_eol(char *line) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
}

static void build_dedup_key(const char *date, int64_t amount_cents,
                            transaction_type_t type, const char *payee,
                            char *out, size_t out_sz) {
    if (!out || out_sz == 0)
        return;
    snprintf(out, out_sz, "%s|%lld|%d|%s", date ? date : "",
             (long long)amount_cents, (int)type, payee ? payee : "");
}

static void trim_whitespace_in_place(char *s) {
    if (!s)
        return;

    int len = (int)strlen(s);
    int start = 0;
    while (start < len && isspace((unsigned char)s[start]))
        start++;

    int end = len;
    while (end > start && isspace((unsigned char)s[end - 1]))
        end--;

    if (start > 0)
        memmove(s, s + start, (size_t)(end - start));
    s[end - start] = '\0';
}

static bool copy_import_category(const char *src, char *dst, size_t dst_sz) {
    if (!src || !dst || dst_sz == 0)
        return false;
    snprintf(dst, dst_sz, "%s", src);
    trim_whitespace_in_place(dst);
    return dst[0] != '\0';
}

static bool ensure_result_capacity(csv_parse_result_t *result, int *capacity) {
    if (result->row_count < *capacity)
        return true;
    *capacity *= 2;
    csv_row_t *tmp = realloc(result->rows, (size_t)(*capacity) * sizeof(csv_row_t));
    if (!tmp)
        return false;
    result->rows = tmp;
    return true;
}

static csv_parse_result_t csv_parse_stream(FILE *f) {
    csv_parse_result_t result = {0};
    result.type = CSV_TYPE_UNKNOWN;

    char line[LINE_BUF];
    line[0] = '\0';

    // Read and parse header row
    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] != '\0')
            break;
    }
    if (line[0] == '\0') {
        snprintf(result.error, sizeof(result.error), "File is empty");
        return result;
    }

    char hdr_buf[FIELD_BUF];
    char *hdr_fields[MAX_COLS];
    int ncols = csv_parse_line(line, hdr_fields, MAX_COLS, hdr_buf, sizeof(hdr_buf));

    int col_date = -1, col_card = -1;
    int col_debit = -1, col_credit = -1;
    int col_amount = -1, col_txn_type = -1, col_desc = -1, col_txn_desc = -1;
    int col_category = -1;

    for (int i = 0; i < ncols; i++) {
        char norm[128];
        normalize_col(hdr_fields[i], norm, sizeof(norm));

        if (col_date < 0 &&
            (strcmp(norm, "transaction date") == 0 || strcmp(norm, "date") == 0)) {
            col_date = i;
        } else if (col_card < 0 && strstr(norm, "card")) {
            col_card = i;
        } else if (strcmp(norm, "debit") == 0) {
            col_debit = i;
        } else if (strcmp(norm, "credit") == 0) {
            col_credit = i;
        } else if (col_amount < 0 &&
                   (strcmp(norm, "transaction amount") == 0 ||
                    strcmp(norm, "amount") == 0)) {
            col_amount = i;
        } else if (col_txn_type < 0 &&
                   (strcmp(norm, "transaction type") == 0 ||
                    strcmp(norm, "type") == 0)) {
            col_txn_type = i;
        } else if (col_txn_desc < 0 &&
                   strcmp(norm, "transaction description") == 0) {
            col_txn_desc = i;
        } else if (col_desc < 0 &&
                   (strcmp(norm, "description") == 0 ||
                    strcmp(norm, "memo") == 0 || strcmp(norm, "payee") == 0 ||
                    strcmp(norm, "merchant") == 0)) {
            col_desc = i;
        } else if (col_category < 0 &&
                   (strcmp(norm, "category") == 0 ||
                    strcmp(norm, "transaction category") == 0)) {
            col_category = i;
        }
    }

    if (col_date < 0) {
        snprintf(result.error, sizeof(result.error), "No date column found");
        return result;
    }

    result.type = (col_card >= 0) ? CSV_TYPE_CREDIT_CARD : CSV_TYPE_CHECKING_SAVINGS;

    int capacity = 32;
    result.rows = malloc(capacity * sizeof(csv_row_t));
    if (!result.rows) {
        snprintf(result.error, sizeof(result.error), "Out of memory");
        result.type = CSV_TYPE_UNKNOWN;
        return result;
    }

    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0')
            continue;

        char row_buf[FIELD_BUF];
        char *row_fields[MAX_COLS];
        int rnc = csv_parse_line(line, row_fields, MAX_COLS, row_buf, sizeof(row_buf));

        if (!ensure_result_capacity(&result, &capacity)) {
            snprintf(result.error, sizeof(result.error), "Out of memory");
            free(result.rows);
            result.rows = NULL;
            result.row_count = 0;
            result.type = CSV_TYPE_UNKNOWN;
            return result;
        }

        csv_row_t *row = &result.rows[result.row_count];
        memset(row, 0, sizeof(*row));

        // Date (required)
        if (col_date < rnc && row_fields[col_date][0]) {
            if (!normalize_date(row_fields[col_date], row->date)) {
                snprintf(row->date, sizeof(row->date), "%.10s", row_fields[col_date]);
            }
        } else {
            continue;
        }

        // Payee: CC uses "Description" column; checking/savings uses "Transaction Description"
        if (result.type == CSV_TYPE_CREDIT_CARD) {
            if (col_desc >= 0 && col_desc < rnc)
                snprintf(row->payee, sizeof(row->payee), "%s", row_fields[col_desc]);
        } else {
            if (col_txn_desc >= 0 && col_txn_desc < rnc)
                snprintf(row->payee, sizeof(row->payee), "%s", row_fields[col_txn_desc]);
            else if (col_desc >= 0 && col_desc < rnc)
                snprintf(row->payee, sizeof(row->payee), "%s", row_fields[col_desc]);
        }

        if (col_category >= 0 && col_category < rnc) {
            row->has_category = copy_import_category(
                row_fields[col_category], row->category, sizeof(row->category));
        }

        if (result.type == CSV_TYPE_CREDIT_CARD) {
            // Card last 4
            if (col_card >= 0 && col_card < rnc)
                extract_last4(row_fields[col_card], row->card_last4);

            // Prefer signed amount if provided; otherwise fall back to debit/credit
            int64_t signed_amount = 0;
            bool has_amount = false;
            if (col_amount >= 0 && col_amount < rnc && row_fields[col_amount][0]) {
                has_amount = parse_csv_amount(row_fields[col_amount], &signed_amount);
            }

            if (has_amount) {
                if (signed_amount >= 0) {
                    row->type = TRANSACTION_INCOME;
                    row->amount_cents = signed_amount;
                } else {
                    row->type = TRANSACTION_EXPENSE;
                    row->amount_cents = -signed_amount;
                }
            } else {
                // Debit → EXPENSE, Credit → INCOME
                int64_t debit_cents = 0, credit_cents = 0;
                if (col_debit >= 0 && col_debit < rnc && row_fields[col_debit][0])
                    parse_csv_amount(row_fields[col_debit], &debit_cents);
                if (col_credit >= 0 && col_credit < rnc && row_fields[col_credit][0])
                    parse_csv_amount(row_fields[col_credit], &credit_cents);

                if (debit_cents > 0) {
                    row->amount_cents = debit_cents;
                    row->type = TRANSACTION_EXPENSE;
                } else if (credit_cents > 0) {
                    row->amount_cents = credit_cents;
                    row->type = TRANSACTION_INCOME;
                } else {
                    continue; // skip rows with no amount
                }
            }
        } else {
            // Checking/savings: single amount column
            int64_t amount = 0;
            if (col_amount >= 0 && col_amount < rnc && row_fields[col_amount][0]) {
                parse_csv_amount(row_fields[col_amount], &amount);
            } else {
                continue;
            }

            int dir = 0;
            if (col_txn_type >= 0 && col_txn_type < rnc)
                dir = direction_from_txn_type(row_fields[col_txn_type]);

            // Prefer explicit txn type direction when available; otherwise use
            // sign of amount.
            if (dir > 0 || (dir == 0 && amount >= 0)) {
                row->type = TRANSACTION_INCOME;
                row->amount_cents = amount >= 0 ? amount : -amount;
            } else {
                row->type = TRANSACTION_EXPENSE;
                row->amount_cents = amount >= 0 ? amount : -amount;
            }
        }

        result.row_count++;
    }

    if (result.row_count == 0 && result.error[0] == '\0')
        snprintf(result.error, sizeof(result.error), "No transactions found in file");

    return result;
}

static bool qif_type_supports_transactions(const char *type_line) {
    char norm[32];
    normalize_col(type_line, norm, sizeof(norm));
    return strcmp(norm, "ccard") == 0 ||
           strcmp(norm, "bank") == 0 ||
           strcmp(norm, "cash") == 0;
}

static csv_parse_result_t qif_parse_stream(FILE *f) {
    csv_parse_result_t result = {0};
    result.type = CSV_TYPE_QIF;

    int capacity = 64;
    result.rows = malloc((size_t)capacity * sizeof(csv_row_t));
    if (!result.rows) {
        snprintf(result.error, sizeof(result.error), "Out of memory");
        result.type = CSV_TYPE_UNKNOWN;
        return result;
    }

    char line[LINE_BUF];
    bool in_account_block = false;
    bool in_txn_block = false;
    char pending_account[64] = "";
    char active_account[64] = "";
    char seen_account[64] = "";
    bool multi_account = false;

    char txn_date[64] = "";
    char txn_amount[64] = "";
    char txn_payee[128] = "";
    char txn_memo[256] = "";
    char txn_category[64] = "";

    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0')
            continue;

        if (line[0] == '!') {
            in_txn_block = false;
            if (strcmp(line, "!Account") == 0) {
                in_account_block = true;
                pending_account[0] = '\0';
            } else if (strncmp(line, "!Type:", 6) == 0) {
                in_account_block = false;
                if (qif_type_supports_transactions(line + 6)) {
                    in_txn_block = true;
                    snprintf(active_account, sizeof(active_account), "%s",
                             pending_account);
                }
            }
            continue;
        }

        if (in_account_block) {
            if (line[0] == 'N') {
                snprintf(pending_account, sizeof(pending_account), "%s", line + 1);
            } else if (line[0] == '^') {
                in_account_block = false;
            }
            continue;
        }

        if (!in_txn_block)
            continue;

        if (line[0] == '^') {
            if (txn_date[0] && txn_amount[0]) {
                int64_t signed_amount = 0;
                if (parse_csv_amount(txn_amount, &signed_amount)) {
                    if (!ensure_result_capacity(&result, &capacity)) {
                        snprintf(result.error, sizeof(result.error), "Out of memory");
                        free(result.rows);
                        result.rows = NULL;
                        result.row_count = 0;
                        result.type = CSV_TYPE_UNKNOWN;
                        return result;
                    }

                    csv_row_t *row = &result.rows[result.row_count];
                    memset(row, 0, sizeof(*row));
                    if (!normalize_date(txn_date, row->date))
                        snprintf(row->date, sizeof(row->date), "%.10s", txn_date);
                    row->type = signed_amount < 0 ? TRANSACTION_EXPENSE
                                                  : TRANSACTION_INCOME;
                    row->amount_cents = signed_amount < 0 ? -signed_amount
                                                          : signed_amount;
                    snprintf(row->payee, sizeof(row->payee), "%s", txn_payee);
                    snprintf(row->description, sizeof(row->description), "%s",
                             txn_memo);
                    if (copy_import_category(txn_category, row->category,
                                             sizeof(row->category)) &&
                        row->category[0] != '[') {
                        row->has_category = true;
                    }
                    result.row_count++;

                    if (active_account[0] != '\0') {
                        if (seen_account[0] == '\0') {
                            snprintf(seen_account, sizeof(seen_account), "%s",
                                     active_account);
                        } else if (strcmp(seen_account, active_account) != 0) {
                            multi_account = true;
                        }
                    }
                }
            }

            txn_date[0] = '\0';
            txn_amount[0] = '\0';
            txn_payee[0] = '\0';
            txn_memo[0] = '\0';
            txn_category[0] = '\0';
            continue;
        }

        switch (line[0]) {
        case 'D':
            snprintf(txn_date, sizeof(txn_date), "%s", line + 1);
            break;
        case 'T':
            snprintf(txn_amount, sizeof(txn_amount), "%s", line + 1);
            break;
        case 'P':
            snprintf(txn_payee, sizeof(txn_payee), "%s", line + 1);
            break;
        case 'M':
            snprintf(txn_memo, sizeof(txn_memo), "%s", line + 1);
            break;
        case 'L':
            snprintf(txn_category, sizeof(txn_category), "%s", line + 1);
            break;
        default:
            break;
        }
    }

    if (multi_account) {
        snprintf(result.error, sizeof(result.error),
                 "QIF import supports one account per file.");
        free(result.rows);
        result.rows = NULL;
        result.row_count = 0;
        result.type = CSV_TYPE_UNKNOWN;
        return result;
    }
    if (seen_account[0] != '\0')
        snprintf(result.source_account, sizeof(result.source_account), "%s",
                 seen_account);
    if (result.row_count == 0 && result.error[0] == '\0')
        snprintf(result.error, sizeof(result.error), "No transactions found in file");

    return result;
}

static bool file_looks_like_qif(FILE *f) {
    char line[LINE_BUF];
    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0')
            continue;
        return line[0] == '!';
    }
    return false;
}

csv_parse_result_t csv_parse_file(const char *path) {
    csv_parse_result_t result = {0};
    result.type = CSV_TYPE_UNKNOWN;

    // Expand leading ~ to $HOME
    char expanded[1024];
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (home)
            snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
        else
            snprintf(expanded, sizeof(expanded), "%s", path);
    } else {
        snprintf(expanded, sizeof(expanded), "%s", path);
    }

    FILE *f = fopen(expanded, "r");
    if (!f) {
        snprintf(result.error, sizeof(result.error), "Cannot open: %.240s", expanded);
        return result;
    }

    bool is_qif = file_looks_like_qif(f);
    rewind(f);
    result = is_qif ? qif_parse_stream(f) : csv_parse_stream(f);
    fclose(f);
    return result;
}

// Per-account cache of existing transactions used for dedup during import.
typedef struct {
    int64_t account_id;
    txn_row_t *txns;
    dedup_map_t dedup;
    int count;
} acct_txn_cache_t;

// Find or load a cache entry for account_id. Returns NULL on allocation failure.
// caches must have room for at least one more entry (caller ensures capacity).
static acct_txn_cache_t *get_acct_cache(sqlite3 *db, acct_txn_cache_t *caches,
                                         int *ncaches, int64_t account_id) {
    for (int i = 0; i < *ncaches; i++) {
        if (caches[i].account_id == account_id)
            return &caches[i];
    }

    acct_txn_cache_t *c = &caches[*ncaches];
    c->account_id = account_id;
    c->txns = NULL;
    memset(&c->dedup, 0, sizeof(c->dedup));
    c->count = 0;

    int cnt = db_get_transactions(db, account_id, &c->txns);
    if (cnt < 0) {
        free(c->txns);
        c->txns = NULL;
        return NULL;
    }
    c->count = cnt;

    if (!dedup_map_init(&c->dedup, c->count * 2 + 16)) {
        free(c->txns);
        c->txns = NULL;
        c->count = 0;
        return NULL;
    }

    for (int i = 0; i < c->count; i++) {
        char key[512];
        build_dedup_key(c->txns[i].date, c->txns[i].amount_cents,
                        c->txns[i].type, c->txns[i].payee, key, sizeof(key));
        if (!dedup_map_add(&c->dedup, key)) {
            dedup_map_free(&c->dedup);
            free(c->txns);
            c->txns = NULL;
            c->count = 0;
            return NULL;
        }
    }

    (*ncaches)++;
    return c;
}

// Find a unique unlinked counterparty transaction in another account with
// matching date+amount. If multiple matches exist, prefer the unique opposite
// direction (EXPENSE vs INCOME) when available.
// Returns 0 when exactly one match exists, -2 when none/ambiguous, -1 on error.
static int find_unique_transfer_counterparty(sqlite3 *db, int64_t account_id,
                                             const char *date,
                                             int64_t amount_cents,
                                             transaction_type_t type,
                                             int64_t *out_txn_id,
                                             int64_t *out_account_id) {
    if (!db || !date || !out_txn_id || !out_account_id)
        return -1;
    *out_txn_id = 0;
    *out_account_id = 0;
    if (type != TRANSACTION_EXPENSE && type != TRANSACTION_INCOME)
        return -2;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT id, account_id, type FROM transactions"
        " WHERE account_id != ?"
        "   AND transfer_id IS NULL"
        "   AND type != 'TRANSFER'"
        "   AND amount_cents = ?"
        "   AND ABS(julianday(date) - julianday(?)) <= ?"
        " ORDER BY ABS(julianday(date) - julianday(?)) ASC, id DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "find_unique_transfer_counterparty prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    sqlite3_bind_int64(stmt, 2, amount_cents);
    sqlite3_bind_text(stmt, 3, date, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, transfer_match_date_window_days);
    sqlite3_bind_text(stmt, 5, date, -1, SQLITE_STATIC);

    int total = 0;
    int opposite_count = 0;
    int64_t first_txn_id = 0;
    int64_t first_account_id = 0;
    int64_t opposite_txn_id = 0;
    int64_t opposite_account_id = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t txn_id = sqlite3_column_int64(stmt, 0);
        int64_t acct_id = sqlite3_column_int64(stmt, 1);
        const char *row_type = (const char *)sqlite3_column_text(stmt, 2);
        transaction_type_t txn_type =
            (row_type && strcmp(row_type, "INCOME") == 0) ? TRANSACTION_INCOME
                                                           : TRANSACTION_EXPENSE;

        total++;
        if (total == 1) {
            first_txn_id = txn_id;
            first_account_id = acct_id;
        }

        if ((type == TRANSACTION_EXPENSE && txn_type == TRANSACTION_INCOME) ||
            (type == TRANSACTION_INCOME && txn_type == TRANSACTION_EXPENSE)) {
            opposite_count++;
            if (opposite_count == 1) {
                opposite_txn_id = txn_id;
                opposite_account_id = acct_id;
            }
        }
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "find_unique_transfer_counterparty step: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    if (total == 0)
        return -2;

    if (total == 1) {
        *out_txn_id = first_txn_id;
        *out_account_id = first_account_id;
        return 0;
    }

    if (opposite_count == 1) {
        *out_txn_id = opposite_txn_id;
        *out_account_id = opposite_account_id;
        return 0;
    }

    return -2;
}

static int maybe_autolink_imported_transfer(sqlite3 *db, int64_t txn_id,
                                            int64_t account_id,
                                            const char *date,
                                            int64_t amount_cents,
                                            transaction_type_t type) {
    int64_t counterparty_txn_id = 0;
    int64_t counterparty_account_id = 0;
    int rc = find_unique_transfer_counterparty(
        db, account_id, date, amount_cents, type, &counterparty_txn_id,
        &counterparty_account_id);
    if (rc == -2)
        return 0;
    if (rc < 0)
        return -1;

    if (type == TRANSACTION_EXPENSE) {
        transaction_t source = {0};
        if (db_get_transaction_by_id(db, (int)txn_id, &source) != 0)
            return -1;
        source.type = TRANSACTION_TRANSFER;
        source.category_id = 0;
        source.payee[0] = '\0';
        rc = db_update_transfer(db, &source, counterparty_account_id, true);
    } else {
        transaction_t source = {0};
        if (db_get_transaction_by_id(db, (int)counterparty_txn_id, &source) !=
            0)
            return -1;
        source.type = TRANSACTION_TRANSFER;
        source.category_id = 0;
        source.payee[0] = '\0';
        rc = db_update_transfer(db, &source, account_id, true);
    }

    if (rc == -2 || rc == -3)
        return 0;
    if (rc < 0)
        return -1;
    return 0;
}

static int begin_import_txn(sqlite3 *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int commit_import_txn(sqlite3 *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static void rollback_import_txn(sqlite3 *db) {
    char *err = NULL;
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, &err);
    sqlite3_free(err);
}

void csv_parse_result_free(csv_parse_result_t *r) {
    if (!r)
        return;
    free(r->rows);
    r->rows = NULL;
    r->row_count = 0;
    r->type = CSV_TYPE_UNKNOWN;
    r->source_account[0] = '\0';
    r->error[0] = '\0';
}

int csv_import_credit_card(sqlite3 *db, const csv_parse_result_t *r,
                           int *imported, int *skipped) {
    *imported = 0;
    *skipped = 0;

    account_t *accounts = NULL;
    int account_count = db_get_accounts(db, &accounts);
    if (account_count < 0) {
        free(accounts);
        return -1;
    }

    // One cache entry per CC account (at most account_count entries needed).
    acct_txn_cache_t *caches = calloc(account_count > 0 ? account_count : 1,
                                       sizeof(acct_txn_cache_t));
    if (!caches) {
        free(accounts);
        return -1;
    }
    int ncaches = 0;
    int ret = 0;
    bool txn_open = false;

    if (begin_import_txn(db) < 0) {
        ret = -1;
        goto cleanup;
    }
    txn_open = true;

    for (int i = 0; i < r->row_count; i++) {
        const csv_row_t *row = &r->rows[i];

        int64_t account_id = 0;
        for (int j = 0; j < account_count; j++) {
            if (accounts[j].type == ACCOUNT_CREDIT_CARD &&
                strcmp(accounts[j].card_last4, row->card_last4) == 0) {
                account_id = accounts[j].id;
                break;
            }
        }

        if (account_id == 0) {
            (*skipped)++;
            continue;
        }

        acct_txn_cache_t *cache = get_acct_cache(db, caches, &ncaches, account_id);
        if (!cache) {
            ret = -1;
            goto cleanup;
        }

        // Check for a matching unconsumed existing transaction (dedup).
        char key[512];
        build_dedup_key(row->date, row->amount_cents, row->type, row->payee, key,
                        sizeof(key));
        bool is_dup = dedup_map_take(&cache->dedup, key);
        if (is_dup) {
            (*skipped)++;
            continue;
        }

        transaction_t txn = {0};
        txn.amount_cents = row->amount_cents;
        txn.type = row->type;
        txn.account_id = account_id;
        snprintf(txn.date, sizeof(txn.date), "%s", row->date);
        snprintf(txn.payee, sizeof(txn.payee), "%s", row->payee);
        if (row->has_category) {
            txn.category_id = row->category_id;
        } else {
            if (db_get_most_recent_category_for_payee(
                    db, account_id, row->payee, row->type, &txn.category_id) < 0) {
                ret = -1;
                goto cleanup;
            }
        }

        int64_t row_id = db_insert_transaction(db, &txn);
        if (row_id < 0) {
            ret = -1;
            goto cleanup;
        }
        if (maybe_autolink_imported_transfer(db, row_id, account_id, txn.date,
                                             txn.amount_cents, txn.type) < 0) {
            ret = -1;
            goto cleanup;
        }
        (*imported)++;
    }

cleanup:
    if (txn_open) {
        if (ret == 0) {
            if (commit_import_txn(db) < 0) {
                rollback_import_txn(db);
                ret = -1;
            }
        } else {
            rollback_import_txn(db);
        }
    }
    for (int i = 0; i < ncaches; i++) {
        free(caches[i].txns);
        dedup_map_free(&caches[i].dedup);
    }
    free(caches);
    free(accounts);
    return ret;
}

int csv_import_checking(sqlite3 *db, const csv_parse_result_t *r,
                        int64_t account_id, int *imported, int *skipped) {
    *imported = 0;
    *skipped = 0;

    txn_row_t *existing = NULL;
    int nexisting = db_get_transactions(db, account_id, &existing);
    if (nexisting < 0) {
        free(existing);
        return -1;
    }

    dedup_map_t dedup = {0};
    if (!dedup_map_init(&dedup, nexisting * 2 + 16)) {
        free(existing);
        return -1;
    }
    for (int i = 0; i < nexisting; i++) {
        char key[512];
        build_dedup_key(existing[i].date, existing[i].amount_cents,
                        existing[i].type, existing[i].payee, key, sizeof(key));
        if (!dedup_map_add(&dedup, key)) {
            dedup_map_free(&dedup);
            free(existing);
            return -1;
        }
    }

    int ret = 0;
    bool txn_open = false;
    if (begin_import_txn(db) < 0) {
        dedup_map_free(&dedup);
        free(existing);
        return -1;
    }
    txn_open = true;
    for (int i = 0; i < r->row_count; i++) {
        const csv_row_t *row = &r->rows[i];

        char key[512];
        build_dedup_key(row->date, row->amount_cents, row->type, row->payee, key,
                        sizeof(key));
        bool is_dup = dedup_map_take(&dedup, key);
        if (is_dup) {
            (*skipped)++;
            continue;
        }

        transaction_t txn = {0};
        txn.amount_cents = row->amount_cents;
        txn.type = row->type;
        txn.account_id = account_id;
        snprintf(txn.date, sizeof(txn.date), "%s", row->date);
        snprintf(txn.payee, sizeof(txn.payee), "%s", row->payee);
        if (row->has_category) {
            txn.category_id = row->category_id;
        } else {
            if (db_get_most_recent_category_for_payee(
                    db, account_id, row->payee, row->type, &txn.category_id) < 0) {
                ret = -1;
                break;
            }
        }

        int64_t row_id = db_insert_transaction(db, &txn);
        if (row_id < 0) {
            ret = -1;
            break;
        }
        if (maybe_autolink_imported_transfer(db, row_id, account_id, txn.date,
                                             txn.amount_cents, txn.type) < 0) {
            ret = -1;
            break;
        }
        (*imported)++;
    }

    if (txn_open) {
        if (ret == 0) {
            if (commit_import_txn(db) < 0) {
                rollback_import_txn(db);
                ret = -1;
            }
        } else {
            rollback_import_txn(db);
        }
    }

    dedup_map_free(&dedup);
    free(existing);
    return ret;
}
