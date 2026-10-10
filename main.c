/*
 * SPDX-FileCopyrightText: 2026 esp-claw contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * gh: read-only GitHub through the device's "github" API profile (bearer token).
 *   gh_notifications  unread notifications
 *   gh_prs            open PRs involving me, or of one repo (search API)
 *   gh_issues         issues of a repo
 *   gh_ci             latest GitHub Actions workflow runs
 *   gh_failed_log     jobs/failed steps of a run + tail of the first failed job's log
 *   gh_releases       latest releases
 *   gh_repo           repository overview
 *   gh_set_owner      default owner for "name"-only repo arguments (kv "owner")
 * The device injects the token; the app never sees it. Outputs are compact column/row JSON.
 */
#define CLAW_ARENA_SIZE 1024
#include "claw.h"
#include "claw_util.h"

CLAW_DEFINE_ARENA()

#define PROFILE     "github"
#define OUT_RESERVE 128 /* kept free while writing rows, for the closing text */

static int32_t s_t_notif = -1, s_t_prs = -1, s_t_issues = -1, s_t_ci = -1;
static int32_t s_t_log = -1, s_t_releases = -1, s_t_repo = -1, s_t_owner = -1;

static char s_resp[32 * 1024 + 1];
static uint32_t s_resp_len;
static char s_out[16 * 1024];
static claw_buf_t s_b;
static uint32_t s_mark;
static int s_trunc;
static char s_path[1024];
static claw_buf_t s_p;
static const char *s_args, *s_args_end;

/* ---------- small helpers (no libc) ---------- */

static int str_eq(const char *a, uint32_t an, const char *b)
{
    uint32_t i = 0;

    for (; i < an && b[i] && a[i] == b[i]; i++) {
    }
    return i == an && !b[i];
}

/* ---------- output ---------- */

/* Out of line: the SDK helpers are static inline and would otherwise be inlined at every call. */
#define NOINLINE __attribute__((noinline))

static NOINLINE void P(const char *s)
{
    cb_put(&s_b, s);
}

static NOINLINE void bput(claw_buf_t *b, const char *s)
{
    cb_put(b, s);
}

static NOINLINE void bput_u32(claw_buf_t *b, uint32_t v)
{
    cb_put_u32(b, v);
}

static void out_begin(void)
{
    s_b.buf = s_out;
    s_b.cap = sizeof(s_out) - OUT_RESERVE;
    s_b.len = 0;
    s_b.full = 0;
    s_out[0] = '\0';
    s_trunc = 0;
}

static void out_end(void)
{
    s_b.cap = sizeof(s_out);
    claw_tool_result(s_b.buf, s_b.len);
}

static void row_begin(void)
{
    s_mark = s_b.len;
}

/* Rolls back a row that did not fit; returns 0 then (stop adding rows). */
static int row_end(void)
{
    if (!s_b.full) {
        return 1;
    }
    s_b.len = s_mark;
    s_b.buf[s_mark] = '\0';
    s_b.full = 0;
    s_trunc = 1;
    return 0;
}

static NOINLINE void put_u64(uint64_t v)
{
    char tmp[20];
    char out[20];
    int n = 0;
    int i = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        out[i++] = tmp[--n];
    }
    cb_putn(&s_b, out, (uint32_t)i);
}

static void put_i32(int32_t v)
{
    if (v < 0) {
        P("-");
        v = -v;
    }
    bput_u32(&s_b, (uint32_t)v);
}

/* Appends text from [p, e) as JSON string contents, at most `max` source bytes, cut at a character
 * boundary. Flags: SAFE_JSON = the text is already JSON-escaped (escape sequences are copied whole),
 * otherwise quotes/backslashes are escaped and control bytes/ANSI sequences dropped; SAFE_CUT = cut
 * silently (dates), else "..." marks a cut. Invalid UTF-8 bytes are dropped in all modes. */
#define SAFE_JSON 1
#define SAFE_CUT  2

static NOINLINE void put_safe(const char *p, const char *e, uint32_t max, int flags)
{
    uint32_t used = 0;
    int json = flags & SAFE_JSON;

    while (p < e) {
        unsigned char c = (unsigned char)*p;
        uint32_t unit = 1;

        if (json && c == '\\') {
            unit = p + 1 < e && p[1] == 'u' ? 6 : 2;
            if (unit == 6 && p + 12 <= e && (p[2] == 'd' || p[2] == 'D') &&
                    (p[3] == '8' || p[3] == '9' || p[3] == 'a' || p[3] == 'b' || p[3] == 'A' || p[3] == 'B')) {
                unit = 12; /* surrogate pair stays together */
            }
            if (p + unit > e) {
                break;
            }
        } else if (c >= 0x80) {
            unsigned char c1 = p + 1 < e ? (unsigned char)p[1] : 0;
            int ok = 1;

            unit = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            if (!unit || p + unit > e || (c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 > 0x9f) ||
                    (c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 > 0x8f)) {
                ok = 0;
            }
            for (uint32_t i = 1; ok && i < unit; i++) {
                ok = ((unsigned char)p[i] & 0xc0) == 0x80;
            }
            if (!ok) {
                p++;
                continue;
            }
        } else if (!json && c == 0x1b) {
            p++;
            if (p < e && *p == '[') {
                for (p++; p < e && !(*p >= 0x40 && *p <= 0x7e); p++) {
                }
                p += p < e;
            }
            continue;
        } else if (!json && (c < 0x20 || c == 0x7f)) {
            if (c == '\t') {
                cb_putn(&s_b, " ", 1);
                used++;
            }
            p++;
            continue;
        }
        if (used + unit > max) {
            if (!(flags & SAFE_CUT)) {
                P("...");
            }
            return;
        }
        if (!json && (c == '"' || c == '\\')) {
            cb_putn(&s_b, "\\", 1);
        }
        cb_putn(&s_b, p, unit);
        used += unit;
        p += unit;
    }
}

/* The JSON string at v as a quoted string trimmed to `max` bytes; null for anything else. */
static NOINLINE void put_jstr(const char *v, const char *end, uint32_t max)
{
    const char *close;

    if (!v || v >= end || *v != '"' || !(close = cj_skip(v, end))) {
        P("null");
        return;
    }
    P("\"");
    put_safe(v + 1, close - 1, max, SAFE_JSON);
    P("\"");
}

/* The JSON string at v cut to `max` bytes without a marker (timestamps: 16 = minutes, 10 = date). */
static NOINLINE void put_jcut(const char *v, const char *end, uint32_t max)
{
    const char *close;

    if (!v || v >= end || *v != '"' || !(close = cj_skip(v, end))) {
        P("null");
        return;
    }
    P("\"");
    put_safe(v + 1, close - 1, max, SAFE_JSON | SAFE_CUT);
    P("\"");
}

/* A scalar (number, true/false/null) copied as-is; strings trimmed; objects/arrays become null. */
static NOINLINE void put_raw(const char *v, const char *end)
{
    const char *e;

    if (v && v < end && *v == '"') {
        put_jstr(v, end, 200);
    } else if (!v || v >= end || *v == '{' || *v == '[' || !(e = cj_skip(v, end)) || e - v > 24) {
        P("null");
    } else {
        cb_putn(&s_b, v, (uint32_t)(e - v));
    }
}

static const char *mem(const char *obj, const char *end, const char *k1, const char *k2)
{
    const char *v = cj_member(obj, end, k1);

    return k2 && v ? cj_member(v, end, k2) : v;
}

static NOINLINE void err_result(const char *msg, const char *detail)
{
    out_begin();
    P("{\"error\":\"");
    P(msg);
    if (detail) {
        P("\",\"value\":\"");
        put_safe(detail, detail + claw_strlen(detail), 200, 0);
    }
    P("\"}");
    out_end();
}

/* ---------- args ---------- */

static int32_t arg_str(const char *key, char *out, uint32_t cap)
{
    return cj_string(cj_member(s_args, s_args_end, key), s_args_end, out, cap);
}

/* 1 / 0, or -1 when absent or not a boolean. */
static int arg_bool(const char *key)
{
    const char *v = cj_member(s_args, s_args_end, key);

    if (v && *v == '"') {
        v++;
    }
    if (v && s_args_end - v >= 4 && v[0] == 't' && v[1] == 'r' && v[2] == 'u' && v[3] == 'e') {
        return 1;
    }
    if (v && s_args_end - v >= 5 && v[0] == 'f' && v[1] == 'a' && v[2] == 'l' && v[3] == 's' && v[4] == 'e') {
        return 0;
    }
    return -1;
}

/* ---------- HTTP ---------- */

static void path_begin(const char *s)
{
    s_p.buf = s_path;
    s_p.cap = sizeof(s_path);
    s_p.len = 0;
    s_p.full = 0;
    bput(&s_p, s);
}

/* GitHub requires a User-Agent; the device passes app headers through (except auth headers). */
#define GH_HEADERS "{\"Accept\":\"application/vnd.github+json\",\"X-GitHub-Api-Version\":\"2022-11-28\"," \
                   "\"User-Agent\":\"esp-claw-app-gh/1.0\""

/* GET through the profile; `range` adds a Range header (job logs). Body lands in s_resp. */
static NOINLINE int32_t get_h(const char *range)
{
    char headers[256];
    claw_buf_t h = { headers, sizeof(headers), 0, 0 };
    int32_t rc;
    int32_t n;

    if (s_p.full) {
        s_resp_len = 0;
        return -CLAW_E2BIG;
    }
    bput(&h, GH_HEADERS);
    if (range) {
        bput(&h, ",\"Range\":\"");
        bput(&h, range);
        bput(&h, "\"");
    }
    bput(&h, "}");
    rc = claw_http_profile_request(CLAW_S(PROFILE), CLAW_S("GET"), s_p.buf, s_p.len, h.buf, h.len, "", 0);
    s_resp_len = 0;
    while (s_resp_len < sizeof(s_resp) - 1 &&
            (n = claw_http_response_read(s_resp + s_resp_len, sizeof(s_resp) - 1 - s_resp_len)) > 0) {
        s_resp_len += (uint32_t)n;
    }
    s_resp[s_resp_len] = '\0';
    return rc;
}

static int32_t get(void)
{
    return get_h(0);
}

/* Reports a failed request with GitHub's "message", else the device's error text. */
static NOINLINE void fail(const char *what, int32_t rc)
{
    const char *end = s_resp + s_resp_len;
    const char *m = cj_member(s_resp, end, "message");

    out_begin();
    P("{\"error\":\"");
    P(what);
    P(" failed\",\"status\":");
    put_i32(rc);
    if (m) {
        P(",\"message\":");
        put_jstr(m, end, 300);
    } else if (s_resp_len) {
        P(",\"detail\":\"");
        put_safe(s_resp, end, 300, 0);
        P("\"");
    }
    if (rc == 401) {
        P(",\"hint\":\"token invalid or expired\"");
    } else if (rc == 403 || rc == 404) {
        P(",\"hint\":\"missing token scope/permission, rate limit, or no access to this repository\"");
    }
    P("}");
    out_end();
}

/* ---------- rows ---------- */

typedef int (*row_fn)(const char *item, const char *end);

/* Appends a row for every complete element of the array `arr` that `fn` accepts (fn returns 0 to
 * skip). `done` rows were written before. Returns the rows added; sets s_trunc when out of space or
 * the response was cut at 32 KB. */
static NOINLINE uint32_t rows(const char *arr, const char *end, row_fn fn, uint32_t done)
{
    uint32_t n = 0;

    for (const char *it = cj_first(arr, end); it; it = cj_next(it, end)) {
        if (!cj_skip(it, end)) {
            s_trunc = 1;
            break;
        }
        row_begin();
        P(done + n ? ",[" : "[");
        if (!fn(it, end)) {
            s_b.len = s_mark;
            s_b.buf[s_mark] = '\0';
            s_b.full = 0;
            continue;
        }
        P("]");
        if (!row_end()) {
            break;
        }
        n++;
    }
    return n;
}

/* Number of elements in the array (a cut-off response counts as a full page). */
static uint32_t items(const char *arr, const char *end, uint32_t per_page)
{
    uint32_t n = 0;
    const char *it;

    for (it = cj_first(arr, end); it; it = cj_next(it, end)) {
        n++;
        if (!cj_skip(it, end)) {
            return per_page;
        }
    }
    return n;
}

/* Closes the rows array; `total` (the API's total count) is added when >= 0. */
static void close_rows(uint32_t count, int64_t total)
{
    s_b.cap = sizeof(s_out);
    P("],\"count\":");
    bput_u32(&s_b, count);
    if (total >= 0) {
        P(",\"total\":");
        put_u64((uint64_t)total);
    }
    P(s_trunc ? ",\"truncated\":true}" : "}");
    out_end();
}

/* ---------- repo argument ---------- */

static char s_repo[160];

static int valid_part(const char *s, uint32_t n, int owner)
{
    if (n == 0 || n > 100) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                (!owner && (c == '.' || c == '_')))) {
            return 0;
        }
    }
    return !(n == 1 && s[0] == '.') && !(n == 2 && s[0] == '.' && s[1] == '.');
}

/* Fills s_repo with "owner/name" from args.repo ("owner/name" or "name" + kv owner).
 * 1 = ok, 0 = not given (only if !required), -1 = error result set. */
static int repo_arg(int required)
{
    char in[128];
    int32_t n = arg_str("repo", in, sizeof(in));
    int32_t slash = -1;
    claw_buf_t r = { s_repo, sizeof(s_repo), 0, 0 };

    if (n <= 0) {
        if (!required) {
            return 0;
        }
        err_result("repo is required (owner/name, or name with gh_set_owner)", 0);
        return -1;
    }
    for (int32_t i = 0; i < n; i++) {
        if (in[i] == '/') {
            slash = slash < 0 ? i : -2;
        }
    }
    if (slash == -1) {
        char owner[64];
        int32_t on = claw_kv_get(CLAW_S("owner"), owner, sizeof(owner) - 1);

        if (on <= 0) {
            err_result("no default owner: pass owner/name or call gh_set_owner first", in);
            return -1;
        }
        owner[on] = '\0';
        bput(&r, owner);
        bput(&r, "/");
        bput(&r, in);
        slash = on;
    } else {
        bput(&r, in);
    }
    if (slash < 0 || !valid_part(s_repo, (uint32_t)slash, 1) ||
            !valid_part(s_repo + slash + 1, r.len - (uint32_t)slash - 1, 0)) {
        err_result("repo must look like owner/name", in);
        return -1;
    }
    return 1;
}

static void repo_path(const char *suffix)
{
    path_begin("/repos/");
    bput(&s_p, s_repo);
    bput(&s_p, suffix);
}

/* ---------- row writers ---------- */

/* "owner/name" from an API URL like https://api.github.com/repos/owner/name. */
static void put_repo_from_url(const char *v, const char *end)
{
    char url[256];
    int32_t n = cj_string(v, end, url, sizeof(url));
    int32_t i = 0;

    for (; n > 7 && i + 7 <= n; i++) {
        if (url[i] == '/' && url[i + 1] == 'r' && url[i + 2] == 'e' && url[i + 3] == 'p' && url[i + 4] == 'o' &&
                url[i + 5] == 's' && url[i + 6] == '/') {
            break;
        }
    }
    if (n <= 7 || i + 7 > n) {
        P("null");
        return;
    }
    P("\"");
    put_safe(url + i + 7, url + n, 120, SAFE_JSON);
    P("\"");
}

static int row_notification(const char *it, const char *end)
{
    put_jstr(mem(it, end, "repository", "full_name"), end, 100);
    P(",");
    put_jstr(mem(it, end, "subject", "type"), end, 20);
    P(",");
    put_jstr(mem(it, end, "subject", "title"), end, 140);
    P(",");
    put_jstr(cj_member(it, end, "reason"), end, 20);
    P(",");
    put_jcut(cj_member(it, end, "updated_at"), end, 16);
    return 1;
}

static int row_pr(const char *it, const char *end)
{
    put_repo_from_url(cj_member(it, end, "repository_url"), end);
    P(",");
    put_raw(cj_member(it, end, "number"), end);
    P(",");
    put_jstr(cj_member(it, end, "title"), end, 140);
    P(",");
    put_jstr(mem(it, end, "user", "login"), end, 40);
    P(",");
    put_jcut(cj_member(it, end, "updated_at"), end, 16);
    P(",");
    put_raw(cj_member(it, end, "draft"), end);
    return 1;
}

static int row_issue(const char *it, const char *end)
{
    uint32_t nl = 0;

    if (cj_member(it, end, "pull_request")) {
        return 0; /* the issues endpoint also returns pull requests */
    }
    put_raw(cj_member(it, end, "number"), end);
    P(",");
    put_jstr(cj_member(it, end, "title"), end, 140);
    P(",");
    put_jstr(mem(it, end, "user", "login"), end, 40);
    P(",");
    put_raw(cj_member(it, end, "comments"), end);
    P(",\"");
    for (const char *l = cj_first(cj_member(it, end, "labels"), end); l && nl < 4; l = cj_next(l, end)) {
        const char *name = cj_member(l, end, "name");
        const char *close = name && *name == '"' ? cj_skip(name, end) : 0;

        if (close) {
            P(nl++ ? "," : "");
            put_safe(name + 1, close - 1, 30, SAFE_JSON);
        }
    }
    P("\",");
    put_jcut(cj_member(it, end, "updated_at"), end, 16);
    return 1;
}

static int row_run(const char *it, const char *end)
{
    put_raw(cj_member(it, end, "id"), end);
    P(",");
    put_jstr(cj_member(it, end, "name"), end, 60);
    P(",");
    put_jstr(cj_member(it, end, "head_branch"), end, 60);
    P(",");
    put_jstr(cj_member(it, end, "event"), end, 20);
    P(",");
    put_jstr(cj_member(it, end, "status"), end, 16);
    P(",");
    put_jstr(cj_member(it, end, "conclusion"), end, 16);
    P(",");
    put_jcut(cj_member(it, end, "created_at"), end, 16);
    P(",");
    put_jstr(cj_member(it, end, "display_title"), end, 100);
    return 1;
}

static int row_release(const char *it, const char *end)
{
    const char *assets = cj_member(it, end, "assets");
    uint64_t downloads = 0;
    uint32_t na = 0;
    const char *pre = cj_member(it, end, "prerelease");
    const char *draft = cj_member(it, end, "draft");

    for (const char *a = cj_first(assets, end); a; a = cj_next(a, end)) {
        int64_t d = cj_uint(cj_member(a, end, "download_count"), end);

        na++;
        downloads += d > 0 ? (uint64_t)d : 0;
    }
    put_jstr(cj_member(it, end, "tag_name"), end, 60);
    P(",");
    put_jstr(cj_member(it, end, "name"), end, 100);
    P(",");
    put_jcut(cj_member(it, end, "published_at"), end, 10);
    P(draft && *draft == 't' ? ",\"draft\"," : pre && *pre == 't' ? ",\"pre\"," : ",\"\",");
    bput_u32(&s_b, na);
    P(",");
    put_u64(downloads);
    return 1;
}

/* ---------- tools ---------- */

static void tool_set_owner(void)
{
    char owner[64];
    int32_t n = arg_str("owner", owner, sizeof(owner));

    if (n == 0) {
        claw_kv_del(CLAW_S("owner"));
        CLAW_RESULT("{\"ok\":true,\"owner\":null}");
        return;
    }
    if (n < 0 || n > 39 || !valid_part(owner, (uint32_t)n, 1)) {
        err_result("owner must be a GitHub user or organization name", n > 0 ? owner : 0);
        return;
    }
    if (claw_kv_set(CLAW_S("owner"), owner, (uint32_t)n) < 0) {
        err_result("could not store the owner", 0);
        return;
    }
    out_begin();
    P("{\"ok\":true,\"owner\":\"");
    P(owner);
    P("\"}");
    out_end();
}

static void tool_notifications(void)
{
    int all = arg_bool("all") == 1;
    uint32_t count = 0;

    out_begin();
    P("{\"cols\":[\"repo\",\"type\",\"title\",\"reason\",\"updated\"],\"rows\":[");
    /* Each notification carries a ~5 KB repository object: small pages keep a page under 32 KB. */
    for (uint32_t page = 1; page <= 4 && !s_b.full; page++) {
        const char *end;
        int32_t rc;

        path_begin(all ? "/notifications?all=true&per_page=5&page=" : "/notifications?per_page=5&page=");
        bput_u32(&s_p, page);
        rc = get();
        if (rc != 200) {
            fail("GET /notifications", rc);
            return;
        }
        end = s_resp + s_resp_len;
        count += rows(s_resp, end, row_notification, count);
        if (s_trunc && s_resp_len < sizeof(s_resp) - 1) {
            break; /* out of output space */
        }
        if (items(s_resp, end, 5) < 5) {
            break;
        }
        if (page == 4) {
            s_trunc = 1;
        }
    }
    close_rows(count, -1);
}

static void tool_prs(void)
{
    int r = repo_arg(0);
    uint32_t count = 0;
    int64_t total = -1;

    if (r < 0) {
        return;
    }
    out_begin();
    P("{\"query\":\"");
    P(r ? "repo:" : "involves:@me");
    P(r ? s_repo : "");
    P("\",\"cols\":[\"repo\",\"number\",\"title\",\"author\",\"updated\",\"draft\"],\"rows\":[");
    for (uint32_t page = 1; page <= 3; page++) {
        const char *end;
        int32_t rc;

        path_begin("/search/issues?q=is%3Aopen+is%3Apr+");
        if (r) {
            bput(&s_p, "repo%3A");
            bput(&s_p, s_repo);
        } else {
            bput(&s_p, "involves%3A%40me");
        }
        bput(&s_p, "&sort=updated&per_page=10&page=");
        bput_u32(&s_p, page);
        rc = get();
        if (rc != 200) {
            fail("GET /search/issues", rc);
            return;
        }
        end = s_resp + s_resp_len;
        total = cj_uint(cj_member(s_resp, end, "total_count"), end);
        count += rows(cj_member(s_resp, end, "items"), end, row_pr, count);
        if ((s_trunc && s_resp_len < sizeof(s_resp) - 1) || total <= (int64_t)page * 10) {
            break;
        }
        if (page == 3) {
            s_trunc = 1;
        }
    }
    close_rows(count, total);
}

static void tool_issues(void)
{
    char state[8];
    int32_t sn = arg_str("state", state, sizeof(state));
    const char *end;
    int32_t rc;

    if (repo_arg(1) < 0) {
        return;
    }
    if (sn <= 0) {
        state[0] = 'o', state[1] = 'p', state[2] = 'e', state[3] = 'n', state[4] = '\0';
    } else if (!str_eq(state, (uint32_t)sn, "open") && !str_eq(state, (uint32_t)sn, "closed") &&
               !str_eq(state, (uint32_t)sn, "all")) {
        err_result("state must be open, closed or all", state);
        return;
    }
    repo_path("/issues?sort=updated&per_page=20&state=");
    bput(&s_p, state);
    rc = get();
    if (rc != 200) {
        fail("GET issues", rc);
        return;
    }
    end = s_resp + s_resp_len;
    out_begin();
    P("{\"repo\":\"");
    P(s_repo);
    P("\",\"state\":\"");
    P(state);
    P("\",\"cols\":[\"number\",\"title\",\"author\",\"comments\",\"labels\",\"updated\"],\"rows\":[");
    close_rows(rows(s_resp, end, row_issue, 0), -1);
}

static void tool_ci(void)
{
    char branch[128];
    int32_t bn = arg_str("branch", branch, sizeof(branch));
    uint32_t count = 0;
    int64_t total = -1;

    if (repo_arg(1) < 0) {
        return;
    }
    for (int32_t i = 0; i < bn; i++) {
        char c = branch[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '/')) {
            err_result("branch has unsupported characters", 0);
            return;
        }
    }
    out_begin();
    P("{\"repo\":\"");
    P(s_repo);
    P("\",\"cols\":[\"run_id\",\"workflow\",\"branch\",\"event\",\"status\",\"conclusion\",\"created\",\"title\"],"
      "\"rows\":[");
    /* A workflow run object is ~10-15 KB (two repository objects): two per page stay under 32 KB. */
    for (uint32_t page = 1; page <= 4; page++) {
        const char *end;
        int32_t rc;

        repo_path("/actions/runs?exclude_pull_requests=true&per_page=2&page=");
        bput_u32(&s_p, page);
        if (bn > 0) {
            bput(&s_p, "&branch=");
            bput(&s_p, branch);
        }
        rc = get();
        if (rc != 200) {
            fail("GET actions/runs", rc);
            return;
        }
        end = s_resp + s_resp_len;
        total = cj_uint(cj_member(s_resp, end, "total_count"), end);
        count += rows(cj_member(s_resp, end, "workflow_runs"), end, row_run, count);
        if ((s_trunc && s_resp_len < sizeof(s_resp) - 1) || total <= (int64_t)page * 2) {
            break;
        }
    }
    close_rows(count, total);
}

#define TAIL_LINES  40
#define ERROR_LINES 10

/* Writes the ##[error] lines and the last TAIL_LINES lines of the log text in s_resp. */
static void put_log_lines(int partial)
{
    static const char *tail_p[TAIL_LINES];
    static uint32_t tail_n[TAIL_LINES];
    const char *p = s_resp;
    const char *end = s_resp + s_resp_len;
    uint32_t lines = 0;
    uint32_t errors = 0;

    if (partial) { /* a range response starts mid-line */
        while (p < end && *p != '\n') {
            p++;
        }
        p += p < end;
    }
    if (end - p >= 3 && (unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) {
        p += 3;
    }
    P(",\"errors\":[");
    while (p < end) {
        const char *e = p;
        const char *s;

        while (e < end && *e != '\n') {
            e++;
        }
        s = p;
        /* drop the "2026-01-01T00:00:00.0000000Z " timestamp prefix */
        if (e - s > 20 && s[4] == '-' && s[7] == '-' && s[10] == 'T') {
            const char *q = s;

            while (q < e && *q != ' ') {
                q++;
            }
            if (q < e && q[-1] == 'Z') {
                s = q + 1;
            }
        }
        if (e > s && e[-1] == '\r') {
            e--;
        }
        if (e > s) {
            for (const char *q = s; q + 9 <= e && errors < ERROR_LINES; q++) {
                if (q[0] == '#' && q[1] == '#' && q[2] == '[' && q[3] == 'e' && q[4] == 'r' && q[5] == 'r' &&
                        q[6] == 'o' && q[7] == 'r' && q[8] == ']') {
                    P(errors++ ? ",\"" : "\"");
                    put_safe(q + 9, e, 200, 0);
                    P("\"");
                    break;
                }
            }
            tail_p[lines % TAIL_LINES] = s;
            tail_n[lines % TAIL_LINES] = (uint32_t)(e - s);
            lines++;
        }
        p = e;
        while (p < end && (*p == '\r' || *p == '\n')) {
            p++;
        }
    }
    P("],\"tail\":[");
    for (uint32_t i = lines > TAIL_LINES ? lines - TAIL_LINES : 0, k = 0; i < lines; i++, k++) {
        P(k ? ",\"" : "\"");
        put_safe(tail_p[i % TAIL_LINES], tail_p[i % TAIL_LINES] + tail_n[i % TAIL_LINES], 200, 0);
        P("\"");
    }
    P("]");
}

static void tool_failed_log(void)
{
    char run[24];
    char job[24] = "";
    int32_t n = arg_str("run_id", run, sizeof(run));
    const char *end;
    uint32_t count = 0;
    int32_t rc;

    if (repo_arg(1) < 0) {
        return;
    }
    if (n <= 0) { /* the model may pass the id as a number */
        const char *v = cj_member(s_args, s_args_end, "run_id");
        const char *ve = v ? cj_skip(v, s_args_end) : 0;

        n = ve && ve - v < (int32_t)sizeof(run) ? (int32_t)(ve - v) : 0;
        for (int32_t i = 0; i < n; i++) {
            run[i] = v[i];
        }
        run[n] = '\0';
    }
    if (n <= 0) {
        err_result("run_id is required (see gh_ci)", 0);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        if (run[i] < '0' || run[i] > '9') {
            err_result("run_id must be numeric", run);
            return;
        }
    }
    repo_path("/actions/runs/");
    bput(&s_p, run);
    bput(&s_p, "/jobs?filter=latest&per_page=10");
    rc = get();
    if (rc != 200) {
        fail("GET run jobs", rc);
        return;
    }
    end = s_resp + s_resp_len;
    out_begin();
    P("{\"repo\":\"");
    P(s_repo);
    P("\",\"run_id\":");
    P(run);
    P(",\"jobs\":{\"cols\":[\"job_id\",\"name\",\"conclusion\",\"failed_steps\"],\"rows\":[");
    for (const char *j = cj_first(cj_member(s_resp, end, "jobs"), end); j; j = cj_next(j, end)) {
        char concl[20] = "";
        uint32_t nf = 0;

        if (!cj_skip(j, end)) {
            s_trunc = 1;
            break;
        }
        cj_string(cj_member(j, end, "conclusion"), end, concl, sizeof(concl));
        if (!job[0] && str_eq(concl, claw_strlen(concl), "failure")) {
            const char *id = cj_member(j, end, "id");
            const char *ide = id ? cj_skip(id, end) : 0;

            for (uint32_t i = 0; ide && i < (uint32_t)(ide - id) && i < sizeof(job) - 1; i++) {
                job[i] = id[i];
                job[i + 1] = '\0';
            }
        }
        row_begin();
        P(count ? ",[" : "[");
        put_raw(cj_member(j, end, "id"), end);
        P(",");
        put_jstr(cj_member(j, end, "name"), end, 80);
        P(",");
        put_jstr(cj_member(j, end, "conclusion"), end, 16);
        P(",[");
        for (const char *s = cj_first(cj_member(j, end, "steps"), end); s; s = cj_next(s, end)) {
            char sc[20] = "";

            cj_string(cj_member(s, end, "conclusion"), end, sc, sizeof(sc));
            if (str_eq(sc, claw_strlen(sc), "failure")) {
                P(nf++ ? "," : "");
                put_jstr(cj_member(s, end, "name"), end, 80);
            }
        }
        P("]]");
        if (!row_end()) {
            break;
        }
        count++;
    }
    P("]}");
    for (uint32_t i = 0; job[i]; i++) {
        if (job[i] < '0' || job[i] > '9') {
            job[0] = '\0';
        }
    }
    if (job[0]) {
        /* 302 to a short-lived blob URL (Azure, *.blob.core.windows.net); the device follows it only
         * if that host is in the profile's hosts. Range asks for the tail only; if the blob host
         * ignores it the device returns the first 32 KB instead (UNVERIFIED on the device). */
        repo_path("/actions/jobs/");
        bput(&s_p, job);
        bput(&s_p, "/logs");
        rc = get_h("bytes=-24576");
        P(",\"log\":{\"job_id\":");
        P(job);
        P(",\"status\":");
        put_i32(rc);
        if (rc == 200 || rc == 206) {
            if (rc == 200 && s_resp_len >= sizeof(s_resp) - 1) {
                P(",\"head_only\":true");
            }
            put_log_lines(rc == 206);
        } else {
            P(",\"note\":\"log download failed; the github profile needs the log redirect host "
              "(*.blob.core.windows.net) in hosts\",\"detail\":\"");
            put_safe(s_resp, s_resp + s_resp_len, 200, 0);
            P("\"");
        }
        P("}");
    }
    s_b.cap = sizeof(s_out);
    P(s_trunc ? ",\"truncated\":true}" : "}");
    out_end();
}

static void tool_releases(void)
{
    const char *end;
    int32_t rc;

    if (repo_arg(1) < 0) {
        return;
    }
    repo_path("/releases?per_page=5");
    rc = get();
    if (rc != 200) {
        fail("GET releases", rc);
        return;
    }
    end = s_resp + s_resp_len;
    out_begin();
    P("{\"repo\":\"");
    P(s_repo);
    P("\",\"cols\":[\"tag\",\"name\",\"published\",\"flag\",\"assets\",\"downloads\"],\"rows\":[");
    close_rows(rows(s_resp, end, row_release, 0), -1);
}

static void tool_repo(void)
{
    const char *end;
    int32_t rc;
    static const char *const nums[] = { "stargazers_count", "forks_count", "open_issues_count", "subscribers_count" };
    static const char *const outn[] = { "stars", "forks", "open_issues_and_prs", "watchers" };

    if (repo_arg(1) < 0) {
        return;
    }
    repo_path("");
    rc = get();
    if (rc != 200) {
        fail("GET repo", rc);
        return;
    }
    end = s_resp + s_resp_len;
    out_begin();
    P("{\"repo\":");
    put_jstr(cj_member(s_resp, end, "full_name"), end, 120);
    P(",\"description\":");
    put_jstr(cj_member(s_resp, end, "description"), end, 200);
    for (int i = 0; i < 4; i++) {
        P(",\"");
        P(outn[i]);
        P("\":");
        put_raw(cj_member(s_resp, end, nums[i]), end);
    }
    P(",\"default_branch\":");
    put_jstr(cj_member(s_resp, end, "default_branch"), end, 60);
    P(",\"pushed\":");
    put_jcut(cj_member(s_resp, end, "pushed_at"), end, 16);
    P(",\"language\":");
    put_jstr(cj_member(s_resp, end, "language"), end, 30);
    P(",\"license\":");
    put_jstr(mem(s_resp, end, "license", "spdx_id"), end, 30);
    P(",\"visibility\":");
    put_jstr(cj_member(s_resp, end, "visibility"), end, 12);
    P(",\"archived\":");
    put_raw(cj_member(s_resp, end, "archived"), end);
    P("}");
    out_end();
}

/* ---------- registration and dispatch ---------- */

#define REPO_PROP "\"repo\":{\"type\":\"string\",\"description\":\"owner/name, or name for the default owner\"}"

CLAW_EXPORT("app_init") void app_init(void)
{
    s_t_notif = CLAW_REGISTER("notifications", "Unread GitHub notifications (all=true includes read ones): repo, type, "
                              "title, reason, updated. Needs a classic token with the notifications scope.",
                              "{\"type\":\"object\",\"properties\":{\"all\":{\"type\":\"boolean\"}}}");
    s_t_prs = CLAW_REGISTER("prs", "Open pull requests involving me, or all open PRs of a repo: repo, number, title, "
                            "author, updated, draft.",
                            "{\"type\":\"object\",\"properties\":{" REPO_PROP "}}");
    s_t_issues = CLAW_REGISTER("issues", "Issues of a repo (no PRs), newest activity first: number, title, author, "
                               "comments, labels, updated.",
                               "{\"type\":\"object\",\"properties\":{" REPO_PROP ",\"state\":{\"type\":\"string\","
                               "\"enum\":[\"open\",\"closed\",\"all\"]}},\"required\":[\"repo\"]}");
    s_t_ci = CLAW_REGISTER("ci", "Latest GitHub Actions workflow runs of a repo (optional branch): run_id, workflow, "
                           "branch, event, status, conclusion, created, title.",
                           "{\"type\":\"object\",\"properties\":{" REPO_PROP ",\"branch\":{\"type\":\"string\"}},"
                           "\"required\":[\"repo\"]}");
    s_t_log = CLAW_REGISTER("failed_log", "Jobs of a workflow run with failed steps, plus the error lines and last 40 "
                            "log lines of the first failed job.",
                            "{\"type\":\"object\",\"properties\":{" REPO_PROP ",\"run_id\":{\"type\":\"string\"}},"
                            "\"required\":[\"repo\",\"run_id\"]}");
    s_t_releases = CLAW_REGISTER("releases", "Latest 5 releases of a repo: tag, name, published, draft/pre flag, assets, "
                                 "downloads.",
                                 "{\"type\":\"object\",\"properties\":{" REPO_PROP "},\"required\":[\"repo\"]}");
    s_t_repo = CLAW_REGISTER("repo", "Repository overview: stars, forks, open issues+PRs, watchers, default branch, "
                             "last push, language, license.",
                             "{\"type\":\"object\",\"properties\":{" REPO_PROP "},\"required\":[\"repo\"]}");
    s_t_owner = CLAW_REGISTER("set_owner", "Set the default owner (user/org) for repo arguments without owner/. "
                              "Empty string clears it.",
                              "{\"type\":\"object\",\"properties\":{\"owner\":{\"type\":\"string\"}},"
                              "\"required\":[\"owner\"]}");
}

CLAW_EXPORT("tool_call") int32_t tool_call(int32_t tool, const char *args, uint32_t len)
{
    s_args = args;
    s_args_end = args + len;
    if (tool == s_t_notif) {
        tool_notifications();
    } else if (tool == s_t_prs) {
        tool_prs();
    } else if (tool == s_t_issues) {
        tool_issues();
    } else if (tool == s_t_ci) {
        tool_ci();
    } else if (tool == s_t_log) {
        tool_failed_log();
    } else if (tool == s_t_releases) {
        tool_releases();
    } else if (tool == s_t_repo) {
        tool_repo();
    } else if (tool == s_t_owner) {
        tool_set_owner();
    } else {
        return -1;
    }
    return 0;
}
