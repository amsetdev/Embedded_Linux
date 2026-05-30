/*
 * pi_terminal_agent.c
 *
 * MQTT remote terminal agent for STM32MP1
 * Features a PERSISTENT shell session — cd, export, etc. work correctly.
 *
 * Cross-compile in WSL:
 *   arm-linux-gnueabihf-gcc -o pi_terminal_agent \
 *       pi_terminal_agent.c cJSON.c \
 *       -I. \
 *       -I mosquitto-2.0.18/include \
 *       -static \
 *       mosquitto-2.0.18/lib/libmosquitto_static.a \
 *       -lssl -lcrypto -lpthread -ldl \
 *       -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>

#include <mosquitto.h>
#include "cJSON.h"

/* ─── CONFIG ──────────────────────────────────────────────────────────────── */
#define MQTT_BROKER     "3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT       8883
#define MQTT_USERNAME   "prasad"
#define MQTT_PASSWORD   "prasad#12$A"
#define CLIENT_ID       "stm32-terminal-agent"

#define TOPIC_INPUT     "data/terminal/input"
#define TOPIC_OUTPUT    "data/terminal/output"

#define CA_CERT_FILE    "/etc/ssl/certs/ca-certificates.crt"

#define CMD_TIMEOUT_SEC  30
#define OUTPUT_BUF_SIZE  (64 * 1024)
/* ──────────────────────────────────────────────────────────────────────────── */

/* Blocked command substrings */
static const char *BLOCKED[] = {
    "rm -rf /",
    "mkfs",
    ":(){ :|:& };:",
    NULL
};

static struct mosquitto *mosq = NULL;

/* ── Persistent shell state ──────────────────────────────────────────────── */
typedef struct {
    pid_t  pid;
    int    stdin_fd;   /* we write commands here  */
    int    stdout_fd;  /* we read output from here */
    int    running;
} Shell;

static Shell shell = { .pid = -1, .stdin_fd = -1, .stdout_fd = -1, .running = 0 };
static pthread_mutex_t shell_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Unique sentinel marker to detect end of command output */
#define SENTINEL_FMT  "__END_CMD_%d__"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void get_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
}

static void publish_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        mosquitto_publish(mosq, NULL, TOPIC_OUTPUT,
                          (int)strlen(str), str, 0, false);
        free(str);
    }
}

static void publish_status(const char *message)
{
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",      "status");
    cJSON_AddStringToObject(root, "message",   message);
    cJSON_AddStringToObject(root, "timestamp", ts);
    publish_json(root);
    cJSON_Delete(root);
}

static void publish_error(const char *command, const char *reason)
{
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",      "error");
    cJSON_AddStringToObject(root, "command",   command);
    cJSON_AddStringToObject(root, "output",    reason);
    cJSON_AddStringToObject(root, "timestamp", ts);
    publish_json(root);
    cJSON_Delete(root);
}

/* ── Persistent shell management ─────────────────────────────────────────── */

static int shell_start(void)
{
    int to_shell[2];    /* parent writes, child reads  (stdin)  */
    int from_shell[2];  /* child writes, parent reads  (stdout) */

    if (pipe(to_shell) < 0 || pipe(from_shell) < 0) {
        perror("[SHELL] pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[SHELL] fork");
        return -1;
    }

    if (pid == 0) {
        /* ── Child: become /bin/sh ── */
        close(to_shell[1]);
        close(from_shell[0]);

        dup2(to_shell[0],   STDIN_FILENO);
        dup2(from_shell[1], STDOUT_FILENO);
        dup2(from_shell[1], STDERR_FILENO);

        close(to_shell[0]);
        close(from_shell[1]);

        execl("/bin/sh", "sh", NULL);
        _exit(1);
    }

    /* ── Parent ── */
    close(to_shell[0]);
    close(from_shell[1]);

    /* Make read end non-blocking */
    int flags = fcntl(from_shell[0], F_GETFL, 0);
    fcntl(from_shell[0], F_SETFL, flags | O_NONBLOCK);

    shell.pid       = pid;
    shell.stdin_fd  = to_shell[1];
    shell.stdout_fd = from_shell[0];
    shell.running   = 1;

    printf("[SHELL] Started pid=%d\n", pid);
    return 0;
}

static void shell_stop(void)
{
    if (shell.pid > 0) {
        kill(shell.pid, SIGTERM);
        waitpid(shell.pid, NULL, 0);
        shell.pid = -1;
    }
    if (shell.stdin_fd  >= 0) { close(shell.stdin_fd);  shell.stdin_fd  = -1; }
    if (shell.stdout_fd >= 0) { close(shell.stdout_fd); shell.stdout_fd = -1; }
    shell.running = 0;
    printf("[SHELL] Stopped\n");
}

/*
 * Run one command in the persistent shell.
 * Strategy:
 *   1. Write:  <command>\n
 *   2. Write:  echo SENTINEL\n   (marks end of output)
 *   3. Read until we see the sentinel line, or timeout.
 */
static char *shell_run(const char *command, int timeout_sec)
{
    static int seq = 0;
    char sentinel[64];
    snprintf(sentinel, sizeof(sentinel), SENTINEL_FMT, ++seq);

    char *output = (char *)calloc(1, OUTPUT_BUF_SIZE);
    if (!output) return NULL;

    /* Write command + sentinel echo */
    char line[4096 + 64];
    int n = snprintf(line, sizeof(line), "%s\necho %s\n", command, sentinel);
    if (write(shell.stdin_fd, line, n) < 0) {
        snprintf(output, OUTPUT_BUF_SIZE, "[ERROR] write to shell failed: %s", strerror(errno));
        return output;
    }

    /* Read output until sentinel */
    size_t total      = 0;
    time_t start      = time(NULL);
    char   rbuf[4096];
    char   linebuf[4096];
    size_t linelen    = 0;
    int    done       = 0;

    while (!done) {
        /* Timeout check */
        if (difftime(time(NULL), start) >= timeout_sec) {
            const char *warn = "\n[ERROR] Command timed out";
            strncat(output, warn, OUTPUT_BUF_SIZE - total - 1);
            /* Kill and restart the shell so it's clean for next command */
            shell_stop();
            shell_start();
            break;
        }

        /* Wait up to 200ms for data */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(shell.stdout_fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int sel = select(shell.stdout_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        ssize_t rd = read(shell.stdout_fd, rbuf, sizeof(rbuf) - 1);
        if (rd <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        rbuf[rd] = '\0';

        /* Process char by char, detect sentinel line */
        for (ssize_t i = 0; i < rd && !done; i++) {
            char c = rbuf[i];
            if (c == '\n') {
                linebuf[linelen] = '\0';

                if (strstr(linebuf, sentinel)) {
                    /* Sentinel found — we're done */
                    done = 1;
                } else {
                    /* Normal output line */
                    size_t out_len = linelen + 1; /* +1 for \n */
                    if (total + out_len < (size_t)(OUTPUT_BUF_SIZE - 1)) {
                        memcpy(output + total, linebuf, linelen);
                        total += linelen;
                        output[total++] = '\n';
                    }
                }
                linelen = 0;
            } else {
                if (linelen < sizeof(linebuf) - 1)
                    linebuf[linelen++] = c;
            }
        }
    }

    /* Trim trailing newline */
    while (total > 0 && (output[total-1] == '\n' || output[total-1] == '\r'))
        output[--total] = '\0';

    if (total == 0)
        strncpy(output, "(no output)", OUTPUT_BUF_SIZE - 1);

    return output;
}

/* ── Command execution thread ────────────────────────────────────────────── */

typedef struct {
    char command[4096];
} CmdArg;

static void *execute_thread(void *arg)
{
    CmdArg *ca = (CmdArg *)arg;
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&shell_mutex);
    char *output = shell_run(ca->command, CMD_TIMEOUT_SEC);
    pthread_mutex_unlock(&shell_mutex);

    if (!output) output = strdup("[ERROR] out of memory");

    printf("[OUT] %.200s\n", output);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",      "output");
    cJSON_AddStringToObject(root, "command",   ca->command);
    cJSON_AddStringToObject(root, "output",    output);
    cJSON_AddStringToObject(root, "timestamp", ts);
    publish_json(root);
    cJSON_Delete(root);

    free(output);
    free(ca);
    return NULL;
}

/* ── MQTT Callbacks ──────────────────────────────────────────────────────── */

static void on_connect(struct mosquitto *m, void *userdata, int rc)
{
    (void)userdata;
    static const char *rc_msg[] = {
        "Connected successfully",
        "Bad protocol version",
        "Client ID rejected",
        "Broker unavailable",
        "Bad username or password",
        "Not authorized",
    };
    const char *desc = (rc >= 0 && rc <= 5) ? rc_msg[rc] : "Unknown error";
    printf("[MQTT] %s (rc=%d)\n", desc, rc);

    if (rc == 0) {
        mosquitto_subscribe(m, NULL, TOPIC_INPUT, 0);
        printf("[MQTT] Subscribed to: %s\n", TOPIC_INPUT);
        publish_status("STM32MP1 terminal online");
    }
}

static void on_message(struct mosquitto *m, void *userdata,
                        const struct mosquitto_message *msg)
{
    (void)m;
    (void)userdata;

    if (!msg->payload || msg->payloadlen <= 0) return;

    char *raw = strndup((char *)msg->payload, (size_t)msg->payloadlen);
    if (!raw) return;

    /* Trim */
    char *p = raw;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t plen = strlen(p);
    while (plen > 0 && (p[plen-1] == ' ' || p[plen-1] == '\t' ||
                         p[plen-1] == '\r' || p[plen-1] == '\n'))
        p[--plen] = '\0';

    char command[4096] = {0};

    cJSON *json = cJSON_Parse(p);
    if (json) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "cmd");
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
            strncpy(command, item->valuestring, sizeof(command) - 1);
        cJSON_Delete(json);
    } else {
        strncpy(command, p, sizeof(command) - 1);
    }
    free(raw);

    if (command[0] == '\0') return;

    printf("[CMD] %s\n", command);

    /* Block dangerous commands */
    for (int i = 0; BLOCKED[i] != NULL; i++) {
        if (strstr(command, BLOCKED[i])) {
            printf("[BLOCKED] %s\n", command);
            publish_error(command, "[BLOCKED] Dangerous command refused.");
            return;
        }
    }

    CmdArg *ca = (CmdArg *)malloc(sizeof(CmdArg));
    if (!ca) return;
    strncpy(ca->command, command, sizeof(ca->command) - 1);
    ca->command[sizeof(ca->command) - 1] = '\0';

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, execute_thread, ca) != 0) {
        fprintf(stderr, "[ERROR] pthread_create failed\n");
        free(ca);
    }
    pthread_attr_destroy(&attr);
}

static void on_disconnect(struct mosquitto *m, void *userdata, int rc)
{
    (void)m;
    (void)userdata;
    printf("[MQTT] Disconnected (rc=%d) — will reconnect\n", rc);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Start persistent shell first */
    if (shell_start() < 0) {
        fprintf(stderr, "[ERROR] Failed to start shell\n");
        return 1;
    }

    mosquitto_lib_init();

    mosq = mosquitto_new(CLIENT_ID, true, NULL);
    if (!mosq) {
        fprintf(stderr, "[ERROR] mosquitto_new() failed\n");
        shell_stop();
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD);

    int tls_rc = mosquitto_tls_set(mosq, CA_CERT_FILE, NULL, NULL, NULL, NULL);
    if (tls_rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[ERROR] TLS setup failed: %s\n"
                        "        CA file: %s\n",
                mosquitto_strerror(tls_rc), CA_CERT_FILE);
        shell_stop();
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    /* Last-will */
    {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        cJSON *will = cJSON_CreateObject();
        cJSON_AddStringToObject(will, "type",    "status");
        cJSON_AddStringToObject(will, "message", "STM32MP1 terminal offline");
        cJSON_AddStringToObject(will, "timestamp", ts);
        char *will_str = cJSON_PrintUnformatted(will);
        cJSON_Delete(will);
        if (will_str) {
            mosquitto_will_set(mosq, TOPIC_OUTPUT,
                               (int)strlen(will_str), will_str, 0, true);
            free(will_str);
        }
    }

    printf("[*] Connecting to %s:%d ...\n", MQTT_BROKER, MQTT_PORT);

    int rc = mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[ERROR] Connect failed: %s\n", mosquitto_strerror(rc));
        shell_stop();
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_loop_forever(mosq, -1, 1);

    shell_stop();
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}