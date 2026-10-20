#include "sutekh/audio_graph.h"
#include "../ir/su_json.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NODE_SOUND, NODE_RANDOM, NODE_SEQUENCE, NODE_PARALLEL, NODE_SWITCH, NODE_BLEND };
typedef struct GraphEdge {
    uint32_t node, weight, value;
    uint64_t offset;
    float point;
} GraphEdge;
typedef struct GraphNode {
    uint32_t id, type, first, count, control;
    uint32_t color, depth, expansion;
    uint64_t delay;
    SuAudioGraphLeaf leaf;
    int pinned;
} GraphNode;
typedef struct GraphEvent { uint32_t id, root; } GraphEvent;
struct SuAudioGraph {
    uint32_t refs, sample_rate, node_count, edge_count, event_count;
    SuAudioGraphLimits limits;
    SuAudioAssets *assets;
    GraphNode *nodes;
    GraphEdge *edges;
    GraphEvent *events;
};
typedef struct Prepare {
    const unsigned char *json;
    size_t len;
    SuAudioGraph *graph;
    SuAudioGraphDiagnostic *diagnostic;
    uint32_t node_id;
} Prepare;

static uint32_t fail(SuAudioGraphDiagnostic *diagnostic, uint32_t code, uint32_t node_id,
                     size_t offset, const char *bound, const char *message) {
    if (diagnostic) {
        memset(diagnostic, 0, sizeof *diagnostic);
        diagnostic->code = code;
        diagnostic->node_id = node_id;
        diagnostic->offset = offset;
        snprintf(diagnostic->bound, sizeof diagnostic->bound, "%s", bound);
        snprintf(diagnostic->message, sizeof diagnostic->message, "%s", message);
    }
    return code;
}
static uint32_t bad(Prepare *prep, const SuJsonValue *value, const char *message) {
    return fail(prep->diagnostic, SU_E_SCHEMA_MISMATCH, prep->node_id,
                value ? value->offset : 0, "schema", message);
}
static int fields(const SuJsonValue *object, const char *const *allowed, size_t count) {
    size_t member, field;
    if (!object || object->type != SU_JSON_OBJECT) return 0;
    for (member = 0; member < object->u.object.count; member++) {
        for (field = 0; field < count; field++)
            if (!strcmp(object->u.object.members[member].key, allowed[field])) break;
        if (field == count) return 0;
    }
    return 1;
}
#define FIELDS(object, ...) fields(object, (const char *const[]){__VA_ARGS__}, \
    sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *))
static int text(const SuJsonValue *value, const char *expected) {
    return value && value->type == SU_JSON_STRING && !strcmp(value->u.string.data, expected);
}
static int integer(const SuJsonValue *value, uint32_t *out, int nonzero) {
    uint64_t result;
    if (!su_json_get_uint(value, &result) || result > UINT32_MAX || (nonzero && !result)) return 0;
    *out = (uint32_t)result;
    return 1;
}
static int number(Prepare *prep, const SuJsonValue *value, float *out) {
    size_t cursor, start;
    double magnitude = 0.0, result;
    int negative = 0, fraction = 0, exponent = 0, exponent_negative = 0;
    unsigned digits = 0;
    if (!value || value->type != SU_JSON_NUMBER) return 0;
    cursor = start = value->offset;
    if (prep->json[cursor] == '-') { negative = 1; cursor++; }
    while (cursor < prep->len) {
        unsigned char token = prep->json[cursor];
        if (token == '.') { fraction = 1; cursor++; continue; }
        if (token < '0' || token > '9') break;
        if (++digits > 64) return 0;
        magnitude = magnitude * 10.0 + (token - '0');
        if (fraction) exponent--;
        cursor++;
    }
    if (cursor < prep->len && (prep->json[cursor] == 'e' || prep->json[cursor] == 'E')) {
        int explicit_exponent = 0;
        cursor++;
        if (cursor < prep->len && (prep->json[cursor] == '+' || prep->json[cursor] == '-')) {
            exponent_negative = prep->json[cursor] == '-'; cursor++;
        }
        while (cursor < prep->len && prep->json[cursor] >= '0' && prep->json[cursor] <= '9') {
            explicit_exponent = explicit_exponent * 10 + prep->json[cursor++] - '0';
            if (explicit_exponent > 10000) return 0;
        }
        exponent += exponent_negative ? -explicit_exponent : explicit_exponent;
    }
    if (cursor - start > 96) return 0;
    result = magnitude == 0.0 ? 0.0 : magnitude * pow(10.0, exponent);
    if (negative) result = -result;
    if (!isfinite(result) || fabs(result) > FLT_MAX) return 0;
    *out = (float)result;
    return isfinite(*out);
}
static int compare_node(const void *left, const void *right) {
    uint32_t first = ((const GraphNode *)left)->id, second = ((const GraphNode *)right)->id;
    return (first > second) - (first < second);
}
static int compare_event(const void *left, const void *right) {
    uint32_t first = ((const GraphEvent *)left)->id, second = ((const GraphEvent *)right)->id;
    return (first > second) - (first < second);
}
static uint32_t find_node(const SuAudioGraph *graph, uint32_t id) {
    uint32_t low = 0, high = graph->node_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (graph->nodes[middle].id < id) low = middle + 1;
        else high = middle;
    }
    return low < graph->node_count && graph->nodes[low].id == id ? low : UINT32_MAX;
}
static int compare_u32(const void *left, const void *right) {
    uint32_t first = *(const uint32_t *)left, second = *(const uint32_t *)right;
    return (first > second) - (first < second);
}

static uint32_t parse_node(Prepare *prep, const SuJsonValue *value, GraphNode *node) {
    const SuJsonValue *kind, *children = NULL, *option;
    uint32_t index, fallback = 0;
    if (!value || value->type != SU_JSON_OBJECT ||
        !integer(su_json_member(value, "id"), &node->id, 1)) return bad(prep, value, "node requires a nonzero u32 id");
    prep->node_id = node->id;
    kind = su_json_member(value, "type");
    node->first = prep->graph->edge_count;
    if (text(kind, "sound")) {
        const SuJsonValue *asset = su_json_member(value, "asset");
        if (!FIELDS(value, "id", "type", "asset", "gain", "pitch", "bus_id", "flags") ||
            !FIELDS(asset, "id", "generation") ||
            !integer(su_json_member(asset, "id"), &node->leaf.asset.id, 1) ||
            !integer(su_json_member(asset, "generation"), &node->leaf.asset.generation, 1))
            return bad(prep, value, "invalid sound fields or asset reference");
        node->type = NODE_SOUND;
        node->leaf.node_id = node->id;
        node->leaf.gain = node->leaf.pitch = 1.0f;
        option = su_json_member(value, "gain");
        if (option && (!number(prep, option, &node->leaf.gain) || node->leaf.gain < 0.0f))
            return bad(prep, option, "gain must be finite and nonnegative");
        option = su_json_member(value, "pitch");
        if (option && (!number(prep, option, &node->leaf.pitch) || node->leaf.pitch <= 0.0f || node->leaf.pitch > 64.0f))
            return bad(prep, option, "pitch must be finite in (0,64]");
        option = su_json_member(value, "bus_id");
        if (option && !integer(option, &node->leaf.bus_id, 0)) return bad(prep, option, "invalid bus_id");
        option = su_json_member(value, "flags");
        if (option && (!integer(option, &node->leaf.flags, 0) ||
            (node->leaf.flags & ~(SU_AUDIO_LOOP | SU_AUDIO_SPATIAL)))) return bad(prep, option, "unsupported sound flags");
        return SU_OK;
    }
    if (text(kind, "random") || text(kind, "sequence") || text(kind, "parallel")) {
        if (!FIELDS(value, "id", "type", "children")) return bad(prep, value, "unknown node property");
        node->type = text(kind, "random") ? NODE_RANDOM : text(kind, "sequence") ? NODE_SEQUENCE : NODE_PARALLEL;
        children = su_json_member(value, "children");
    } else if (text(kind, "switch")) {
        node->type = NODE_SWITCH;
        if (!FIELDS(value, "id", "type", "switch_id", "default", "cases") ||
            !integer(su_json_member(value, "switch_id"), &node->control, 1) ||
            !integer(su_json_member(value, "default"), &fallback, 1))
            return bad(prep, value, "switch requires numeric switch_id and explicit default node");
        children = su_json_member(value, "cases");
    } else if (text(kind, "blend")) {
        node->type = NODE_BLEND;
        if (!FIELDS(value, "id", "type", "parameter_id", "points") ||
            !integer(su_json_member(value, "parameter_id"), &node->control, 1))
            return bad(prep, value, "blend requires numeric parameter_id");
        children = su_json_member(value, "points");
    } else return bad(prep, kind, "unknown node type");
    if (!children || children->type != SU_JSON_ARRAY ||
        (children->u.array.count == 0 && node->type != NODE_SWITCH)) return bad(prep, children, "expected nonempty children array");
    if (children->u.array.count > prep->graph->limits.max_edges - prep->graph->edge_count ||
        (node->type == NODE_SWITCH && children->u.array.count == prep->graph->limits.max_edges - prep->graph->edge_count))
        return fail(prep->diagnostic, SU_E_RESOURCE_LIMIT, node->id, children->offset, "max_edges", "edge capacity exceeded");
    node->count = (uint32_t)children->u.array.count;
    for (index = 0; index < node->count; index++) {
        const SuJsonValue *child = children->u.array.items[index];
        GraphEdge *edge = &prep->graph->edges[prep->graph->edge_count++];
        if (node->type == NODE_PARALLEL) {
            if (!integer(child, &edge->node, 1)) return bad(prep, child, "parallel children must be node IDs");
        } else {
            if (!integer(su_json_member(child, "node"), &edge->node, 1)) return bad(prep, child, "missing child node ID");
            if (node->type == NODE_RANDOM) {
                if (!FIELDS(child, "node", "weight") || !integer(su_json_member(child, "weight"), &edge->weight, 1))
                    return bad(prep, child, "random weights must be positive u32 integers");
            } else if (node->type == NODE_SEQUENCE) {
                if (!FIELDS(child, "node", "offset_frames") || !su_json_get_uint(su_json_member(child, "offset_frames"), &edge->offset))
                    return bad(prep, child, "sequence requires exact u64 offset_frames");
            } else if (node->type == NODE_SWITCH) {
                if (!FIELDS(child, "node", "value") || !integer(su_json_member(child, "value"), &edge->value, 0))
                    return bad(prep, child, "switch case value must be u32");
            } else {
                if (!FIELDS(child, "node", "value") || !number(prep, su_json_member(child, "value"), &edge->point) ||
                    (index && edge->point <= prep->graph->edges[node->first + index - 1].point))
                    return bad(prep, child, "blend points must be finite and strictly increasing");
            }
        }
    }
    if ((node->type == NODE_RANDOM || node->type == NODE_SWITCH) && node->count > 1) {
        uint32_t *keys = malloc((size_t)node->count * sizeof *keys);
        int duplicate = 0;
        if (!keys) return fail(prep->diagnostic, SU_E_RESOURCE_LIMIT, node->id, 0, "allocation", "duplicate-check storage allocation failed");
        for (index = 0; index < node->count; index++)
            keys[index] = node->type == NODE_RANDOM ? prep->graph->edges[node->first + index].node : prep->graph->edges[node->first + index].value;
        qsort(keys, node->count, sizeof *keys, compare_u32);
        for (index = 1; index < node->count; index++) if (keys[index] == keys[index - 1]) { duplicate = 1; break; }
        free(keys);
        if (duplicate) return bad(prep, children, "duplicate random child ID or switch value");
    }
    if (node->type == NODE_SWITCH) {
        prep->graph->edges[prep->graph->edge_count++].node = fallback;
        node->count++;
    }
    return SU_OK;
}

static uint32_t validate_node(Prepare *prep, uint32_t index, uint32_t level) {
    GraphNode *node = &prep->graph->nodes[index];
    uint32_t child_index, code;
    uint64_t expansion = 0;
    if (node->color == 1) return fail(prep->diagnostic, SU_E_SCHEMA_MISMATCH, node->id, 0, "dag", "graph cycle");
    if (level > prep->graph->limits.max_depth)
        return fail(prep->diagnostic, SU_E_RESOURCE_LIMIT, node->id, 0, "max_depth", "graph depth exceeded");
    if (node->color == 2) {
        if (node->depth > prep->graph->limits.max_depth - level + 1)
            return fail(prep->diagnostic, SU_E_RESOURCE_LIMIT, node->id, 0, "max_depth", "graph depth exceeded");
        return SU_OK;
    }
    node->color = 1;
    node->depth = 1;
    if (node->type == NODE_SOUND) expansion = 1;
    for (child_index = 0; child_index < node->count; child_index++) {
        GraphEdge *edge = &prep->graph->edges[node->first + child_index];
        GraphNode *child = &prep->graph->nodes[edge->node];
        uint64_t delay;
        code = validate_node(prep, edge->node, level + 1);
        if (code) return code;
        if (child->depth + 1 > node->depth) node->depth = child->depth + 1;
        if (UINT64_MAX - child->delay < edge->offset)
            return fail(prep->diagnostic, SU_E_RESOURCE_LIMIT, node->id, 0, "delay_frames", "sequence delay overflow");
        delay = child->delay + edge->offset;
        if (delay > node->delay) node->delay = delay;
        if (node->type == NODE_SEQUENCE || node->type == NODE_PARALLEL) expansion += child->expansion;
        else if (node->type != NODE_BLEND && child->expansion > expansion) expansion = child->expansion;
        if (node->type == NODE_BLEND) {
            uint64_t adjacent = child->expansion;
            if (child_index) adjacent += prep->graph->nodes[prep->graph->edges[node->first + child_index - 1].node].expansion;
            if (adjacent > expansion) expansion = adjacent;
        }
        if (expansion > prep->graph->limits.max_expansion)
            return fail(prep->diagnostic, SU_E_RESOURCE_LIMIT, node->id, 0, "max_expansion", "leaf expansion exceeded");
    }
    node->expansion = (uint32_t)expansion;
    node->color = 2;
    return SU_OK;
}

uint32_t su_audio_graph_prepare(const void *json, size_t len, SuAudioAssets *assets,
    const SuAudioGraphLimits *limits, SuAudioGraph **out, SuAudioGraphDiagnostic *diagnostic) {
    SuJsonDoc document = {0};
    SuJsonError error;
    SuJsonLimits json_limits;
    SuAudioGraph *graph = NULL;
    Prepare prep;
    const SuJsonValue *root, *nodes, *events;
    uint32_t code = SU_OK, index;
    if (diagnostic) memset(diagnostic, 0, sizeof *diagnostic);
    if (out) *out = NULL;
    if (!json || !assets || !limits || !out || !limits->max_nodes || limits->max_nodes > 65536 ||
        !limits->max_edges || limits->max_edges > 262144 || !limits->max_depth || limits->max_depth > 128 ||
        !limits->max_expansion || limits->max_expansion > 65536 || !limits->max_json_bytes || limits->max_json_bytes > 16777216)
        return fail(diagnostic, SU_E_INVALID_ARGUMENT, 0, 0, "limits", "invalid argument or implementation ceiling exceeded");
    json_limits.max_bytes = limits->max_json_bytes;
    json_limits.max_depth = 8;
    code = su_json_parse(json, len, &json_limits, &document, &error);
    if (code) {
        fail(diagnostic, code, 0, error.offset, len > limits->max_json_bytes ? "max_json_bytes" : "json", error.message);
        su_json_doc_free(&document);
        return code;
    }
    graph = calloc(1, sizeof *graph);
    if (!graph) { code = fail(diagnostic, SU_E_RESOURCE_LIMIT, 0, 0, "allocation", "graph allocation failed"); goto done; }
    graph->refs = 1;
    graph->limits = *limits;
    graph->assets = assets;
    prep = (Prepare){json, len, graph, diagnostic, 0};
    root = document.root;
    if (!FIELDS(root, "schema", "numeric_profile", "seed_policy", "sample_rate", "nodes", "events") ||
        !text(su_json_member(root, "schema"), "su_audio_events_v1") ||
        !text(su_json_member(root, "numeric_profile"), "audio_float_local_v1") ||
        !text(su_json_member(root, "seed_policy"), "event_emitter_sequence_v1") ||
        !integer(su_json_member(root, "sample_rate"), &graph->sample_rate, 1) || graph->sample_rate < 1000 || graph->sample_rate > 384000) {
        code = bad(&prep, root, "invalid root fields, profile, or sample_rate"); goto done;
    }
    nodes = su_json_member(root, "nodes"); events = su_json_member(root, "events");
    if (!nodes || nodes->type != SU_JSON_ARRAY || !nodes->u.array.count ||
        !events || events->type != SU_JSON_ARRAY || !events->u.array.count) {
        code = bad(&prep, root, "nodes and events must be nonempty arrays"); goto done;
    }
    if (nodes->u.array.count > limits->max_nodes || events->u.array.count > limits->max_nodes) {
        code = fail(diagnostic, SU_E_RESOURCE_LIMIT, 0, 0, "max_nodes", "node or event count exceeded"); goto done;
    }
    graph->node_count = (uint32_t)nodes->u.array.count;
    graph->event_count = (uint32_t)events->u.array.count;
    graph->nodes = calloc(graph->node_count, sizeof *graph->nodes);
    graph->edges = calloc(limits->max_edges, sizeof *graph->edges);
    graph->events = calloc(graph->event_count, sizeof *graph->events);
    if (!graph->nodes || !graph->edges || !graph->events) {
        code = fail(diagnostic, SU_E_RESOURCE_LIMIT, 0, 0, "allocation", "graph storage allocation failed"); goto done;
    }
    for (index = 0; index < graph->node_count; index++) {
        code = parse_node(&prep, nodes->u.array.items[index], &graph->nodes[index]);
        if (code) goto done;
    }
    qsort(graph->nodes, graph->node_count, sizeof *graph->nodes, compare_node);
    for (index = 1; index < graph->node_count; index++)
        if (graph->nodes[index - 1].id == graph->nodes[index].id) {
            prep.node_id = graph->nodes[index].id; code = bad(&prep, nodes, "duplicate node ID"); goto done;
        }
    for (index = 0; index < graph->node_count; index++) {
        uint32_t child;
        prep.node_id = graph->nodes[index].id;
        for (child = 0; child < graph->nodes[index].count; child++) {
            GraphEdge *edge = &graph->edges[graph->nodes[index].first + child];
            uint32_t resolved = find_node(graph, edge->node);
            if (resolved == UINT32_MAX) { code = bad(&prep, nodes, "undefined child node ID"); goto done; }
            edge->node = resolved;
        }
    }
    for (index = 0; index < graph->node_count; index++) {
        code = validate_node(&prep, index, 1);
        if (code) goto done;
    }
    prep.node_id = 0;
    for (index = 0; index < graph->event_count; index++) {
        const SuJsonValue *event = events->u.array.items[index];
        if (!FIELDS(event, "id", "root") || !integer(su_json_member(event, "id"), &graph->events[index].id, 1) ||
            !integer(su_json_member(event, "root"), &graph->events[index].root, 1)) {
            code = bad(&prep, event, "invalid event mapping"); goto done;
        }
        graph->events[index].root = find_node(graph, graph->events[index].root);
        if (graph->events[index].root == UINT32_MAX) { code = bad(&prep, event, "undefined event root"); goto done; }
    }
    qsort(graph->events, graph->event_count, sizeof *graph->events, compare_event);
    for (index = 1; index < graph->event_count; index++)
        if (graph->events[index - 1].id == graph->events[index].id) { code = bad(&prep, events, "duplicate event ID"); goto done; }
    for (index = 0; index < graph->node_count; index++) {
        GraphNode *node = &graph->nodes[index];
        if (node->type == NODE_SOUND) {
            const SuAudioClip *clip = NULL;
            code = su_audio_asset_acquire(assets, node->leaf.asset, &clip);
            if (code) { fail(diagnostic, code, node->id, 0, "asset", "asset reference is unavailable"); goto done; }
            node->pinned = 1;
        }
    }
    *out = graph;
    graph = NULL;
done:
    su_json_doc_free(&document);
    su_audio_graph_release(graph);
    return code;
}

uint32_t su_audio_graph_retain(SuAudioGraph *graph) {
    if (!graph) return SU_E_INVALID_ARGUMENT;
    if (graph->refs == UINT32_MAX) return SU_E_RESOURCE_LIMIT;
    graph->refs++;
    return SU_OK;
}
void su_audio_graph_release(SuAudioGraph *graph) {
    uint32_t index;
    if (!graph || --graph->refs) return;
    if (graph->nodes) for (index = 0; index < graph->node_count; index++)
        if (graph->nodes[index].pinned) su_audio_asset_release(graph->assets, graph->nodes[index].leaf.asset);
    free(graph->nodes); free(graph->edges); free(graph->events); free(graph);
}
uint32_t su_audio_graph_sample_rate(const SuAudioGraph *graph) { return graph ? graph->sample_rate : 0; }

static uint64_t mix64(uint64_t value) {
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
typedef struct Expand {
    const SuAudioGraph *graph;
    const SuAudioGraphExpandState *state;
    uint64_t seed, emitter, sequence;
    SuAudioGraphLeaf *leaves;
    size_t count;
    SuAudioGraphDiagnostic *diagnostic;
    int write;
} Expand;
static SuAudioGraphHistory *history_for(const Expand *expand, uint32_t node_id) {
    size_t index;
    if (expand->state) for (index = 0; index < expand->state->history_count; index++)
        if (expand->state->history[index].node_id == node_id) return &expand->state->history[index];
    return NULL;
}
static uint32_t expand_node(Expand *expand, uint32_t index, uint64_t delay, float gain) {
    const GraphNode *node = &expand->graph->nodes[index];
    uint32_t begin = 0, end = node->count, child_index, code;
    float blend = 0.0f;
    if (node->type == NODE_SOUND) {
        float final_gain = gain * node->leaf.gain;
        if (!isfinite(final_gain)) return fail(expand->diagnostic, SU_E_RESOURCE_LIMIT, node->id, 0, "gain", "expanded gain overflow");
        if (expand->write) {
            expand->leaves[expand->count] = node->leaf;
            expand->leaves[expand->count].delay_frames = delay;
            expand->leaves[expand->count].gain = final_gain;
        }
        expand->count++;
        return SU_OK;
    }
    if (node->type == NODE_RANDOM) {
        SuAudioGraphHistory *history = history_for(expand, node->id);
        uint64_t total = 0, random, pick;
        uint32_t excluded = history ? history->last_child_id : 0;
        for (child_index = 0; child_index < node->count; child_index++) {
            const GraphEdge *edge = &expand->graph->edges[node->first + child_index];
            if (expand->graph->nodes[edge->node].id != excluded) total += edge->weight;
        }
        random = mix64(expand->seed ^ mix64(expand->emitter) ^ mix64(expand->sequence + UINT64_C(0x9e3779b97f4a7c15)) ^ mix64(node->id));
        pick = random % total;
        for (child_index = 0; child_index < node->count; child_index++) {
            const GraphEdge *edge = &expand->graph->edges[node->first + child_index];
            if (expand->graph->nodes[edge->node].id == excluded) continue;
            if (pick < edge->weight) break;
            pick -= edge->weight;
        }
        begin = child_index; end = begin + 1;
        if (history && expand->write) history->next_child_id = expand->graph->nodes[expand->graph->edges[node->first + begin].node].id;
    } else if (node->type == NODE_SWITCH) {
        size_t snapshot;
        begin = node->count - 1;
        if (expand->state) for (snapshot = 0; snapshot < expand->state->switch_count; snapshot++) {
            if (expand->state->switches[snapshot].switch_id != node->control) continue;
            for (child_index = 0; child_index + 1 < node->count; child_index++)
                if (expand->graph->edges[node->first + child_index].value == expand->state->switches[snapshot].value) { begin = child_index; break; }
            break;
        }
        end = begin + 1;
    } else if (node->type == NODE_BLEND) {
        float value = 0.0f;
        size_t snapshot;
        int found = 0;
        if (expand->state) for (snapshot = 0; snapshot < expand->state->parameter_count; snapshot++)
            if (expand->state->parameters[snapshot].parameter_id == node->control) { value = expand->state->parameters[snapshot].value; found = 1; break; }
        if (!found) return fail(expand->diagnostic, SU_E_SCHEMA_MISMATCH, node->id, 0, "parameter", "missing blend parameter snapshot");
        begin = 0;
        while (begin + 1 < node->count && value >= expand->graph->edges[node->first + begin + 1].point) begin++;
        end = begin + 1;
        if (value > expand->graph->edges[node->first + begin].point && end < node->count) {
            double low = expand->graph->edges[node->first + begin].point;
            double high = expand->graph->edges[node->first + end].point;
            blend = (float)(((double)value - low) / (high - low));
            end++;
        }
    }
    for (child_index = begin; child_index < end; child_index++) {
        const GraphEdge *edge = &expand->graph->edges[node->first + child_index];
        float child_gain = gain;
        if (node->type == NODE_BLEND && end - begin == 2) child_gain *= child_index == begin ? 1.0f - blend : blend;
        code = expand_node(expand, edge->node, delay + edge->offset, child_gain);
        if (code) return code;
    }
    return SU_OK;
}
static uint32_t validate_state(const SuAudioGraph *graph, const SuAudioGraphExpandState *state, SuAudioGraphDiagnostic *diagnostic) {
    size_t index, prior;
    if (!state) return SU_OK;
    if ((state->switch_count && !state->switches) || (state->parameter_count && !state->parameters) ||
        (state->history_count && !state->history))
        return fail(diagnostic, SU_E_INVALID_ARGUMENT, 0, 0, "snapshot", "null snapshot storage");
    if (state->switch_count > graph->node_count || state->parameter_count > graph->node_count || state->history_count > graph->node_count)
        return fail(diagnostic, SU_E_RESOURCE_LIMIT, 0, 0, "snapshot", "snapshot count exceeds node count");
    for (index = 0; index < state->switch_count; index++) {
        if (!state->switches[index].switch_id) return fail(diagnostic, SU_E_SCHEMA_MISMATCH, 0, 0, "snapshot", "invalid switch ID");
        for (prior = 0; prior < index; prior++) if (state->switches[index].switch_id == state->switches[prior].switch_id)
            return fail(diagnostic, SU_E_SCHEMA_MISMATCH, 0, 0, "snapshot", "duplicate switch snapshot");
    }
    for (index = 0; index < state->parameter_count; index++) {
        if (!state->parameters[index].parameter_id || !isfinite(state->parameters[index].value))
            return fail(diagnostic, SU_E_SCHEMA_MISMATCH, 0, 0, "snapshot", "invalid parameter snapshot");
        for (prior = 0; prior < index; prior++) if (state->parameters[index].parameter_id == state->parameters[prior].parameter_id)
            return fail(diagnostic, SU_E_SCHEMA_MISMATCH, 0, 0, "snapshot", "duplicate parameter snapshot");
    }
    for (index = 0; index < state->history_count; index++) {
        const SuAudioGraphHistory *history = &state->history[index];
        uint32_t resolved = find_node(graph, history->node_id), child;
        int valid = history->last_child_id == 0;
        if (resolved == UINT32_MAX || graph->nodes[resolved].type != NODE_RANDOM || graph->nodes[resolved].count < 2)
            return fail(diagnostic, SU_E_SCHEMA_MISMATCH, history->node_id, 0, "history", "history requires a random node with at least two distinct children");
        for (prior = 0; prior < index; prior++) if (state->history[prior].node_id == history->node_id)
            return fail(diagnostic, SU_E_SCHEMA_MISMATCH, history->node_id, 0, "history", "duplicate history node");
        for (child = 0; child < graph->nodes[resolved].count; child++)
            if (graph->nodes[graph->edges[graph->nodes[resolved].first + child].node].id == history->last_child_id) valid = 1;
        if (!valid) return fail(diagnostic, SU_E_SCHEMA_MISMATCH, history->node_id, 0, "history", "history child does not belong to node");
    }
    return SU_OK;
}
uint32_t su_audio_graph_expand_ex(const SuAudioGraph *graph, uint32_t event_id,
    uint64_t seed, uint64_t emitter, uint64_t commit_seq, const SuAudioGraphExpandState *state,
    SuAudioGraphLeaf *leaves, size_t capacity, size_t *count, SuAudioGraphDiagnostic *diagnostic) {
    uint32_t low = 0, high, code;
    Expand expand;
    size_t index;
    if (diagnostic) memset(diagnostic, 0, sizeof *diagnostic);
    if (count) *count = 0;
    if (!graph || !count || (capacity && !leaves)) return fail(diagnostic, SU_E_INVALID_ARGUMENT, 0, 0, "argument", "invalid expansion argument");
    code = validate_state(graph, state, diagnostic);
    if (code) return code;
    high = graph->event_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (graph->events[middle].id < event_id) low = middle + 1;
        else high = middle;
    }
    if (low == graph->event_count || graph->events[low].id != event_id)
        return fail(diagnostic, SU_E_SCHEMA_MISMATCH, 0, 0, "event_id", "undefined event ID");
    expand = (Expand){graph, state, seed, emitter, commit_seq, leaves, 0, diagnostic, 0};
    code = expand_node(&expand, graph->events[low].root, 0, 1.0f);
    if (code) return code;
    if (expand.count > capacity) return fail(diagnostic, SU_E_RESOURCE_LIMIT, graph->nodes[graph->events[low].root].id, 0, "capacity", "leaf output capacity exceeded");
    if (state) for (index = 0; index < state->history_count; index++) state->history[index].next_child_id = state->history[index].last_child_id;
    expand.count = 0; expand.write = 1;
    code = expand_node(&expand, graph->events[low].root, 0, 1.0f);
    if (code) return code;
    if (state) for (index = 0; index < state->history_count; index++) state->history[index].last_child_id = state->history[index].next_child_id;
    *count = expand.count;
    return SU_OK;
}
uint32_t su_audio_graph_expand(const SuAudioGraph *graph, uint32_t event_id,
    uint64_t seed, uint64_t emitter, uint64_t commit_seq, SuAudioGraphLeaf *leaves,
    size_t capacity, size_t *count, SuAudioGraphDiagnostic *diagnostic) {
    return su_audio_graph_expand_ex(graph, event_id, seed, emitter, commit_seq, NULL, leaves, capacity, count, diagnostic);
}
