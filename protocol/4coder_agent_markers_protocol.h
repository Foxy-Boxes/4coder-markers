/*
4coder_agent_markers_protocol.h - Wire format shared by the 4coder-markers service
and the editor's marker client. Plain C++ with no 4coder dependencies, so the
standalone service can include it too.

A marker, as JSON:
  {
    "id": 12,                         // assigned by the service
    "file": "/abs/path/to/file.cpp",  // absolute (or /ssh:host:/path); see matching rules in the README
    "line": 40,                       // 1-based first line
    "end_line": 52,                   // optional, defaults to line
    "justification": "Why this code matters, in one sentence.",
    "explanation": "What this code does, in one sentence.",
    "agent": "claude",                // optional, who posted it
    "created": 1792873518             // unix time, assigned by the service
  }
*/

#if !defined(FCODER_AGENT_MARKERS_PROTOCOL_H)
#define FCODER_AGENT_MARKERS_PROTOCOL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define AGENT_MARKERS_DEFAULT_PORT 7341
#define AGENT_MARKERS_MAX_SENTENCE 300

////////////////////////////////
// NOTE: growable text

struct AM_Text{
    char *str;
    size_t size;
    size_t cap;
};

static void
am_text_append(AM_Text *t, const char *s, size_t n){
    if (t->size + n + 1 > t->cap){
        size_t cap = t->cap*2;
        if (cap < t->size + n + 1) cap = t->size + n + 1;
        if (cap < 256) cap = 256;
        t->str = (char*)realloc(t->str, cap);
        t->cap = cap;
    }
    memcpy(t->str + t->size, s, n);
    t->size += n;
    t->str[t->size] = 0;
}

static void
am_text_append(AM_Text *t, const char *s){
    am_text_append(t, s, strlen(s));
}

static void
am_text_appendf(AM_Text *t, const char *fmt, long long v){
    char buf[64];
    int n = snprintf(buf, sizeof(buf), fmt, v);
    am_text_append(t, buf, (size_t)n);
}

static void
am_text_free(AM_Text *t){
    free(t->str);
    t->str = 0;
    t->size = t->cap = 0;
}

static void
am_json_append_string(AM_Text *t, const char *s){
    am_text_append(t, "\"");
    for (const unsigned char *c = (const unsigned char*)s; *c; c += 1){
        switch (*c){
            case '"':  am_text_append(t, "\\\""); break;
            case '\\': am_text_append(t, "\\\\"); break;
            case '\n': am_text_append(t, "\\n"); break;
            case '\r': am_text_append(t, "\\r"); break;
            case '\t': am_text_append(t, "\\t"); break;
            default:
            {
                if (*c < 0x20){
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", *c);
                    am_text_append(t, buf);
                }
                else{
                    am_text_append(t, (const char*)c, 1);
                }
            }break;
        }
    }
    am_text_append(t, "\"");
}

////////////////////////////////
// NOTE: minimal JSON reader

enum AM_Json_Kind{
    AMJson_Null,
    AMJson_Bool,
    AMJson_Number,
    AMJson_String,
    AMJson_Array,
    AMJson_Object,
};

struct AM_Json{
    AM_Json_Kind kind;
    double number;
    int boolean;
    char *string;       // string value, or the key when this is an object member
    char *key;
    AM_Json *first_child;
    AM_Json *next;
};

struct AM_Json_Parser{
    const char *at;
    const char *end;
    int failed;
};

static void
am_json_skip_ws(AM_Json_Parser *p){
    while (p->at < p->end && (*p->at == ' ' || *p->at == '\t' || *p->at == '\n' || *p->at == '\r')){
        p->at += 1;
    }
}

static void
am_utf8_append(AM_Text *t, unsigned int cp){
    char b[4];
    if (cp < 0x80){ b[0] = (char)cp; am_text_append(t, b, 1); }
    else if (cp < 0x800){ b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); am_text_append(t, b, 2); }
    else if (cp < 0x10000){ b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); am_text_append(t, b, 3); }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); am_text_append(t, b, 4); }
}

static int
am_json_hex4(AM_Json_Parser *p, unsigned int *out){
    if (p->end - p->at < 4) return(0);
    unsigned int v = 0;
    for (int i = 0; i < 4; i += 1){
        char c = p->at[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned int)(c - 'A' + 10);
        else return(0);
    }
    p->at += 4;
    *out = v;
    return(1);
}

static char*
am_json_parse_string_raw(AM_Json_Parser *p){
    if (p->at >= p->end || *p->at != '"'){ p->failed = 1; return(0); }
    p->at += 1;
    AM_Text t = {};
    am_text_append(&t, "", 0);
    while (p->at < p->end && *p->at != '"'){
        char c = *p->at++;
        if (c == '\\'){
            if (p->at >= p->end){ p->failed = 1; break; }
            char e = *p->at++;
            switch (e){
                case '"': am_text_append(&t, "\"", 1); break;
                case '\\': am_text_append(&t, "\\", 1); break;
                case '/': am_text_append(&t, "/", 1); break;
                case 'b': am_text_append(&t, "\b", 1); break;
                case 'f': am_text_append(&t, "\f", 1); break;
                case 'n': am_text_append(&t, "\n", 1); break;
                case 'r': am_text_append(&t, "\r", 1); break;
                case 't': am_text_append(&t, "\t", 1); break;
                case 'u':
                {
                    unsigned int cp = 0;
                    if (!am_json_hex4(p, &cp)){ p->failed = 1; break; }
                    if (cp >= 0xD800 && cp <= 0xDBFF && p->end - p->at >= 6 && p->at[0] == '\\' && p->at[1] == 'u'){
                        p->at += 2;
                        unsigned int lo = 0;
                        if (am_json_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF){
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    am_utf8_append(&t, cp);
                }break;
                default: p->failed = 1; break;
            }
        }
        else{
            am_text_append(&t, &c, 1);
        }
    }
    if (p->at < p->end && *p->at == '"'){
        p->at += 1;
    }
    else{
        p->failed = 1;
    }
    return(t.str);
}

static void am_json_free(AM_Json *j);

static AM_Json*
am_json_parse_value(AM_Json_Parser *p, int depth){
    am_json_skip_ws(p);
    if (p->failed || p->at >= p->end || depth > 32){
        p->failed = 1;
        return(0);
    }
    AM_Json *j = (AM_Json*)calloc(1, sizeof(AM_Json));
    char c = *p->at;
    if (c == '{' || c == '['){
        char close = (c == '{') ? '}' : ']';
        j->kind = (c == '{') ? AMJson_Object : AMJson_Array;
        p->at += 1;
        AM_Json *last = 0;
        am_json_skip_ws(p);
        if (p->at < p->end && *p->at == close){
            p->at += 1;
            return(j);
        }
        for (;;){
            char *key = 0;
            if (j->kind == AMJson_Object){
                am_json_skip_ws(p);
                key = am_json_parse_string_raw(p);
                am_json_skip_ws(p);
                if (p->failed || p->at >= p->end || *p->at != ':'){ free(key); p->failed = 1; break; }
                p->at += 1;
            }
            AM_Json *child = am_json_parse_value(p, depth + 1);
            if (child == 0){ free(key); break; }
            child->key = key;
            if (last) last->next = child; else j->first_child = child;
            last = child;
            am_json_skip_ws(p);
            if (p->at < p->end && *p->at == ','){ p->at += 1; continue; }
            if (p->at < p->end && *p->at == close){ p->at += 1; break; }
            p->failed = 1;
            break;
        }
    }
    else if (c == '"'){
        j->kind = AMJson_String;
        j->string = am_json_parse_string_raw(p);
    }
    else if (c == 't' && p->end - p->at >= 4 && memcmp(p->at, "true", 4) == 0){
        j->kind = AMJson_Bool; j->boolean = 1; p->at += 4;
    }
    else if (c == 'f' && p->end - p->at >= 5 && memcmp(p->at, "false", 5) == 0){
        j->kind = AMJson_Bool; j->boolean = 0; p->at += 5;
    }
    else if (c == 'n' && p->end - p->at >= 4 && memcmp(p->at, "null", 4) == 0){
        j->kind = AMJson_Null; p->at += 4;
    }
    else if (c == '-' || (c >= '0' && c <= '9')){
        char buf[64];
        size_t n = 0;
        while (p->at < p->end && n + 1 < sizeof(buf) &&
               (strchr("+-.eE", *p->at) != 0 || (*p->at >= '0' && *p->at <= '9'))){
            buf[n++] = *p->at++;
        }
        buf[n] = 0;
        j->kind = AMJson_Number;
        j->number = strtod(buf, 0);
    }
    else{
        p->failed = 1;
    }
    if (p->failed){
        am_json_free(j);
        return(0);
    }
    return(j);
}

static AM_Json*
am_json_parse(const char *text, size_t size){
    AM_Json_Parser p = {text, text + size, 0};
    AM_Json *result = am_json_parse_value(&p, 0);
    am_json_skip_ws(&p);
    if (result != 0 && p.at != p.end){
        am_json_free(result);
        result = 0;
    }
    return(result);
}

static void
am_json_free(AM_Json *j){
    while (j != 0){
        AM_Json *next = j->next;
        am_json_free(j->first_child);
        free(j->string);
        free(j->key);
        free(j);
        j = next;
    }
}

static AM_Json*
am_json_get(AM_Json *obj, const char *key){
    if (obj == 0 || obj->kind != AMJson_Object) return(0);
    for (AM_Json *c = obj->first_child; c != 0; c = c->next){
        if (c->key != 0 && strcmp(c->key, key) == 0) return(c);
    }
    return(0);
}

static const char*
am_json_get_string(AM_Json *obj, const char *key){
    AM_Json *v = am_json_get(obj, key);
    return((v != 0 && v->kind == AMJson_String) ? v->string : 0);
}

static long long
am_json_get_int(AM_Json *obj, const char *key, long long fallback){
    AM_Json *v = am_json_get(obj, key);
    return((v != 0 && v->kind == AMJson_Number) ? (long long)v->number : fallback);
}

////////////////////////////////
// NOTE: markers

struct AM_Marker{
    long long id;
    char *file;
    long long line;
    long long end_line;
    char *justification;
    char *explanation;
    char *agent;
    long long created;
};

static char*
am_strdup(const char *s){
    return(s != 0 ? strdup(s) : strdup(""));
}

static void
am_marker_free(AM_Marker *m){
    free(m->file);
    free(m->justification);
    free(m->explanation);
    free(m->agent);
    memset(m, 0, sizeof(*m));
}

// NOTE: returns 0 on success, or a message describing what is wrong
static const char*
am_marker_from_json(AM_Json *obj, AM_Marker *out){
    memset(out, 0, sizeof(*out));
    if (obj == 0 || obj->kind != AMJson_Object) return("marker must be a JSON object");
    const char *file = am_json_get_string(obj, "file");
    const char *why = am_json_get_string(obj, "justification");
    const char *what = am_json_get_string(obj, "explanation");
    long long line = am_json_get_int(obj, "line", 0);
    long long end_line = am_json_get_int(obj, "end_line", line);
    if (file == 0 || file[0] == 0) return("\"file\" (string) is required");
    if (line < 1) return("\"line\" (integer >= 1) is required");
    if (end_line < line) return("\"end_line\" must be >= \"line\"");
    if (why == 0 || why[0] == 0) return("\"justification\" (one sentence) is required");
    if (what == 0 || what[0] == 0) return("\"explanation\" (one sentence) is required");
    const char *sentences[2] = {why, what};
    for (int i = 0; i < 2; i += 1){
        if (strlen(sentences[i]) > AGENT_MARKERS_MAX_SENTENCE) return("justification and explanation must each be one sentence (at most 300 characters)");
        if (strchr(sentences[i], '\n') != 0) return("justification and explanation must each be one sentence (no newlines)");
    }
    out->id = am_json_get_int(obj, "id", 0);
    out->file = am_strdup(file);
    out->line = line;
    out->end_line = end_line;
    out->justification = am_strdup(why);
    out->explanation = am_strdup(what);
    out->agent = am_strdup(am_json_get_string(obj, "agent"));
    out->created = am_json_get_int(obj, "created", 0);
    return(0);
}

static void
am_marker_to_json(AM_Text *t, AM_Marker *m){
    am_text_appendf(t, "{\"id\":%lld,\"file\":", m->id);
    am_json_append_string(t, m->file);
    am_text_appendf(t, ",\"line\":%lld", m->line);
    am_text_appendf(t, ",\"end_line\":%lld,\"justification\":", m->end_line);
    am_json_append_string(t, m->justification);
    am_text_append(t, ",\"explanation\":");
    am_json_append_string(t, m->explanation);
    am_text_append(t, ",\"agent\":");
    am_json_append_string(t, m->agent);
    am_text_appendf(t, ",\"created\":%lld}", m->created);
}

////////////////////////////////
// NOTE: minimal HTTP/1.1 client (one request per connection)

#if !defined(_WIN32)
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int
am_connect(const char *host, int port, int timeout_ms){
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = 0;
    if (getaddrinfo(host, port_text, &hints, &list) != 0){
        return(-1);
    }
    int fd = -1;
    for (struct addrinfo *a = list; a != 0 && fd < 0; a = a->ai_next){
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {timeout_ms/1000, (timeout_ms%1000)*1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0){
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);
    return(fd);
}

static int
am_send_all(int fd, const char *data, size_t size){
    while (size > 0){
#if defined(MSG_NOSIGNAL)
        ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, data, size, 0);
#endif
        if (n <= 0) return(0);
        data += n;
        size -= (size_t)n;
    }
    return(1);
}

// NOTE: returns the HTTP status (0 if the service could not be reached);
// *body_out receives the response body
static int
am_http_request(const char *host, int port, const char *method, const char *path,
                const char *body, size_t body_size, AM_Text *body_out, int timeout_ms){
    int fd = am_connect(host, port, timeout_ms);
    if (fd < 0){
        return(0);
    }
    AM_Text req = {};
    am_text_append(&req, method);
    am_text_append(&req, " ");
    am_text_append(&req, path);
    am_text_append(&req, " HTTP/1.1\r\nHost: ");
    am_text_append(&req, host);
    am_text_append(&req, "\r\nConnection: close\r\nAccept: application/json\r\n");
    if (body != 0){
        am_text_appendf(&req, "Content-Type: application/json\r\nContent-Length: %lld\r\n", (long long)body_size);
    }
    am_text_append(&req, "\r\n");
    if (body != 0){
        am_text_append(&req, body, body_size);
    }

    int status = 0;
    if (am_send_all(fd, req.str, req.size)){
        AM_Text resp = {};
        char chunk[8192];
        for (;;){
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            am_text_append(&resp, chunk, (size_t)n);
        }
        char *header_end = (resp.str != 0) ? strstr(resp.str, "\r\n\r\n") : 0;
        if (header_end != 0 && strncmp(resp.str, "HTTP/1.", 7) == 0){
            status = atoi(resp.str + 9);
            if (body_out != 0){
                const char *b = header_end + 4;
                am_text_append(body_out, b, resp.size - (size_t)(b - resp.str));
            }
        }
        am_text_free(&resp);
    }
    am_text_free(&req);
    close(fd);
    return(status);
}
#endif

#endif

// BOTTOM
