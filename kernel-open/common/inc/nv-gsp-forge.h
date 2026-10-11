/*
 * nv-gsp-forge.h  -  OS-layer entry points for the GSP RPC forger debugfs
 *                    interface.  Included by nv.c (init/exit wiring) and by
 *                    nvidia-gsp-forge.c (the implementation).
 *
 * RESEARCH INSTRUMENT.  See gsp_forge_core.c for what it does.
 */
#ifndef NV_GSP_FORGE_H
#define NV_GSP_FORGE_H

void nv_gsp_forge_init(void);
void nv_gsp_forge_exit(void);

#endif /* NV_GSP_FORGE_H */
