#include "browser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static struct {
    int id;
    browser_state_t state;
    char app[24];
    char url[1024];
    char redirect[128];
    char title[64];
    char *result;
} s = { .state = BROWSER_UNKNOWN };
static int s_next_id = 1;

static bool loopback_redirect(const char *r)
{
    const char *rest = NULL;
    if (!strncmp(r, "http://127.0.0.1:", 17)) rest = r + 17;
    else if (!strncmp(r, "http://localhost:", 17)) rest = r + 17;
    if (!rest) return false;
    /* Digits, then the path or the end: "http://127.0.0.1:80@evil.example/"
       would name a different host. */
    long port = 0;
    int digits = 0;
    while (*rest >= '0' && *rest <= '9' && digits < 6) { port = port * 10 + (*rest++ - '0'); digits++; }
    return digits && port > 0 && port < 65536 && (*rest == 0 || *rest == '/');
}

static bool clean(const char *t)   /* one protocol word: no spaces or control characters */
{
    for (; *t; t++) if ((unsigned char)*t <= ' ') return false;
    return true;
}

static void finish(browser_state_t st)
{
    s.state = st;
    proto_eventf("browser-done %d %s", s.id, st == BROWSER_DONE ? "ok" : "cancelled");
}

int browser_start(const char *app, const char *url, const char *redirect, const char *title, const char **err)
{
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) { *err = "url must be http(s)://"; return 0; }
    if (strlen(url) >= sizeof s.url || !clean(url)) { *err = "url too long or has spaces"; return 0; }
    if (strlen(redirect) >= sizeof s.redirect || !clean(redirect) || !loopback_redirect(redirect)) {
        *err = "redirect must be http://127.0.0.1:<port>/... or http://localhost:<port>/...";
        return 0;
    }
    if (s.state == BROWSER_PENDING) finish(BROWSER_CANCELLED);
    free(s.result);
    s.result = NULL;
    s.id = s_next_id++;
    s.state = BROWSER_PENDING;
    snprintf(s.app, sizeof s.app, "%s", app && *app && clean(app) ? app : "-");
    snprintf(s.url, sizeof s.url, "%s", url);
    snprintf(s.redirect, sizeof s.redirect, "%s", redirect);
    snprintf(s.title, sizeof s.title, "%s", title && *title ? title : "login");
    for (char *p = s.title; *p; p++) if ((unsigned char)*p < ' ') *p = ' ';
    proto_eventf("browser %d %s %s %s %s", s.id, s.app, s.redirect, s.url, s.title);
    return s.id;
}

browser_state_t browser_poll(int id, const char **result)
{
    if (id != s.id) return BROWSER_UNKNOWN;
    if (s.state == BROWSER_DONE && result) *result = s.result ? s.result : "";
    return s.state;
}

void browser_cancel(int id)
{
    if (id == s.id && s.state == BROWSER_PENDING) finish(BROWSER_CANCELLED);
}

bool browser_answer(int id, const char *query)
{
    if (id != s.id || s.state != BROWSER_PENDING) return false;
    if (!strcmp(query, "CANCEL")) { finish(BROWSER_CANCELLED); return true; }
    if (*query == '?') query++;
    s.result = strdup(query);
    finish(BROWSER_DONE);
    return true;
}

bool browser_pending(char *buf, int len)
{
    if (s.state != BROWSER_PENDING) return false;
    snprintf(buf, (size_t)len, "%d %s %s %s %s", s.id, s.app, s.redirect, s.url, s.title);
    return true;
}

void browser_reset(void)
{
    if (s.state == BROWSER_PENDING) finish(BROWSER_CANCELLED);
    /* The answer belongs to the script that asked: the next one can't poll it. */
    free(s.result);
    s.result = NULL;
    s.id = 0;
    s.state = BROWSER_UNKNOWN;
}
