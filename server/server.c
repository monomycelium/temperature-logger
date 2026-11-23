#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 6767
#define PROFILES "profiles"
#define ACTIVE_FILE PROFILES "/.active"
#define LEGACY_FILE "temperature.csv"
#define MAX_NAME 64
#define PATH_SIZE 128
#define BUFFER_SIZE 8192
#define MAX_BODY 256

static char active[MAX_NAME + 1] = "default";

static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > MAX_NAME) return 0;
    for (; *s; ++s) {
        if (!((*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
            return 0;
    }
    return 1;
}

static void profile_path(char path[PATH_SIZE], const char *name)
{
    snprintf(path, PATH_SIZE, PROFILES "/%s.csv", name);
}

// Open only ordinary files, never follow a symbolic link.
static FILE *open_profile(const char *name, int writing)
{
    char path[PATH_SIZE];
    profile_path(path, name);
    int flags = writing ? O_WRONLY | O_CREAT | O_APPEND : O_RDONLY;
    int fd = open(path, flags | O_NOFOLLOW | O_NONBLOCK, 0640);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    FILE *f = fdopen(fd, writing ? "a" : "r");
    if (!f) {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return f;
}

// Never truncate an existing profile. A new one gets a CSV header.
static int ensure_profile(const char *name)
{
    FILE *f = open_profile(name, 1);
    if (!f) return -1;
    struct stat st;
    int failed = fstat(fileno(f), &st) < 0;
    if (!failed && st.st_size == 0)
        if (fputs("timestamp,temperature\n", f) == EOF) failed = 1;
    if (fclose(f) == EOF) failed = 1;
    return failed ? -1 : 0;
}

// Atomically replace the active-profile file; only change RAM on success.
static int select_profile(const char *name)
{
    if (ensure_profile(name) < 0) return -1;
    char tmp[] = PROFILES "/.active.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    int failed = fprintf(f, "%s\n", name) < 0;
    if (fflush(f) == EOF || fsync(fd) < 0) failed = 1;
    if (fclose(f) == EOF) failed = 1;
    if (failed || rename(tmp, ACTIVE_FILE) < 0) {
        unlink(tmp);
        return -1;
    }
    snprintf(active, sizeof(active), "%s", name);
    printf("[PROFILE] Active: %s\n", active);
    return 0;
}

static int init_profiles(void)
{
    if (mkdir(PROFILES, 0750) < 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(PROFILES, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }

    // Migrate the original logger's CSV if default.csv does not exist.
    // A hard link followed by unlink avoids overwriting existing data.
    if (lstat(LEGACY_FILE, &st) == 0) {
        if (!S_ISREG(st.st_mode)) { errno = EINVAL; return -1; }
        char dest[PATH_SIZE];
        profile_path(dest, "default");
        if (link(LEGACY_FILE, dest) == 0) {
            if (unlink(LEGACY_FILE) < 0) {
                perror("unlink legacy CSV (both names retained)");
            } else {
                printf("[PROFILE] Migrated %s to %s\n", LEGACY_FILE, dest);
            }
        } else if (errno != EEXIST) {
            return -1;
        } else {
            fprintf(stderr, "[PROFILE] Legacy CSV left untouched: default already exists\n");
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    FILE *f = fopen(ACTIVE_FILE, "r");
    if (!f) {
        if (errno != ENOENT) return -1;
        return select_profile("default");
    }
    char name[MAX_NAME + 3];
    int ok = fgets(name, sizeof(name), f) != NULL;
    if (ok) {
        name[strcspn(name, "\r\n")] = '\0';
        ok = valid_name(name) && fgetc(f) == EOF && !ferror(f);
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "[PROFILE] Invalid .active file; refusing to guess\n");
        errno = EINVAL;
        return -1;
    }
    return select_profile(name);
}

static int log_temperature(double value)
{
    time_t now = time(NULL);
    struct tm utc;
    char timestamp[32];
    if (now == (time_t)-1 || !gmtime_r(&now, &utc) ||
        !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc))
        return -1;
    if (ensure_profile(active) < 0) return -1;
    FILE *f = open_profile(active, 1);
    if (!f) return -1;
    int failed = fprintf(f, "%s,%.2f\n", timestamp, value) < 0;
    if (fclose(f) == EOF) failed = 1;
    if (failed) return -1;
    printf("[LOG] %s | %s | %.2f C\n", active, timestamp, value);
    return 0;
}

static const char *skip_ws(const char *s)
{
    return s + strspn(s, " \t\r\n");
}

// Deliberately accepts only a JSON object with one numeric temperature key.
static int parse_temperature(const char *p, double *out)
{
    p = skip_ws(p);
    if (*p++ != '{') return -1;
    p = skip_ws(p);
    if (strncmp(p, "\"temperature\"", 13) != 0) return -1;
    p = skip_ws(p + 13);
    if (*p++ != ':') return -1;
    p = skip_ws(p);
    const char *start = p;
    if (*p == '-') ++p;
    if (*p == '0') ++p;
    else {
        if (*p < '1' || *p > '9') return -1;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p == '.') {
        ++p;
        if (!isdigit((unsigned char)*p)) return -1;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (!isdigit((unsigned char)*p)) return -1;
        while (isdigit((unsigned char)*p)) ++p;
    }
    char *end;
    errno = 0;
    double value = strtod(start, &end);
    if (errno == ERANGE || end != p || !(value >= -55 && value <= 125)) return -1;
    p = skip_ws(p);
    if (*p++ != '}' || *skip_ws(p) != '\0') return -1;
    *out = value;
    return 0;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_header(int fd, const char *status, const char *type,
                       uintmax_t length, const char *extra)
{
    char h[512];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %" PRIuMAX "\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
        status, type, length, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof(h)) return -1;
    return send_all(fd, h, (size_t)n);
}

static void reply(int fd, const char *status, const char *type, const char *body)
{
    size_t n = strlen(body);
    if (send_header(fd, status, type, n, NULL) == 0)
        send_all(fd, body, n);
}

static void text(int fd, const char *status, const char *body)
{
    reply(fd, status, "text/plain; charset=utf-8", body);
}

// Stream CSV directly from disk instead of allocating a whole-file buffer.
static void download_profile(int fd, const char *name)
{
    FILE *f = open_profile(name, 0);
    if (!f) {
        if (errno == ENOENT) text(fd, "404 Not Found", "Profile not found\n");
        else text(fd, "500 Internal Server Error", "Cannot open profile\n");
        return;
    }
    struct stat st;
    if (fstat(fileno(f), &st) < 0 || st.st_size < 0) {
        fclose(f);
        text(fd, "500 Internal Server Error", "Cannot read profile\n");
        return;
    }
    char disposition[160];
    snprintf(disposition, sizeof(disposition),
             "Content-Disposition: attachment; filename=\"%s.csv\"\r\n", name);
    uintmax_t remaining = (uintmax_t)st.st_size;
    if (send_header(fd, "200 OK", "text/csv; charset=utf-8", remaining, disposition) == 0) {
        char buf[4096];
        while (remaining) {
            size_t want = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
            size_t n = fread(buf, 1, want, f);
            if (!n || send_all(fd, buf, n) < 0) break;
            remaining -= n;
        }
    }
    fclose(f);
}

// Read only the tail of the active CSV (not the whole log).
// Returns 1 if a reading exists, 0 for an empty profile, -1 on error.
static int latest_temperature(const char *name, char timestamp[32], double *value)
{
    FILE *f = open_profile(name, 0);
    if (!f) return -1;

    if (fseeko(f, 0, SEEK_END) < 0) { fclose(f); return -1; }
    off_t size = ftello(f);
    if (size < 0) { fclose(f); return -1; }
    if (size == 0) { fclose(f); return 0; }

    // A normal reading is ~30 bytes; allow up to 255 for robustness.
    char tail[257];
    size_t count = (uintmax_t)size < sizeof(tail) - 1 ? (size_t)size : sizeof(tail) - 1;
    if (fseeko(f, size - (off_t)count, SEEK_SET) < 0 ||
        fread(tail, 1, count, f) != count) {
        fclose(f);
        return -1;
    }
    fclose(f);

    // Trim final newlines, then find the beginning of the last row.
    size_t end = count;
    while (end && (tail[end - 1] == '\n' || tail[end - 1] == '\r')) --end;
    if (!end) return 0;

    size_t start = end;
    while (start && tail[start - 1] != '\n') --start;
    if (start == 0 && (uintmax_t)size > count) return -1; // Oversized row
    size_t length = end - start;
    if (length >= sizeof(tail)) return -1;
    tail[end] = '\0'; // One extra byte is reserved for this terminator.
    const char *row = tail + start;

    if (!strcmp(row, "timestamp,temperature")) return 0;

    int consumed = 0;
    if (sscanf(row, "%31[0-9TZ:.-],%lf%n", timestamp, value, &consumed) != 2 ||
        consumed != (int)length || strlen(timestamp) != 20 ||
        !(*value >= -55.0 && *value <= 125.0))
        return -1;
    return 1;
}

static void get_latest_temperature(int fd)
{
    char timestamp[32];
    double temperature;
    int result = latest_temperature(active, timestamp, &temperature);
    if (result < 0) {
        text(fd, "500 Internal Server Error", "Could not read latest temperature\n");
    } else if (result == 0) {
        text(fd, "404 Not Found", "No readings in active profile\n");
    } else {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"profile\":\"%s\",\"timestamp\":\"%s\",\"temperature\":%.2f}\n",
                 active, timestamp, temperature);
        reply(fd, "200 OK", "application/json", json);
    }
}

static void list_profiles(int fd)
{
    DIR *dir = opendir(PROFILES);
    if (!dir) { text(fd, "500 Internal Server Error", "Cannot list profiles\n"); return; }
    char *json = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&json, &size);
    if (!f) {
        closedir(dir);
        text(fd, "500 Internal Server Error", "Cannot list profiles\n");
        return;
    }
    int failed = fprintf(f, "{\"active\":\"%s\",\"profiles\":[", active) < 0;
    int first = 1;
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { if (errno) failed = 1; break; }
        size_t n = strlen(ent->d_name);
        if (n <= 4 || n > MAX_NAME + 4 || strcmp(ent->d_name + n - 4, ".csv")) continue;
        char name[MAX_NAME + 1];
        memcpy(name, ent->d_name, n - 4);
        name[n - 4] = '\0';
        if (!valid_name(name)) continue;
        struct stat st;
        if (fstatat(dirfd(dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISREG(st.st_mode)) continue;
        if (fprintf(f, "%s\"%s\"", first ? "" : ",", name) < 0) failed = 1;
        first = 0;
    }
    closedir(dir);
    if (fputs("]}\n", f) == EOF) failed = 1;
    if (fclose(f) == EOF) failed = 1;
    if (failed) text(fd, "500 Internal Server Error", "Cannot list profiles\n");
    else if (send_header(fd, "200 OK", "application/json", size, NULL) == 0)
        send_all(fd, json, size);
    free(json);
}

static void dispatch(int fd, const char *method, const char *path, char *body)
{
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/")) {
            text(fd, "200 OK", "Temperature logger\nGET /temperature - latest reading (JSON)\nGET /temperature.csv - download active CSV\nGET /profiles - list profiles\nGET /profile - active profile\nGET /profiles/NAME.csv - download named CSV\nPOST /profile - select profile (plain text name)\nPOST /temperature - record JSON temperature\n");
        } else if (!strcmp(path, "/temperature.csv")) {
            download_profile(fd, active);
        } else if (!strcmp(path, "/profiles")) {
            list_profiles(fd);
        } else if (!strcmp(path, "/profile")) {
            char result[MAX_NAME + 2];
            snprintf(result, sizeof(result), "%s\n", active);
            text(fd, "200 OK", result);
        } else if (!strncmp(path, "/profiles/", 10)) {
            const char *p = path + 10;
            size_t n = strlen(p);
            if (n <= 4 || n > MAX_NAME + 4 || strcmp(p + n - 4, ".csv")) {
                text(fd, "404 Not Found", "Not found\n"); return;
            }
            char name[MAX_NAME + 1];
            memcpy(name, p, n - 4);
            name[n - 4] = '\0';
            if (!valid_name(name)) text(fd, "400 Bad Request", "Invalid profile name\n");
            else download_profile(fd, name);
        } else if (!strcmp(path, "/temperature")) {
            get_latest_temperature(fd);
        } else text(fd, "404 Not Found", "Not found\n");
        return;
    }
    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/temperature")) {
            double value;
            if (parse_temperature(body, &value) < 0) {
                text(fd, "400 Bad Request", "Invalid temperature\n"); return;
            }
            if (log_temperature(value) < 0) {
                perror("log temperature");
                text(fd, "500 Internal Server Error", "Failed to store reading\n"); return;
            }
            text(fd, "200 OK", "OK\n");
        } else if (!strcmp(path, "/profile")) {
            // Accept a plain text name, optionally surrounded by whitespace.
            char *name = body + strspn(body, " \t\r\n");
            size_t n = strlen(name);
            while (n && strchr(" \t\r\n", name[n - 1])) name[--n] = '\0';
            if (!valid_name(name)) {
                text(fd, "400 Bad Request", "Use 1-64 letters, digits, underscores or hyphens\n"); return;
            }
            if (select_profile(name) < 0) {
                perror("select profile");
                text(fd, "500 Internal Server Error", "Failed to select profile\n"); return;
            }
            char result[MAX_NAME + 2];
            snprintf(result, sizeof(result), "%s\n", active);
            text(fd, "200 OK", result);
        } else text(fd, "404 Not Found", "Not found\n");
        return;
    }
    if (send_header(fd, "405 Method Not Allowed", "text/plain", 19, "Allow: GET, POST\r\n") == 0)
        send_all(fd, "Method not allowed\n", 19);
}

static void handle_client(int fd)
{
    char buf[BUFFER_SIZE + 1];
    size_t used = 0;
    char *header_end = NULL;
    while (used < BUFFER_SIZE) {
        ssize_t n = recv(fd, buf + used, BUFFER_SIZE - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        used += (size_t)n;
        buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }
    if (!header_end) { text(fd, "431 Request Header Fields Too Large", "Headers too large\n"); return; }
    size_t header_length = (size_t)(header_end - buf) + 4;
    char *body = buf + header_length;
    size_t body_received = used - header_length;
    *header_end = '\0';
    char *line_end = strstr(buf, "\r\n");
    if (line_end) *line_end = '\0';
    char method[16], path[256], version[16], extra;
    if (sscanf(buf, "%15s %255s %15s %c", method, path, version, &extra) != 3) {
        text(fd, "400 Bad Request", "Invalid request\n"); return;
    }
    if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) {
        text(fd, "505 HTTP Version Not Supported", "Unsupported HTTP version\n"); return;
    }
    size_t content_length = 0;
    int has_length = 0;
    for (char *line = line_end ? line_end + 2 : NULL; line && *line;) {
        char *next = strstr(line, "\r\n");
        if (next) *next = '\0';
        char *colon = strchr(line, ':');
        if (!colon) { text(fd, "400 Bad Request", "Invalid header\n"); return; }
        *colon = '\0';
        char *value = colon + 1 + strspn(colon + 1, " \t");
        if (!strcasecmp(line, "Content-Length")) {
            if (has_length || !isdigit((unsigned char)*value)) {
                text(fd, "400 Bad Request", "Invalid Content-Length\n"); return;
            }
            char *end;
            errno = 0;
            unsigned long length = strtoul(value, &end, 10);
            end += strspn(end, " \t");
            if (errno == ERANGE || *end) { text(fd, "400 Bad Request", "Invalid Content-Length\n"); return; }
            if (length > MAX_BODY) { text(fd, "413 Content Too Large", "Body too large\n"); return; }
            content_length = (size_t)length;
            has_length = 1;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            text(fd, "501 Not Implemented", "Transfer-Encoding unsupported\n"); return;
        } else if (!strcasecmp(line, "Expect")) {
            text(fd, "417 Expectation Failed", "Expect unsupported\n"); return;
        }
        line = next ? next + 2 : NULL;
    }
    if (!strcmp(method, "POST") && !has_length) {
        text(fd, "411 Length Required", "Content-Length required\n"); return;
    }
    if (header_length + content_length > BUFFER_SIZE) {
        text(fd, "413 Content Too Large", "Request too large\n"); return;
    }
    while (body_received < content_length) {
        ssize_t n = recv(fd, body + body_received, content_length - body_received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        body_received += (size_t)n;
    }
    if (memchr(body, '\0', content_length)) {
        text(fd, "400 Bad Request", "Invalid body\n"); return;
    }
    body[content_length] = '\0';
    printf("[HTTP] %s %s\n", method, path);
    dispatch(fd, method, path, body);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (init_profiles() < 0) { perror("init profiles"); return EXIT_FAILURE; }
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return EXIT_FAILURE; }
    int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt"); close(server); return EXIT_FAILURE;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 8) < 0) {
        perror("bind/listen"); close(server); return EXIT_FAILURE;
    }
    printf("[SERVER] Listening on 0.0.0.0:%d\n", PORT);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("client timeout"); close(client); continue;
        }
        handle_client(client);
        close(client);
    }
    close(server);
    return EXIT_FAILURE;
}
