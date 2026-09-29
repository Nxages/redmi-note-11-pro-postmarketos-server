/* Continuous thermal and charge guard for the always-plugged Linux server.
 * Keeps the battery in a charge window, suspends charging while hot, fails
 * closed on invalid sensors and resumes once they are valid again. Only
 * hardware telemetry is read or written; no network or persistent storage.
 * REDMI_THERMAL_ROOT prefixes every sysfs/procfs path for offline tests. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define STOP_CHARGE_AT 80
#define RESUME_CHARGE_AT 60
#define LOW_BATTERY 15
#define SOFT_BATTERY_HOT 42000
#define SOFT_CHARGER_HOT 55000
#define SOFT_CPU_HOT 80000
#define HARD_BATTERY_HOT 45000
#define HARD_CHARGER_HOT 60000
#define COOL_BATTERY 38000
#define COOL_CHARGER 50000
#define COOL_CPU 70000
#define CRITICAL_BATTERY 55000
#define CRITICAL_SAMPLES 3
#define SAMPLE_SECONDS 10
#define CAP_REFRESH_SAMPLES 60

struct inputs { long battery, cpu, charger, capacity; };
struct state { int charging_allowed, hot, sensor_fault, critical_count; };

static const char *reasons[] = {
    "window_charge", "window_full", "hot", "hard_hot", "sensor_fault",
    "low_battery_override", "critical",
};
enum { R_WINDOW_CHARGE, R_WINDOW_FULL, R_HOT, R_HARD_HOT, R_SENSOR_FAULT,
       R_LOW_BATTERY_OVERRIDE, R_CRITICAL };

static const char *root = "";
static volatile sig_atomic_t stopping;

static void on_signal(int unused) { (void)unused; stopping = 1; }

static void rooted(char *out, size_t size, const char *path) {
    snprintf(out, size, "%s%s", root, path);
}

static long number(const char *path) {
    char full[PATH_MAX];
    rooted(full, sizeof full, path);
    FILE *f = fopen(full, "r");
    long value = LONG_MIN;
    if (f) { if (fscanf(f, "%ld", &value) != 1) value = LONG_MIN; fclose(f); }
    return value;
}

static int put(const char *path, const char *value) {
    char full[PATH_MAX];
    rooted(full, sizeof full, path);
    int fd = open(full, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t size = strlen(value);
    ssize_t n = write(fd, value, size);
    close(fd);
    return n == (ssize_t)size ? 0 : -1;
}

/* Temperature of the thermal zone with the given type, optionally enabling it. */
static long zone(const char *wanted, int enable) {
    char directory[PATH_MAX];
    rooted(directory, sizeof directory, "/sys/class/thermal");
    DIR *d = opendir(directory);
    if (!d) return LONG_MIN;
    struct dirent *entry;
    long result = LONG_MIN;
    while ((entry = readdir(d))) {
        if (strncmp(entry->d_name, "thermal_zone", 12)) continue;
        char path[PATH_MAX + 300], type[128];
        snprintf(path, sizeof path, "%s/%.255s/type", directory, entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int ok = fscanf(f, "%127s", type) == 1;
        fclose(f);
        if (!ok || strcmp(type, wanted)) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/%.255s/mode", entry->d_name);
        if (enable && put(path, "enabled") < 0) break;
        snprintf(path, sizeof path, "/sys/class/thermal/%.255s/temp", entry->d_name);
        result = number(path);
        break;
    }
    closedir(d);
    return result;
}

static int valid(const struct inputs *in) {
    return in->battery >= 0 && in->battery <= 90000 && in->cpu >= 0 && in->cpu <= 150000
        && in->charger >= 0 && in->charger <= 150000
        && in->capacity >= 0 && in->capacity <= 100;
}

/* Pure decision: previous state plus one sample gives the next state. */
static struct state decide(struct state s, const struct inputs *in, int *reason) {
    if (!valid(in)) {
        s.sensor_fault = 1;
        s.charging_allowed = 0;
        *reason = R_SENSOR_FAULT;
        return s;
    }
    s.sensor_fault = 0;
    int soft_hot = in->battery >= SOFT_BATTERY_HOT || in->charger >= SOFT_CHARGER_HOT
        || in->cpu >= SOFT_CPU_HOT;
    int cool = in->battery <= COOL_BATTERY && in->charger <= COOL_CHARGER && in->cpu <= COOL_CPU;
    if (soft_hot) s.hot = 1;
    else if (cool) s.hot = 0;
    s.critical_count = in->battery >= CRITICAL_BATTERY ? s.critical_count + 1 : 0;
    int hard_hot = in->battery >= HARD_BATTERY_HOT || in->charger >= HARD_CHARGER_HOT;
    int window = s.charging_allowed ? in->capacity < STOP_CHARGE_AT
                                    : in->capacity <= RESUME_CHARGE_AT;
    if (s.critical_count >= CRITICAL_SAMPLES) { s.charging_allowed = 0; *reason = R_CRITICAL; }
    else if (hard_hot) { s.charging_allowed = 0; *reason = R_HARD_HOT; }
    else if (s.hot && in->capacity > LOW_BATTERY) { s.charging_allowed = 0; *reason = R_HOT; }
    else if (s.hot) { s.charging_allowed = 1; *reason = R_LOW_BATTERY_OVERRIDE; }
    else { s.charging_allowed = window; *reason = window ? R_WINDOW_CHARGE : R_WINDOW_FULL; }
    return s;
}

static struct inputs sample(void) {
    struct inputs in;
    long battery = number("/sys/class/power_supply/battery/temp");
    in.battery = battery == LONG_MIN ? LONG_MIN : battery * 100;
    in.cpu = zone("mtktscpu", 0);
    in.charger = zone("charger_therm", 0);
    in.capacity = number("/sys/class/power_supply/battery/capacity");
    return in;
}

/* Same conservative caps as the bounded guard, re-asserted periodically. */
static int apply_caps(void) {
    char directory[PATH_MAX];
    rooted(directory, sizeof directory, "/sys/devices/system/cpu/cpufreq");
    DIR *d = opendir(directory);
    if (!d) return -1;
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(d))) {
        if (strncmp(entry->d_name, "policy", 6)) continue;
        char path[512], value[40], full[PATH_MAX + 300];
        long freq, cap = 0;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/%.255s/scaling_available_frequencies", entry->d_name);
        rooted(full, sizeof full, path);
        FILE *f = fopen(full, "r");
        if (!f) { closedir(d); return -1; }
        while (fscanf(f, "%ld", &freq) == 1) if (freq <= 1500000 && freq > cap) cap = freq;
        fclose(f);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/%.255s/scaling_max_freq", entry->d_name);
        long current = number(path);
        if (cap <= 0 || current == LONG_MIN) { closedir(d); return -1; }
        if (current > cap) {
            snprintf(value, sizeof value, "%ld", cap);
            if (put(path, value) < 0) { closedir(d); return -1; }
        }
        count++;
    }
    closedir(d);
    if (put("/proc/ppm/policy/hard_userlimit_max_cpu_freq", "0 1407000") < 0
        || put("/proc/ppm/policy/hard_userlimit_max_cpu_freq", "1 1430000") < 0) return -1;
    return count == 2 ? 0 : -1;
}

static int set_charging(int allowed) {
    const char *path = "/sys/class/power_supply/battery/input_suspend";
    long wanted = allowed ? 0 : 1;
    if (number(path) != wanted && put(path, allowed ? "0" : "1") < 0) return -1;
    return number(path) == wanted ? 0 : -1;
}

static void write_state(long samples, const struct inputs *in, const struct state *s,
                        int reason, int charge_rc, int caps_rc) {
    char path[PATH_MAX], temporary[PATH_MAX + 8];
    rooted(path, sizeof path, "/run/redmi-thermal.state");
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE *f = fopen(temporary, "w");
    if (!f) return;
    fprintf(f, "sample=%ld\nbattery_mC=%ld\ncpu_mC=%ld\ncharger_mC=%ld\ncapacity=%ld\n"
               "charging=%s\nreason=%s\nhot=%d\ncharge_control=%s\ncpu_caps=%s\n",
            samples, in->battery, in->cpu, in->charger, in->capacity,
            s->charging_allowed ? "allowed" : "suspended", reasons[reason], s->hot,
            charge_rc == 0 ? "applied" : "failed", caps_rc == 0 ? "applied" : "failed");
    if (fclose(f) == 0) rename(temporary, path);
}

static void request_poweroff(void) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static int run(int once) {
    struct state s = {0};
    s.charging_allowed = number("/sys/class/power_supply/battery/input_suspend") == 0;
    int caps_rc = -1;
    if (!once) {
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
        zone("mtktscpu", 1); zone("mtktsbattery", 1); zone("mtktscharger", 1);
    }
    for (long samples = 0; !stopping; samples++) {
        if (!once && samples % CAP_REFRESH_SAMPLES == 0) caps_rc = apply_caps();
        struct inputs in = sample();
        int reason;
        s = decide(s, &in, &reason);
        int charge_rc = set_charging(s.charging_allowed);
        write_state(samples, &in, &s, reason, charge_rc, caps_rc);
        if (once) return charge_rc == 0 ? 0 : 1;
        if (reason == R_CRITICAL) request_poweroff();
        for (int i = 0; i < SAMPLE_SECONDS && !stopping; i++) sleep(1);
    }
    /* Leaving the service: charge normally unless heat or sensors say otherwise. */
    if (!s.hot && !s.sensor_fault) set_charging(1);
    return 0;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const char *prefix = getenv("REDMI_THERMAL_ROOT");
    if (prefix) root = prefix;
    if (argc == 9 && !strcmp(argv[1], "--decide")) {
        struct inputs in = { strtol(argv[2], NULL, 10), strtol(argv[3], NULL, 10),
                             strtol(argv[4], NULL, 10), strtol(argv[5], NULL, 10) };
        struct state s = { atoi(argv[6]), atoi(argv[7]), 0, atoi(argv[8]) };
        int reason;
        s = decide(s, &in, &reason);
        printf("allowed=%d hot=%d critical=%d reason=%s\n",
               s.charging_allowed, s.hot, s.critical_count, reasons[reason]);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--once")) return run(1);
    if (argc == 2 && (!strcmp(argv[1], "--daemon") || !strcmp(argv[1], "--monitor"))) return run(0);
    fputs("usage: redmi-thermal-daemon --daemon|--once|--decide B CPU CHG CAP ALLOWED HOT CRIT\n", stderr);
    return 64;
}
