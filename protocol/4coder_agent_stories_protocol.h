/*
4coder_agent_stories_protocol.h - Wire format for stories, an addition to the
4coder-markers protocol (4coder_agent_markers_protocol.h is unchanged).

A story is a graph of steps that gives a narrative over code. Each node is a
"slide": a title, a few sentences of narrative and, usually, a range of lines
in a file. Nodes without a file stand for something outside the code, such as
another program, a service or an interface. Edges say what leads where, so a
story can branch ("on a cache miss" / "on a hit") and loop back, and an edge
can lead into another story.

  {
    "id": "marker-lifecycle",           // [A-Za-z0-9._-], chosen by the agent; posting again replaces it
    "title": "How a marker reaches the screen",
    "summary": "Optional paragraph shown on the first slide.",
    "agent": "claude",
    "start": "cli",                     // optional, defaults to the first node
    "nodes": [
      {
        "id": "cli",
        "title": "The agent posts a marker",
        "file": "/abs/path/4coder_markers.cpp", "line": 520, "end_line": 560,
        "text": "Narrative for this step; a few sentences, newlines allowed.",
        "next": [
          {"to": "store", "label": "service is up"},
          {"to": "intro", "story": "ssh-tunnel", "label": "agent is on another machine"}
        ]
      },
      {"id": "agent", "kind": "external", "title": "Claude on the build server", "text": "..."}
    ]
  }

The service adds "created" and "updated" (unix time).
*/

#if !defined(FCODER_AGENT_STORIES_PROTOCOL_H)
#define FCODER_AGENT_STORIES_PROTOCOL_H

#include "4coder_agent_markers_protocol.h"

#define AM_STORY_MAX_ID      64
#define AM_STORY_MAX_TITLE   200
#define AM_STORY_MAX_LABEL   80
#define AM_STORY_MAX_KIND    32
#define AM_STORY_MAX_TEXT    2000
#define AM_STORY_MAX_SUMMARY 1000
#define AM_STORY_MAX_NODES   256
#define AM_STORY_MAX_EDGES   9

struct AM_Story_Edge{
    char *to;           // node id
    char *story;        // "" for this story, otherwise the id of the story `to` is in
    char *label;        // "" when not given
};

struct AM_Story_Node{
    char *id;
    char *title;
    char *kind;         // "code" when there is a file, otherwise "external" unless the agent names it
    char *file;         // "" for nodes outside the code
    long long line;
    long long end_line;
    char *text;
    AM_Story_Edge *next;
    int next_count;
};

struct AM_Story{
    char *id;
    char *title;
    char *summary;
    char *agent;
    char *start;
    AM_Story_Node *nodes;
    int node_count;
    long long created;
    long long updated;
};

static void
am_story_free(AM_Story *s){
    for (int i = 0; i < s->node_count; i += 1){
        AM_Story_Node *n = &s->nodes[i];
        for (int k = 0; k < n->next_count; k += 1){
            free(n->next[k].to);
            free(n->next[k].story);
            free(n->next[k].label);
        }
        free(n->next);
        free(n->id);
        free(n->title);
        free(n->kind);
        free(n->file);
        free(n->text);
    }
    free(s->nodes);
    free(s->id);
    free(s->title);
    free(s->summary);
    free(s->agent);
    free(s->start);
    memset(s, 0, sizeof(*s));
}

static int
am_story_id_ok(const char *id){
    size_t n = strlen(id);
    if (n == 0 || n > AM_STORY_MAX_ID) return(0);
    for (size_t i = 0; i < n; i += 1){
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')){
            return(0);
        }
    }
    return(1);
}

static const char*
am_story_check_line(const char *s, size_t max, const char *what, char *err, size_t err_size){
    if (strlen(s) > max){
        snprintf(err, err_size, "%s is longer than %zu characters", what, max);
        return(err);
    }
    if (strchr(s, '\n') != 0){
        snprintf(err, err_size, "%s must be a single line", what);
        return(err);
    }
    return(0);
}

static AM_Story_Node*
am_story_find_node(AM_Story *s, const char *id){
    for (int i = 0; i < s->node_count; i += 1){
        if (strcmp(s->nodes[i].id, id) == 0) return(&s->nodes[i]);
    }
    return(0);
}

static AM_Story_Node*
am_story_start_node(AM_Story *s){
    AM_Story_Node *n = am_story_find_node(s, s->start);
    return((n != 0 || s->node_count == 0) ? n : &s->nodes[0]);
}

// NOTE: returns 0 on success, or a message (in err) describing what is wrong
static const char*
am_story_from_json(AM_Json *obj, AM_Story *out, char *err, size_t err_size){
    memset(out, 0, sizeof(*out));
    if (obj == 0 || obj->kind != AMJson_Object){
        snprintf(err, err_size, "story must be a JSON object");
        return(err);
    }
    const char *id = am_json_get_string(obj, "id");
    const char *title = am_json_get_string(obj, "title");
    const char *summary = am_json_get_string(obj, "summary");
    const char *start = am_json_get_string(obj, "start");
    AM_Json *nodes = am_json_get(obj, "nodes");
    if (id == 0 || !am_story_id_ok(id)){
        snprintf(err, err_size, "\"id\" is required: 1-%d characters of [A-Za-z0-9._-]", AM_STORY_MAX_ID);
        return(err);
    }
    if (title == 0 || title[0] == 0){
        snprintf(err, err_size, "\"title\" is required");
        return(err);
    }
    if (am_story_check_line(title, AM_STORY_MAX_TITLE, "\"title\"", err, err_size)) return(err);
    if (summary != 0 && strlen(summary) > AM_STORY_MAX_SUMMARY){
        snprintf(err, err_size, "\"summary\" is longer than %d characters", AM_STORY_MAX_SUMMARY);
        return(err);
    }
    if (nodes == 0 || nodes->kind != AMJson_Array || nodes->first_child == 0){
        snprintf(err, err_size, "\"nodes\" must be a non-empty array");
        return(err);
    }
    int count = 0;
    for (AM_Json *c = nodes->first_child; c != 0; c = c->next) count += 1;
    if (count > AM_STORY_MAX_NODES){
        snprintf(err, err_size, "a story has at most %d nodes", AM_STORY_MAX_NODES);
        return(err);
    }

    out->id = am_strdup(id);
    out->title = am_strdup(title);
    out->summary = am_strdup(summary);
    out->agent = am_strdup(am_json_get_string(obj, "agent"));
    out->start = am_strdup(start);
    out->created = am_json_get_int(obj, "created", 0);
    out->updated = am_json_get_int(obj, "updated", 0);
    out->nodes = (AM_Story_Node*)calloc(count, sizeof(AM_Story_Node));

    const char *error = 0;
    char inner[256];
    int index = 0;
    for (AM_Json *c = nodes->first_child; c != 0 && error == 0; c = c->next, index += 1){
        AM_Story_Node *n = &out->nodes[index];
        out->node_count = index + 1;
        if (c->kind != AMJson_Object){ error = "must be an object"; break; }
        const char *nid = am_json_get_string(c, "id");
        const char *ntitle = am_json_get_string(c, "title");
        const char *kind = am_json_get_string(c, "kind");
        const char *file = am_json_get_string(c, "file");
        const char *text = am_json_get_string(c, "text");
        long long line = am_json_get_int(c, "line", 0);
        long long end_line = am_json_get_int(c, "end_line", line);
        AM_Json *next = am_json_get(c, "next");
        if (nid == 0 || !am_story_id_ok(nid)){ error = "\"id\" is required: 1-64 characters of [A-Za-z0-9._-]"; break; }
        n->id = am_strdup(nid);
        if (am_story_find_node(out, nid) != n){ error = "duplicate node id"; break; }
        if (ntitle == 0 || ntitle[0] == 0){ error = "\"title\" is required"; break; }
        if ((error = am_story_check_line(ntitle, AM_STORY_MAX_TITLE, "\"title\"", inner, sizeof(inner))) != 0) break;
        if (text != 0 && strlen(text) > AM_STORY_MAX_TEXT){ error = "\"text\" is longer than 2000 characters"; break; }
        if (kind != 0 && (error = am_story_check_line(kind, AM_STORY_MAX_KIND, "\"kind\"", inner, sizeof(inner))) != 0) break;
        if (file != 0 && file[0] != 0){
            if (line < 1){ error = "\"line\" (integer >= 1) is required with \"file\""; break; }
            if (end_line < line){ error = "\"end_line\" must be >= \"line\""; break; }
        }
        else if (kind != 0 && strcmp(kind, "code") == 0){
            error = "a \"code\" node needs \"file\" and \"line\"";
            break;
        }
        int has_file = (file != 0 && file[0] != 0);
        n->title = am_strdup(ntitle);
        n->kind = am_strdup((kind != 0 && kind[0] != 0) ? kind : has_file ? "code" : "external");
        n->file = am_strdup(file);
        n->line = has_file ? line : 0;
        n->end_line = has_file ? end_line : 0;
        n->text = am_strdup(text);

        if (next != 0 && next->kind != AMJson_Array){ error = "\"next\" must be an array of edges"; break; }
        int edge_count = 0;
        for (AM_Json *e = next ? next->first_child : 0; e != 0; e = e->next) edge_count += 1;
        if (edge_count > AM_STORY_MAX_EDGES){ error = "a node has at most 9 edges in \"next\""; break; }
        n->next = (AM_Story_Edge*)calloc(edge_count + 1, sizeof(AM_Story_Edge));
        for (AM_Json *e = next ? next->first_child : 0; e != 0 && error == 0; e = e->next){
            // NOTE: an edge may be just the target node id
            const char *to = (e->kind == AMJson_String) ? e->string : am_json_get_string(e, "to");
            const char *story = (e->kind == AMJson_Object) ? am_json_get_string(e, "story") : 0;
            const char *label = (e->kind == AMJson_Object) ? am_json_get_string(e, "label") : 0;
            if (to == 0 || !am_story_id_ok(to)){ error = "every edge needs \"to\": a node id"; break; }
            if (story != 0 && story[0] != 0 && !am_story_id_ok(story)){ error = "edge \"story\" must be a story id"; break; }
            if (label != 0 && (error = am_story_check_line(label, AM_STORY_MAX_LABEL, "edge \"label\"", inner, sizeof(inner))) != 0) break;
            AM_Story_Edge *edge = &n->next[n->next_count++];
            edge->to = am_strdup(to);
            edge->story = am_strdup((story != 0 && strcmp(story, id) != 0) ? story : "");
            edge->label = am_strdup(label);
        }
        if (error != 0) break;
    }

    // NOTE: edges inside the story must land on a node; edges into other stories are
    // checked by the editor, since that story may be posted later
    for (int i = 0; i < out->node_count && error == 0; i += 1){
        AM_Story_Node *n = &out->nodes[i];
        for (int k = 0; k < n->next_count; k += 1){
            if (n->next[k].story[0] == 0 && am_story_find_node(out, n->next[k].to) == 0){
                snprintf(inner, sizeof(inner), "edge to unknown node \"%s\"", n->next[k].to);
                error = inner;
                index = i;
                break;
            }
        }
    }
    if (error == 0 && out->start[0] != 0 && am_story_find_node(out, out->start) == 0){
        snprintf(err, err_size, "\"start\" names unknown node \"%s\"", out->start);
        am_story_free(out);
        return(err);
    }
    if (error != 0){
        const char *nid = (index < out->node_count && out->nodes[index].id) ? out->nodes[index].id : "";
        if (nid[0] != 0) snprintf(err, err_size, "node %d (\"%s\"): %s", index, nid, error);
        else snprintf(err, err_size, "node %d: %s", index, error);
        am_story_free(out);
        return(err);
    }
    return(0);
}

static void
am_story_to_json(AM_Text *t, AM_Story *s){
    am_text_append(t, "{\"id\":");
    am_json_append_string(t, s->id);
    am_text_append(t, ",\"title\":");
    am_json_append_string(t, s->title);
    if (s->summary[0] != 0){
        am_text_append(t, ",\"summary\":");
        am_json_append_string(t, s->summary);
    }
    am_text_append(t, ",\"agent\":");
    am_json_append_string(t, s->agent);
    if (s->start[0] != 0){
        am_text_append(t, ",\"start\":");
        am_json_append_string(t, s->start);
    }
    am_text_appendf(t, ",\"created\":%lld", s->created);
    am_text_appendf(t, ",\"updated\":%lld,\"nodes\":[", s->updated);
    for (int i = 0; i < s->node_count; i += 1){
        AM_Story_Node *n = &s->nodes[i];
        if (i > 0) am_text_append(t, ",");
        am_text_append(t, "{\"id\":");
        am_json_append_string(t, n->id);
        am_text_append(t, ",\"title\":");
        am_json_append_string(t, n->title);
        am_text_append(t, ",\"kind\":");
        am_json_append_string(t, n->kind);
        if (n->file[0] != 0){
            am_text_append(t, ",\"file\":");
            am_json_append_string(t, n->file);
            am_text_appendf(t, ",\"line\":%lld", n->line);
            am_text_appendf(t, ",\"end_line\":%lld", n->end_line);
        }
        am_text_append(t, ",\"text\":");
        am_json_append_string(t, n->text);
        am_text_append(t, ",\"next\":[");
        for (int k = 0; k < n->next_count; k += 1){
            AM_Story_Edge *e = &n->next[k];
            if (k > 0) am_text_append(t, ",");
            am_text_append(t, "{\"to\":");
            am_json_append_string(t, e->to);
            if (e->story[0] != 0){
                am_text_append(t, ",\"story\":");
                am_json_append_string(t, e->story);
            }
            if (e->label[0] != 0){
                am_text_append(t, ",\"label\":");
                am_json_append_string(t, e->label);
            }
            am_text_append(t, "}");
        }
        am_text_append(t, "]}");
    }
    am_text_append(t, "]}");
}

// NOTE: a POST body may be one story, an array of stories or {"stories": [...]};
// returns the array to iterate, using *single as a stand-in list for one story
static AM_Json*
am_stories_items(AM_Json *root, AM_Json *single){
    memset(single, 0, sizeof(*single));
    if (root == 0) return(0);
    if (root->kind == AMJson_Array) return(root);
    if (root->kind == AMJson_Object){
        AM_Json *list = am_json_get(root, "stories");
        if (list != 0 && list->kind == AMJson_Array) return(list);
        single->kind = AMJson_Array;
        single->first_child = root;
        return(single);
    }
    return(0);
}

#endif
