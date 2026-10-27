/*
4coder-markers - the marker service that coding agents post to.

Agents attach a one-sentence justification and a one-sentence explanation to a
location in a file. 4coder polls this service and shows them in a popup when
the cursor is on a marked line.

    4coder-markers serve [--port P] [--store FILE]
    4coder-markers add --file F --line N [--end-line M] --why "..." --what "..." [--agent NAME]
    4coder-markers list [--file F]
    4coder-markers rm ID
    4coder-markers clear [--file F] [--agent NAME]
    4coder-markers health
    4coder-markers daemon start|stop|status [--state DIR] [--port P]
    4coder-markers version
    4coder-markers story post FILE.json|- [--agent NAME]
    4coder-markers story check FILE.json|-
    4coder-markers story list | show ID | rm ID | clear [--agent NAME]
    4coder-markers pseudo wait | list | show ID | claim ID | check FILE | answer ID FILE | fail ID
                          | request FILE... | cancel ID | rm ID | clear

Stories (a graph of narrative steps over the code) live next to markers; see
4coder_agent_stories_protocol.h. The marker API is unchanged by them.

The HTTP API is documented in README.md next to this file.
*/

#include "../protocol/4coder_agent_markers_protocol.h"
#include "../protocol/4coder_agent_stories_protocol.h"
#include "../protocol/4coder_agent_pseudo_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#if !defined(FCODER_MARKERS_VERSION)
#define FCODER_MARKERS_VERSION "dev"
#endif

////////////////////////////////
// NOTE: marker store

struct Store{
    AM_Marker *markers;
    int count;
    int cap;
    long long next_id;
    long long revision;
    const char *path;       // persistence file, or 0
};

static Store store = {0, 0, 0, 1, 1, 0};

static int
str_eq(const char *a, const char *b){
    return(strcmp(a != 0 ? a : "", b != 0 ? b : "") == 0);
}

// NOTE: stories have their own revision, so posting one never wakes marker pollers
struct Story_Store{
    AM_Story *stories;
    int count;
    int cap;
    long long revision;
};

static Story_Store story_store = {0, 0, 0, 1};

static void
story_store_upsert(AM_Story *s){
    for (int i = 0; i < story_store.count; i += 1){
        AM_Story *e = &story_store.stories[i];
        if (strcmp(e->id, s->id) == 0){
            s->created = e->created;
            am_story_free(e);
            *e = *s;
            return;
        }
    }
    if (story_store.count == story_store.cap){
        story_store.cap = story_store.cap ? story_store.cap*2 : 16;
        story_store.stories = (AM_Story*)realloc(story_store.stories, sizeof(AM_Story)*story_store.cap);
    }
    story_store.stories[story_store.count++] = *s;
}

static int
story_store_remove_if(const char *id, const char *agent){
    int removed = 0;
    for (int i = 0; i < story_store.count;){
        AM_Story *s = &story_store.stories[i];
        int match = (id != 0) ? str_eq(s->id, id) : (agent == 0 || str_eq(s->agent, agent));
        if (match){
            am_story_free(s);
            // NOTE: keep posting order, the editor lists stories in it
            memmove(s, s + 1, sizeof(AM_Story)*(story_store.count - i - 1));
            story_store.count -= 1;
            removed += 1;
        }
        else{
            i += 1;
        }
    }
    return(removed);
}

static AM_Story*
story_store_find(const char *id){
    for (int i = 0; i < story_store.count; i += 1){
        if (str_eq(story_store.stories[i].id, id)) return(&story_store.stories[i]);
    }
    return(0);
}

// NOTE: requests have their own revision, so the editor's request poller never
// refetches markers or stories
struct Pseudo_Store{
    AM_Pseudo_Job *jobs;
    int count;
    int cap;
    long long next_id;
    long long revision;
};

static Pseudo_Store pseudo_store = {0, 0, 0, 1, 1};

#define PSEUDO_KEEP_JOBS 100

static AM_Pseudo_Job*
pseudo_find(long long id){
    for (int i = 0; i < pseudo_store.count; i += 1){
        if (pseudo_store.jobs[i].id == id) return(&pseudo_store.jobs[i]);
    }
    return(0);
}

static void
pseudo_remove_at(int i){
    am_pseudo_job_free(&pseudo_store.jobs[i]);
    memmove(&pseudo_store.jobs[i], &pseudo_store.jobs[i + 1], sizeof(AM_Pseudo_Job)*(pseudo_store.count - i - 1));
    pseudo_store.count -= 1;
}

static void
pseudo_append(AM_Pseudo_Job *j){
    // NOTE: forget the oldest finished requests; open ones are never dropped
    for (int i = 0; i < pseudo_store.count && pseudo_store.count >= PSEUDO_KEEP_JOBS;){
        if (am_pseudo_finished(&pseudo_store.jobs[i])) pseudo_remove_at(i);
        else i += 1;
    }
    if (pseudo_store.count == pseudo_store.cap){
        pseudo_store.cap = pseudo_store.cap ? pseudo_store.cap*2 : 16;
        pseudo_store.jobs = (AM_Pseudo_Job*)realloc(pseudo_store.jobs, sizeof(AM_Pseudo_Job)*pseudo_store.cap);
    }
    pseudo_store.jobs[pseudo_store.count++] = *j;
}

static void
store_save(void){
    if (store.path == 0) return;
    AM_Text t = {};
    am_text_appendf(&t, "{\"next_id\":%lld", store.next_id);
    am_text_appendf(&t, ",\"revision\":%lld,\"markers\":[", store.revision);
    for (int i = 0; i < store.count; i += 1){
        if (i > 0) am_text_append(&t, ",");
        am_marker_to_json(&t, &store.markers[i]);
    }
    am_text_appendf(&t, "],\"story_revision\":%lld,\"stories\":[", story_store.revision);
    for (int i = 0; i < story_store.count; i += 1){
        if (i > 0) am_text_append(&t, ",");
        am_story_to_json(&t, &story_store.stories[i]);
    }
    am_text_appendf(&t, "],\"pseudo_next_id\":%lld", pseudo_store.next_id);
    am_text_appendf(&t, ",\"pseudo_revision\":%lld,\"pseudo\":[", pseudo_store.revision);
    for (int i = 0; i < pseudo_store.count; i += 1){
        if (i > 0) am_text_append(&t, ",");
        am_pseudo_job_to_json(&t, &pseudo_store.jobs[i]);
    }
    am_text_append(&t, "]}\n");

    // NOTE: write a temp file and rename, so a crash never leaves half a store
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", store.path);
    FILE *f = fopen(tmp, "wb");
    if (f != 0){
        size_t written = fwrite(t.str, 1, t.size, f);
        int ok = (fclose(f) == 0 && written == t.size);
        if (ok && rename(tmp, store.path) != 0){
            fprintf(stderr, "4coder-markers: could not write %s: %s\n", store.path, strerror(errno));
        }
    }
    am_text_free(&t);
}

static void
store_load(void){
    if (store.path == 0) return;
    FILE *f = fopen(store.path, "rb");
    if (f == 0) return;
    AM_Text t = {};
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) am_text_append(&t, buf, n);
    fclose(f);
    AM_Json *root = (t.str != 0) ? am_json_parse(t.str, t.size) : 0;
    AM_Json *list = am_json_get(root, "markers");
    if (root == 0 || list == 0 || list->kind != AMJson_Array){
        fprintf(stderr, "4coder-markers: ignoring unreadable store %s\n", store.path);
    }
    else{
        for (AM_Json *c = list->first_child; c != 0; c = c->next){
            AM_Marker m;
            if (am_marker_from_json(c, &m) == 0){
                if (store.count == store.cap){
                    store.cap = store.cap ? store.cap*2 : 64;
                    store.markers = (AM_Marker*)realloc(store.markers, sizeof(AM_Marker)*store.cap);
                }
                store.markers[store.count++] = m;
            }
        }
        store.next_id = am_json_get_int(root, "next_id", 1);
        store.revision = am_json_get_int(root, "revision", 1);
        for (int i = 0; i < store.count; i += 1){
            if (store.markers[i].id >= store.next_id) store.next_id = store.markers[i].id + 1;
        }
        // NOTE: stores written before stories existed have neither key
        story_store.revision = am_json_get_int(root, "story_revision", 1);
        AM_Json *stories = am_json_get(root, "stories");
        for (AM_Json *c = (stories && stories->kind == AMJson_Array) ? stories->first_child : 0; c != 0; c = c->next){
            AM_Story s;
            char err[256];
            if (am_story_from_json(c, &s, err, sizeof(err)) == 0){
                story_store_upsert(&s);
            }
        }
        pseudo_store.next_id = am_json_get_int(root, "pseudo_next_id", 1);
        pseudo_store.revision = am_json_get_int(root, "pseudo_revision", 1);
        AM_Json *jobs = am_json_get(root, "pseudo");
        for (AM_Json *c = (jobs && jobs->kind == AMJson_Array) ? jobs->first_child : 0; c != 0; c = c->next){
            AM_Pseudo_Job j;
            char err[256];
            if (am_pseudo_job_from_json(c, &j, err, sizeof(err)) == 0 && j.id > 0){
                pseudo_append(&j);
                if (j.id >= pseudo_store.next_id) pseudo_store.next_id = j.id + 1;
            }
        }
    }
    am_json_free(root);
    am_text_free(&t);
}

// NOTE: markers are keyed by (agent, file, line): posting again updates in place
static long long
store_upsert(AM_Marker *m){
    for (int i = 0; i < store.count; i += 1){
        AM_Marker *e = &store.markers[i];
        if (str_eq(e->agent, m->agent) && str_eq(e->file, m->file) && e->line == m->line){
            m->id = e->id;
            am_marker_free(e);
            *e = *m;
            return(m->id);
        }
    }
    if (store.count == store.cap){
        store.cap = store.cap ? store.cap*2 : 64;
        store.markers = (AM_Marker*)realloc(store.markers, sizeof(AM_Marker)*store.cap);
    }
    m->id = store.next_id++;
    store.markers[store.count++] = *m;
    return(m->id);
}

static int
store_remove_if(long long id, const char *file, const char *agent){
    int removed = 0;
    for (int i = 0; i < store.count;){
        AM_Marker *m = &store.markers[i];
        int match = (id != 0) ? (m->id == id) :
            ((file == 0 || str_eq(m->file, file)) && (agent == 0 || str_eq(m->agent, agent)));
        if (match){
            am_marker_free(m);
            store.markers[i] = store.markers[--store.count];
            removed += 1;
        }
        else{
            i += 1;
        }
    }
    return(removed);
}

static int
marker_compare(const void *a, const void *b){
    const AM_Marker *x = (const AM_Marker*)a;
    const AM_Marker *y = (const AM_Marker*)b;
    int c = strcmp(x->file, y->file);
    if (c != 0) return(c);
    return((x->line > y->line) - (x->line < y->line));
}

////////////////////////////////
// NOTE: HTTP server

struct Request{
    char method[16];
    char path[2048];
    char query[4096];
    AM_Text body;
};

static int
hex_value(char c){
    if (c >= '0' && c <= '9') return(c - '0');
    if (c >= 'a' && c <= 'f') return(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return(c - 'A' + 10);
    return(-1);
}

// NOTE: value of key in an x-www-form-urlencoded query, percent-decoded into out
static int
query_get(const char *query, const char *key, char *out, size_t out_size){
    size_t key_len = strlen(key);
    for (const char *q = query; *q;){
        const char *amp = strchr(q, '&');
        size_t len = amp ? (size_t)(amp - q) : strlen(q);
        if (len > key_len && strncmp(q, key, key_len) == 0 && q[key_len] == '='){
            size_t o = 0;
            for (size_t i = key_len + 1; i < len && o + 1 < out_size; i += 1){
                char c = q[i];
                if (c == '+') c = ' ';
                else if (c == '%' && i + 2 < len && hex_value(q[i+1]) >= 0 && hex_value(q[i+2]) >= 0){
                    c = (char)(hex_value(q[i+1])*16 + hex_value(q[i+2]));
                    i += 2;
                }
                out[o++] = c;
            }
            out[o] = 0;
            return(1);
        }
        q += len;
        if (*q == '&') q += 1;
    }
    return(0);
}

static void
respond(int fd, int status, AM_Text *json){
    const char *reason = (status == 200) ? "OK" : (status == 400) ? "Bad Request" :
        (status == 404) ? "Not Found" : (status == 405) ? "Method Not Allowed" :
        (status == 409) ? "Conflict" : (status == 413) ? "Payload Too Large" : "Error";
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, reason, json->size);
    am_send_all(fd, header, (size_t)n);
    am_send_all(fd, json->str, json->size);
}

static void
respond_error(int fd, int status, const char *message){
    AM_Text t = {};
    am_text_append(&t, "{\"error\":");
    am_json_append_string(&t, message);
    am_text_append(&t, "}\n");
    respond(fd, status, &t);
    am_text_free(&t);
}

#define MAX_BODY (1 << 20)

// NOTE: reads one request; returns 0 on a malformed or oversized request
static int
read_request(int fd, Request *req){
    AM_Text raw = {};
    char chunk[8192];
    char *header_end = 0;
    while (header_end == 0){
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0){ am_text_free(&raw); return(0); }
        am_text_append(&raw, chunk, (size_t)n);
        header_end = strstr(raw.str, "\r\n\r\n");
        if (header_end == 0 && raw.size > 65536){ am_text_free(&raw); return(0); }
    }

    char target[sizeof(req->path) + sizeof(req->query)];
    if (sscanf(raw.str, "%15s %6143s", req->method, target) != 2){ am_text_free(&raw); return(0); }
    char *qmark = strchr(target, '?');
    if (qmark){ *qmark = 0; snprintf(req->query, sizeof(req->query), "%s", qmark + 1); }
    else { req->query[0] = 0; }
    size_t path_size = strlen(target);
    if (path_size >= sizeof(req->path)) path_size = sizeof(req->path) - 1;
    memcpy(req->path, target, path_size);
    req->path[path_size] = 0;

    long long content_length = 0;
    for (char *line = strstr(raw.str, "\r\n"); line != 0 && line < header_end; line = strstr(line + 2, "\r\n")){
        if (strncasecmp(line + 2, "Content-Length:", 15) == 0){
            content_length = atoll(line + 2 + 15);
        }
    }
    if (content_length < 0 || content_length > MAX_BODY){ am_text_free(&raw); return(-1); }

    size_t have = raw.size - (size_t)(header_end + 4 - raw.str);
    am_text_append(&req->body, header_end + 4, have);
    while ((long long)req->body.size < content_length){
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        am_text_append(&req->body, chunk, (size_t)n);
    }
    am_text_free(&raw);
    return((long long)req->body.size >= content_length ? 1 : 0);
}

// NOTE: /v1/stories routes; returns whether the store changed
static int
handle_stories(int fd, Request *req, AM_Text *out){
    const char *id = (req->path[11] == '/') ? req->path + 12 : 0;
    if (id != 0 && !am_story_id_ok(id)){
        respond_error(fd, 404, "no story with that id");
        return(0);
    }
    int is_get = (strcmp(req->method, "GET") == 0);
    int is_post = (strcmp(req->method, "POST") == 0);
    int is_delete = (strcmp(req->method, "DELETE") == 0);

    if (is_get && id == 0){
        char since[32];
        long long since_revision = query_get(req->query, "since_revision", since, sizeof(since)) ? atoll(since) : -1;
        if (since_revision == story_store.revision){
            am_text_appendf(out, "{\"revision\":%lld,\"unchanged\":true}\n", story_store.revision);
        }
        else{
            am_text_appendf(out, "{\"revision\":%lld,\"stories\":[", story_store.revision);
            for (int i = 0; i < story_store.count; i += 1){
                if (i > 0) am_text_append(out, ",");
                am_story_to_json(out, &story_store.stories[i]);
            }
            am_text_append(out, "]}\n");
        }
        respond(fd, 200, out);
        return(0);
    }
    if (is_get){
        AM_Story *s = story_store_find(id);
        if (s == 0){
            respond_error(fd, 404, "no story with that id");
            return(0);
        }
        am_story_to_json(out, s);
        am_text_append(out, "\n");
        respond(fd, 200, out);
        return(0);
    }
    if (is_post && id == 0){
        AM_Json *root = (req->body.str != 0) ? am_json_parse(req->body.str, req->body.size) : 0;
        AM_Json single;
        AM_Json *items = am_stories_items(root, &single);
        int changed = 0;
        if (items == 0){
            respond_error(fd, 400, root == 0 ? "body is not valid JSON" :
                          "body must be a story object, an array of stories, or {\"stories\": [...]}");
        }
        else{
            // NOTE: validate everything before storing anything
            int count = 0;
            for (AM_Json *c = items->first_child; c != 0; c = c->next) count += 1;
            AM_Story *parsed = (AM_Story*)calloc(count ? count : 1, sizeof(AM_Story));
            char err[512], msg[640];
            const char *error = (count == 0) ? "no stories given" : 0;
            int i = 0;
            for (AM_Json *c = items->first_child; c != 0 && error == 0; c = c->next, i += 1){
                if (am_story_from_json(c, &parsed[i], err, sizeof(err)) != 0){
                    const char *sid = am_json_get_string(c, "id");
                    snprintf(msg, sizeof(msg), "story %d%s%s%s: %s", i, sid ? " (\"" : "", sid ? sid : "", sid ? "\")" : "", err);
                    error = msg;
                }
            }
            if (error != 0){
                respond_error(fd, 400, error);
                for (int k = 0; k < count; k += 1) am_story_free(&parsed[k]);
            }
            else{
                am_text_append(out, "{\"ids\":[");
                long long now = (long long)time(0);
                for (int k = 0; k < count; k += 1){
                    if (k > 0) am_text_append(out, ",");
                    am_json_append_string(out, parsed[k].id);
                    parsed[k].created = now;
                    parsed[k].updated = now;
                    story_store_upsert(&parsed[k]);
                }
                story_store.revision += 1;
                changed = 1;
                am_text_appendf(out, "],\"revision\":%lld}\n", story_store.revision);
                respond(fd, 200, out);
            }
            free(parsed);
        }
        am_json_free(root);
        return(changed);
    }
    if (is_delete){
        char agent[256];
        int has_agent = (id == 0) && query_get(req->query, "agent", agent, sizeof(agent));
        int removed = story_store_remove_if(id, has_agent ? agent : 0);
        if (id != 0 && removed == 0){
            respond_error(fd, 404, "no story with that id");
            return(0);
        }
        if (removed > 0) story_store.revision += 1;
        am_text_appendf(out, "{\"removed\":%lld", removed);
        am_text_appendf(out, ",\"revision\":%lld}\n", story_store.revision);
        respond(fd, 200, out);
        return(removed > 0);
    }
    respond_error(fd, 405, "unknown endpoint or method");
    return(0);
}

////////////////////////////////
// NOTE: pseudocode requests

static void
pseudo_set_status(AM_Pseudo_Job *j, const char *status){
    free(j->status);
    j->status = am_strdup(status);
    j->updated = (long long)time(0);
}

// NOTE: a copy of s cut to at most max bytes on a UTF-8 boundary, and to its first line when one_line
static char*
clip_dup(const char *s, size_t max, int one_line){
    size_t n = strlen(s);
    if (one_line){
        const char *nl = strchr(s, '\n');
        if (nl != 0) n = (size_t)(nl - s);
    }
    int cut = 0;
    if (n > max){
        n = max - 3;
        while (n > 0 && (s[n] & 0xC0) == 0x80) n -= 1;
        cut = 1;
    }
    char *buf = (char*)malloc(n + 4);
    memcpy(buf, s, n);
    memcpy(buf + n, cut ? "..." : "", cut ? 4 : 1);
    return(buf);
}

static void
append_clipped(AM_Text *t, const char *s, size_t max, int one_line){
    char *c = clip_dup(s, max, one_line);
    am_json_append_string(t, c);
    free(c);
}

static const char*
path_base(const char *path){
    const char *slash = strrchr(path, '/');
    return(slash ? slash + 1 : path);
}

static void
append_story_range(AM_Text *t, AM_Pseudo_Range *r){
    am_text_append(t, ",\"file\":");
    am_json_append_string(t, r->file);
    am_text_appendf(t, ",\"line\":%lld", r->line);
    am_text_appendf(t, ",\"end_line\":%lld", r->end_line);
}

// NOTE: the first node of link i, starting with its pseudocode
static void
append_link_node_id(AM_Text *t, AM_Pseudo_Job *j, int i, int code_first){
    AM_Pseudo_Link *l = &j->links[i];
    int code = code_first ? (l->code.file[0] != 0) : (l->pseudo.file[0] == 0);
    am_text_appendf(t, code ? "\"c%lld\"" : "\"p%lld\"", i + 1);
}

// NOTE: the story for an answered request: the request, then each link as a pseudocode
// slide and an implementation slide, linked both ways and to the next link
static int
pseudo_build_story(AM_Pseudo_Job *j, AM_Story *out, char *err, size_t err_size){
    AM_Text t = {};
    char id[64];
    snprintf(id, sizeof(id), "pseudo-%lld", j->id);
    char title[96];
    snprintf(title, sizeof(title), "Pseudocode #%lld: ", j->id);
    am_text_append(&t, "{\"id\":");
    am_json_append_string(&t, id);
    am_text_append(&t, ",\"title\":");
    AM_Text full_title = {};
    am_text_append(&full_title, title);
    am_text_append(&full_title, j->instructions[0] ? j->instructions : "implementation");
    append_clipped(&t, full_title.str, AM_STORY_MAX_TITLE, 1);
    am_text_free(&full_title);
    am_text_append(&t, ",\"summary\":");
    append_clipped(&t, j->summary, AM_STORY_MAX_SUMMARY, 0);
    am_text_append(&t, ",\"agent\":");
    am_json_append_string(&t, j->agent);
    am_text_append(&t, ",\"start\":\"request\",\"nodes\":[");

    AM_Text request = {};
    am_text_append(&request, j->instructions[0] ? j->instructions : "(no instructions)");
    am_text_append(&request, "\n\n");
    am_text_append(&request, j->summary);
    am_text_append(&t, "{\"id\":\"request\",\"kind\":\"request\",\"title\":\"The request and what was done\",\"text\":");
    append_clipped(&t, request.str, AM_STORY_MAX_TEXT, 0);
    am_text_free(&request);
    am_text_append(&t, ",\"next\":[");
    if (j->link_count > 0){
        am_text_append(&t, "{\"label\":\"first step\",\"to\":");
        append_link_node_id(&t, j, 0, 0);
        am_text_append(&t, "}");
    }
    am_text_append(&t, "]}");

    for (int i = 0; i < j->link_count; i += 1){
        AM_Pseudo_Link *l = &j->links[i];
        int has_next = (i + 1 < j->link_count);
        if (l->pseudo.file[0] != 0){
            am_text_appendf(&t, ",{\"id\":\"p%lld\",\"kind\":\"pseudocode\",\"title\":", i + 1);
            AM_Text s = {};
            am_text_append(&s, "Pseudocode: ");
            am_text_append(&s, l->title);
            append_clipped(&t, s.str, AM_STORY_MAX_TITLE, 1);
            am_text_free(&s);
            append_story_range(&t, &l->pseudo);
            am_text_append(&t, ",\"text\":");
            AM_Text text = {};
            if (l->code.file[0] != 0){
                char where[512];
                snprintf(where, sizeof(where), "Implemented in %s, lines %lld-%lld. ",
                         path_base(l->code.file), l->code.line, l->code.end_line);
                am_text_append(&text, where);
            }
            else{
                am_text_append(&text, "Not implemented. ");
            }
            am_text_append(&text, l->text);
            append_clipped(&t, text.str, AM_STORY_MAX_TEXT, 0);
            am_text_free(&text);
            am_text_append(&t, ",\"next\":[");
            const char *sep = "";
            if (l->code.file[0] != 0){
                am_text_appendf(&t, "{\"to\":\"c%lld\",\"label\":\"implemented by\"}", i + 1);
                sep = ",";
            }
            if (has_next){
                am_text_append(&t, sep);
                am_text_append(&t, "{\"label\":\"next pseudocode step\",\"to\":");
                append_link_node_id(&t, j, i + 1, 0);
                am_text_append(&t, "}");
            }
            am_text_append(&t, "]}");
        }
        if (l->code.file[0] != 0){
            am_text_appendf(&t, ",{\"id\":\"c%lld\",\"kind\":\"code\",\"title\":", i + 1);
            append_clipped(&t, l->title, AM_STORY_MAX_TITLE, 1);
            append_story_range(&t, &l->code);
            am_text_append(&t, ",\"text\":");
            append_clipped(&t, l->text[0] ? l->text : (l->pseudo.file[0] ? "" : "Added with no pseudocode of its own."), AM_STORY_MAX_TEXT, 0);
            am_text_append(&t, ",\"next\":[");
            const char *sep = "";
            if (has_next){
                am_text_append(&t, "{\"label\":\"next step\",\"to\":");
                append_link_node_id(&t, j, i + 1, 1);
                am_text_append(&t, "}");
                sep = ",";
            }
            if (l->pseudo.file[0] != 0){
                am_text_append(&t, sep);
                am_text_appendf(&t, "{\"to\":\"p%lld\",\"label\":\"its pseudocode\"}", i + 1);
            }
            am_text_append(&t, "]}");
        }
    }
    am_text_append(&t, "]}");

    AM_Json *root = am_json_parse(t.str, t.size);
    int ok = (am_story_from_json(root, out, err, err_size) == 0);
    am_json_free(root);
    am_text_free(&t);
    return(ok);
}

static void
story_node_add_edge(AM_Story_Node *n, const char *to, const char *story, const char *label){
    n->next = (AM_Story_Edge*)realloc(n->next, sizeof(AM_Story_Edge)*(n->next_count + 1));
    AM_Story_Edge *e = &n->next[n->next_count++];
    e->to = am_strdup(to);
    e->story = am_strdup(story);
    e->label = clip_dup(label, AM_STORY_MAX_LABEL, 1);
}

// NOTE: drops every edge into story_id from the other stories; returns how many
static int
stories_unlink(const char *story_id){
    int removed = 0;
    for (int s = 0; s < story_store.count; s += 1){
        AM_Story *story = &story_store.stories[s];
        for (int i = 0; i < story->node_count; i += 1){
            AM_Story_Node *n = &story->nodes[i];
            for (int k = 0; k < n->next_count;){
                if (str_eq(n->next[k].story, story_id)){
                    free(n->next[k].to); free(n->next[k].story); free(n->next[k].label);
                    memmove(&n->next[k], &n->next[k + 1], sizeof(AM_Story_Edge)*(n->next_count - k - 1));
                    n->next_count -= 1;
                    removed += 1;
                }
                else{
                    k += 1;
                }
            }
        }
    }
    return(removed);
}

static int
ranges_overlap(AM_Pseudo_Range *r, AM_Story_Node *n){
    return(r->file[0] != 0 && n->file[0] != 0 && str_eq(r->file, n->file) &&
           r->line <= n->end_line && n->line <= r->end_line);
}

#define PSEUDO_MAX_CROSS_EDGES 2

// NOTE: links the steps of other stories that show the same code (or pseudocode) to the
// matching slides of ps, and those slides back, while both have room for an edge
static void
pseudo_link_stories(AM_Pseudo_Job *j, AM_Story *ps){
    for (int s = 0; s < story_store.count; s += 1){
        AM_Story *story = &story_store.stories[s];
        if (strncmp(story->id, "pseudo-", 7) == 0) continue;
        for (int i = 0; i < story->node_count; i += 1){
            AM_Story_Node *n = &story->nodes[i];
            int added = 0;
            for (int k = 0; k < j->link_count && added < PSEUDO_MAX_CROSS_EDGES; k += 1){
                AM_Pseudo_Link *l = &j->links[k];
                char target[32];
                const char *label;
                if (ranges_overlap(&l->code, n)){
                    snprintf(target, sizeof(target), "c%d", k + 1);
                    label = "written from pseudocode";
                }
                else if (ranges_overlap(&l->pseudo, n)){
                    snprintf(target, sizeof(target), "p%d", k + 1);
                    label = "pseudocode for this";
                }
                else{
                    continue;
                }
                AM_Story_Node *t = am_story_find_node(ps, target);
                if (t == 0 || n->next_count >= AM_STORY_MAX_EDGES) break;
                story_node_add_edge(n, target, ps->id, label);
                added += 1;
                if (t->next_count < AM_STORY_MAX_EDGES){
                    AM_Text back = {};
                    am_text_append(&back, "in story: ");
                    am_text_append(&back, story->title);
                    story_node_add_edge(t, n->id, story->id, back.str);
                    am_text_free(&back);
                }
            }
        }
    }
}

// NOTE: replaces the request's story with one built from its answer
static int
pseudo_publish_story(AM_Pseudo_Job *j, char *err, size_t err_size){
    AM_Story ps;
    if (!pseudo_build_story(j, &ps, err, err_size)) return(0);
    stories_unlink(ps.id);
    pseudo_link_stories(j, &ps);
    long long now = (long long)time(0);
    ps.created = now;
    ps.updated = now;
    free(j->story);
    j->story = am_strdup(ps.id);
    story_store_upsert(&ps);
    story_store.revision += 1;
    return(1);
}

static void
pseudo_list_json(AM_Text *out, const char *status){
    am_text_appendf(out, "{\"revision\":%lld,\"requests\":[", pseudo_store.revision);
    int first = 1;
    for (int i = 0; i < pseudo_store.count; i += 1){
        AM_Pseudo_Job *j = &pseudo_store.jobs[i];
        if (status != 0 && !str_eq(j->status, status)) continue;
        if (!first) am_text_append(out, ",");
        first = 0;
        am_pseudo_job_to_json(out, j);
    }
    am_text_append(out, "]}\n");
}

// NOTE: /v1/pseudo routes; returns whether the store changed
static int
handle_pseudo(int fd, Request *req, AM_Text *out){
    const char *rest = (req->path[10] == '/') ? req->path + 11 : 0;
    long long id = 0;
    const char *action = "";
    if (rest != 0){
        char *end = 0;
        id = strtoll(rest, &end, 10);
        if (id <= 0 || end == rest || (*end != 0 && *end != '/')){
            respond_error(fd, 404, "no request with that id");
            return(0);
        }
        action = (*end == '/') ? end + 1 : "";
    }
    int is_get = (strcmp(req->method, "GET") == 0);
    int is_post = (strcmp(req->method, "POST") == 0);
    int is_delete = (strcmp(req->method, "DELETE") == 0);
    AM_Pseudo_Job *j = (id != 0) ? pseudo_find(id) : 0;
    if (id != 0 && j == 0){
        respond_error(fd, 404, "no request with that id");
        return(0);
    }

    if (is_get && id == 0){
        char since[32], status[32];
        long long since_revision = query_get(req->query, "since_revision", since, sizeof(since)) ? atoll(since) : -1;
        int has_status = query_get(req->query, "status", status, sizeof(status));
        if (since_revision == pseudo_store.revision){
            am_text_appendf(out, "{\"revision\":%lld,\"unchanged\":true}\n", pseudo_store.revision);
        }
        else{
            pseudo_list_json(out, has_status ? status : 0);
        }
        respond(fd, 200, out);
        return(0);
    }
    if (is_get && action[0] == 0){
        am_pseudo_job_to_json(out, j);
        am_text_append(out, "\n");
        respond(fd, 200, out);
        return(0);
    }
    if (is_post && id == 0){
        AM_Json *root = (req->body.str != 0) ? am_json_parse(req->body.str, req->body.size) : 0;
        AM_Pseudo_Job job;
        char err[512];
        if (root == 0){
            respond_error(fd, 400, "body is not valid JSON");
            return(0);
        }
        if (am_pseudo_request_from_json(root, &job, err, sizeof(err)) != 0){
            am_json_free(root);
            respond_error(fd, 400, err);
            return(0);
        }
        am_json_free(root);
        job.id = pseudo_store.next_id++;
        pseudo_set_status(&job, "pending");
        free(job.agent);
        job.agent = am_strdup(0);
        job.created = job.updated;
        pseudo_append(&job);
        pseudo_store.revision += 1;
        am_text_appendf(out, "{\"id\":%lld", job.id);
        am_text_appendf(out, ",\"revision\":%lld}\n", pseudo_store.revision);
        respond(fd, 200, out);
        return(1);
    }
    if (is_post && (strcmp(action, "claim") == 0 || strcmp(action, "answer") == 0 || strcmp(action, "cancel") == 0)){
        AM_Json *root = (req->body.str != 0 && req->body.size > 0) ? am_json_parse(req->body.str, req->body.size) : 0;
        if (req->body.size > 0 && root == 0){
            respond_error(fd, 400, "body is not valid JSON");
            return(0);
        }
        const char *agent = am_json_get_string(root, "agent");
        char msg[512];
        int changed = 0;
        if (strcmp(action, "claim") == 0){
            if (!str_eq(j->status, "pending")){
                snprintf(msg, sizeof(msg), "request %lld is %s%s%s", j->id, j->status, j->agent[0] ? " by " : "", j->agent);
                respond_error(fd, 409, msg);
            }
            else{
                pseudo_set_status(j, "claimed");
                free(j->agent);
                j->agent = am_strdup(agent);
                changed = 1;
            }
        }
        else if (strcmp(action, "cancel") == 0){
            if (am_pseudo_finished(j)){
                snprintf(msg, sizeof(msg), "request %lld is already %s", j->id, j->status);
                respond_error(fd, 409, msg);
            }
            else{
                pseudo_set_status(j, "canceled");
                changed = 1;
            }
        }
        else if (str_eq(j->status, "canceled")){
            snprintf(msg, sizeof(msg), "request %lld was canceled", j->id);
            respond_error(fd, 409, msg);
        }
        else{
            char err[512];
            if (am_pseudo_answer_from_json(root, j, err, sizeof(err)) != 0){
                respond_error(fd, 400, err);
            }
            else{
                if (agent != 0 && agent[0] != 0){
                    free(j->agent);
                    j->agent = am_strdup(agent);
                }
                int failed = (j->error[0] != 0);
                if (j->link_count > 0 && !pseudo_publish_story(j, err, sizeof(err))){
                    // NOTE: the answer was valid, so this is a bug in the story builder
                    fprintf(stderr, "4coder-markers: story for request %lld: %s\n", j->id, err);
                }
                pseudo_set_status(j, failed ? "failed" : "done");
                changed = 1;
            }
        }
        am_json_free(root);
        if (changed){
            pseudo_store.revision += 1;
            am_pseudo_job_to_json(out, j);
            am_text_append(out, "\n");
            respond(fd, 200, out);
        }
        return(changed);
    }
    if (is_delete){
        int removed = 0;
        if (id != 0){
            for (int i = 0; i < pseudo_store.count; i += 1){
                if (pseudo_store.jobs[i].id == id){ pseudo_remove_at(i); removed = 1; break; }
            }
        }
        else{
            // NOTE: clears finished requests; open ones stay
            for (int i = 0; i < pseudo_store.count;){
                if (am_pseudo_finished(&pseudo_store.jobs[i])){ pseudo_remove_at(i); removed += 1; }
                else i += 1;
            }
        }
        if (removed > 0) pseudo_store.revision += 1;
        am_text_appendf(out, "{\"removed\":%lld", removed);
        am_text_appendf(out, ",\"revision\":%lld}\n", pseudo_store.revision);
        respond(fd, 200, out);
        return(removed > 0);
    }
    respond_error(fd, 405, "unknown endpoint or method");
    return(0);
}

static void
handle(int fd, Request *req){
    AM_Text out = {};
    int changed = 0;

    if (strcmp(req->path, "/v1/health") == 0){
        am_text_appendf(&out, "{\"ok\":true,\"revision\":%lld", store.revision);
        am_text_appendf(&out, ",\"count\":%lld", store.count);
        am_text_appendf(&out, ",\"stories\":%lld", story_store.count);
        int pending = 0;
        for (int i = 0; i < pseudo_store.count; i += 1) pending += str_eq(pseudo_store.jobs[i].status, "pending");
        am_text_appendf(&out, ",\"pseudo_pending\":%lld,\"version\":", pending);
        am_json_append_string(&out, FCODER_MARKERS_VERSION);
        am_text_append(&out, "}\n");
        respond(fd, 200, &out);
    }
    else if (strcmp(req->path, "/v1/markers") == 0 && strcmp(req->method, "GET") == 0){
        char since[32], file[PATH_MAX];
        long long since_revision = query_get(req->query, "since_revision", since, sizeof(since)) ? atoll(since) : -1;
        int filter = query_get(req->query, "file", file, sizeof(file));
        if (since_revision == store.revision){
            am_text_appendf(&out, "{\"revision\":%lld,\"unchanged\":true}\n", store.revision);
        }
        else{
            qsort(store.markers, store.count, sizeof(AM_Marker), marker_compare);
            am_text_appendf(&out, "{\"revision\":%lld,\"markers\":[", store.revision);
            int first = 1;
            for (int i = 0; i < store.count; i += 1){
                if (filter && !str_eq(store.markers[i].file, file)) continue;
                if (!first) am_text_append(&out, ",");
                first = 0;
                am_marker_to_json(&out, &store.markers[i]);
            }
            am_text_append(&out, "]}\n");
        }
        respond(fd, 200, &out);
    }
    else if (strcmp(req->path, "/v1/markers") == 0 && strcmp(req->method, "POST") == 0){
        AM_Json *root = (req->body.str != 0) ? am_json_parse(req->body.str, req->body.size) : 0;
        AM_Json *items = root;
        AM_Json single_list = {};
        if (root != 0 && root->kind == AMJson_Object){
            AM_Json *list = am_json_get(root, "markers");
            if (list != 0 && list->kind == AMJson_Array){
                items = list;
            }
            else{
                // NOTE: a single marker object
                single_list.kind = AMJson_Array;
                single_list.first_child = root;
                items = &single_list;
            }
        }
        if (items == 0 || items->kind != AMJson_Array){
            respond_error(fd, 400, "body must be a marker object, an array of markers, or {\"markers\": [...]}");
        }
        else{
            // NOTE: validate everything before storing anything
            int count = 0;
            for (AM_Json *c = items->first_child; c != 0; c = c->next) count += 1;
            AM_Marker *parsed = (AM_Marker*)calloc(count ? count : 1, sizeof(AM_Marker));
            const char *error = 0;
            int bad_index = -1;
            int i = 0;
            for (AM_Json *c = items->first_child; c != 0 && error == 0; c = c->next, i += 1){
                error = am_marker_from_json(c, &parsed[i]);
                if (error) bad_index = i;
            }
            if (count == 0){
                error = "no markers given";
            }
            if (error != 0){
                char msg[512];
                if (bad_index >= 0) snprintf(msg, sizeof(msg), "marker %d: %s", bad_index, error);
                else snprintf(msg, sizeof(msg), "%s", error);
                respond_error(fd, 400, msg);
                for (int k = 0; k < count; k += 1) am_marker_free(&parsed[k]);
            }
            else{
                am_text_append(&out, "{\"ids\":[");
                for (int k = 0; k < count; k += 1){
                    parsed[k].created = (long long)time(0);
                    long long id = store_upsert(&parsed[k]);
                    if (k > 0) am_text_append(&out, ",");
                    am_text_appendf(&out, "%lld", id);
                }
                store.revision += 1;
                changed = 1;
                am_text_appendf(&out, "],\"revision\":%lld}\n", store.revision);
                respond(fd, 200, &out);
            }
            free(parsed);
        }
        if (items == &single_list) single_list.first_child = 0;
        am_json_free(root);
    }
    else if (strncmp(req->path, "/v1/markers", 11) == 0 && strcmp(req->method, "DELETE") == 0){
        int removed = 0;
        if (req->path[11] == '/'){
            long long id = atoll(req->path + 12);
            removed = (id > 0) ? store_remove_if(id, 0, 0) : 0;
            if (removed == 0){
                respond_error(fd, 404, "no marker with that id");
                return;
            }
        }
        else if (req->path[11] == 0){
            char file[PATH_MAX], agent[256];
            int has_file = query_get(req->query, "file", file, sizeof(file));
            int has_agent = query_get(req->query, "agent", agent, sizeof(agent));
            removed = store_remove_if(0, has_file ? file : 0, has_agent ? agent : 0);
        }
        else{
            respond_error(fd, 404, "unknown endpoint");
            return;
        }
        if (removed > 0){
            store.revision += 1;
            changed = 1;
        }
        am_text_appendf(&out, "{\"removed\":%lld", removed);
        am_text_appendf(&out, ",\"revision\":%lld}\n", store.revision);
        respond(fd, 200, &out);
    }
    else if (strncmp(req->path, "/v1/stories", 11) == 0 && (req->path[11] == 0 || req->path[11] == '/')){
        changed = handle_stories(fd, req, &out);
    }
    else if (strncmp(req->path, "/v1/pseudo", 10) == 0 && (req->path[10] == 0 || req->path[10] == '/')){
        changed = handle_pseudo(fd, req, &out);
    }
    else if (strncmp(req->path, "/v1/", 4) == 0){
        respond_error(fd, strncmp(req->path, "/v1/markers", 11) == 0 ? 405 : 404, "unknown endpoint or method");
    }
    else{
        respond_error(fd, 404, "unknown endpoint");
    }

    if (changed){
        store_save();
    }
    am_text_free(&out);
}

static int
serve(int port){
    signal(SIGPIPE, SIG_IGN);
    store_load();

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    // NOTE: loopback only; anything that can reach this port can post markers
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 32) != 0){
        fprintf(stderr, "4coder-markers: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        return(1);
    }
    fprintf(stderr, "4coder-markers: listening on http://127.0.0.1:%d (%d markers%s%s)\n",
            port, store.count, store.path ? ", store " : "", store.path ? store.path : "");

    for (;;){
        int fd = accept(listener, 0, 0);
        if (fd < 0){
            if (errno == EINTR) continue;
            break;
        }
        // NOTE: a slow client can't hold up the editor's polling for long
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        Request req = {};
        int r = read_request(fd, &req);
        if (r == 1) handle(fd, &req);
        else if (r < 0) respond_error(fd, 413, "request body larger than 1MB");
        else respond_error(fd, 400, "malformed request");
        am_text_free(&req.body);
        close(fd);
    }
    return(0);
}

////////////////////////////////
// NOTE: command line client

static int port_option = 0;

static int
service_port(void){
    if (port_option > 0) return(port_option);
    const char *env = getenv("FCODER_MARKERS_PORT");
    if (env != 0 && atoi(env) > 0) return(atoi(env));
    return(AGENT_MARKERS_DEFAULT_PORT);
}

static void
url_encode(AM_Text *t, const char *s){
    for (const unsigned char *c = (const unsigned char*)s; *c; c += 1){
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.~/", *c)){
            am_text_append(t, (const char*)c, 1);
        }
        else{
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", *c);
            am_text_append(t, buf);
        }
    }
}

static int
call_service(const char *method, const char *path, AM_Text *body, AM_Text *out){
    int status = am_http_request("127.0.0.1", service_port(), method, path,
                                 body ? body->str : 0, body ? body->size : 0, out, 5000);
    if (status == 0){
        fprintf(stderr, "4coder-markers: no service on 127.0.0.1:%d (start it with `4coder-markers serve`)\n", service_port());
    }
    return(status);
}

// NOTE: print the service's error message and exit non-zero on failure
static int
report(int status, AM_Text *out){
    if (status == 0) return(2);
    if (status != 200){
        AM_Json *j = out->str ? am_json_parse(out->str, out->size) : 0;
        const char *msg = am_json_get_string(j, "error");
        fprintf(stderr, "4coder-markers: %s\n", msg ? msg : (out->str ? out->str : "request failed"));
        am_json_free(j);
        return(1);
    }
    return(0);
}

////////////////////////////////
// NOTE: daemon

static void
make_dirs(const char *path){
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *c = buf + 1; *c; c += 1){
        if (*c == '/'){
            *c = 0;
            mkdir(buf, 0755);
            *c = '/';
        }
    }
    mkdir(buf, 0755);
}

// NOTE: --state DIR, $FCODER_MARKERS_HOME or ~/.4coder-markers, made absolute
static int
state_dir(const char *option, char *out, size_t out_size){
    char raw[PATH_MAX];
    const char *home = getenv("HOME");
    const char *env = getenv("FCODER_MARKERS_HOME");
    if (option != 0) snprintf(raw, sizeof(raw), "%s", option);
    else if (env != 0 && env[0] != 0) snprintf(raw, sizeof(raw), "%s", env);
    else snprintf(raw, sizeof(raw), "%s/.4coder-markers", home ? home : ".");
    make_dirs(raw);
    if (realpath(raw, out) == 0 || strlen(out) + 32 > out_size){
        fprintf(stderr, "4coder-markers: cannot use state directory %s: %s\n", raw, strerror(errno));
        return(0);
    }
    return(1);
}

static long
read_pid(const char *dir){
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/daemon.pid", dir);
    FILE *f = fopen(path, "rb");
    long pid = 0;
    if (f != 0){
        if (fscanf(f, "%ld", &pid) != 1) pid = 0;
        fclose(f);
    }
    if (pid > 0 && kill((pid_t)pid, 0) != 0 && errno == ESRCH) pid = 0;
    return(pid);
}

static int call_service(const char *method, const char *path, AM_Text *body, AM_Text *out);

// NOTE: the running service's health reply, or 0 when nothing answers (no error printed)
static AM_Json*
probe_health(void){
    AM_Text out = {};
    int status = am_http_request("127.0.0.1", service_port(), "GET", "/v1/health", 0, 0, &out, 2000);
    AM_Json *j = (status == 200 && out.str) ? am_json_parse(out.str, out.size) : 0;
    am_text_free(&out);
    return(j);
}

static int
daemon_command(const char *sub, const char *state_option){
    char dir[PATH_MAX];
    if (!state_dir(state_option, dir, sizeof(dir))) return(1);
    char pid_path[PATH_MAX], log_path[PATH_MAX], store_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/daemon.pid", dir);
    snprintf(log_path, sizeof(log_path), "%s/daemon.log", dir);
    snprintf(store_path, sizeof(store_path), "%s/store.json", dir);
    int port = service_port();

    if (strcmp(sub, "status") == 0){
        AM_Json *health = probe_health();
        long pid = read_pid(dir);
        if (health == 0){
            printf("not running (port %d, state %s)\n", port, dir);
            return(3);
        }
        const char *version = am_json_get_string(health, "version");
        printf("running: port %d, version %s, %lld markers, %lld stories", port, version ? version : "(old)",
               am_json_get_int(health, "count", 0), am_json_get_int(health, "stories", 0));
        if (pid > 0) printf(", pid %ld, state %s\n", pid, dir);
        else printf(" (not started by this daemon's state %s)\n", dir);
        am_json_free(health);
        return(0);
    }

    if (strcmp(sub, "stop") == 0){
        long pid = read_pid(dir);
        if (pid <= 0){
            AM_Json *health = probe_health();
            if (health != 0){
                am_json_free(health);
                fprintf(stderr, "4coder-markers: a service answers on port %d but %s has no pid for it\n", port, pid_path);
                return(1);
            }
            printf("not running\n");
            unlink(pid_path);
            return(0);
        }
        kill((pid_t)pid, SIGTERM);
        for (int i = 0; i < 50 && kill((pid_t)pid, 0) == 0; i += 1){
            usleep(100*1000);
        }
        if (kill((pid_t)pid, 0) == 0){
            fprintf(stderr, "4coder-markers: pid %ld did not stop\n", pid);
            return(1);
        }
        unlink(pid_path);
        printf("stopped pid %ld\n", pid);
        return(0);
    }

    if (strcmp(sub, "start") != 0){
        fputs("4coder-markers daemon: start, stop or status\n", stderr);
        return(1);
    }

    AM_Json *health = probe_health();
    if (health != 0){
        const char *version = am_json_get_string(health, "version");
        printf("already running on port %d (version %s)\n", port, version ? version : "(old)");
        am_json_free(health);
        return(0);
    }

    pid_t child = fork();
    if (child < 0){
        fprintf(stderr, "4coder-markers: fork failed: %s\n", strerror(errno));
        return(1);
    }
    if (child == 0){
        // NOTE: double fork so the service is not a child of the shell (or ssh) that started it
        setsid();
        pid_t grandchild = fork();
        if (grandchild != 0) _exit(grandchild < 0 ? 1 : 0);
        if (chdir("/") != 0) _exit(1);
        int null_fd = open("/dev/null", O_RDONLY);
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (null_fd < 0 || log_fd < 0) _exit(1);
        dup2(null_fd, 0);
        dup2(log_fd, 1);
        dup2(log_fd, 2);
        close(null_fd);
        close(log_fd);
        FILE *f = fopen(pid_path, "wb");
        if (f != 0){
            fprintf(f, "%ld\n", (long)getpid());
            fclose(f);
        }
        time_t now = time(0);
        fprintf(stderr, "4coder-markers %s: daemon starting %s", FCODER_MARKERS_VERSION, ctime(&now));
        store.path = store_path;
        int result = serve(port);
        unlink(pid_path);
        _exit(result);
    }
    int status = 0;
    waitpid(child, &status, 0);
    for (int i = 0; i < 30; i += 1){
        health = probe_health();
        if (health != 0){
            am_json_free(health);
            printf("started on port %d, pid %ld, state %s\n", port, read_pid(dir), dir);
            return(0);
        }
        usleep(100*1000);
    }
    fprintf(stderr, "4coder-markers: the daemon did not come up; see %s\n", log_path);
    return(1);
}

static const char *usage =
"4coder-markers: attach one-sentence notes to code locations for 4coder to show.\n"
"\n"
"usage:\n"
"  4coder-markers serve [--port P] [--store FILE]   run the service (127.0.0.1 only)\n"
"  4coder-markers add --file F --line N [--end-line M] --why TEXT --what TEXT [--agent NAME]\n"
"                                                   add or update a marker\n"
"  4coder-markers list [--file F]                   show markers\n"
"  4coder-markers rm ID                             delete one marker\n"
"  4coder-markers clear [--file F] [--agent NAME]   delete markers (all if no filter)\n"
"  4coder-markers health                            check the service is up\n"
"  4coder-markers daemon start|stop|status [--state DIR]\n"
"                                                   run the service in the background, keeping\n"
"                                                   its store, pid and log in DIR\n"
"                                                   (default $FCODER_MARKERS_HOME or ~/.4coder-markers)\n"
"  4coder-markers version                           print the version\n"
"\n"
"  --why   justification: why this code matters, in ONE sentence (<= 300 chars)\n"
"  --what  explanation: what this code does, in ONE sentence (<= 300 chars)\n"
"  --file  path to the file; relative paths are made absolute\n"
"  --port  service port (default 7341, or $FCODER_MARKERS_PORT)\n"
"\n"
"Posting the same (agent, file, line) again replaces that marker.\n"
"\n"
"stories (a branching walk-through of the code, shown as slides in 4coder):\n"
"  4coder-markers story post FILE.json|- [--agent NAME]   add or replace stories (by id)\n"
"  4coder-markers story check FILE.json|-                 validate without posting\n"
"  4coder-markers story list                              show stories\n"
"  4coder-markers story show ID                           print a story as an outline\n"
"  4coder-markers story rm ID                             delete one story\n"
"  4coder-markers story clear [--agent NAME]              delete stories (all if no filter)\n"
"\n"
"  Node \"file\" paths are made absolute. See README.md for the story format.\n"
"\n"
"pseudocode requests (the editor sends files with pseudocode; an agent implements them):\n"
"  4coder-markers pseudo wait [--agent NAME] [--timeout S] [--no-claim]\n"
"                                     wait for a request (default 540 s, exit 4 on timeout),\n"
"                                     claim it and print it\n"
"  4coder-markers pseudo list [--status S] | show ID | claim ID [--agent NAME]\n"
"  4coder-markers pseudo check FILE.json|-                validate an answer\n"
"  4coder-markers pseudo answer ID FILE.json|- [--agent NAME]\n"
"                                     answer with links from pseudocode to code; the service\n"
"                                     builds story pseudo-ID from them\n"
"  4coder-markers pseudo fail ID --reason TEXT [--agent NAME]\n"
"  4coder-markers pseudo request [--instructions TEXT] FILE...   post a request (as the editor does)\n"
"  4coder-markers pseudo cancel ID | rm ID | clear\n";

static const char *arg_value(int argc, char **argv, const char *name);

static int
read_all(const char *path, AM_Text *out){
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (f == 0){
        fprintf(stderr, "4coder-markers: cannot read %s: %s\n", path, strerror(errno));
        return(0);
    }
    char buf[8192];
    size_t n;
    am_text_append(out, "", 0);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) am_text_append(out, buf, n);
    if (f != stdin) fclose(f);
    return(1);
}

static void
print_story_outline(AM_Story *s){
    printf("%s  \"%s\"", s->id, s->title);
    if (s->agent[0]) printf("  [%s]", s->agent);
    printf("\n");
    if (s->summary[0]) printf("  %s\n", s->summary);
    AM_Story_Node *start = am_story_start_node(s);
    for (int i = 0; i < s->node_count; i += 1){
        AM_Story_Node *n = &s->nodes[i];
        printf("  %s %s: %s", (n == start) ? "*" : "-", n->id, n->title);
        if (n->file[0]) printf("  (%s:%lld-%lld)", n->file, n->line, n->end_line);
        else printf("  (%s)", n->kind);
        printf("\n");
        for (int k = 0; k < n->next_count; k += 1){
            AM_Story_Edge *e = &n->next[k];
            printf("      %d -> %s%s%s", k + 1, e->story, e->story[0] ? ":" : "", e->to);
            if (e->label[0]) printf("  \"%s\"", e->label);
            printf("\n");
        }
    }
}

// NOTE: parses stories from FILE, makes node paths absolute and re-encodes them as {"stories":[...]}
static int
load_stories(const char *path, const char *agent, AM_Text *body, int print){
    AM_Text text = {};
    if (!read_all(path, &text)) return(1);
    AM_Json *root = am_json_parse(text.str, text.size);
    AM_Json single;
    AM_Json *items = am_stories_items(root, &single);
    int result = 0;
    if (items == 0){
        fprintf(stderr, "4coder-markers: %s: %s\n", path, root == 0 ? "not valid JSON" :
                "expected a story object, an array of stories, or {\"stories\": [...]}");
        result = 1;
    }
    am_text_append(body, "{\"stories\":[");
    int index = 0;
    for (AM_Json *c = items ? items->first_child : 0; c != 0; c = c->next, index += 1){
        AM_Story story;
        char err[512];
        if (am_story_from_json(c, &story, err, sizeof(err)) != 0){
            const char *sid = am_json_get_string(c, "id");
            fprintf(stderr, "4coder-markers: story %d%s%s%s: %s\n", index,
                    sid ? " (\"" : "", sid ? sid : "", sid ? "\")" : "", err);
            result = 1;
            continue;
        }
        if (agent != 0 && story.agent[0] == 0){
            free(story.agent);
            story.agent = am_strdup(agent);
        }
        for (int i = 0; i < story.node_count; i += 1){
            char abs_path[PATH_MAX];
            AM_Story_Node *n = &story.nodes[i];
            if (n->file[0] != 0 && strncmp(n->file, "/ssh:", 5) != 0 && realpath(n->file, abs_path) != 0){
                free(n->file);
                n->file = am_strdup(abs_path);
            }
        }
        if (index > 0) am_text_append(body, ",");
        am_story_to_json(body, &story);
        if (print) print_story_outline(&story);
        am_story_free(&story);
    }
    am_text_append(body, "]}");
    am_json_free(root);
    am_text_free(&text);
    return(result);
}

static int
story_command(int argc, char **argv, AM_Text *out){
    const char *sub = (argc >= 3) ? argv[2] : "";
    const char *arg = (argc >= 4) ? argv[3] : 0;
    const char *agent = arg_value(argc, argv, "--agent");
    int result = 0;
    if ((strcmp(sub, "post") == 0 || strcmp(sub, "check") == 0) && arg != 0){
        int check = (strcmp(sub, "check") == 0);
        AM_Text body = {};
        result = load_stories(arg, agent, &body, check);
        if (result == 0 && check){
            printf("ok\n");
        }
        else if (result == 0){
            result = report(call_service("POST", "/v1/stories", &body, out), out);
            if (result == 0){
                AM_Json *j = am_json_parse(out->str, out->size);
                AM_Json *ids = am_json_get(j, "ids");
                for (AM_Json *c = ids ? ids->first_child : 0; c != 0; c = c->next){
                    if (c->kind == AMJson_String) printf("story %s posted\n", c->string);
                }
                am_json_free(j);
            }
        }
        am_text_free(&body);
    }
    else if (strcmp(sub, "list") == 0 || (strcmp(sub, "show") == 0 && arg != 0)){
        int show = (strcmp(sub, "show") == 0);
        AM_Text path = {};
        am_text_append(&path, "/v1/stories");
        if (show){
            am_text_append(&path, "/");
            url_encode(&path, arg);
        }
        result = report(call_service("GET", path.str, 0, out), out);
        if (result == 0){
            AM_Json *j = am_json_parse(out->str, out->size);
            AM_Json *list = show ? 0 : am_json_get(j, "stories");
            AM_Json *c = show ? j : (list ? list->first_child : 0);
            int count = 0;
            for (; c != 0; c = show ? 0 : c->next){
                AM_Story story;
                char err[512];
                if (am_story_from_json(c, &story, err, sizeof(err)) != 0) continue;
                if (show){
                    print_story_outline(&story);
                }
                else{
                    int edges = 0;
                    for (int i = 0; i < story.node_count; i += 1) edges += story.nodes[i].next_count;
                    printf("%s  \"%s\"  %d nodes, %d edges", story.id, story.title, story.node_count, edges);
                    if (story.agent[0]) printf("  [%s]", story.agent);
                    printf("\n");
                }
                am_story_free(&story);
                count += 1;
            }
            if (count == 0 && !show) printf("no stories\n");
            am_json_free(j);
        }
        am_text_free(&path);
    }
    else if (strcmp(sub, "rm") == 0 && arg != 0){
        AM_Text path = {};
        am_text_append(&path, "/v1/stories/");
        url_encode(&path, arg);
        result = report(call_service("DELETE", path.str, 0, out), out);
        if (result == 0) printf("removed story %s\n", arg);
        am_text_free(&path);
    }
    else if (strcmp(sub, "clear") == 0){
        AM_Text path = {};
        am_text_append(&path, "/v1/stories");
        if (agent != 0){
            am_text_append(&path, "?agent=");
            url_encode(&path, agent);
        }
        result = report(call_service("DELETE", path.str, 0, out), out);
        if (result == 0){
            AM_Json *j = am_json_parse(out->str, out->size);
            printf("removed %lld stories\n", am_json_get_int(j, "removed", 0));
            am_json_free(j);
        }
        am_text_free(&path);
    }
    else{
        fputs(usage, stderr);
        result = 1;
    }
    return(result);
}

static void
print_range(const char *what, AM_Pseudo_Range *r){
    if (r->file[0] != 0) printf("%s %s:%lld-%lld", what, r->file, r->line, r->end_line);
    else printf("%s (none)", what);
}

static void
print_pseudo_job(AM_Pseudo_Job *j, int full){
    printf("#%lld %s", j->id, j->status);
    if (j->agent[0]) printf(" [%s]", j->agent);
    printf("  %d file%s", j->file_count, j->file_count == 1 ? "" : "s");
    if (j->instructions[0]){
        char *first = clip_dup(j->instructions, 72, 1);
        printf("  \"%s\"", first);
        free(first);
    }
    printf("\n");
    if (!full) return;
    if (j->instructions[0]) printf("  instructions:\n    %s\n", j->instructions);
    printf("  files:\n");
    for (int i = 0; i < j->file_count; i += 1){
        printf("    %s", j->files[i].file);
        if (j->files[i].host[0]) printf("  (on %s)", j->files[i].host);
        printf("\n");
    }
    if (j->summary[0] || j->error[0]){
        if (j->error[0]) printf("  error: %s\n", j->error);
        if (j->summary[0]) printf("  summary: %s\n", j->summary);
        if (j->story[0]) printf("  story: %s\n", j->story);
        for (int i = 0; i < j->changed_count; i += 1) printf("  changed: %s\n", j->changed[i]);
        for (int i = 0; i < j->link_count; i += 1){
            AM_Pseudo_Link *l = &j->links[i];
            printf("  %d. %s\n     ", i + 1, l->title);
            print_range("pseudo", &l->pseudo);
            printf("\n     ");
            print_range("code  ", &l->code);
            printf("\n");
        }
    }
}

// NOTE: parses the service's reply as one job (or the "requests" list) and prints it
static int
print_pseudo_reply(AM_Text *out, int full){
    AM_Json *root = am_json_parse(out->str, out->size);
    AM_Json *list = am_json_get(root, "requests");
    int count = 0;
    AM_Json *c = (list != 0) ? list->first_child : root;
    for (; c != 0; c = (list != 0) ? c->next : 0){
        AM_Pseudo_Job j;
        char err[512];
        if (am_pseudo_job_from_json(c, &j, err, sizeof(err)) != 0) continue;
        print_pseudo_job(&j, full);
        am_pseudo_job_free(&j);
        count += 1;
    }
    am_json_free(root);
    return(count);
}

static void
make_absolute(char **path){
    char abs_path[PATH_MAX];
    if ((*path)[0] != 0 && strncmp(*path, "/ssh:", 5) != 0 && realpath(*path, abs_path) != 0){
        free(*path);
        *path = am_strdup(abs_path);
    }
}

// NOTE: reads an answer from FILE, makes its paths absolute and encodes it into body
static int
load_answer(const char *path, const char *agent, AM_Text *body){
    AM_Text text = {};
    if (!read_all(path, &text)) return(1);
    AM_Json *root = am_json_parse(text.str, text.size);
    AM_Pseudo_Job j = {};
    j.summary = am_strdup(0); j.error = am_strdup(0); j.story = am_strdup(0);
    char err[512];
    int result = 0;
    if (root == 0){
        fprintf(stderr, "4coder-markers: %s: not valid JSON\n", path);
        result = 1;
    }
    else if (am_pseudo_answer_from_json(root, &j, err, sizeof(err)) != 0){
        fprintf(stderr, "4coder-markers: %s: %s\n", path, err);
        result = 1;
    }
    else{
        for (int i = 0; i < j.changed_count; i += 1) make_absolute(&j.changed[i]);
        for (int i = 0; i < j.link_count; i += 1){
            make_absolute(&j.links[i].pseudo.file);
            make_absolute(&j.links[i].code.file);
        }
        AM_Text answer = {};
        am_pseudo_answer_to_json(&answer, &j);
        // NOTE: the agent goes in next to the answer's fields
        am_text_append(body, answer.str, answer.size - 1);
        am_text_append(body, ",\"agent\":");
        am_json_append_string(body, agent ? agent : "");
        am_text_append(body, "}");
        am_text_free(&answer);
    }
    free(j.summary); free(j.error); free(j.story);
    j.summary = j.error = j.story = 0;
    am_pseudo_answer_free(&j);
    free(j.summary); free(j.error); free(j.story);
    am_json_free(root);
    am_text_free(&text);
    return(result);
}

static int
pseudo_post(long long id, const char *action, AM_Text *body, AM_Text *out){
    char path[96];
    snprintf(path, sizeof(path), "/v1/pseudo/%lld/%s", id, action);
    AM_Text empty = {};
    am_text_append(&empty, "{}");
    int status = call_service("POST", path, body ? body : &empty, out);
    am_text_free(&empty);
    return(status);
}

static void
agent_body(AM_Text *body, const char *agent){
    am_text_append(body, "{\"agent\":");
    am_json_append_string(body, agent ? agent : "");
    am_text_append(body, "}");
}

static int
pseudo_command(int argc, char **argv, AM_Text *out){
    const char *sub = (argc >= 3) ? argv[2] : "";
    const char *arg = (argc >= 4) ? argv[3] : 0;
    const char *agent = arg_value(argc, argv, "--agent");
    long long id = arg ? atoll(arg) : 0;
    int result = 0;
    if (strcmp(sub, "list") == 0){
        const char *status = arg_value(argc, argv, "--status");
        AM_Text path = {};
        am_text_append(&path, "/v1/pseudo");
        if (status != 0){
            am_text_append(&path, "?status=");
            url_encode(&path, status);
        }
        result = report(call_service("GET", path.str, 0, out), out);
        if (result == 0 && print_pseudo_reply(out, 0) == 0) printf("no requests\n");
        am_text_free(&path);
    }
    else if (strcmp(sub, "show") == 0 && id > 0){
        char path[64];
        snprintf(path, sizeof(path), "/v1/pseudo/%lld", id);
        result = report(call_service("GET", path, 0, out), out);
        if (result == 0) print_pseudo_reply(out, 1);
    }
    else if (strcmp(sub, "wait") == 0){
        const char *timeout_arg = arg_value(argc, argv, "--timeout");
        long timeout = timeout_arg ? atol(timeout_arg) : 540;
        int claim = 1;
        for (int i = 2; i < argc; i += 1) if (strcmp(argv[i], "--no-claim") == 0) claim = 0;
        time_t until = time(0) + timeout;
        int warned = 0;
        result = 4;
        while (result == 4){
            AM_Text list = {};
            int status = am_http_request("127.0.0.1", service_port(), "GET", "/v1/pseudo?status=pending", 0, 0, &list, 2000);
            if (status != 200 && !warned){
                fprintf(stderr, "4coder-markers: no service on 127.0.0.1:%d yet (%d); still waiting\n", service_port(), status);
                warned = 1;
            }
            AM_Json *root = (status == 200 && list.str) ? am_json_parse(list.str, list.size) : 0;
            AM_Json *jobs = am_json_get(root, "requests");
            for (AM_Json *c = jobs ? jobs->first_child : 0; c != 0 && result == 4; c = c->next){
                long long jid = am_json_get_int(c, "id", 0);
                out->size = 0;
                if (!claim){
                    char path[64];
                    snprintf(path, sizeof(path), "/v1/pseudo/%lld", jid);
                    if (call_service("GET", path, 0, out) == 200) result = 0;
                    continue;
                }
                AM_Text body = {};
                agent_body(&body, agent);
                // NOTE: another agent may claim it first (409); try the next one
                if (pseudo_post(jid, "claim", &body, out) == 200) result = 0;
                am_text_free(&body);
            }
            am_json_free(root);
            am_text_free(&list);
            if (result == 0){
                print_pseudo_reply(out, 1);
            }
            else if (time(0) >= until){
                fprintf(stderr, "4coder-markers: no pseudocode request within %ld seconds\n", timeout);
                break;
            }
            else{
                sleep(1);
            }
        }
    }
    else if (strcmp(sub, "claim") == 0 && id > 0){
        AM_Text body = {};
        agent_body(&body, agent);
        result = report(pseudo_post(id, "claim", &body, out), out);
        if (result == 0) print_pseudo_reply(out, 1);
        am_text_free(&body);
    }
    else if (strcmp(sub, "check") == 0 && arg != 0){
        AM_Text body = {};
        result = load_answer(arg, agent, &body);
        if (result == 0) printf("ok\n");
        am_text_free(&body);
    }
    else if (strcmp(sub, "answer") == 0 && id > 0 && argc >= 5){
        AM_Text body = {};
        result = load_answer(argv[4], agent, &body);
        if (result == 0){
            result = report(pseudo_post(id, "answer", &body, out), out);
            if (result == 0){
                AM_Json *j = am_json_parse(out->str, out->size);
                AM_Json *answer = am_json_get(j, "answer");
                AM_Json *links = am_json_get(answer, "links");
                int n = 0;
                for (AM_Json *c = links ? links->first_child : 0; c != 0; c = c->next) n += 1;
                const char *story = am_json_get_string(answer, "story");
                printf("answered #%lld: %d links", id, n);
                if (story != 0) printf(", story %s", story);
                printf("\n");
                am_json_free(j);
            }
        }
        am_text_free(&body);
    }
    else if (strcmp(sub, "fail") == 0 && id > 0){
        const char *reason = arg_value(argc, argv, "--reason");
        AM_Text body = {};
        am_text_append(&body, "{\"error\":");
        am_json_append_string(&body, reason ? reason : "the agent could not implement this request");
        am_text_append(&body, ",\"agent\":");
        am_json_append_string(&body, agent ? agent : "");
        am_text_append(&body, "}");
        result = report(pseudo_post(id, "answer", &body, out), out);
        if (result == 0) printf("request #%lld marked failed\n", id);
        am_text_free(&body);
    }
    else if (strcmp(sub, "cancel") == 0 && id > 0){
        result = report(pseudo_post(id, "cancel", 0, out), out);
        if (result == 0) printf("request #%lld canceled\n", id);
    }
    else if (strcmp(sub, "rm") == 0 && id > 0){
        char path[64];
        snprintf(path, sizeof(path), "/v1/pseudo/%lld", id);
        result = report(call_service("DELETE", path, 0, out), out);
        if (result == 0) printf("removed request #%lld\n", id);
    }
    else if (strcmp(sub, "clear") == 0){
        result = report(call_service("DELETE", "/v1/pseudo", 0, out), out);
        if (result == 0){
            AM_Json *j = am_json_parse(out->str, out->size);
            printf("removed %lld finished requests\n", am_json_get_int(j, "removed", 0));
            am_json_free(j);
        }
    }
    else if (strcmp(sub, "request") == 0){
        const char *instructions = arg_value(argc, argv, "--instructions");
        AM_Text body = {};
        am_text_append(&body, "{\"client\":\"cli\",\"instructions\":");
        am_json_append_string(&body, instructions ? instructions : "");
        am_text_append(&body, ",\"files\":[");
        int count = 0;
        for (int i = 3; i < argc; i += 1){
            if (argv[i][0] == '-' && argv[i][1] == '-'){ i += 1; continue; }
            char *file = am_strdup(argv[i]);
            make_absolute(&file);
            if (count++ > 0) am_text_append(&body, ",");
            am_text_append(&body, "{\"file\":");
            am_json_append_string(&body, file);
            am_text_append(&body, "}");
            free(file);
        }
        am_text_append(&body, "]}");
        result = report(call_service("POST", "/v1/pseudo", &body, out), out);
        if (result == 0){
            AM_Json *j = am_json_parse(out->str, out->size);
            printf("request #%lld posted\n", am_json_get_int(j, "id", 0));
            am_json_free(j);
        }
        am_text_free(&body);
    }
    else{
        fputs(usage, stderr);
        result = 1;
    }
    return(result);
}

static const char*
arg_value(int argc, char **argv, const char *name){
    for (int i = 2; i + 1 < argc; i += 1){
        if (strcmp(argv[i], name) == 0) return(argv[i + 1]);
    }
    return(0);
}

int
main(int argc, char **argv){
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0){
        fputs(usage, argc < 2 ? stderr : stdout);
        return(argc < 2 ? 1 : 0);
    }
    const char *cmd = argv[1];
    const char *port = arg_value(argc, argv, "--port");
    if (port != 0) port_option = atoi(port);

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0){
        printf("4coder-markers %s\n", FCODER_MARKERS_VERSION);
        return(0);
    }
    if (strcmp(cmd, "daemon") == 0){
        return(daemon_command(argc >= 3 ? argv[2] : "", arg_value(argc, argv, "--state")));
    }
    if (strcmp(cmd, "serve") == 0){
        store.path = arg_value(argc, argv, "--store");
        return(serve(service_port()));
    }

    AM_Text out = {};
    int result = 0;
    if (strcmp(cmd, "add") == 0){
        const char *file = arg_value(argc, argv, "--file");
        const char *line = arg_value(argc, argv, "--line");
        const char *end_line = arg_value(argc, argv, "--end-line");
        const char *why = arg_value(argc, argv, "--why");
        const char *what = arg_value(argc, argv, "--what");
        const char *agent = arg_value(argc, argv, "--agent");
        if (file == 0 || line == 0 || why == 0 || what == 0){
            fputs("4coder-markers add: --file, --line, --why and --what are required\n", stderr);
            return(1);
        }
        char abs_path[PATH_MAX];
        if (strncmp(file, "/ssh:", 5) != 0 && realpath(file, abs_path) != 0){
            file = abs_path;
        }
        AM_Text body = {};
        am_text_append(&body, "{\"file\":");
        am_json_append_string(&body, file);
        am_text_appendf(&body, ",\"line\":%lld", atoll(line));
        if (end_line != 0) am_text_appendf(&body, ",\"end_line\":%lld", atoll(end_line));
        am_text_append(&body, ",\"justification\":");
        am_json_append_string(&body, why);
        am_text_append(&body, ",\"explanation\":");
        am_json_append_string(&body, what);
        if (agent != 0){
            am_text_append(&body, ",\"agent\":");
            am_json_append_string(&body, agent);
        }
        am_text_append(&body, "}");
        result = report(call_service("POST", "/v1/markers", &body, &out), &out);
        if (result == 0){
            AM_Json *j = am_json_parse(out.str, out.size);
            AM_Json *ids = am_json_get(j, "ids");
            if (ids && ids->first_child) printf("marker %lld: %s:%s\n", (long long)ids->first_child->number, file, line);
            am_json_free(j);
        }
        am_text_free(&body);
    }
    else if (strcmp(cmd, "list") == 0){
        AM_Text path = {};
        am_text_append(&path, "/v1/markers");
        const char *file = arg_value(argc, argv, "--file");
        char abs_path[PATH_MAX];
        if (file != 0){
            if (strncmp(file, "/ssh:", 5) != 0 && realpath(file, abs_path) != 0) file = abs_path;
            am_text_append(&path, "?file=");
            url_encode(&path, file);
        }
        result = report(call_service("GET", path.str, 0, &out), &out);
        if (result == 0){
            AM_Json *j = am_json_parse(out.str, out.size);
            AM_Json *list = am_json_get(j, "markers");
            int count = 0;
            for (AM_Json *c = list ? list->first_child : 0; c != 0; c = c->next){
                AM_Marker m;
                if (am_marker_from_json(c, &m) == 0){
                    if (m.end_line != m.line) printf("#%lld %s:%lld-%lld", m.id, m.file, m.line, m.end_line);
                    else printf("#%lld %s:%lld", m.id, m.file, m.line);
                    if (m.agent[0]) printf("  [%s]", m.agent);
                    printf("\n  why:  %s\n  what: %s\n", m.justification, m.explanation);
                    am_marker_free(&m);
                    count += 1;
                }
            }
            if (count == 0) printf("no markers\n");
            am_json_free(j);
        }
        am_text_free(&path);
    }
    else if (strcmp(cmd, "rm") == 0 && argc >= 3){
        char path[64];
        snprintf(path, sizeof(path), "/v1/markers/%lld", atoll(argv[2]));
        result = report(call_service("DELETE", path, 0, &out), &out);
        if (result == 0) printf("removed marker %s\n", argv[2]);
    }
    else if (strcmp(cmd, "clear") == 0){
        AM_Text path = {};
        am_text_append(&path, "/v1/markers");
        const char *file = arg_value(argc, argv, "--file");
        const char *agent = arg_value(argc, argv, "--agent");
        char abs_path[PATH_MAX];
        const char *sep = "?";
        if (file != 0){
            if (strncmp(file, "/ssh:", 5) != 0 && realpath(file, abs_path) != 0) file = abs_path;
            am_text_append(&path, "?file=");
            url_encode(&path, file);
            sep = "&";
        }
        if (agent != 0){
            am_text_append(&path, sep);
            am_text_append(&path, "agent=");
            url_encode(&path, agent);
        }
        result = report(call_service("DELETE", path.str, 0, &out), &out);
        if (result == 0){
            AM_Json *j = am_json_parse(out.str, out.size);
            printf("removed %lld markers\n", am_json_get_int(j, "removed", 0));
            am_json_free(j);
        }
        am_text_free(&path);
    }
    else if (strcmp(cmd, "story") == 0){
        result = story_command(argc, argv, &out);
    }
    else if (strcmp(cmd, "pseudo") == 0){
        result = pseudo_command(argc, argv, &out);
    }
    else if (strcmp(cmd, "health") == 0){
        result = report(call_service("GET", "/v1/health", 0, &out), &out);
        if (result == 0) printf("%s", out.str);
    }
    else{
        fputs(usage, stderr);
        result = 1;
    }
    am_text_free(&out);
    return(result);
}
