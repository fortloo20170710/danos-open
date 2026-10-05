/*
 * VPP backend capability advertisement.
 *
 * The table in vpp_capability.c states exactly what the adapter can program.
 * It must be registered with the core registry at install time, or backend
 * selection sees an empty registry.
 */

#ifndef DANOS_VPP_CAPABILITY_H__
#define DANOS_VPP_CAPABILITY_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Register the VPP capability table with the core backend registry.
 * Idempotent; safe to call when no backend is installed. */
int danos_vpp_capability_register(void);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_VPP_CAPABILITY_H__ */
