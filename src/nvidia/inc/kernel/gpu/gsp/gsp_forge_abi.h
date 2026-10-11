/*
 * gsp_forge_abi.h  -  binary ABI between the userspace control tool and the
 *                     kernel-side GSP RPC "forger" built into nvidia.ko.
 *
 * RESEARCH INSTRUMENT.  Lets a privileged (kernel/host-RM) caller issue forged
 * GSP_RM_CONTROL / GSP_RM_ALLOC RPCs to the GSP with attacker-chosen handles
 * and param bytes, and to enumerate the live RM resource database.  This is the
 * CC "untrusted host RM" vantage: it sits below the CPU-RM permission gate that
 * stops a userspace client (the NV_ERR_INSUFFICIENT_PERMISSIONS / 0x1b wall).
 *
 * ---- EXTENSIBILITY CONTRACT ----------------------------------------------
 *   - The request/response headers are FIXED-SIZE and VERSIONED.  Never
 *     renumber or resize an existing field; grow only by consuming a
 *     reserved[] slot and bumping GSP_FORGE_ABI_VERSION's minor meaning.
 *   - New test scenarios are new GSP_FORGE_OP_* values.  Old tools keep
 *     working because the kernel rejects unknown ops with E_OP rather than
 *     mis-parsing.  Reserved ops (FREE, RAW, EVENT) are already numbered so a
 *     future build can fill them without shifting anything.
 *   - params[] is an opaque blob whose meaning depends on op.  The kernel does
 *     not interpret it for CONTROL/ALLOC (it is handed to the RPC verbatim and
 *     GSP writes results back), so new control commands need no ABI change.
 * --------------------------------------------------------------------------
 *
 * This header is shared verbatim by gsp_forge_core.c (kernel) and
 * gsp_forge_ctl.c (userspace).  Keep it dependency-light: only nvtypes.
 */
#ifndef GSP_FORGE_ABI_H
#define GSP_FORGE_ABI_H

#include "nvtypes.h"

#define GSP_FORGE_REQ_MAGIC    0x47465251u  /* "GFRQ" */
#define GSP_FORGE_RESP_MAGIC   0x47465250u  /* "GFRP" */
#define GSP_FORGE_ABI_VERSION  1u

/* ------- operations (never renumber; append only) ------------------------ */
#define GSP_FORGE_OP_CONTROL        1u  /* GSP_RM_CONTROL  (wire func 76)     */
#define GSP_FORGE_OP_ALLOC          2u  /* GSP_RM_ALLOC    (wire func 103)    */
#define GSP_FORGE_OP_ENUM_HANDLES   3u  /* dump RM client/resource database   */
#define GSP_FORGE_OP_FREE           4u  /* RESERVED: NV_RM_RPC_FREE           */
#define GSP_FORGE_OP_RAW            5u  /* RESERVED: raw func+blob to send    */
#define GSP_FORGE_OP_EVENT          6u  /* RESERVED: inject/drain event path  */

/* ------- request flags --------------------------------------------------- */
#define GSP_FORGE_F_DRYRUN   (1u << 0) /* build + log the RPC, do NOT send    */
#define GSP_FORGE_F_NOECHO   (1u << 1) /* do not copy GSP-updated params back */

/* ------- forge-layer result codes (in resp.forgeErrno; != NV_STATUS) ----- */
#define GSP_FORGE_E_OK        0u
#define GSP_FORGE_E_MAGIC     1u
#define GSP_FORGE_E_VERSION   2u
#define GSP_FORGE_E_SIZE      3u
#define GSP_FORGE_E_OP        4u
#define GSP_FORGE_E_NOGPU     5u
#define GSP_FORGE_E_NOMEM     6u
#define GSP_FORGE_E_DRYRUN    7u  /* informational: not armed, send suppressed */
#define GSP_FORGE_E_LOCK      8u
#define GSP_FORGE_E_NOTIMPL   9u

/* Bounds (kernel enforces). params blob for one submit. */
#define GSP_FORGE_MAX_PARAMS  (64u * 1024u)

typedef struct GspForgeReq
{
    NvU32 magic;        /* GSP_FORGE_REQ_MAGIC                               */
    NvU32 version;      /* GSP_FORGE_ABI_VERSION                             */
    NvU32 op;           /* GSP_FORGE_OP_*                                    */
    NvU32 flags;        /* GSP_FORGE_F_*                                     */
    NvU32 gpuInstance;  /* 0 = first available GPU                           */
    NvU32 hClient;      /* control/alloc: owning client handle               */
    NvU32 hParent;      /* alloc: parent handle; control: unused             */
    NvU32 hObject;      /* control: target object; alloc: NEW object handle  */
    NvU32 cmd;          /* control: 2080/0080/... cmd; alloc: hClass         */
    NvU32 paramsSize;   /* bytes of params[] that follow this header         */
    NvU32 timeoutMs;    /* reserved (self-locking send path ignores for now) */
    NvU32 reserved[5];  /* future fields, zeroed                             */
    /* NvU8 params[paramsSize] follows immediately */
} GspForgeReq;

typedef struct GspForgeResp
{
    NvU32 magic;          /* GSP_FORGE_RESP_MAGIC                            */
    NvU32 version;        /* GSP_FORGE_ABI_VERSION                           */
    NvU32 op;             /* echoed op                                       */
    NvU32 flagsEffective; /* flags actually applied (DRYRUN may be forced)   */
    NvU32 status;         /* NV_STATUS from the RPC (0 == NV_OK)             */
    NvU32 forgeErrno;     /* GSP_FORGE_E_*                                   */
    NvU32 cmd;            /* echoed cmd/hClass                               */
    NvU32 paramsSize;     /* bytes of params[] that follow                   */
    NvU32 durationUs;     /* wall time spent in the RPC                      */
    NvU32 recordCount;    /* ENUM_HANDLES: number of records in params[]     */
    NvU32 reserved[6];
    /* NvU8 params[paramsSize] follows:
     *   CONTROL: the (possibly GSP-updated) in/out param struct
     *   ENUM_HANDLES: recordCount * GspForgeHandleRec                       */
} GspForgeResp;

/* ENUM_HANDLES record */
typedef struct GspForgeHandleRec
{
    NvU32 hClient;
    NvU32 hParent;
    NvU32 hObject;
    NvU32 externalClassId;
    NvU32 internalClassId;
    NvU32 pad;
} GspForgeHandleRec;

#endif /* GSP_FORGE_ABI_H */
