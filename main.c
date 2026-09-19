/*
 * netd — сетевой сторожевой демон CactOS (аналог ifplugd/network monitor).
 *
 * В отличие от networkd (который настраивает интерфейс) netd только следит
 * за состоянием единственной сетевой карты через /dev/net (CACT_NETCTL_NETCFG_GET)
 * и пишет события в журнал: поднятие/падение линка, смену IP/шлюза/DNS/MAC.
 *
 * Запускается супервизором cgoct как /sbin/netd (см. Cgoct-x86_32).
 *
 * /etc/netd.conf (все ключи необязательны; создаётся при первом запуске):
 *   file=/var/log/netd.log  — журнал событий
 *   interval=3              — период опроса линка (сек)
 *   console=0               — дублировать события на консоль
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <ioctl_abi.h>

#define CONFIG_PATH "/etc/netd.conf"
#define LOG_DEFAULT "/var/log/netd.log"

static char log_path[128] = LOG_DEFAULT;
static int  interval_sec  = 3;
static int  console_on    = 0;
static int  out_fd        = -1;

/* Конфиг по умолчанию: пишется при первом запуске, если файла ещё нет. */
static const char default_config[] =
    "# netd config - auto-generated on first start.\n"
    "#\n"
    "# file     - журнал событий\n"
    "# interval - период опроса линка (сек)\n"
    "# console  - дублировать на /dev/console (0|1)\n"
    "\n"
    "file=/var/log/netd.log\n"
    "interval=3\n"
    "console=0\n";

static void ensure_dir(const char *path) {
    (void)mkdir(path, 0755);
}

static void config_write_default(void) {
    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;
    write(fd, default_config, sizeof(default_config) - 1);
    close(fd);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_write_default();
        f = fopen(CONFIG_PATH, "r");
        if (!f) return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';
        if (strcmp(key, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(key, "interval") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 3600) interval_sec = v;
        } else if (strcmp(key, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        }
    }
    fclose(f);
}

static void log_event(const char *msg) {
    if (out_fd >= 0) {
        write(out_fd, msg, strlen(msg));
    }
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, msg, strlen(msg));
            close(cfd);
        }
    }
}

static int net_get(cact_netcfg_get_t *g) {
    int fd = open("/dev/net", O_RDWR);
    if (fd < 0) return -1;
    memset(g, 0, sizeof(*g));
    int r = ioctl(fd, CACT_NETCTL_NETCFG_GET, g);
    close(fd);
    return r;
}

static void fmt_ip4(uint32_t v, char *buf, int cap) {
    snprintf(buf, (size_t)cap, "%u.%u.%u.%u",
             (unsigned)((v >> 24) & 0xFF), (unsigned)((v >> 16) & 0xFF),
             (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
}

/* Разница предыдущего и текущего состояния → одна строка в лог. */
static void report_change(const cact_netcfg_get_t *prev,
                          const cact_netcfg_get_t *cur) {
    char line[160];
    char a[16], b[16];

    if (prev->link_up != cur->link_up) {
        if (cur->link_up) {
            snprintf(line, sizeof(line),
                     "netd: link up, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                     cur->mac[0], cur->mac[1], cur->mac[2],
                     cur->mac[3], cur->mac[4], cur->mac[5]);
        } else {
            snprintf(line, sizeof(line), "netd: link down\n");
        }
        log_event(line);
        printf("%s", line);
        return;
    }
    if (!cur->link_up) return;

    if (prev->ip_host != cur->ip_host || prev->netmask_host != cur->netmask_host) {
        fmt_ip4(cur->ip_host, a, sizeof(a));
        fmt_ip4(cur->netmask_host, b, sizeof(b));
        snprintf(line, sizeof(line), "netd: addr %s netmask %s\n", a, b);
        log_event(line);
        printf("%s", line);
    }
    if (prev->gateway_host != cur->gateway_host) {
        fmt_ip4(cur->gateway_host, a, sizeof(a));
        snprintf(line, sizeof(line), "netd: gateway %s\n", a);
        log_event(line);
        printf("%s", line);
    }
    if (prev->dns_host != cur->dns_host) {
        fmt_ip4(cur->dns_host, a, sizeof(a));
        snprintf(line, sizeof(line), "netd: dns %s\n", a);
        log_event(line);
        printf("%s", line);
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("netd: starting\n");
    config_load();
    ensure_dir("/var/log");

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("netd: cannot open %s\n", log_path);
    }

    cact_netcfg_get_t prev;
    memset(&prev, 0, sizeof(prev));

    for (;;) {
        cact_netcfg_get_t cur;
        if (net_get(&cur) == 0) {
            if (prev.link_up != cur.link_up ||
                prev.ip_host != cur.ip_host ||
                prev.netmask_host != cur.netmask_host ||
                prev.gateway_host != cur.gateway_host ||
                prev.dns_host != cur.dns_host) {
                report_change(&prev, &cur);
            }
            prev = cur;
        } else {
            char line[64];
            snprintf(line, sizeof(line), "netd: /dev/net unavailable\n");
            log_event(line);
        }
        sleep((unsigned int)interval_sec);
    }
    return 0;
}
