#define _POSIX_C_SOURCE 200809L

#include "agent/su_module.h"

#include <string.h>

#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "sutekh/status.h"

uint32_t su_module_capsule_meta(const uint8_t *cap, size_t len, char *source, size_t source_cap,
                                uint32_t *schema_version, uint32_t *layout_generation) {
    SuJsonLimits limits;
    SuJsonDoc doc;
    SuJsonError err;
    const SuJsonValue *root;
    const SuJsonValue *v;
    const SuJsonValue *schema;
    uint32_t st;

    limits.max_bytes = len;
    limits.max_depth = SU_PROTO_MAX_DEPTH;
    memset(&doc, 0, sizeof doc);
    st = su_json_parse(cap, len, &limits, &doc, &err);
    if (st != SU_OK) {
        su_json_doc_free(&doc);
        return st;
    }
    root = doc.root;
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        su_json_doc_free(&doc);
        return SU_E_TYPE;
    }
    v = su_json_member(root, "source");
    if (!su_json_is_string(v) || v->u.string.len == 0 || v->u.string.len > SU_MODULE_NAME_MAX ||
        su_json_copy_string(v, source, source_cap) != 0) {
        su_json_doc_free(&doc);
        return SU_E_TYPE;
    }
    schema = su_json_member(root, "schema");
    if (schema == NULL || schema->type != SU_JSON_OBJECT ||
        su_json_get_u32(schema, "version", schema_version) != 0 ||
        su_json_get_u32(schema, "layout_generation", layout_generation) != 0) {
        su_json_doc_free(&doc);
        return SU_E_TYPE;
    }
    su_json_doc_free(&doc);
    return SU_OK;
}
