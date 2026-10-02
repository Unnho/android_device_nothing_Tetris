/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * chargingctl: command line front end for the chargingd daemon.
 *
 * It only sets and reads persist.vendor.charging.* (plus the compact
 * vendor.charging.state snapshot), so it never touches the kernel nodes
 * itself and cannot fight LineageOS ChargingControl.
 *
 * Deliberately bionic-only: shell may execute this vendor binary, but it
 * may not open files out of /vendor/lib64, so the tool must not link
 * libbase/liblog/libc++.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/system_properties.h>

static const char *kStateProp = "vendor.charging.state";

static const char *kConfigProps[] = {
    "persist.vendor.charging.profile",
    "persist.vendor.charging.soc_limit",
    "persist.vendor.charging.soc_resume",
    "persist.vendor.charging.temp_limit_mC",
    "persist.vendor.charging.temp_stop_mC",
    "persist.vendor.charging.superfast_minutes",
    "persist.vendor.charging.superfast_deadline",
    "persist.vendor.charging.superfast_prev",
    "persist.vendor.charging.fcc_slow_ma",
    "persist.vendor.charging.fcc_fast_ma",
    "persist.vendor.charging.fcc_superfast_ma",
    "persist.vendor.charging.voltage_mode",
    "persist.vendor.charging.enabled",
    NULL
};

static void usage(void)
{
    fprintf(stderr,
            "usage: chargingctl <command> [args]\n"
            "\n"
            "  status                          show configuration and live state\n"
            "  profile <slow|fast|superfast> [minutes]\n"
            "                                  select the charge wattage profile\n"
            "                                  (superfast reverts after [minutes],\n"
            "                                  default 30, 0 = until changed)\n"
            "  soc <0-100>                     stop charging at <0-100>%% with\n"
            "                                  hysteresis (0 disables the limit)\n"
            "  resume <1-99>                   resume floor for the SOC limit\n"
            "                                  (default: limit - 5)\n"
            "  temp <limit_mC> [stop_mC]       soft gate / hard stop temperature\n"
            "                                  in millidegrees Celsius\n"
            "  timeout <minutes>               super-fast window length (0 = none)\n"
            "  enable <0|1>                    pause or resume all control\n"
            "  reset                           back to defaults\n");
}

static int get_prop(const char *name, char *out, size_t out_len)
{
    int len = __system_property_get(name, out);
    if (len <= 0) {
        if (out_len > 0)
            out[0] = '\0';
        return 0;
    }
    if ((size_t)len >= out_len) {
        out[out_len - 1] = '\0';
        return (int)(out_len - 1);
    }
    out[len] = '\0';
    return len;
}

static int set_prop(const char *name, const char *value)
{
    if (__system_property_set(name, value) != 0) {
        fprintf(stderr, "chargingctl: failed to set %s (SELinux denial?)\n", name);
        return 0;
    }
    printf("%s = %s\n", name, value);
    return 1;
}

static int set_int(const char *name, long value)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%ld", value);
    return set_prop(name, buf);
}

static int parse_long(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0')
        return 0;
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0')
        return 0;
    if (value < min || value > max)
        return 0;
    *out = value;
    return 1;
}

static void print_state(void)
{
    char state[PROP_VALUE_MAX];
    char deadline_buf[PROP_VALUE_MAX];
    char *field;
    long deadline;

    get_prop(kStateProp, state, sizeof(state));
    if (state[0] == '\0') {
        printf("  (chargingd is not running)\n");
        return;
    }

    for (field = strtok(state, ";"); field != NULL; field = strtok(NULL, ";")) {
        char *value = strchr(field, '=');

        if (value == NULL) {
            printf("  %s\n", field);
            continue;
        }
        *value = '\0';
        printf("  %-22s %s\n", field, value + 1);
    }

    get_prop("persist.vendor.charging.superfast_deadline", deadline_buf,
             sizeof(deadline_buf));
    deadline = strtol(deadline_buf, NULL, 10);
    if (deadline > (long)time(NULL)) {
        long left = deadline - (long)time(NULL);

        printf("  %-22s %ldm%02lds\n", "superfast_remaining", left / 60, left % 60);
    }
}

static void print_status(void)
{
    char buf[PROP_VALUE_MAX];
    int i;

    printf("== configuration ==\n");
    for (i = 0; kConfigProps[i] != NULL; i++) {
        get_prop(kConfigProps[i], buf, sizeof(buf));
        printf("  %-38s %s\n", kConfigProps[i], buf[0] != '\0' ? buf : "-");
    }

    printf("== state ==\n");
    print_state();
}

int main(int argc, char **argv)
{
    const char *cmd;
    long value;
    long value2;

    if (argc < 2) {
        usage();
        return 2;
    }
    cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        print_status();
        return 0;
    }

    if (strcmp(cmd, "profile") == 0) {
        int ok;

        if (argc < 3) {
            usage();
            return 2;
        }
        if (strcmp(argv[2], "slow") != 0 && strcmp(argv[2], "fast") != 0 &&
            strcmp(argv[2], "superfast") != 0) {
            fprintf(stderr, "chargingctl: profile must be slow, fast or superfast\n");
            return 2;
        }
        ok = set_prop("persist.vendor.charging.profile", argv[2]);
        if (ok && argc >= 4) {
            if (!parse_long(argv[3], 0, 24 * 60, &value)) {
                fprintf(stderr, "chargingctl: minutes must be 0..1440\n");
                return 2;
            }
            ok = set_int("persist.vendor.charging.superfast_minutes", value);
            /* Restart the window with the new duration. */
            if (ok)
                ok = set_int("persist.vendor.charging.superfast_deadline", 0);
        }
        return ok ? 0 : 1;
    }

    if (strcmp(cmd, "soc") == 0) {
        if (!parse_long(argc >= 3 ? argv[2] : NULL, 0, 100, &value)) {
            fprintf(stderr, "chargingctl: soc needs a value between 0 and 100\n");
            return 2;
        }
        return set_int("persist.vendor.charging.soc_limit", value) ? 0 : 1;
    }

    if (strcmp(cmd, "resume") == 0) {
        if (!parse_long(argc >= 3 ? argv[2] : NULL, 1, 99, &value)) {
            fprintf(stderr, "chargingctl: resume needs a value between 1 and 99\n");
            return 2;
        }
        return set_int("persist.vendor.charging.soc_resume", value) ? 0 : 1;
    }

    if (strcmp(cmd, "temp") == 0) {
        if (!parse_long(argc >= 3 ? argv[2] : NULL, 10000, 80000, &value)) {
            fprintf(stderr, "chargingctl: temp limit must be 10000..80000 mC\n");
            return 2;
        }
        if (argc >= 4) {
            if (!parse_long(argv[3], 0, 90000, &value2) || value2 <= value) {
                fprintf(stderr, "chargingctl: temp stop must be above the limit (mC)\n");
                return 2;
            }
            if (!set_int("persist.vendor.charging.temp_limit_mC", value))
                return 1;
            return set_int("persist.vendor.charging.temp_stop_mC", value2) ? 0 : 1;
        }
        return set_int("persist.vendor.charging.temp_limit_mC", value) ? 0 : 1;
    }

    if (strcmp(cmd, "timeout") == 0) {
        int ok;

        if (!parse_long(argc >= 3 ? argv[2] : NULL, 0, 24 * 60, &value)) {
            fprintf(stderr, "chargingctl: timeout must be 0..1440 minutes\n");
            return 2;
        }
        ok = set_int("persist.vendor.charging.superfast_minutes", value);
        if (ok)
            ok = set_int("persist.vendor.charging.superfast_deadline", 0);
        return ok ? 0 : 1;
    }

    if (strcmp(cmd, "enable") == 0) {
        if (!parse_long(argc >= 3 ? argv[2] : NULL, 0, 1, &value)) {
            fprintf(stderr, "chargingctl: enable needs 0 or 1\n");
            return 2;
        }
        return set_int("persist.vendor.charging.enabled", value) ? 0 : 1;
    }

    if (strcmp(cmd, "reset") == 0) {
        int ok = set_prop("persist.vendor.charging.profile", "fast");

        ok = ok && set_int("persist.vendor.charging.soc_limit", 0);
        ok = ok && set_int("persist.vendor.charging.soc_resume", 0);
        ok = ok && set_int("persist.vendor.charging.temp_limit_mC", 40000);
        ok = ok && set_int("persist.vendor.charging.temp_stop_mC", 50000);
        ok = ok && set_int("persist.vendor.charging.superfast_deadline", 0);
        ok = ok && set_prop("persist.vendor.charging.voltage_mode", "cap");
        ok = ok && set_int("persist.vendor.charging.enabled", 1);
        return ok ? 0 : 1;
    }

    usage();
    return 2;
}
