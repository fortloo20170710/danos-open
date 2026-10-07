/*
 * DANOS-Open Core: capability query across registered backends.
 *
 * A backend declares what it can program with danos_backend_register(); this
 * answers whether any (or a named) backend supports a given object type. The
 * programming pipeline's type_skipped() and the backend's capability table
 * must agree - a type advertised as supported but skipped by the pipeline (or
 * the reverse) is a false promise to anything doing backend selection.
 */

#ifndef DANOS_CORE_CAPABILITY_REGISTRY_H__
#define DANOS_CORE_CAPABILITY_REGISTRY_H__

#include <danos/dpa.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if any registered backend advertises support for `type`. */
bool danos_core_capability_supported(danos_obj_type_t type);

/* Check a specific backend. `backend_name` of NULL means the first backend
 * that advertises support.
 *
 * Returns NOT_SUPPORTED when no backend advertises the type, NO_CAPACITY
 * when `required_count` exceeds what the backend advertised, and OK
 * otherwise. */
danos_status_t danos_core_capability_check(const char *backend_name,
                                           danos_obj_type_t type,
                                           uint32_t required_count);

/* Number of backends currently registered. Bounded so a broken
 * danos_backend_get_info() cannot spin here forever. */
#define DANOS_MAX_BACKENDS 16

#ifdef __cplusplus
}
#endif

#endif /* DANOS_CORE_CAPABILITY_REGISTRY_H__ */
