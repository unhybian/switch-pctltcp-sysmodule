// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag
//
// v1.8.0: Fix http_server_stop() race (close socket before join),
//         support negative minutes, restart timer after set,
//         reduce WiFi wait delay from 2s to 0.2s.

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "http_server.h"

/* ================================================================
 * Sysmodule CRT0 overrides - CRITICAL for boot survival
 * ================================================================ */

#define INNER_HEAP_SIZE 0x80000  /* 512 KiB - same as sys-con */

/* ---- CRT0 global overrides ---- */
u32 __nx_applet_type = AppletType_None;  /* 0 - no applet */
u32 __nx_fs_num_sessions = 2;            /* FS sessions for sysmodule */

/* ---- Custom heap ---- */
void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

/* ---- __appInit - initialize all needed services ---- */
Result __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) return rc;

    /* Get firmware version - REQUIRED for libnx version-aware functions */
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    /* NOTE: Removed pscmInitialize() - we no longer use PSC monitoring.
     * PSC with WlanSockets/Nifm dependencies caused the system to freeze
     * on wake from sleep because the PSC scheduler deadlocked waiting
     * for WlanSockets to reinitialize while our module still held stale
     * IPC sessions. Instead, we detect broken networking via health
     * checks in the main loop and reinitialize automatically. */

    rc = fsInitialize();
    if (R_FAILED(rc)) return rc;

    /* Time service - REQUIRED for correct day-of-week calculation.
     * Without this, time(NULL) returns epoch 0 (Thursday 1970-01-01). */
    rc = timeInitialize();
    if (R_FAILED(rc)) {
        /* Non-fatal: pctl day-of-week may be wrong, but module can still run */
    }

    /* DO NOT call smExit() here! We need SM for nifm/pctl later. */
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

/* ---- __appExit ---- */
void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    timeExit();
    smExit();  /* SM is kept alive for the entire process lifetime */
}

/* ---- Constants ---- */
#define PROGRAM_ID  0x010000000000BD23ULL
#define LOG_FILE    "sdmc:/switch/pctltcp-sysmodule/sysmodule.log"
#define MAX_LOG_SIZE (100 * 1024)

/* ---- Logging ---- */
static void rotate_log_if_needed(void) {
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size > MAX_LOG_SIZE) {
            char old[256];
            snprintf(old, sizeof(old), "%s.old", LOG_FILE);
            rename(LOG_FILE, old);
        }
    }
}

void log_msg(const char *msg) {
    rotate_log_if_needed();
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        /* Try all clock sources for reliable timestamps */
        u64 now_posix = 0;
        Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
        }
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);
        }

        if (R_SUCCEEDED(rc) && now_posix > 946684800ULL) {
            TimeCalendarTime cal;
            TimeCalendarAdditionalInfo additional;

            rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            if (R_FAILED(rc)) {
                /* Try again - sometimes first call fails in sysmodule context */
                rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            }

            if (R_SUCCEEDED(rc)) {
                fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                        cal.year, cal.month, cal.day,
                        cal.hour, cal.minute, cal.second, msg);
                fclose(f);
                return;
            }
        }
        /* Fallback: no timestamp */
        fprintf(f, "[?] %s\n", msg);
        fclose(f);
    }
}

static void log_result(const char *ctx, Result rc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s (0x%08X)",
             ctx, R_SUCCEEDED(rc) ? "OK" : "FAILED", (unsigned)rc);
    log_msg(buf);
}

/* ---- IP to string ---- */
static void ip_to_str(u32 ip, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%d.%d.%d.%d",
             (int)((ip >>  0) & 0xFF),
             (int)((ip >>  8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
}

/* ================================================================
 * Network service management
 * ================================================================ */

static bool g_net_up = false;

/* ---- Network init ---- */
static Result net_init(void) {
    Result rc;

    /* Network interface manager - try System type for sysmodule context */
    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        rc = nifmInitialize(NifmServiceType_User);
    }
    if (R_FAILED(rc)) {
        log_result("nifmInitialize", rc);
        return rc;
    }

    /* Sockets - use System service type with explicit config */
    SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x4000,
        .tcp_tx_buf_max_size = 0x10000,
        .tcp_rx_buf_max_size = 0x10000,
        .udp_tx_buf_size = 0x1000,
        .udp_rx_buf_size = 0x4000,
        .sb_efficiency = 2,
        .bsd_service_type = BsdServiceType_System,
    };
    rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        log_result("socketInitialize", rc);
        nifmExit();
        return rc;
    }

    /* HTTP server */
    http_server_start();
    if (!http_server_is_running()) {
        log_msg("HTTP server start FAILED.");
        socketExit();
        nifmExit();
        return -1;
    }

    g_net_up = true;
    log_msg("Network services initialized, HTTP server started.");

    /* Log IP address */
    char ip[64] = {0};
    u32 ipaddr = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ipaddr)) && ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
        char msg[256];
        snprintf(msg, sizeof(msg), "Web UI: http://%s:%d", ip, HTTP_PORT);
        log_msg(msg);
    }

    return 0;
}

/* ---- Network cleanup ---- */
static void net_cleanup(void) {
    if (http_server_is_running()) {
        http_server_stop();
        log_msg("HTTP server stopped.");
    }

    if (g_net_up) {
        socketExit();
        nifmExit();
        log_msg("Network services cleaned up.");
    }
    g_net_up = false;
}

/* ---- HTTP server restart (for sleep/wake recovery) ----
 * Does NOT tear down socket/nifm - in a boot2 sysmodule,
 * once socketExit() is called, we can't re-acquire the
 * bsd service session. So we only restart the HTTP layer.
 *
 * CRITICAL: After sleep/wake, WiFi takes 3-10 seconds to
 * reconnect. If we create a new socket before WiFi is ready,
 * bind() succeeds but the socket is bound to a dead interface
 * → "restarted successfully" but actually unreachable. */

/* ================================================================
 * Main service init - called once at startup
 * ================================================================ */
static bool s_base_ready = false;

static Result init_services(void) {
    /* Create log directory */
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting (v1.8.0 - race fix)...");

    /* Load timezone rule for correct day-of-week calculation.
     * This MUST be called after timeInitialize() (which is in __appInit).
     * Without this, timeToCalendarTimeWithMyRule() may fail in sysmodule
     * context, causing pctl_get_today_day() to return the wrong day. */
    {
        Result tz_rc = pctl_load_timezone();
        if (R_FAILED(tz_rc)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "WARNING: timezone load failed (0x%08X), day-of-week may be wrong", (unsigned)tz_rc);
            log_msg(buf);
        } else {
            log_msg("Timezone rule loaded successfully.");
        }
    }

    /* Log time service status */
    {
        u64 test_time = 0;
        Result time_rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &test_time);
        if (R_FAILED(time_rc)) {
            time_rc = timeGetCurrentTime(TimeType_UserSystemClock, &test_time);
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "Time service: %s (time=%llu, rc=0x%08X)",
                 R_SUCCEEDED(time_rc) ? "OK" : "FAILED",
                 (unsigned long long)test_time, (unsigned)time_rc);
        log_msg(buf);
    }

    s_base_ready = true;
    return 0;
}

/* ================================================================
 * sysmodule entry point
 * ================================================================ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* At boot2, many services are not yet registered with SM.
     * Wait longer for the system to finish initializing services. */
    svcSleepThread(15000000000ULL);  /* 15 seconds */

    /* Initialize base services (time, timezone, etc.) */
    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: Service initialization failed, entering idle loop.");
    }

    /* Initialize network (socket, nifm, HTTP server) with sparse retry. */
    if (s_base_ready) {
        for (int attempt = 0; attempt < 30; attempt++) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) break;
            svcSleepThread(5000000000ULL);  /* 5 seconds */
        }
        if (R_FAILED(rc)) {
            log_msg("WARNING: Network init failed after 30 retries");
        }
    }

    if (R_SUCCEEDED(rc)) {
        pctl_custom_timer_set_limit(0);  /* 初始化自定义计时器，无限制 */
        pctl_custom_timer_reset();
        log_msg("Custom timer initialized.");
    }

    log_msg("pctltcp-sysmodule initialization complete.");

    /* ---- Main loop ----
     *
     * No PSC monitoring - we use health checks instead.
     *
     * When the system goes to sleep, all threads are suspended.
     * When it wakes up, threads resume but networking may be broken
     * (WlanSockets reinitializes and invalidates old sockets).
     *
     * The HTTP server thread detects broken sockets via select()/accept()
     * errors and exits cleanly. This main loop then detects the server
     * is no longer running and does a full network reinit.
     *
     * Health check strategy:
     * - Every 5 seconds: check if HTTP server is still running
     * - Every 10 seconds: check if nifm is responsive
     * - Every 30 seconds: retry network init if it's down
     * - Every 5 minutes: check for IP address changes
     */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;
    int nifm_fail_count = 0;  /* Consecutive nifm failures */

    while (1) {
        /* ---- Sleep/wake detection ----
         * On Switch, svcSleepThread(1s) suspends the thread. If the system
         * enters sleep mode, the thread stays suspended until wake.
         * By comparing timestamps before & after, we can detect sleep/wake
         * events and force a full network reinit.
         *
         * Skip the first 5 loops (5s) to avoid false positives at boot. */
        u64 t_before = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &t_before);
        svcSleepThread(1000000000ULL);  /* 1 second (could be longer if slept) */
        u64 t_after = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &t_after);
        loop++;

        if (loop > 5 && g_net_up && (t_after - t_before) > 5) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Sleep/wake detected (%llus jump), waiting for WiFi...",
                     (unsigned long long)(t_after - t_before));
            log_msg(msg);
            http_server_restart();
            nifm_fail_count = 0;
            continue;
        }

        /* ---- Health check: HTTP server running? ---- */
        if (g_net_up && (loop % 5 == 0)) {
            if (!http_server_is_running()) {
                log_msg("HTTP server down, reinitializing network...");
                http_server_restart();
                nifm_fail_count = 0;
                continue;
            }
        }

        /* ---- Health check: nifm responsive? ----
         * After sleep/wake, the network interface may be broken even
         * though the HTTP server thread hasn't detected it yet.
         * If nifmGetCurrentIpAddress() fails repeatedly, networking
         * is broken and we need to reinit. */
        if (g_net_up && (loop % 10 == 0)) {
            u32 ipaddr = 0;
            Result nifm_rc = nifmGetCurrentIpAddress(&ipaddr);
            if (R_FAILED(nifm_rc)) {
                nifm_fail_count++;
                if (nifm_fail_count >= 3) {
                    log_msg("nifm unresponsive (3 failures), reinitializing...");
                    http_server_restart();
                    nifm_fail_count = 0;
                    continue;
                }
            } else {
                nifm_fail_count = 0;
            }
        }

        /* ---- Startup retry: if network is not up yet ---- */
        if (!g_net_up && s_base_ready && (loop % 30 == 0)) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) {
                log_msg("Network init succeeded on retry!");
            }
        }

        /* ---- Periodic restart if HTTP died but net is still up ---- */
        if (g_net_up && (loop % 60 == 0) && !http_server_is_running()) {
            log_msg("HTTP server down (periodic check), reinitializing...");
            http_server_restart();
            nifm_fail_count = 0;
        }

        /* ---- Check for IP change every 5 minutes ---- */
        if (g_net_up && (loop - last_ip_check >= 300)) {
            char new_ip[64] = {0};
            u32 a = 0;
            if (R_SUCCEEDED(nifmGetCurrentIpAddress(&a)) && a != 0) {
                ip_to_str(a, new_ip, sizeof(new_ip));
            }
            if (new_ip[0] && strcmp(last_ip, new_ip) != 0) {
                char m[256];
                snprintf(m, sizeof(m), "IP changed: %s -> %s",
                         last_ip[0] ? last_ip : "(none)", new_ip);
                log_msg(m);
                strcpy(last_ip, new_ip);
                {
                    char u[256];
                    snprintf(u, sizeof(u), "Web UI: http://%s:%d", new_ip, HTTP_PORT);
                    log_msg(u);
                }
            }
            last_ip_check = loop;
        }
    }

    /* Unreachable */
    return 0;
}
