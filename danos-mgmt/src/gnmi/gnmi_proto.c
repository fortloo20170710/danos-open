/*
 * DANOS-Open Management: gNMI Protobuf Codec (v0.3)
 *
 * Standard proto3 wire format:
 *   tag = (field_number << 3) | wire_type, varint-encoded
 *   wire types: 0=varint, 1=64-bit, 2=length-delimited, 5=32-bit
 *   proto3 default: fields with default values are not serialized;
 *   varint fields are emitted even when zero is intentional for
 *   explicitness in enums (proto3 would omit; gNMI clients accept both).
 */

#include "gnmi_proto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * Writer
 * ========================================================================= */

void gnmi_pb_init(gnmi_pb_t *w, void *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

static void pb_write(gnmi_pb_t *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void gnmi_pb_put_varint(gnmi_pb_t *w, uint64_t v)
{
    uint8_t t[10];
    int n = 0;
    while (v >= 0x80) {
        t[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    t[n++] = (uint8_t)v;
    pb_write(w, t, (size_t)n);
}

void gnmi_pb_put_tag(gnmi_pb_t *w, uint32_t field, uint32_t wire)
{
    gnmi_pb_put_varint(w, ((uint64_t)field << 3) | wire);
}

void gnmi_pb_put_len_delim(gnmi_pb_t *w, uint32_t field,
                           const void *data, size_t len)
{
    gnmi_pb_put_tag(w, field, 2);
    gnmi_pb_put_varint(w, len);
    pb_write(w, data, len);
}

void gnmi_pb_put_string(gnmi_pb_t *w, uint32_t field, const char *s)
{
    if (!s) s = "";
    gnmi_pb_put_len_delim(w, field, s, strlen(s));
}

void gnmi_pb_put_uint64(gnmi_pb_t *w, uint32_t field, uint64_t v)
{
    /* proto3: skip zero */
    if (v == 0) return;
    gnmi_pb_put_tag(w, field, 0);
    gnmi_pb_put_varint(w, v);
}

void gnmi_pb_put_bool(gnmi_pb_t *w, uint32_t field, bool v)
{
    if (!v) return;
    gnmi_pb_put_tag(w, field, 0);
    gnmi_pb_put_varint(w, 1);
}

void gnmi_pb_put_enum(gnmi_pb_t *w, uint32_t field, int32_t v)
{
    if (v == 0) return;   /* proto3 default */
    gnmi_pb_put_tag(w, field, 0);
    gnmi_pb_put_varint(w, (uint64_t)(int64_t)v);
}

size_t gnmi_pb_begin_nested(gnmi_pb_t *w, uint32_t field)
{
    gnmi_pb_put_tag(w, field, 2);
    /* reserve one byte for length; extend if needed */
    size_t saved = w->len;
    uint8_t zero = 0;
    pb_write(w, &zero, 1);
    return saved;
}

static void pb_fix_len(gnmi_pb_t *w, size_t saved)
{
    size_t body = w->len - saved - 1;
    if (body < 0x80) {
        w->buf[saved] = (uint8_t)body;
    } else {
        /* need two bytes: shift body right by one */
        size_t n = body;
        if (saved + 1 + n + 1 > w->cap) {
            w->overflow = true;
            return;
        }
        memmove(w->buf + saved + 2, w->buf + saved + 1, n);
        w->buf[saved] = (uint8_t)(body | 0x80);
        w->buf[saved + 1] = (uint8_t)(body >> 7);
        w->len += 1;
    }
}

void gnmi_pb_end_nested(gnmi_pb_t *w, size_t saved)
{
    pb_fix_len(w, saved);
}

/* =========================================================================
 * Reader
 * ========================================================================= */

void gnmi_pbr_init(gnmi_pb_reader_t *r, const void *buf, size_t len)
{
    r->p = buf;
    r->len = len;
    r->pos = 0;
    r->err = false;
}

uint64_t gnmi_pbr_varint(gnmi_pb_reader_t *r)
{
    uint64_t v = 0;
    int shift = 0;
    while (shift < 64) {
        if (r->pos >= r->len) {
            r->err = true;
            return 0;
        }
        uint8_t b = r->p[r->pos++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
    r->err = true;
    return 0;
}

uint32_t gnmi_pbr_tag(gnmi_pb_reader_t *r, uint32_t *wire)
{
    if (r->pos >= r->len) return 0;
    uint64_t t = gnmi_pbr_varint(r);
    if (r->err) return 0;
    *wire = (uint32_t)(t & 7);
    return (uint32_t)(t >> 3);
}

bool gnmi_pbr_bytes(gnmi_pb_reader_t *r, const uint8_t **data, size_t *len)
{
    uint64_t n = gnmi_pbr_varint(r);
    if (r->err || r->pos + n > r->len) {
        r->err = true;
        return false;
    }
    *data = r->p + r->pos;
    *len = n;
    r->pos += n;
    return true;
}

void gnmi_pbr_skip(gnmi_pb_reader_t *r, uint32_t wire)
{
    switch (wire) {
    case 0: (void)gnmi_pbr_varint(r); break;
    case 1: r->pos += 8; break;
    case 2: {
        const uint8_t *d; size_t n;
        (void)gnmi_pbr_bytes(r, &d, &n);
        break;
    }
    case 5: r->pos += 4; break;
    default: r->err = true; break;
    }
    if (r->pos > r->len) r->err = true;
}

/* =========================================================================
 * Path
 * ========================================================================= */

void gnmi_encode_path(gnmi_pb_t *w, uint32_t field, const gnmi_path_t *path)
{
    if (!path) return;
    size_t saved = gnmi_pb_begin_nested(w, field);

    if (path->origin[0])
        gnmi_pb_put_string(w, 2 /* origin */, path->origin);

    for (uint32_t i = 0; i < path->elem_count; i++) {
        const gnmi_path_elem_t *e = &path->elems[i];
        /* PathElem { name=1, key=2 } */
        size_t esaved = gnmi_pb_begin_nested(w, 3 /* elem */);
        gnmi_pb_put_string(w, 1, e->name);
        if (e->has_key) {
            /* map<string,string> entry: key=1, value=2 */
            size_t ksaved = gnmi_pb_begin_nested(w, 2);
            gnmi_pb_put_string(w, 1, e->key_name);
            gnmi_pb_put_string(w, 2, e->key_value);
            gnmi_pb_end_nested(w, ksaved);
        }
        if (e->has_extra_key) {
            size_t ksaved = gnmi_pb_begin_nested(w, 2);
            gnmi_pb_put_string(w, 1, e->extra_key_name);
            gnmi_pb_put_string(w, 2, e->extra_key_value);
            gnmi_pb_end_nested(w, ksaved);
        }
        gnmi_pb_end_nested(w, esaved);
    }
    gnmi_pb_end_nested(w, saved);
}

bool gnmi_decode_path(const uint8_t *data, size_t len, gnmi_path_t *out)
{
    memset(out, 0, sizeof(*out));
    gnmi_pb_reader_t r;
    gnmi_pbr_init(&r, data, len);

    uint32_t field, wire;
    while ((field = gnmi_pbr_tag(&r, &wire)) != 0) {
        if (r.err) return false;
        switch (field) {
        case 2: {  /* origin */
            const uint8_t *d; size_t n;
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (n >= sizeof(out->origin) || memchr(d, 0, n)) return false;
            size_t c = n;
            memcpy(out->origin, d, c);
            out->origin[c] = '\0';
            break;
        }
        case 3: {  /* elem */
            const uint8_t *d; size_t n;
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (out->elem_count >= GNMI_MAX_ELEMS) return false;
            gnmi_path_elem_t *e = &out->elems[out->elem_count++];
            memset(e, 0, sizeof(*e));

            gnmi_pb_reader_t er;
            gnmi_pbr_init(&er, d, n);
            uint32_t ef, ew;
            while ((ef = gnmi_pbr_tag(&er, &ew)) != 0) {
                if (er.err) return false;
                if (ef == 1 && ew == 2) {  /* name */
                    const uint8_t *nd; size_t nn;
                    if (!gnmi_pbr_bytes(&er, &nd, &nn)) return false;
                    if (!nn || nn >= GNMI_MAX_NAME || memchr(nd, 0, nn)) return false;
                    size_t c = nn;
                    memcpy(e->name, nd, c);
                    e->name[c] = '\0';
                } else if (ef == 2 && ew == 2) {  /* key entry */
                    const uint8_t *kd; size_t kn;
                    if (!gnmi_pbr_bytes(&er, &kd, &kn)) return false;
                    gnmi_pb_reader_t kr;
                    gnmi_pbr_init(&kr, kd, kn);
                    uint32_t kf, kw;
                    char kname[GNMI_MAX_NAME] = {0};
                    char kval[GNMI_MAX_NAME] = {0};
                    while ((kf = gnmi_pbr_tag(&kr, &kw)) != 0) {
                        const uint8_t *vd; size_t vn;
                        if (kf == 1 && kw == 2 && gnmi_pbr_bytes(&kr, &vd, &vn)) {
                            if (vn >= GNMI_MAX_NAME || memchr(vd, 0, vn)) return false;
                            size_t c = vn;
                            memcpy(kname, vd, c);
                            kname[c] = '\0';
                        } else if (kf == 2 && kw == 2 && gnmi_pbr_bytes(&kr, &vd, &vn)) {
                            if (vn >= GNMI_MAX_NAME || memchr(vd, 0, vn)) return false;
                            size_t c = vn;
                            memcpy(kval, vd, c);
                            kval[c] = '\0';
                        } else {
                            gnmi_pbr_skip(&kr, kw);
                        }
                        if (kr.err) return false;
                    }
                    if (!kname[0] || !kval[0]) return false;
                    if (!e->has_key) {
                        snprintf(e->key_name, sizeof(e->key_name), "%s", kname);
                        snprintf(e->key_value, sizeof(e->key_value), "%s", kval);
                        e->has_key = true;
                    } else {
                        if (e->has_extra_key || !strcmp(e->key_name, kname)) return false;
                        snprintf(e->extra_key_name, sizeof(e->extra_key_name), "%s", kname);
                        snprintf(e->extra_key_value, sizeof(e->extra_key_value), "%s", kval);
                        e->has_extra_key = true;
                    }
                } else {
                    gnmi_pbr_skip(&er, ew);
                }
            }
            break;
        }
        default:
            gnmi_pbr_skip(&r, wire);
            break;
        }
    }
    return !r.err;
}

bool gnmi_path_from_str(gnmi_path_t *p, const char *text)
{
    memset(p, 0, sizeof(*p));
    if (!text) return false;
    const char *s = text;
    while (*s == '/') s++;
    while (*s) {
        if (p->elem_count == GNMI_MAX_ELEMS) return false;
        gnmi_path_elem_t *e = &p->elems[p->elem_count++];
        size_t n = strcspn(s, "/[");
        if (!n || n >= GNMI_MAX_NAME) return false;
        memcpy(e->name, s, n); s += n;
        unsigned keys = 0;
        while (*s == '[') {
            if (++keys > 2) return false;
            const char *close = strchr(s, ']');
            const char *eq = strchr(s, '=');
            if (!close || !eq || eq > close || eq == s + 1) return false;
            size_t kn = (size_t)(eq - s - 1), vn = (size_t)(close - eq - 1);
            if (!vn || kn >= GNMI_MAX_NAME || vn >= GNMI_MAX_NAME) return false;
            char *name = keys == 1 ? e->key_name : e->extra_key_name;
            char *value = keys == 1 ? e->key_value : e->extra_key_value;
            memcpy(name, s + 1, kn); memcpy(value, eq + 1, vn);
            if (keys == 1) e->has_key = true;
            else {
                if (!strcmp(e->key_name, e->extra_key_name)) return false;
                e->has_extra_key = true;
            }
            s = close + 1;
        }
        if (*s && *s != '/') return false;
        if (*s == '/') s++;
    }
    return p->elem_count > 0;
}

/* =========================================================================
 * TypedValue
 * ========================================================================= */

void gnmi_encode_typed_value(gnmi_pb_t *w, uint32_t field,
                             const gnmi_typed_value_t *val)
{
    if (!val || val->kind == GNMI_VAL_UNSET) return;
    size_t saved = gnmi_pb_begin_nested(w, field);
    switch (val->kind) {
    case GNMI_VAL_STRING:
    case GNMI_VAL_JSON:
    case GNMI_VAL_JSON_IETF:
    case GNMI_VAL_ASCII:
        gnmi_pb_put_string(w, (uint32_t)val->kind, val->s);
        break;
    case GNMI_VAL_INT:
        /* int64 as varint (two's complement) */
        gnmi_pb_put_tag(w, 2, 0);
        gnmi_pb_put_varint(w, (uint64_t)val->i);
        break;
    case GNMI_VAL_UINT:
        gnmi_pb_put_uint64(w, 3, val->u);
        break;
    case GNMI_VAL_BOOL:
        gnmi_pb_put_tag(w, 4, 0);
        gnmi_pb_put_varint(w, val->b ? 1 : 0);
        break;
    default:
        break;
    }
    gnmi_pb_end_nested(w, saved);
}

bool gnmi_decode_typed_value(const uint8_t *data, size_t len,
                             gnmi_typed_value_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = GNMI_VAL_UNSET;
    gnmi_pb_reader_t r;
    gnmi_pbr_init(&r, data, len);

    uint32_t field, wire;
    while ((field = gnmi_pbr_tag(&r, &wire)) != 0) {
        if (r.err) return false;
        switch (field) {
        case 1: case 10: case 11: case 12: {  /* string-ish */
            const uint8_t *d; size_t n;
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (n >= GNMI_MAX_VAL || memchr(d, 0, n)) return false;
            size_t c = n;
            memcpy(out->s, d, c);
            out->s[c] = '\0';
            out->kind = (gnmi_val_kind_t)field;
            break;
        }
        case 2:
            out->i = (int64_t)gnmi_pbr_varint(&r);
            out->kind = GNMI_VAL_INT;
            break;
        case 3:
            out->u = gnmi_pbr_varint(&r);
            out->kind = GNMI_VAL_UINT;
            break;
        case 4:
            out->b = gnmi_pbr_varint(&r) != 0;
            out->kind = GNMI_VAL_BOOL;
            break;
        default:
            gnmi_pbr_skip(&r, wire);
            break;
        }
        if (r.err) return false;
    }
    return out->kind != GNMI_VAL_UNSET;
}

/* =========================================================================
 * Update / Notification
 * ========================================================================= */

static void encode_update(gnmi_pb_t *w, uint32_t field, const gnmi_update_t *u)
{
    size_t saved = gnmi_pb_begin_nested(w, field);
    gnmi_encode_path(w, 1, &u->path);
    gnmi_encode_typed_value(w, 3, &u->val);
    gnmi_pb_end_nested(w, saved);
}

static bool decode_update(const uint8_t *data, size_t len, gnmi_update_t *out)
{
    memset(out, 0, sizeof(*out));
    gnmi_pb_reader_t r;
    gnmi_pbr_init(&r, data, len);
    uint32_t field, wire;
    while ((field = gnmi_pbr_tag(&r, &wire)) != 0) {
        if (r.err) return false;
        const uint8_t *d; size_t n;
        switch (field) {
        case 1:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (!gnmi_decode_path(d, n, &out->path)) return false;
            break;
        case 3:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (!gnmi_decode_typed_value(d, n, &out->val)) return false;
            break;
        default:
            gnmi_pbr_skip(&r, wire);
            break;
        }
    }
    return !r.err;
}

void gnmi_encode_notification(gnmi_pb_t *w, uint32_t field,
                              const gnmi_notification_t *n)
{
    size_t saved = gnmi_pb_begin_nested(w, field);
    gnmi_pb_put_uint64(w, 1, n->timestamp);   /* proto3 skips 0 */
    for (uint32_t i = 0; i < n->update_count; i++) {
        encode_update(w, 4, &n->updates[i]);
    }
    gnmi_pb_end_nested(w, saved);
}

/* =========================================================================
 * Get
 * ========================================================================= */

bool gnmi_decode_get_request(const uint8_t *data, size_t len,
                             gnmi_get_request_t *out)
{
    memset(out, 0, sizeof(*out));
    out->encoding = GNMI_ENC_JSON;
    gnmi_pb_reader_t r;
    gnmi_pbr_init(&r, data, len);
    uint32_t field, wire;
    while ((field = gnmi_pbr_tag(&r, &wire)) != 0) {
        if (r.err) return false;
        const uint8_t *d; size_t n;
        switch (field) {
        case 1:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (!gnmi_decode_path(d, n, &out->prefix)) return false;
            break;
        case 2:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (out->path_count >= GNMI_MAX_ELEMS) return false;
            if (!gnmi_decode_path(d, n, &out->paths[out->path_count++]))
                return false;
            break;
        case 3:
            out->data_type = (int32_t)gnmi_pbr_varint(&r);
            break;
        case 5:
            out->encoding = (gnmi_encoding_t)gnmi_pbr_varint(&r);
            break;
        default:
            gnmi_pbr_skip(&r, wire);
            break;
        }
    }
    return !r.err && out->path_count > 0;
}

bool gnmi_encode_get_response(gnmi_pb_t *w,
                              const gnmi_notification_t *notifs,
                              uint32_t notif_count)
{
    for (uint32_t i = 0; i < notif_count; i++) {
        gnmi_encode_notification(w, 1, &notifs[i]);
    }
    return !w->overflow;
}

/* =========================================================================
 * Set
 * ========================================================================= */

bool gnmi_decode_set_request(const uint8_t *data, size_t len,
                             gnmi_set_request_t *out)
{
    memset(out, 0, sizeof(*out));
    gnmi_pb_reader_t r;
    gnmi_pbr_init(&r, data, len);
    uint32_t field, wire;
    while ((field = gnmi_pbr_tag(&r, &wire)) != 0) {
        if (r.err) return false;
        const uint8_t *d; size_t n;
        switch (field) {
        case 1:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (!gnmi_decode_path(d, n, &out->prefix)) return false;
            break;
        case 2:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (out->delete_count >= GNMI_MAX_ELEMS) return false;
            if (!gnmi_decode_path(d, n, &out->deletes[out->delete_count++]))
                return false;
            break;
        case 3:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (out->replace_count >= GNMI_MAX_UPDATES) return false;
            if (!decode_update(d, n, &out->replaces[out->replace_count++]))
                return false;
            break;
        case 4:
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            if (out->update_count >= GNMI_MAX_UPDATES) return false;
            if (!decode_update(d, n, &out->updates[out->update_count++]))
                return false;
            break;
        case 6: /* union_replace is not implemented: never report a no-op success. */
            return false;
        default:
            gnmi_pbr_skip(&r, wire);
            break;
        }
    }
    return !r.err;
}

bool gnmi_encode_set_response(gnmi_pb_t *w, const gnmi_set_response_t *resp)
{
    /* prefix = 1 (omit if empty) */
    if (resp->prefix.elem_count > 0)
        gnmi_encode_path(w, 1, &resp->prefix);

    for (uint32_t i = 0; i < resp->result_count; i++) {
        size_t saved = gnmi_pb_begin_nested(w, 2 /* response */);
        gnmi_encode_path(w, 2, &resp->result_paths[i]);
        gnmi_pb_put_enum(w, 4, (int32_t)resp->ops[i]);
        gnmi_pb_end_nested(w, saved);
    }
    gnmi_pb_put_uint64(w, 4, resp->timestamp);
    return !w->overflow;
}

/* =========================================================================
 * Capabilities
 * ========================================================================= */

bool gnmi_encode_capabilities_response(gnmi_pb_t *w,
                                       const gnmi_model_data_t *models,
                                       uint32_t model_count,
                                       const gnmi_encoding_t *encodings,
                                       uint32_t encoding_count,
                                       const char *gnmi_version)
{
    for (uint32_t i = 0; i < model_count; i++) {
        size_t saved = gnmi_pb_begin_nested(w, 1 /* supported_models */);
        gnmi_pb_put_string(w, 1, models[i].name);
        gnmi_pb_put_string(w, 2, models[i].organization);
        gnmi_pb_put_string(w, 3, models[i].version);
        gnmi_pb_end_nested(w, saved);
    }
    for (uint32_t i = 0; i < encoding_count; i++) {
        gnmi_pb_put_enum(w, 2, (int32_t)encodings[i]);
    }
    gnmi_pb_put_string(w, 3, gnmi_version);
    return !w->overflow;
}

/* =========================================================================
 * Subscribe (v0.4)
 * ========================================================================= */

bool gnmi_decode_subscribe_request(const uint8_t *data, size_t len,
                                   gnmi_subscribe_request_t *out)
{
    memset(out, 0, sizeof(*out));
    out->subscribe.mode = GNMI_SUB_MODE_STREAM;
    out->subscribe.encoding = GNMI_ENC_JSON;
    gnmi_pb_reader_t r;
    gnmi_pbr_init(&r, data, len);
    uint32_t field, wire;
    while ((field = gnmi_pbr_tag(&r, &wire)) != 0) {
        if (r.err) return false;
        const uint8_t *d; size_t n;
        switch (field) {
        case 1:  /* subscribe = SubscriptionList */
            if (!gnmi_pbr_bytes(&r, &d, &n)) return false;
            {
                gnmi_pb_reader_t lr;
                gnmi_pbr_init(&lr, d, n);
                uint32_t lf, lw;
                while ((lf = gnmi_pbr_tag(&lr, &lw)) != 0) {
                    if (lr.err) return false;
                    const uint8_t *ld; size_t ln;
                    switch (lf) {
                    case 2:  /* subscription */
                        if (!gnmi_pbr_bytes(&lr, &ld, &ln)) return false;
                        if (out->subscribe.sub_count >= GNMI_MAX_ELEMS) return false;
                        {
                            gnmi_subscription_t *sub =
                                &out->subscribe.subs[out->subscribe.sub_count++];
                            memset(sub, 0, sizeof(*sub));
                            gnmi_pb_reader_t sr;
                            gnmi_pbr_init(&sr, ld, ln);
                            uint32_t sf, sw;
                            while ((sf = gnmi_pbr_tag(&sr, &sw)) != 0) {
                                const uint8_t *sd; size_t sn;
                                if (sf == 1 && gnmi_pbr_bytes(&sr, &sd, &sn)) {
                                    if (!gnmi_decode_path(sd, sn, &sub->path))
                                        return false;
                                } else {
                                    gnmi_pbr_skip(&sr, sw);
                                }
                            }
                        }
                        break;
                    case 5:
                        out->subscribe.mode = (gnmi_sub_mode_t)gnmi_pbr_varint(&lr);
                        break;
                    case 8:
                        out->subscribe.encoding = (gnmi_encoding_t)gnmi_pbr_varint(&lr);
                        break;
                    case 9:
                        out->subscribe.updates_only = gnmi_pbr_varint(&lr) != 0;
                        break;
                    default:
                        gnmi_pbr_skip(&lr, lw);
                        break;
                    }
                }
            }
            break;
        case 3:  /* poll */
            out->is_poll = true;
            gnmi_pbr_skip(&r, wire);
            break;
        default:
            gnmi_pbr_skip(&r, wire);
            break;
        }
    }
    return !r.err;
}

bool gnmi_encode_subscribe_update(gnmi_pb_t *w, const gnmi_notification_t *n)
{
    gnmi_encode_notification(w, 1, n);
    return !w->overflow;
}

bool gnmi_encode_subscribe_sync(gnmi_pb_t *w)
{
    gnmi_pb_put_bool(w, 3, true);
    return !w->overflow;
}
