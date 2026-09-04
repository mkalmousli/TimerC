/* parse.c — Time string parsing */
#include "timer.h"

void format_time(int secs, char *out, size_t sz) {
    if (secs < 0) secs = 0;
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (h > 0) snprintf(out, sz, "%02d:%02d:%02d", h, m, s);
    else        snprintf(out, sz, "%02d:%02d", m, s);
}

/* Extract leading switches: d a r f p */
static int extract_switches(const char *s, char *remainder, int rem_sz,
                             ParseResult *out) {
    out->sw_shutdown = out->sw_alert = out->sw_fullscreen = out->sw_pausable = 0;
    const char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    const char *q = p;
    while (*q && strchr("daprfDAPRF", *q)) q++;
    const char *r = q;
    while (*r && isspace((unsigned char)*r)) r++;
    if (!*r || (!isdigit((unsigned char)*r) && tolower((unsigned char)*r) != 'u')) {
        snprintf(remainder, rem_sz, "%s", s);
        return 0;
    }
    for (const char *c = p; c < q; c++) {
        char lc = (char)tolower((unsigned char)*c);
        if (lc == 'd') out->sw_shutdown   = 1;
        if (lc == 'a') out->sw_alert      = 1;
        if (lc == 'r') out->sw_fullscreen = 1;
        if (lc == 'f') out->sw_fullscreen = 1;
        if (lc == 'p') out->sw_pausable   = 1;
    }
    snprintf(remainder, rem_sz, "%s", r);
    return 1;
}

/* Parse "u15:30", "u1520", "u15" → seconds until HH:MM.
 * Also "u:30" — hour omitted, colon still present — means "at minute 30
 * of the current hour, or the next hour if that's already past". */
static int parse_until(const char *s, ParseResult *out) {
    if (tolower((unsigned char)s[0]) != 'u') return 0;
    const char *p = s + 1;
    while (*p && isspace((unsigned char)*p)) p++;

    int hh = -1, mm = 0, colon = 0, di = 0, aci = 0;
    char digits[8] = "", ac[4] = "";
    while (*p && isdigit((unsigned char)*p) && di < 4) digits[di++] = *p++;
    digits[di] = '\0';
    if (*p == ':') {
        colon = 1; p++;
        while (*p && isdigit((unsigned char)*p) && aci < 2) ac[aci++] = *p++;
        ac[aci] = '\0';
    }
    int hour_omitted = (di == 0 && colon && aci > 0);
    if (di == 0 && !hour_omitted) return 0;

    if (hour_omitted) {
        mm = atoi(ac);
    } else if (colon) {
        hh = atoi(digits); mm = aci > 0 ? atoi(ac) : 0;
    } else {
        int val = atoi(digits);
        if (di <= 2)      { hh = val; mm = 0; }
        else if (di == 3) { hh = digits[0]-'0'; mm = atoi(digits+1); }
        else              { hh = atoi(digits)/100; mm = atoi(digits)%100; }
    }
    if (mm < 0 || mm > 59) return 0;
    if (!hour_omitted && (hh < 0 || hh > 23)) return 0;

    time_t now_t = time(NULL);
    struct tm *lt = localtime(&now_t);
    struct tm target = *lt;
    if (hour_omitted) hh = lt->tm_hour;
    target.tm_hour = hh; target.tm_min = mm; target.tm_sec = 0;
    time_t target_t = mktime(&target);
    if (target_t <= now_t) {
        /* Hour was given explicitly: same time tomorrow. Hour omitted
         * ("u:30"): just roll to the next hour at that minute. */
        if (hour_omitted) {
            target.tm_hour += 1;
            target_t = mktime(&target);
        } else {
            target_t += 86400;
        }
    }

    int secs = (int)(target_t - now_t);
    if (secs <= 0) return 0;

    struct tm *rt = localtime(&target_t);
    hh = rt->tm_hour; mm = rt->tm_min;

    out->seconds = secs;
    snprintf(out->until, sizeof(out->until), "%02d:%02d", hh, mm);
    while (*p && isspace((unsigned char)*p)) p++;
    strncpy(out->task, p, sizeof(out->task)-1);
    out->task[sizeof(out->task)-1] = '\0';
    return 1;
}

void parse_time_input(const char *raw, ParseResult *out) {
    memset(out, 0, sizeof(*out));
    out->seconds = -1;
    if (!raw || !raw[0]) return;

    char remainder[MAX_INPUT];
    extract_switches(raw, remainder, sizeof(remainder), out);
    const char *s = remainder;
    while (*s && isspace((unsigned char)*s)) s++;

    if (tolower((unsigned char)*s) == 'u') {
        if (parse_until(s, out)) return;
    }

    /* Colon-separated: MM:SS or HH:MM:SS */
    {
        const char *p = s;
        int parts[3] = {0,0,0}, nparts = 0;
        while (*p && nparts < 3) {
            if (!isdigit((unsigned char)*p)) break;
            int v = 0;
            while (*p && isdigit((unsigned char)*p)) v = v*10 + (*p++ - '0');
            parts[nparts++] = v;
            if (*p == ':') p++; else break;
        }
        if (nparts >= 2) {
            int total = 0;
            if (nparts == 2) {
                if (parts[1] > 59) goto skip_colon;
                total = parts[0]*60 + parts[1];
            } else {
                if (parts[1] > 59 || parts[2] > 59) goto skip_colon;
                total = parts[0]*3600 + parts[1]*60 + parts[2];
            }
            if (total > 0) {
                out->seconds = total;
                while (*p && isspace((unsigned char)*p)) p++;
                strncpy(out->task, p, sizeof(out->task)-1);
                out->task[sizeof(out->task)-1] = '\0';
                return;
            }
        }
    }
skip_colon:;

    /* Standard: [value][unit] [task] */
    const char *p = s;
    if (!isdigit((unsigned char)*p)) return;
    int value = 0;
    while (*p && isdigit((unsigned char)*p)) value = value*10 + (*p++ - '0');
    char unit = 'm';
    if (*p && strchr("hHmMsS", *p)) { unit = (char)tolower((unsigned char)*p); p++; }
    int secs2 = (unit=='h') ? value*3600 : (unit=='m') ? value*60 : value;
    if (secs2 <= 0) return;
    out->seconds = secs2;
    while (*p && isspace((unsigned char)*p)) p++;
    strncpy(out->task, p, sizeof(out->task)-1);
    out->task[sizeof(out->task)-1] = '\0';
}

void get_preview_switches(const char *input,
                          int *sd, int *sa, int *sf, int *sp,
                          char *until_buf, int until_sz) {
    (void)until_sz;
    *sd = *sa = *sf = *sp = 0;
    if (until_buf) until_buf[0] = '\0';
    if (!input || !input[0]) return;
    ParseResult pr;
    parse_time_input(input, &pr);
    *sd = pr.sw_shutdown; *sa = pr.sw_alert;
    *sf = pr.sw_fullscreen; *sp = pr.sw_pausable;
    if (until_buf) snprintf(until_buf, until_sz, "%s", pr.until);
}
