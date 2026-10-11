/*
 * gsp_forge_core.c  -  kernel/host-RM side of the GSP RPC forger.
 *
 * RESEARCH INSTRUMENT.  Compiled into nv-kernel (the OS-agnostic RM core) so it
 * can see the internal RM symbols the OS layer cannot:
 *
 *   - NV_RM_RPC_CONTROL / NV_RM_RPC_ALLOC_OBJECT : on a GSP-offload (FW client)
 *     GPU these route through GPU_GET_PHYSICAL_RMAPI(pGpu)->Control/AllocWithHandle
 *     == rpcRmApiControl_GSP / rpcRmApiAlloc_GSP, i.e. straight onto the wire to
 *     the GSP, using the PHYSICAL RMAPI's defaultSecInfo (kernel/root privilege).
 *     That bypasses the CPU-RM ResServ permission gate that returns
 *     NV_ERR_INSUFFICIENT_PERMISSIONS (0x1b) to a userspace client.
 *
 *   - the ResServ client/resource database : walked read-only to discover the
 *     objects the running system already allocated, so forged controls can be
 *     aimed at live handles of any class without re-allocating them.
 *
 * rpcRmApiControl_GSP self-acquires the GPU group lock if the caller does not
 * hold it (SAFE_LOCK_UPGRADE), so the only lock this layer must take is the
 * API lock, in the standard API->GPU order.
 *
 * The single entry point nv_gsp_forge_core_submit() takes opaque byte buffers
 * (no NVOC types in its signature) so the Linux OS-layer glue can call it
 * across the build boundary.  All ABI parsing and policy lives here.
 *
 *  !!  Signature MUST stay in sync with the extern in nvidia-gsp-forge.c.  !!
 */

#include "gpu/gsp/gsp_forge_abi.h"

#include "nvstatus.h"
#include "os/os.h"
#include "kernel/core/locks.h"
#include "gpu/gpu.h"
#include "gpu_mgr/gpu_mgr.h"
#include "rmapi/rmapi.h"
#include "rmapi/rs_utils.h"
#include "resserv/rs_client.h"
#include "vgpu/rpc.h"
#include "nvport/nvport.h"
#include "utils/nvprintf.h"

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

static OBJGPU *_forgeGetGpu(NvU32 gpuInstance)
{
    OBJGPU *pGpu;

    //
    // v1: single discrete target.  gpuInstance is accepted in the ABI and
    // reserved for multi-GPU; for now any non-zero value still resolves to the
    // same "some GPU" so old tools and single-board benches behave identically.
    //
    (void)gpuInstance;
    pGpu = gpumgrGetSomeGpu();
    return pGpu;
}

//
// ENUM_HANDLES: walk every client and every resource under it, emitting one
// GspForgeHandleRec per resource.  Read-only; no locks are taken against the
// resource server here beyond the fact that the forge path is single-threaded
// and serialized by the OS-layer mutex.  Returns record count via *pCount and
// bytes written via *pBytes (clamped to capacity).
//
static NvU32 _forgeEnumHandles(NvU8 *pOut, NvU32 outCap, NvU32 *pCount, NvU32 *pBytes)
{
    RmClient **ppClient;
    NvU32      recSize = (NvU32)sizeof(GspForgeHandleRec);
    NvU32      count   = 0;
    NvU32      bytes   = 0;

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient   *pClient   = *ppClient;
        RsClient   *pRsClient = staticCast(pClient, RsClient);
        RS_ITERATOR it;

        if (pRsClient == NULL)
            continue;

        // internalClassId 0 + bExactMatch NV_FALSE + DESCENDANTS => every resource
        it = clientRefIter(pRsClient, NULL, 0, RS_ITERATE_DESCENDANTS, NV_FALSE);

        while (clientRefIterNext(pRsClient, &it))
        {
            RsResourceRef    *pRef = it.pResourceRef;
            GspForgeHandleRec rec;

            if (pRef == NULL)
                continue;

            rec.hClient         = pRsClient->hClient;
            rec.hParent         = (pRef->pParentRef != NULL) ? pRef->pParentRef->hResource : 0;
            rec.hObject         = pRef->hResource;
            rec.externalClassId = pRef->externalClassId;
            rec.internalClassId = pRef->internalClassId;
            rec.pad             = 0;

            if ((bytes + recSize) <= outCap)
            {
                portMemCopy(pOut + bytes, outCap - bytes, &rec, recSize);
                bytes += recSize;
            }
            count++;   // keep counting even once truncated, so the tool can retry bigger
        }
    }

    *pCount = count;
    *pBytes = bytes;
    return GSP_FORGE_E_OK;
}

/* ------------------------------------------------------------------------ */
/* entry point                                                               */
/* ------------------------------------------------------------------------ */

//
// Returns a forge-layer errno (GSP_FORGE_E_*); the NV_STATUS from the RPC (if
// any) is reported inside the response header.  *pRespLen is set to the total
// bytes written into pResp (header + params).
//
NvU32 nv_gsp_forge_core_submit(void *pReqBuf, NvU32 reqLen,
                               void *pRespBuf, NvU32 respCap,
                               NvU32 *pRespLen, NvU32 armed)
{
    GspForgeReq  *pReq;
    GspForgeResp *pResp;
    NvU8         *pReqParams;
    NvU8         *pRespParams;
    NvU32         respParamCap;
    NvU32         forgeErrno = GSP_FORGE_E_OK;
    NvU32         effFlags;
    NvBool        bDryRun;
    OBJGPU       *pGpu;
    NV_STATUS     status = NV_OK;

    if (pRespLen != NULL)
        *pRespLen = 0;

    if (pReqBuf == NULL || pRespBuf == NULL || respCap < (NvU32)sizeof(GspForgeResp))
        return GSP_FORGE_E_SIZE;

    pResp        = (GspForgeResp *)pRespBuf;
    pRespParams  = (NvU8 *)pRespBuf + sizeof(GspForgeResp);
    respParamCap = respCap - (NvU32)sizeof(GspForgeResp);

    portMemSet(pResp, 0, sizeof(*pResp));
    pResp->magic   = GSP_FORGE_RESP_MAGIC;
    pResp->version = GSP_FORGE_ABI_VERSION;

    if (reqLen < (NvU32)sizeof(GspForgeReq))
        return GSP_FORGE_E_SIZE;

    pReq       = (GspForgeReq *)pReqBuf;
    pReqParams = (NvU8 *)pReqBuf + sizeof(GspForgeReq);

    if (pReq->magic != GSP_FORGE_REQ_MAGIC)
        return GSP_FORGE_E_MAGIC;
    if (pReq->version != GSP_FORGE_ABI_VERSION)
        return GSP_FORGE_E_VERSION;
    if (pReq->paramsSize > GSP_FORGE_MAX_PARAMS)
        return GSP_FORGE_E_SIZE;
    if (((NvU64)sizeof(GspForgeReq) + pReq->paramsSize) > reqLen)
        return GSP_FORGE_E_SIZE;

    pResp->op  = pReq->op;
    pResp->cmd = pReq->cmd;

    // ---- the ARMED gate -------------------------------------------------
    // Unless the box is explicitly armed, every send is downgraded to a dry
    // run: the RPC is built and logged but never put on the wire.  An explicit
    // DRYRUN flag is always honoured regardless of arm state.
    effFlags = pReq->flags;
    if (!armed)
        effFlags |= GSP_FORGE_F_DRYRUN;
    bDryRun = (effFlags & GSP_FORGE_F_DRYRUN) ? NV_TRUE : NV_FALSE;
    pResp->flagsEffective = effFlags;

    // ---- ENUM_HANDLES has no GPU/RPC dependency -------------------------
    if (pReq->op == GSP_FORGE_OP_ENUM_HANDLES)
    {
        NvU32 count = 0, bytes = 0;
        forgeErrno = _forgeEnumHandles(pRespParams, respParamCap, &count, &bytes);
        pResp->recordCount = count;
        pResp->paramsSize  = bytes;
        pResp->status      = NV_OK;
        pResp->forgeErrno  = forgeErrno;
        if (pRespLen != NULL)
            *pRespLen = (NvU32)sizeof(GspForgeResp) + bytes;
        NV_PRINTF(LEVEL_NOTICE,
                  "GSPFORGE ENUM: %u resources (%u bytes, cap %u)\n",
                  count, bytes, respParamCap);
        return forgeErrno;
    }

    pGpu = _forgeGetGpu(pReq->gpuInstance);
    if (pGpu == NULL)
    {
        pResp->forgeErrno = GSP_FORGE_E_NOGPU;
        pResp->status     = NV_ERR_INVALID_STATE;
        if (pRespLen != NULL)
            *pRespLen = (NvU32)sizeof(GspForgeResp);
        return GSP_FORGE_E_NOGPU;
    }

    switch (pReq->op)
    {
        case GSP_FORGE_OP_CONTROL:
        case GSP_FORGE_OP_ALLOC:
        {
            NvU32  ps     = pReq->paramsSize;
            void  *pForge = portMemAllocNonPaged(ps ? ps : 1);

            if (pForge == NULL)
            {
                pResp->forgeErrno = GSP_FORGE_E_NOMEM;
                pResp->status     = NV_ERR_NO_MEMORY;
                if (pRespLen != NULL)
                    *pRespLen = (NvU32)sizeof(GspForgeResp);
                return GSP_FORGE_E_NOMEM;
            }
            if (ps != 0)
                portMemCopy(pForge, ps, pReqParams, ps);

            NV_PRINTF(LEVEL_NOTICE,
                      "GSPFORGE %s%s hClient=0x%08x hParent=0x%08x hObject=0x%08x "
                      "%s=0x%08x psize=%u\n",
                      (pReq->op == GSP_FORGE_OP_CONTROL) ? "CONTROL" : "ALLOC",
                      bDryRun ? " [DRYRUN]" : "",
                      pReq->hClient, pReq->hParent, pReq->hObject,
                      (pReq->op == GSP_FORGE_OP_CONTROL) ? "cmd" : "hClass",
                      pReq->cmd, ps);

            if (!bDryRun)
            {
                NV_STATUS lockStatus = rmapiLockAcquire(RMAPI_LOCK_FLAGS_NONE,
                                                        RM_LOCK_MODULES_RPC);
                if (lockStatus != NV_OK)
                {
                    forgeErrno = GSP_FORGE_E_LOCK;
                    status     = lockStatus;
                }
                else
                {
                    status = NV_OK;
                    if (pReq->op == GSP_FORGE_OP_CONTROL)
                    {
                        NV_RM_RPC_CONTROL(pGpu, pReq->hClient, pReq->hObject,
                                          pReq->cmd, pForge, ps, status);
                    }
                    else /* GSP_FORGE_OP_ALLOC: hParent=parent, hObject=new, cmd=hClass */
                    {
                        NV_RM_RPC_ALLOC_OBJECT(pGpu, pReq->hClient, pReq->hParent,
                                               pReq->hObject, pReq->cmd,
                                               pForge, ps, status);
                    }
                    rmapiLockRelease();
                }
            }
            else
            {
                forgeErrno = armed ? GSP_FORGE_E_OK : GSP_FORGE_E_DRYRUN;
                status     = NV_OK;
            }

            // echo the (possibly GSP-updated) param buffer back
            if (!(effFlags & GSP_FORGE_F_NOECHO) && ps != 0)
            {
                NvU32 n = (ps <= respParamCap) ? ps : respParamCap;
                portMemCopy(pRespParams, respParamCap, pForge, n);
                pResp->paramsSize = n;
            }
            else
            {
                pResp->paramsSize = 0;
            }

            portMemFree(pForge);
            break;
        }

        case GSP_FORGE_OP_FREE:
        case GSP_FORGE_OP_RAW:
        case GSP_FORGE_OP_EVENT:
            // Reserved extension points.  The ABI already carries these ops so
            // a later build can implement them without any userspace change.
            forgeErrno        = GSP_FORGE_E_NOTIMPL;
            status            = NV_ERR_NOT_SUPPORTED;
            pResp->paramsSize = 0;
            break;

        default:
            forgeErrno        = GSP_FORGE_E_OP;
            status            = NV_ERR_NOT_SUPPORTED;
            pResp->paramsSize = 0;
            break;
    }

    pResp->status     = (NvU32)status;
    pResp->forgeErrno = forgeErrno;
    if (pRespLen != NULL)
        *pRespLen = (NvU32)sizeof(GspForgeResp) + pResp->paramsSize;

    NV_PRINTF(LEVEL_NOTICE,
              "GSPFORGE done op=%u status=0x%x forgeErrno=%u echo=%u\n",
              pReq->op, (NvU32)status, forgeErrno, pResp->paramsSize);

    return forgeErrno;
}
