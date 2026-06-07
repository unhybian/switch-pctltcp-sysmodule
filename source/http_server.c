/**
 * http_server.c - Minimal HTTP server for Switch parental control
 *
 * REST API:
 *   GET  /              -> Embedded HTML UI
 *   GET  /api/status    -> JSON: {daily_limit_min, remaining_min, played_min, today, today_name, version, custom_played, custom_remaining}
 *   POST /api/allow     -> Add minutes to today's limit (additive)
 *                          body: minutes=N
 *                          calc: new_limit = current_limit + N
 *   POST /api/set       -> Set exact minutes for today's limit
 *                          body: minutes=N
 *   POST /api/reset     -> Reset today's played time to 0
 *   Version: v1.5.1
 */
#include "http_server.h"
#include "pctl_handler.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* Forward declaration — defined in main.c. NOT variadic! */
extern void log_msg(const char *msg);

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static int       s_server_fd = -1;
static bool      s_running   = false;
static pthread_t s_thread;
static volatile int s_generation = 0;  /* bumped on each restart */
static bool      s_thread_active = false;  /* true after pthread_create, false after join */

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */
static void http_send(int fd, const char *status, const char *ctype, const char *body)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        status, ctype, (int)strlen(body));
    write(fd, header, hlen);
    write(fd, body, strlen(body));
}

static int http_read_request(int fd, char *buf, int bufsize)
{
    int total = 0;
    while (total < bufsize - 1) {
        int n = read(fd, buf + total, bufsize - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* API handlers                                                        */
/* ------------------------------------------------------------------ */
static u32 clamp_remaining_min(u64 remaining_ns)
{
    if (remaining_ns == 0)
        return 0;
    if (remaining_ns > 86400000000000ULL)
        return 0;
    return (u32)NS_TO_MINUTES(remaining_ns);
}

static void api_status(int fd)
{
    u64 remaining_ns = 0;
    u32 daily_limit  = 0;
    u32 remaining_min = 0;
    u32 played_min    = 0;
    int today = 0;
    bool timer_enabled = false;

    Result rc = pctl_init();
    if (R_SUCCEEDED(rc)) {
        pctl_get_remaining_time(&remaining_ns);
        pctl_get_daily_limit_minutes(&daily_limit);
        pctl_is_enabled(&timer_enabled);
        today = pctl_get_today_day();
        pctl_exit();
    }

    /* Use custom timer values when system timer is not providing valid data */
    u64 custom_played_ns  = pctl_custom_timer_get_played_ns();
    u64 custom_limit_ns   = pctl_custom_timer_get_limit_ns();
    u32 custom_played_min = (u32)NS_TO_MINUTES(custom_played_ns);
    u32 custom_remaining_min = 0;
    if (custom_limit_ns > custom_played_ns) {
        custom_remaining_min = (u32)NS_TO_MINUTES(custom_limit_ns - custom_played_ns);
    }

    /* Decide which values to report:
     *  - If system timer is running and remaining_ns > 0, use system values.
     *  - Otherwise use our custom tracked values.
     */
    if (timer_enabled && remaining_ns > 0 && remaining_ns < 86400000000000ULL) {
        remaining_min = clamp_remaining_min(remaining_ns);
        if (daily_limit > remaining_min) {
            played_min = daily_limit - remaining_min;
        }
    } else {
        /* System timer not running — use custom tracked values */
        played_min     = custom_played_min;
        remaining_min  = custom_remaining_min;
        /* Also update daily_limit display to match custom limit */
        daily_limit = (u32)NS_TO_MINUTES(custom_limit_ns);
    }

    char json[512];
    static const char *day_names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(json, sizeof(json),
        "{\"daily_limit_min\":%u,\"remaining_min\":%u,\"played_min\":%u,\"today\":%d,\"today_name\":\"%s\",\"version\":\"v1.5.1\",\"custom_played\":%u,\"custom_remaining\":%u}",
        daily_limit, remaining_min, played_min, today, day_names[today],
        custom_played_min, custom_remaining_min);

    http_send(fd, "200 OK", "application/json", json);
}

static void api_allow(int fd, const char *body)
{
    int allow_min = 0;
    const char *p = strstr(body, "minutes");
    if (p) {
        p = strchr(p + 7, '=');
        if (p) allow_min = atoi(p + 1);
    }

    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        http_send(fd, "200 OK", "application/json", "{\"success\":0,\"error\":\"pctl_init_failed\"}");
        return;
    }

    int today = pctl_get_today_day();

    if (allow_min == 0) {
        rc = pctl_set_day_limit_minutes(today, 0);
    } else {
        u32 daily_limit = 0;
        pctl_get_daily_limit_minutes(&daily_limit);

        /* Additive logic: new_limit = current_limit + allow_min */
        int new_limit = (int)daily_limit + allow_min;
        if (new_limit < 0) new_limit = 0;
        if (new_limit > 1440) new_limit = 1440;

        rc = pctl_set_day_limit_minutes(today, (u32)new_limit);

        /* Also update our custom limit */
        pctl_custom_timer_set_limit((u32)new_limit);
    }

    pctl_exit();

    char json[128];
    snprintf(json, sizeof(json),
        "{\"success\":%d}",
        R_SUCCEEDED(rc) ? 1 : 0);

    http_send(fd, "200 OK", "application/json", json);
}

/* NEW: Set exact limit */
static void api_set(int fd, const char *body)
{
    int set_min = 0;
    const char *p = strstr(body, "minutes");
    if (p) {
        p = strchr(p + 7, '=');
        if (p) set_min = atoi(p + 1);
    }

    if (set_min < 0) set_min = 0;
    if (set_min > 1440) set_min = 1440;

    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        http_send(fd, "200 OK", "application/json", "{\"success\":0,\"error\":\"pctl_init_failed\"}");
        return;
    }

    int today = pctl_get_today_day();
    rc = pctl_set_day_limit_minutes(today, (u32)set_min);

    pctl_exit();

    /* Also update our custom limit */
    pctl_custom_timer_set_limit((u32)set_min);
    /* Reset custom played time when setting a new limit */
    pctl_custom_timer_reset();
    /* Restart custom timer with new limit */
    if (set_min > 0) {
        pctl_custom_timer_start((u32)set_min);
    } else {
        pctl_custom_timer_stop();
    }

    char json[128];
    snprintf(json, sizeof(json),
        "{\"success\":%d}",
        R_SUCCEEDED(rc) ? 1 : 0);

    http_send(fd, "200 OK", "application/json", json);
}

/* NEW: Reset played time */
static void api_reset(int fd)
{
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        http_send(fd, "200 OK", "application/json", "{\"success\":0,\"error\":\"pctl_init_failed\"}");
        return;
    }

    rc = pctl_reset_play_time();

    pctl_exit();

    /* Also reset our custom played time */
    pctl_custom_timer_reset();
    /* Restart timer if there is a limit set */
    u64 limit_ns = pctl_custom_timer_get_limit_ns();
    if (limit_ns > 0) {
        pctl_custom_timer_start((u32)NS_TO_MINUTES(limit_ns));
    }

    char json[128];
    snprintf(json, sizeof(json),
        "{\"success\":%d}",
        R_SUCCEEDED(rc) ? 1 : 0);

    http_send(fd, "200 OK", "application/json", json);
}

/* Embedded Web UI (updated to v1.5.1) */
/* ------------------------------------------------------------------ */
static const char *WEB_HTML =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Switch Timer v1.5.1</title>"
"<style>"
"body{font-family:sans-serif;background:#1a1a2e;color:#fff;text-align:center;padding:20px;margin:0}"
".box{background:rgba(255,255,255,0.1);border-radius:12px;padding:20px;margin:15px 0}"
".big{font-size:2.5em;font-weight:bold;margin:10px 0}"
".lbl{color:rgba(255,255,255,0.6);font-size:0.9em}"
".row{display:flex;gap:10px;justify-content:center;margin:15px 0}"
".tile{flex:1;background:rgba(255,255,255,0.08);border-radius:10px;padding:14px}"
"input{width:90px;font-size:1.5em;text-align:center;padding:8px;border:none;border-radius:8px;background:rgba(255,255,255,0.15);color:#fff}"
".btns{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin:12px 0}"
"button{font-size:1em;padding:10px 18px;border:none;border-radius:8px;background:#3b82f6;color:#fff;cursor:pointer}"
"button:active{transform:scale(0.95)}"
".btn-sm{background:#374151;font-size:0.9em;padding:8px 14px}"
".btn-danger{background:#ef4444}"
"#msg{margin-top:8px;color:#fbbf24;font-size:0.9em;min-height:20px}"
"</style>"
"</head>"
"<body>"
"<h2>Switch Parental Control <small>v1.5.1</small></h2>"
"<div class='box'>"
"<div class='row'>"
"<div class='tile'><div class='lbl'>Played</div><div class='big' id='played'>--</div></div>"
"<div class='tile'><div class='lbl'>Remaining</div><div class='big' id='remain'>--</div></div>"
"</div>"
"<div class='lbl' style='margin-top:4px'>Limit: <span id='limit'>--</span> min</div>"
"</div>"
"<div class='box'>"
"<div class='lbl'>Add minutes (additive)</div>"
"<input type='number' id='min' value='30' min='-300' max='300'>"
"<br>"
"<div class='btns'>"
"<button class='btn-sm' onclick='quickAdd(15)'>+15</button>"
"<button class='btn-sm' onclick='quickAdd(30)'>+30</button>"
"<button class='btn-sm' onclick='quickAdd(60)'>+60</button>"
"<button class='btn-sm' onclick='quickAdd(90)'>+90</button>"
"</div>"
"<div class='btns'>"
"<button class='btn-sm' onclick='quickAdd(-10)'>-10</button>"
"<button class='btn-sm' onclick='quickAdd(-30)'>-30</button>"
"</div>"
"<button onclick='allow()'>Confirm Add</button>"
"</div>"
"<div class='box'>"
"<div class='lbl'>Set exact limit (minutes)</div>"
"<input type='number' id='setmin' value='60' min='0' max='1440'>"
"<br>"
"<button onclick='setExact()'>Set Exact</button>"
"</div>"
"<div class='box'>"
"<button class='btn-danger' onclick='resetTime()'>Reset Played Time</button>"
"<div id='msg'></div>"
"</div>"
"<script>"
"function load(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('limit').textContent=d.daily_limit_min;"
"document.getElementById('remain').textContent=d.remaining_min+'m';"
"document.getElementById('played').textContent=d.played_min+'m';"
"}).catch(()=>{document.getElementById('msg').textContent='Load failed'});"
"}"
"function quickAdd(m){"
"document.getElementById('min').value=m;"
"}"
"function allow(){"
"var m=parseInt(document.getElementById('min').value)||0;"
"document.getElementById('msg').textContent='Saving...';"
"fetch('/api/allow',{method:'POST',body:'minutes='+m}).then(r=>r.json()).then(d=>{"
"document.getElementById('msg').textContent=d.success?'Done!':'Failed';"
"setTimeout(function(){document.getElementById('msg').textContent='';load();},1200);"
"}).catch(()=>{document.getElementById('msg').textContent='Error'});"
"}"
"function setExact(){"
"var m=parseInt(document.getElementById('setmin').value)||0;"
"document.getElementById('msg').textContent='Setting...';"
"fetch('/api/set',{method:'POST',body:'minutes='+m}).then(r=>r.json()).then(d=>{"
"document.getElementById('msg').textContent=d.success?'Done!':'Failed';"
"setTimeout(function(){document.getElementById('msg').textContent='';load();},1200);"
"}).catch(()=>{document.getElementById('msg').textContent='Error'});"
"}"
"function resetTime(){"
"document.getElementById('msg').textContent='Resetting...';"
"fetch('/api/reset',{method:'POST'}).then(r=>r.json()).then(d=>{"
"document.getElementById('msg').textContent=d.success?'Reset!':'Failed';"
"setTimeout(function(){document.getElementById('msg').textContent='';load();},1200);"
"}).catch(()=>{document.getElementById('msg').textContent='Error'});"
"}"
"load();"
"setInterval(load,30000);"
"</script>"
"</body>"
"</html>";

/* ------------------------------------------------------------------ */
/* Route dispatcher                                                    */
/* ------------------------------------------------------------------ */
static void handle_request(int fd)
{
    char buf[2048];
    int n = http_read_request(fd, buf, sizeof(buf));
    if (n <= 0) { close(fd); return; }

    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        http_send(fd, "204 No Content", "text/plain", "");
        close(fd);
        return;
    }

    char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;

    if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0) {
        http_send(fd, "200 OK", "text/html; charset=utf-8", WEB_HTML);
    } else if (strcmp(path, "/api/status") == 0) {
        api_status(fd);
    } else if (strcmp(path, "/api/allow") == 0 && strcmp(method, "POST") == 0) {
        api_allow(fd, body ? body : "");
    } else if (strcmp(path, "/api/set") == 0 && strcmp(method, "POST") == 0) {
        api_set(fd, body ? body : "");
    } else if (strcmp(path, "/api/reset") == 0 && strcmp(method, "POST") == 0) {
        api_reset(fd);
    } else {
        http_send(fd, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/* Server thread                                                       */
/* ------------------------------------------------------------------ */
static void *http_thread_func(void *arg)
{
    (void)arg;
    int gen = s_generation;  /* snapshot at thread start */

    while (s_running) {
        /* If http_restart() happened, our socket is stale — exit immediately */
        if (s_generation != gen) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_server_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int ret = select(s_server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0 || s_generation != gen) {
            s_running = false;
            break;
        }
        if (ret == 0) continue;

        if (FD_ISSET(s_server_fd, &rfds)) {
            /* Double-check: don't accept if we've been restarted */
            if (s_generation != gen) break;
            int client_fd = accept(s_server_fd, NULL, NULL);
            if (client_fd < 0 || s_generation != gen) {
                if (client_fd >= 0) close(client_fd);
                s_running = false;
                break;
            }
            handle_request(client_fd);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void http_server_start(void)
{
    struct sockaddr_in addr;

    /* Always close any leftover socket fd before creating a new one.
     * This handles the case where the HTTP thread exited on its own
     * (select error after sleep) but http_server_stop() was never called. */
    if (s_server_fd >= 0) {
        close(s_server_fd);
        s_server_fd = -1;
    }

    /* If there's an orphaned thread (exited but never joined), join it now. */
    if (s_thread_active) {
        pthread_join(s_thread, NULL);
        s_thread_active = false;
    }

    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0) {
        log_msg("http_server_start: socket() failed");
        return;
    }

    int optval = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("http_server_start: bind() failed");
        close(s_server_fd);
        s_server_fd = -1;
        return;
    }

    if (listen(s_server_fd, 4) < 0) {
        log_msg("http_server_start: listen() failed");
        close(s_server_fd);
        s_server_fd = -1;
        return;
    }

    s_running = true;
    s_generation++;  /* bump so old threads know to exit */

    /* Use explicit stack size for the HTTP server thread.
     * Default pthread stack on Switch/newlib is often too small. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);  /* 64KB */
    pthread_create(&s_thread, &attr, http_thread_func, NULL);
    s_thread_active = true;
    pthread_attr_destroy(&attr);

    log_msg("http_server_start: OK");
}

void http_server_stop(void)
{
    s_running = false;
    s_generation++;  /* 让旧线程的 select() 检测到代际变化后退出 */

    /* 先关 socket，打断 select() 等待，避免死锁 */
    if (s_server_fd >= 0) {
        int fd = s_server_fd;
        s_server_fd = -1;
        close(fd);
    }

    if (s_thread_active) {
        pthread_join(s_thread, NULL);
        s_thread_active = false;
    }
}

bool http_server_is_running(void)
{
    return s_running;
}

void http_server_restart(void)
{
    log_msg("HTTP server restarting...");
    http_server_stop();
    svcSleepThread(500000000ULL);  /* 0.5s */
    http_server_start();
    log_msg("HTTP server restart done.");
}
