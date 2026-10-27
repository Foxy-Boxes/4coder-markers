/*
4coder_agent_pseudo_protocol.h - Wire format for pseudocode requests, an
addition to the 4coder-markers protocol (markers and stories are unchanged).

The editor records the files the user opens while writing pseudocode and sends
them as a request. An agent claims the request, implements the pseudocode as
real code and answers with links, each pairing a range of pseudocode with the
range of code that implements it. The service turns the links into a story
("pseudo-ID") that walks pseudocode and implementation side by side, and links
the steps of other stories that cover the same code to it.

A request, as the editor posts it:

  {
    "client": "4coder",
    "instructions": "Optional note from the user, e.g. which language or file to write into.",
    "files": [
      {"file": "/abs/path/design.pseudo", "host": "user@server"},   // host: "" when local
      {"file": "/abs/path/server.c", "host": "user@server"}
    ]
  }

The service adds "id" (integer), "status" (pending, claimed, done, failed or
canceled), "agent", "created" and "updated". An answer, as the agent posts it:

  {
    "summary": "What was implemented and where; a few sentences.",
    "changed": ["/abs/path/server.c"],                 // files written or created
    "links": [
      {"title": "Parse the request line",               // one line, <= 200 characters
       "text": "How the code realises this pseudocode; 1-3 sentences.",
       "pseudo": {"file": "/abs/path/design.pseudo", "line": 3, "end_line": 6},
       "code": {"file": "/abs/path/server.c", "line": 40, "end_line": 72}}
    ]
  }

A link needs "pseudo", "code" or both. A failed request is answered with
{"error": "why"} instead (the summary and links are then optional).
*/

#if !defined(FCODER_AGENT_PSEUDO_PROTOCOL_H)
#define FCODER_AGENT_PSEUDO_PROTOCOL_H

#include "4coder_agent_stories_protocol.h"

#define AM_PSEUDO_MAX_FILES        64
#define AM_PSEUDO_MAX_INSTRUCTIONS 4000
#define AM_PSEUDO_MAX_SUMMARY      4000
#define AM_PSEUDO_MAX_LINKS        120
#define AM_PSEUDO_MAX_CHANGED      128
#define AM_PSEUDO_MAX_PATH         4096

struct AM_Pseudo_File{
    char *file;
    char *host;         // "" for a file on the agent's machine
};

struct AM_Pseudo_Range{
    char *file;         // "" when the link has no range on this side
    long long line;
    long long end_line;
};

struct AM_Pseudo_Link{
    char *title;
    char *text;
    AM_Pseudo_Range pseudo;
    AM_Pseudo_Range code;
};

struct AM_Pseudo_Job{
    long long id;
    char *status;
    char *client;
    char *agent;
    char *instructions;
    AM_Pseudo_File *files;
    int file_count;
    long long created;
    long long updated;

    // NOTE: the answer; empty until the job is done or failed
    char *summary;
    char *error;
    char *story;        // id of the story built from the links
    char **changed;
    int changed_count;
    AM_Pseudo_Link *links;
    int link_count;
};

static void
am_pseudo_range_free(AM_Pseudo_Range *r){
    free(r->file);
    memset(r, 0, sizeof(*r));
}

static void
am_pseudo_answer_free(AM_Pseudo_Job *j){
    for (int i = 0; i < j->changed_count; i += 1) free(j->changed[i]);
    for (int i = 0; i < j->link_count; i += 1){
        AM_Pseudo_Link *l = &j->links[i];
        free(l->title);
        free(l->text);
        am_pseudo_range_free(&l->pseudo);
        am_pseudo_range_free(&l->code);
    }
    free(j->changed);
    free(j->links);
    free(j->summary);
    free(j->error);
    free(j->story);
    j->changed = 0;
    j->changed_count = 0;
    j->links = 0;
    j->link_count = 0;
    j->summary = am_strdup(0);
    j->error = am_strdup(0);
    j->story = am_strdup(0);
}

static void
am_pseudo_job_free(AM_Pseudo_Job *j){
    am_pseudo_answer_free(j);
    for (int i = 0; i < j->file_count; i += 1){
        free(j->files[i].file);
        free(j->files[i].host);
    }
    free(j->files);
    free(j->status);
    free(j->client);
    free(j->agent);
    free(j->instructions);
    free(j->summary);
    free(j->error);
    free(j->story);
    memset(j, 0, sizeof(*j));
}

static int
am_pseudo_status_ok(const char *s){
    return(s != 0 && (strcmp(s, "pending") == 0 || strcmp(s, "claimed") == 0 || strcmp(s, "done") == 0 ||
                      strcmp(s, "failed") == 0 || strcmp(s, "canceled") == 0));
}

static int
am_pseudo_finished(AM_Pseudo_Job *j){
    return(strcmp(j->status, "done") == 0 || strcmp(j->status, "failed") == 0 || strcmp(j->status, "canceled") == 0);
}

static const char*
am_pseudo_range_from_json(AM_Json *obj, AM_Pseudo_Range *out, const char *what, char *err, size_t err_size){
    memset(out, 0, sizeof(*out));
    if (obj == 0 || obj->kind == AMJson_Null){
        out->file = am_strdup(0);
        return(0);
    }
    const char *file = (obj->kind == AMJson_Object) ? am_json_get_string(obj, "file") : 0;
    long long line = am_json_get_int(obj, "line", 0);
    long long end_line = am_json_get_int(obj, "end_line", line);
    if (file == 0 || file[0] == 0 || strlen(file) > AM_PSEUDO_MAX_PATH){
        snprintf(err, err_size, "\"%s\" needs \"file\", \"line\" and optionally \"end_line\"", what);
        return(err);
    }
    if (line < 1 || end_line < line){
        snprintf(err, err_size, "\"%s\": \"line\" must be >= 1 and \"end_line\" >= \"line\"", what);
        return(err);
    }
    out->file = am_strdup(file);
    out->line = line;
    out->end_line = end_line;
    return(0);
}

// NOTE: reads the request part of a job (files, instructions); returns 0 or a message in err
static const char*
am_pseudo_request_from_json(AM_Json *obj, AM_Pseudo_Job *out, char *err, size_t err_size){
    memset(out, 0, sizeof(*out));
    if (obj == 0 || obj->kind != AMJson_Object){
        snprintf(err, err_size, "a request must be a JSON object");
        return(err);
    }
    const char *instructions = am_json_get_string(obj, "instructions");
    AM_Json *files = am_json_get(obj, "files");
    if (instructions != 0 && strlen(instructions) > AM_PSEUDO_MAX_INSTRUCTIONS){
        snprintf(err, err_size, "\"instructions\" is longer than %d characters", AM_PSEUDO_MAX_INSTRUCTIONS);
        return(err);
    }
    if (files == 0 || files->kind != AMJson_Array || files->first_child == 0){
        snprintf(err, err_size, "\"files\" must be a non-empty array");
        return(err);
    }
    int count = 0;
    for (AM_Json *c = files->first_child; c != 0; c = c->next) count += 1;
    if (count > AM_PSEUDO_MAX_FILES){
        snprintf(err, err_size, "a request has at most %d files", AM_PSEUDO_MAX_FILES);
        return(err);
    }
    out->files = (AM_Pseudo_File*)calloc(count, sizeof(AM_Pseudo_File));
    for (AM_Json *c = files->first_child; c != 0; c = c->next){
        // NOTE: a file may be just its path
        const char *file = (c->kind == AMJson_String) ? c->string : am_json_get_string(c, "file");
        const char *host = (c->kind == AMJson_Object) ? am_json_get_string(c, "host") : 0;
        if (file == 0 || file[0] == 0 || strlen(file) > AM_PSEUDO_MAX_PATH ||
            (host != 0 && am_story_check_line(host, 256, "\"host\"", err, err_size) != 0)){
            snprintf(err, err_size, "file %d: needs \"file\", a path, and optionally \"host\"", out->file_count);
            am_pseudo_job_free(out);
            return(err);
        }
        AM_Pseudo_File *f = &out->files[out->file_count++];
        f->file = am_strdup(file);
        f->host = am_strdup(host);
    }
    out->id = am_json_get_int(obj, "id", 0);
    const char *status = am_json_get_string(obj, "status");
    out->status = am_strdup(am_pseudo_status_ok(status) ? status : "pending");
    out->client = am_strdup(am_json_get_string(obj, "client"));
    out->agent = am_strdup(am_json_get_string(obj, "agent"));
    out->instructions = am_strdup(instructions);
    out->created = am_json_get_int(obj, "created", 0);
    out->updated = am_json_get_int(obj, "updated", 0);
    out->summary = am_strdup(0);
    out->error = am_strdup(0);
    out->story = am_strdup(0);
    return(0);
}

// NOTE: reads an answer into j (replacing any earlier one); returns 0 or a message in err
static const char*
am_pseudo_answer_from_json(AM_Json *obj, AM_Pseudo_Job *j, char *err, size_t err_size){
    if (obj == 0 || obj->kind != AMJson_Object){
        snprintf(err, err_size, "an answer must be a JSON object");
        return(err);
    }
    const char *summary = am_json_get_string(obj, "summary");
    const char *error = am_json_get_string(obj, "error");
    const char *story = am_json_get_string(obj, "story");
    AM_Json *changed = am_json_get(obj, "changed");
    AM_Json *links = am_json_get(obj, "links");
    if (summary != 0 && strlen(summary) > AM_PSEUDO_MAX_SUMMARY){
        snprintf(err, err_size, "\"summary\" is longer than %d characters", AM_PSEUDO_MAX_SUMMARY);
        return(err);
    }
    if (error != 0 && strlen(error) > AM_PSEUDO_MAX_SUMMARY){
        snprintf(err, err_size, "\"error\" is longer than %d characters", AM_PSEUDO_MAX_SUMMARY);
        return(err);
    }
    int failed = (error != 0 && error[0] != 0);
    if (!failed && (summary == 0 || summary[0] == 0)){
        snprintf(err, err_size, "\"summary\" is required (or \"error\" when the request could not be done)");
        return(err);
    }
    if (changed != 0 && changed->kind != AMJson_Array){
        snprintf(err, err_size, "\"changed\" must be an array of paths");
        return(err);
    }
    if (links != 0 && links->kind != AMJson_Array){
        snprintf(err, err_size, "\"links\" must be an array");
        return(err);
    }
    int changed_count = 0, link_count = 0;
    for (AM_Json *c = changed ? changed->first_child : 0; c != 0; c = c->next) changed_count += 1;
    for (AM_Json *c = links ? links->first_child : 0; c != 0; c = c->next) link_count += 1;
    if (changed_count > AM_PSEUDO_MAX_CHANGED){
        snprintf(err, err_size, "\"changed\" has at most %d paths", AM_PSEUDO_MAX_CHANGED);
        return(err);
    }
    if (link_count > AM_PSEUDO_MAX_LINKS){
        snprintf(err, err_size, "an answer has at most %d links", AM_PSEUDO_MAX_LINKS);
        return(err);
    }
    if (!failed && link_count == 0){
        snprintf(err, err_size, "\"links\" is required: pair each piece of pseudocode with the code that implements it");
        return(err);
    }

    // NOTE: build into a copy so a bad answer leaves the job as it was
    AM_Pseudo_Job a = {};
    a.summary = am_strdup(summary);
    a.error = am_strdup(error);
    a.story = am_strdup(story);
    a.changed = (char**)calloc(changed_count + 1, sizeof(char*));
    a.links = (AM_Pseudo_Link*)calloc(link_count + 1, sizeof(AM_Pseudo_Link));
    const char *bad = 0;
    char inner[256];
    for (AM_Json *c = changed ? changed->first_child : 0; c != 0 && bad == 0; c = c->next){
        if (c->kind != AMJson_String || c->string[0] == 0 || strlen(c->string) > AM_PSEUDO_MAX_PATH){
            snprintf(err, err_size, "\"changed\" %d must be a path", a.changed_count);
            bad = err;
            break;
        }
        a.changed[a.changed_count++] = am_strdup(c->string);
    }
    for (AM_Json *c = links ? links->first_child : 0; c != 0 && bad == 0; c = c->next){
        int index = a.link_count;
        AM_Pseudo_Link *l = &a.links[a.link_count++];
        const char *title = (c->kind == AMJson_Object) ? am_json_get_string(c, "title") : 0;
        const char *text = (c->kind == AMJson_Object) ? am_json_get_string(c, "text") : 0;
        const char *problem = 0;
        if (title == 0 || title[0] == 0) problem = "\"title\" is required";
        else problem = am_story_check_line(title, AM_STORY_MAX_TITLE, "\"title\"", inner, sizeof(inner));
        if (problem == 0 && text != 0 && strlen(text) > AM_STORY_MAX_TEXT) problem = "\"text\" is longer than 2000 characters";
        l->title = am_strdup(title);
        l->text = am_strdup(text);
        if (problem == 0) problem = am_pseudo_range_from_json(am_json_get(c, "pseudo"), &l->pseudo, "pseudo", inner, sizeof(inner));
        if (problem == 0) problem = am_pseudo_range_from_json(am_json_get(c, "code"), &l->code, "code", inner, sizeof(inner));
        if (problem == 0 && l->pseudo.file[0] == 0 && l->code.file[0] == 0) problem = "needs \"pseudo\", \"code\" or both";
        if (problem != 0){
            if (l->pseudo.file == 0) l->pseudo.file = am_strdup(0);
            if (l->code.file == 0) l->code.file = am_strdup(0);
            snprintf(err, err_size, "link %d%s%s%s: %s", index, title ? " (\"" : "", title ? title : "", title ? "\")" : "", problem);
            bad = err;
        }
    }
    if (bad != 0){
        am_pseudo_answer_free(&a);
        free(a.summary); free(a.error); free(a.story);
        return(bad);
    }
    am_pseudo_answer_free(j);
    free(j->summary); free(j->error); free(j->story);
    j->summary = a.summary;
    j->error = a.error;
    j->story = a.story;
    j->changed = a.changed;
    j->changed_count = a.changed_count;
    j->links = a.links;
    j->link_count = a.link_count;
    return(0);
}

static void
am_pseudo_range_to_json(AM_Text *t, const char *key, AM_Pseudo_Range *r){
    if (r->file[0] == 0) return;
    am_text_append(t, ",\"");
    am_text_append(t, key);
    am_text_append(t, "\":{\"file\":");
    am_json_append_string(t, r->file);
    am_text_appendf(t, ",\"line\":%lld", r->line);
    am_text_appendf(t, ",\"end_line\":%lld}", r->end_line);
}

static void
am_pseudo_answer_to_json(AM_Text *t, AM_Pseudo_Job *j){
    am_text_append(t, "{\"summary\":");
    am_json_append_string(t, j->summary);
    if (j->error[0] != 0){
        am_text_append(t, ",\"error\":");
        am_json_append_string(t, j->error);
    }
    if (j->story[0] != 0){
        am_text_append(t, ",\"story\":");
        am_json_append_string(t, j->story);
    }
    am_text_append(t, ",\"changed\":[");
    for (int i = 0; i < j->changed_count; i += 1){
        if (i > 0) am_text_append(t, ",");
        am_json_append_string(t, j->changed[i]);
    }
    am_text_append(t, "],\"links\":[");
    for (int i = 0; i < j->link_count; i += 1){
        AM_Pseudo_Link *l = &j->links[i];
        if (i > 0) am_text_append(t, ",");
        am_text_append(t, "{\"title\":");
        am_json_append_string(t, l->title);
        am_text_append(t, ",\"text\":");
        am_json_append_string(t, l->text);
        am_pseudo_range_to_json(t, "pseudo", &l->pseudo);
        am_pseudo_range_to_json(t, "code", &l->code);
        am_text_append(t, "}");
    }
    am_text_append(t, "]}");
}

static void
am_pseudo_job_to_json(AM_Text *t, AM_Pseudo_Job *j){
    am_text_appendf(t, "{\"id\":%lld,\"status\":", j->id);
    am_json_append_string(t, j->status);
    am_text_append(t, ",\"client\":");
    am_json_append_string(t, j->client);
    am_text_append(t, ",\"agent\":");
    am_json_append_string(t, j->agent);
    am_text_append(t, ",\"instructions\":");
    am_json_append_string(t, j->instructions);
    am_text_appendf(t, ",\"created\":%lld", j->created);
    am_text_appendf(t, ",\"updated\":%lld,\"files\":[", j->updated);
    for (int i = 0; i < j->file_count; i += 1){
        if (i > 0) am_text_append(t, ",");
        am_text_append(t, "{\"file\":");
        am_json_append_string(t, j->files[i].file);
        am_text_append(t, ",\"host\":");
        am_json_append_string(t, j->files[i].host);
        am_text_append(t, "}");
    }
    am_text_append(t, "]");
    if (j->summary[0] != 0 || j->error[0] != 0 || j->link_count > 0){
        am_text_append(t, ",\"answer\":");
        am_pseudo_answer_to_json(t, j);
    }
    am_text_append(t, "}");
}

// NOTE: a job as the service sends it: the request plus an optional "answer" object
static const char*
am_pseudo_job_from_json(AM_Json *obj, AM_Pseudo_Job *out, char *err, size_t err_size){
    if (am_pseudo_request_from_json(obj, out, err, err_size) != 0) return(err);
    AM_Json *answer = am_json_get(obj, "answer");
    if (answer != 0 && am_pseudo_answer_from_json(answer, out, err, err_size) != 0){
        am_pseudo_job_free(out);
        return(err);
    }
    return(0);
}

#endif
