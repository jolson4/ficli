#include "db/db.h"
#include "ui/ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int ensure_dir_exists(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int get_key_file_path(char *out, size_t out_sz) {
    const char *env_path = getenv("FICLI_DB_KEY_FILE");
    if (env_path && env_path[0] != '\0') {
        snprintf(out, out_sz, "%s", env_path);
        return 0;
    }

    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && base[0] != '\0') {
        snprintf(out, out_sz, "%s/ficli/db.key", base);
        return 0;
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return -1;
    }
    snprintf(out, out_sz, "%s/.config/ficli/db.key", home);
    return 0;
}

static int read_key_file(const char *path, char *out, size_t out_sz) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    size_t len = strcspn(buf, "\r\n");
    buf[len] = '\0';
    if (buf[0] == '\0') {
        return -1;
    }

    snprintf(out, out_sz, "%s", buf);
    memset(buf, 0, sizeof(buf));
    return 0;
}

static int write_key_file(const char *path, const char *key) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
    if (!last_slash) {
        return -1;
    }
    *last_slash = '\0';
    if (ensure_dir_exists(dir) != 0) {
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }

    size_t len = strlen(key);
    ssize_t wrote = write(fd, key, len);
    if (wrote < 0 || (size_t)wrote != len) {
        close(fd);
        return -1;
    }

    if (write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }

    if (close(fd) != 0) {
        return -1;
    }
    return 0;
}

static int read_key_from_1password(char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return -1;
    }

    FILE *pipe = popen("op read 'op://Private/Ficli/password' 2>/dev/null", "r");
    if (!pipe) {
        return -1;
    }

    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), pipe)) {
        pclose(pipe);
        return -1;
    }

    int status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        memset(buf, 0, sizeof(buf));
        return -1;
    }

    size_t len = strcspn(buf, "\r\n");
    buf[len] = '\0';
    if (buf[0] == '\0') {
        memset(buf, 0, sizeof(buf));
        return -1;
    }

    snprintf(out, out_sz, "%s", buf);
    memset(buf, 0, sizeof(buf));
    return 0;
}

int main(void) {
    // Build the database path: ~/.local/share/ficli/ficli.db
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME environment variable not set\n");
        return 1;
    }

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/.local/share/ficli/ficli.db", home);

    char key_path[512];
    if (get_key_file_path(key_path, sizeof(key_path)) != 0) {
        fprintf(stderr, "Failed to resolve encryption key file path\n");
        return 1;
    }

    char key[256] = {0};
    char saved_key[256] = {0};
    bool has_saved_key =
        (read_key_file(key_path, saved_key, sizeof(saved_key)) == 0);
    const char *prompt_error = NULL;

    ui_init();

    sqlite3 *db = NULL;
    if (read_key_from_1password(key, sizeof(key)) == 0) {
        db = db_init(db_path, key);
        if (!db) {
            prompt_error =
                "1Password password failed. Enter a replacement.";
        }
    }

    if (has_saved_key) {
        if (!db) {
            snprintf(key, sizeof(key), "%s", saved_key);
            db = db_init(db_path, key);
        }
        if (!db) {
            prompt_error = "Saved password failed. Enter a replacement.";
        }
    }

    while (!db) {
        if (!ui_prompt_encryption_password(prompt_error, key, sizeof(key))) {
            ui_cleanup();
            memset(key, 0, sizeof(key));
            return 1;
        }
        db = db_init(db_path, key);
        if (!db) {
            prompt_error = "Unable to unlock database with that password.";
            continue;
        }

        if (write_key_file(key_path, key) != 0) {
            ui_cleanup();
            db_close(db);
            memset(key, 0, sizeof(key));
            fprintf(stderr, "Failed to write key file: %s\n", key_path);
            return 1;
        }
    }

    memset(key, 0, sizeof(key));
    memset(saved_key, 0, sizeof(saved_key));

    if (!db) {
        ui_cleanup();
        return 1;
    }

    ui_run(db);
    ui_cleanup();

    db_close(db);
    return 0;
}
