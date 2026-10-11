/*
 * nvidia-gsp-forge.c  -  Linux OS-layer debugfs interface for the GSP RPC forger.
 *
 * RESEARCH INSTRUMENT.  Exposes:
 *
 *   /sys/kernel/debug/gsp_forge/armed   (rw, u8)  0 = every send downgraded to
 *                                                 a dry run; 1 = sends go on the
 *                                                 wire.  Default 0.
 *   /sys/kernel/debug/gsp_forge/rpc     (rw)      write() submits one binary
 *                                                 GspForgeReq (+params); read()
 *                                                 returns the last GspForgeResp
 *                                                 (+params) from offset 0.
 *   /sys/kernel/debug/gsp_forge/info    (r)       text banner / usage.
 *
 * This file knows NOTHING about the forge ABI structs: it copies opaque bytes
 * to/from userspace and hands them to the RM core, which validates the magic,
 * version, sizes and op.  That keeps the OS/core build boundary clean (the ABI
 * header lives only on the RM-core include path).
 *
 *  !!  The extern below MUST match nv_gsp_forge_core_submit() in              !!
 *  !!  src/nvidia/src/kernel/gpu/gsp/gsp_forge_core.c exactly.                !!
 */

#include "nv-linux.h"
#include "nv-gsp-forge.h"

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/* Generous OS-side bounds; the RM core enforces the real per-op limits. */
#define FORGE_MAX_REQ   (64 * 1024 + 512)
#define FORGE_MAX_RESP  (64 * 1024 + 512)

/* Matches NvU32 nv_gsp_forge_core_submit(void*,NvU32,void*,NvU32,NvU32*,NvU32) */
extern NvU32 nv_gsp_forge_core_submit(void *pReqBuf, NvU32 reqLen,
                                      void *pRespBuf, NvU32 respCap,
                                      NvU32 *pRespLen, NvU32 armed);

static struct dentry *gsp_forge_dir;
static DEFINE_MUTEX(gsp_forge_lock);
static u8   gsp_forge_armed;           /* exported via debugfs_create_u8 */
static u8  *gsp_forge_resp;            /* last response (header + params) */
static u32  gsp_forge_resp_len;

static const char gsp_forge_info[] =
    "nvidia GSP RPC forger (research instrument)\n"
    "  write a binary GspForgeReq(+params) to 'rpc', then read 'rpc' for the\n"
    "  GspForgeResp(+params).  'armed' must be 1 for sends to reach the GSP;\n"
    "  while 0 every submit is logged and dry-run only.  Single-user.\n";

/* -------- rpc file ------------------------------------------------------- */

static ssize_t gsp_forge_rpc_write(struct file *f, const char __user *ubuf,
                                   size_t count, loff_t *ppos)
{
    void *reqbuf;
    NvU32 resp_len = 0;
    int   ret = (int)count;

    if (count < 1 || count > FORGE_MAX_REQ)
        return -EINVAL;

    reqbuf = kmalloc(count, GFP_KERNEL);
    if (reqbuf == NULL)
        return -ENOMEM;

    if (copy_from_user(reqbuf, ubuf, count))
    {
        kfree(reqbuf);
        return -EFAULT;
    }

    mutex_lock(&gsp_forge_lock);

    if (gsp_forge_resp == NULL)
    {
        mutex_unlock(&gsp_forge_lock);
        kfree(reqbuf);
        return -ENODEV;
    }

    (void) nv_gsp_forge_core_submit(reqbuf, (NvU32)count,
                                    gsp_forge_resp, FORGE_MAX_RESP,
                                    &resp_len, (NvU32)gsp_forge_armed);

    gsp_forge_resp_len = (resp_len <= FORGE_MAX_RESP) ? resp_len : FORGE_MAX_RESP;

    mutex_unlock(&gsp_forge_lock);
    kfree(reqbuf);

    /* A write consumes the whole request. */
    return ret;
}

static ssize_t gsp_forge_rpc_read(struct file *f, char __user *ubuf,
                                  size_t count, loff_t *ppos)
{
    ssize_t n;

    mutex_lock(&gsp_forge_lock);
    n = simple_read_from_buffer(ubuf, count, ppos,
                                gsp_forge_resp, gsp_forge_resp_len);
    mutex_unlock(&gsp_forge_lock);
    return n;
}

static const struct file_operations gsp_forge_rpc_fops = {
    .owner = THIS_MODULE,
    .open  = simple_open,
    .write = gsp_forge_rpc_write,
    .read  = gsp_forge_rpc_read,
    .llseek = default_llseek,
};

/* -------- info file ------------------------------------------------------ */

static ssize_t gsp_forge_info_read(struct file *f, char __user *ubuf,
                                   size_t count, loff_t *ppos)
{
    return simple_read_from_buffer(ubuf, count, ppos,
                                   gsp_forge_info, sizeof(gsp_forge_info) - 1);
}

static const struct file_operations gsp_forge_info_fops = {
    .owner = THIS_MODULE,
    .open  = simple_open,
    .read  = gsp_forge_info_read,
    .llseek = default_llseek,
};

/* -------- init / exit ---------------------------------------------------- */

void nv_gsp_forge_init(void)
{
    gsp_forge_resp = kzalloc(FORGE_MAX_RESP, GFP_KERNEL);
    if (gsp_forge_resp == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: gsp_forge: no memory for response buffer\n");
        return;
    }
    gsp_forge_resp_len = 0;
    gsp_forge_armed    = 0;

    gsp_forge_dir = debugfs_create_dir("gsp_forge", NULL);
    if (IS_ERR_OR_NULL(gsp_forge_dir))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: gsp_forge: debugfs unavailable; interface disabled\n");
        gsp_forge_dir = NULL;
        kfree(gsp_forge_resp);
        gsp_forge_resp = NULL;
        return;
    }

    debugfs_create_u8("armed", 0600, gsp_forge_dir, &gsp_forge_armed);
    debugfs_create_file("rpc",  0600, gsp_forge_dir, NULL, &gsp_forge_rpc_fops);
    debugfs_create_file("info", 0400, gsp_forge_dir, NULL, &gsp_forge_info_fops);

    nv_printf(NV_DBG_ERRORS, "NVRM: gsp_forge: debugfs interface ready at /sys/kernel/debug/gsp_forge\n");
}

void nv_gsp_forge_exit(void)
{
    if (gsp_forge_dir != NULL)
    {
        debugfs_remove_recursive(gsp_forge_dir);
        gsp_forge_dir = NULL;
    }
    if (gsp_forge_resp != NULL)
    {
        kfree(gsp_forge_resp);
        gsp_forge_resp = NULL;
    }
    gsp_forge_resp_len = 0;
}
