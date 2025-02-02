/*
* Copyright (c) 2015-2021 Cadence Design Systems Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include <stdbool.h>
#include "library_load.h"
#include "xaf-api.h"
#include "xaf-version.h"
#include "xaf-threads-priority.h"
#include "dsp_codec_interface.h"

#define MODULE_TAG                      DEVAPI

/***** GLOBALS *****/
/* Info table */

#define TENA_2356   1

#define XAF_4BYTE_ALIGN    4
#define XAF_8BYTE_ALIGN    8
#define XAF_32BYTE_ALIGN    32
#define XAF_DEV_AND_AP_STRUCT_MEM_SIZE \
   (sizeof(xf_ap_t) + (XAF_8BYTE_ALIGN-1) + \
   (sizeof(xaf_adev_t) + (XAF_4BYTE_ALIGN-1)))

/* ...check null pointer */ 
#define XAF_CHK_PTR(ptr)                                     \
({                                                          \
    int __ret;                                              \
                                                            \
    if ((__ret = (long int)(ptr)) == 0)                          \
    {                                                       \
        TRACE(ERROR, _x("Null pointer error: %d"), __ret);  \
        return XAF_INVALIDPTR_ERR;                               \
    }                                                       \
    __ret;                                                  \
})

/* ...check range */
#define XAF_CHK_RANGE(val, min, max)                         \
({                                                          \
    int __ret = val;                                        \
                                                            \
    if ((__ret < (int)min) || (__ret > (int)max))           \
    {                                                       \
        TRACE(ERROR, _x("Invalid value: %d"), __ret);       \
        return XAF_INVALIDVAL_ERR;                           \
    }                                                       \
    __ret;                                                  \
})

/* ...check alignment */
#define XAF_CHK_ALIGN(val, bytes)                                                                 \
({                                                                                                \
    int __ret = (int) val;                                                                        \
                                                                                                  \
    if ((__ret) & (bytes - 1))                                                                    \
    {                                                                                             \
        TRACE(ERROR, _x("Invalid alignment: %d. Required alignment: %d bytes"), __ret, bytes);    \
        return XAF_INVALIDVAL_ERR;                                                                 \
    }                                                                                             \
    __ret;                                                                                        \
})

#define XAF_ADEV_STATE_CHK(ptr, adev_st)                            \
({                                                                  \
    XAF_CHK_RANGE(adev_st, XAF_ADEV_RESET, XAF_ADEV_INIT);          \
    if ((ptr)->adev_state == adev_st)                               \
    {                                                               \
        TRACE(ERROR, _x("Device API Error"));                       \
        return XAF_API_ERR;                                         \
    };                                                              \
})

#define XAF_COMP_STATE_CHK(ptr)                                 \
({                                                              \
    if ((ptr)->comp_state != XAF_COMP_CREATE)                   \
    {                                                           \
        TRACE(ERROR, _x("Component API Error"));                \
        return XAF_API_ERR;                                     \
    };                                                          \
})

/* Minimum component memory requirement is based on: 
 * (common scratch bytes) + (internal housekeeping data-structure bytes) + (XF_CFG_MESSAGE_POOL_SIZE*(sizeof msg 64b-aligned)(256*64) bytes) 
 * as referred to in ProgrammersGuide */
#define XA_AUDIO_COMP_BUF_SIZE_MIN  (XF_CFG_CODEC_SCRATCHMEM_SIZE + 1024 + 256*64)
#define XA_AUDIO_COMP_BUF_SIZE_MAX ((1UL<<31)-1)

/* Minimum framework memory requirement is based on: 
 * ipc-buffer 16384 bytes
 * as referred to in ProgrammersGuide */
#define XA_AUDIO_FRMWK_BUF_SIZE_MIN (16384)
#define XA_AUDIO_FRMWK_BUF_SIZE_MAX ((1UL<<31)-1)

#ifndef XA_DISABLE_EVENT

#define XF_EVENT_BUFFER_ALIGNMENT 8
#define XF_COMP_ERROR_CHANNEL_BUF_SIZE 4
#define XF_EVENT_DST_CFG_PARAM_UNDEFINED 0xFFFFFFFF

typedef enum
{
    XF_EVENT_CHANNEL_STATE_ACTIVE     =  0,
    XF_EVENT_CHANNEL_STATE_INACTIVE   =  1
} xf_event_channel_state;

struct xf_app_event_channel
{ 
    xf_app_event_channel_t  *next;                                                                                   
    xf_pool_t               *pool;
    xaf_comp_t              *p_comp_src;
    xaf_comp_t              *p_comp_dst;
    UWORD32                 event_id_src;                                                                            
    UWORD32                 event_id_dst;                                                                            
    UWORD32                 pending_buffers; 
    xf_event_channel_state  channel_state; 
};

#endif /* XA_DISABLE_EVENT */

/* ****************************************************
 * Generic node chain operations
 * ***************************************************/ 
static int xaf_sync_chain_init(xaf_node_chain_t *chain, UWORD32 next_offset)
{
    chain->head = NULL;
    __xf_lock_init(&chain->lock);
    chain->next_offset = next_offset; 

    return 0;
}

static int xaf_sync_chain_deinit(xaf_node_chain_t *chain)
{
    chain->head = NULL;
    __xf_lock_destroy(&chain->lock);

    return 0;
}

static int xaf_sync_chain_add_node(xaf_node_chain_t *chain, void *node)
{
    __xf_lock(&chain->lock);

    *(xaf_node_chain_t **)((long)node + chain->next_offset) = chain->head;
    chain->head = (xaf_node_chain_t *)node;

    __xf_unlock(&chain->lock);

    return 0;
}

static int xaf_sync_chain_delete_node(xaf_node_chain_t *chain, void *node)
{
    xaf_node_chain_t *p_curr_node = NULL;
    xaf_node_chain_t **pp_curr_node;
    UWORD32 next_offset = chain->next_offset;

    __xf_lock(&chain->lock);

    pp_curr_node = &chain->head;

    do 
    {
        p_curr_node = *pp_curr_node;

        if (!p_curr_node) break;

        if (p_curr_node == node) break;
        
        pp_curr_node = (xaf_node_chain_t **)((unsigned long)p_curr_node + next_offset);

    } while(1);

    if (p_curr_node != node)
    {
        __xf_unlock(&chain->lock);
        return -1;
    }   

    *pp_curr_node = *(xaf_node_chain_t **)((unsigned long)p_curr_node + next_offset);

    __xf_unlock(&chain->lock);

    return 0;
}

/* ****************************************************
 * component chain specific node chain operations
 * ***************************************************/ 
static inline void *xaf_sync_chain_get_head(xaf_node_chain_t *chain)
{
    void *p_node;

    __xf_lock(&chain->lock);
    p_node = chain->head;
    __xf_unlock(&chain->lock);

    return p_node;
}

#ifndef XA_DISABLE_EVENT
/* ****************************************************
 * Event channel specific node chain operations
 * ***************************************************/ 
static int xaf_sync_chain_set_node_state(xaf_node_chain_t *chain, xf_app_event_channel_t *node, xf_event_channel_state state)
{
    __xf_lock(&chain->lock);

    node->channel_state = state;

    __xf_unlock(&chain->lock);

    return 0;
}

static xf_event_channel_state xaf_sync_chain_get_node_state(xaf_node_chain_t *chain, xf_app_event_channel_t *node)
{
    xf_event_channel_state state;

    __xf_lock(&chain->lock);

    state = node->channel_state;

    __xf_unlock(&chain->lock);

    return state;
}

static xf_app_event_channel_t * xaf_sync_chain_find_node_by_comp(xaf_node_chain_t *chain, pVOID p_comp)
{
    xf_app_event_channel_t *curr_node = NULL;

    __xf_lock(&chain->lock);

    curr_node = (xf_app_event_channel_t *)chain->head;

    do 
    {
        if (!curr_node) break;

        if ((curr_node->p_comp_src == p_comp) || (curr_node->p_comp_dst == p_comp)) break;
        
        curr_node = curr_node->next;
    } while(1);

    __xf_unlock(&chain->lock);

    return curr_node;
}

static xf_app_event_channel_t * xaf_sync_chain_find_node_by_param(xaf_node_chain_t *chain, pVOID p_src, UWORD32 id_src, UWORD32 id_dst)
{
    xf_app_event_channel_t *curr_node = NULL;

    __xf_lock(&chain->lock);

    curr_node = (xf_app_event_channel_t *)chain->head;

    do 
    {
        if (!curr_node) break;

        if (id_dst == XF_EVENT_DST_CFG_PARAM_UNDEFINED)
        {
            if ((curr_node->p_comp_src == p_src) && (curr_node->event_id_src == id_src)) break;
        }
        else
        {
            if ((curr_node->p_comp_src == p_src) && (curr_node->event_id_src == id_src) && (curr_node->event_id_dst == id_dst)) break;
        }
        
        curr_node = curr_node->next;
    } while(1);

    __xf_unlock(&chain->lock);

    return curr_node;
}

/* ...receive the event from proxy and relay it to the application with matching component handle */
static XAF_ERR_CODE xaf_event_relay(xa_app_submit_event_cb_t *cdata, UWORD32 comp_id, UWORD32 event_id, void *event_buf, UWORD32 buf_size)
{
    xaf_adev_t *p_adev = container_of(cdata, xaf_adev_t, cdata);
    xf_ap_t    *xf_g_ap;
    xaf_comp_t *p_comp = NULL;
    UWORD32 i, ncomp;
    ncomp = p_adev->n_comp;
    xf_g_ap = p_adev->xf_g_ap;

    __xf_lock(&p_adev->comp_chain.lock);
    if ((ncomp > 0) && (p_adev->comp_chain.head != NULL))
    {
        p_comp = (xaf_comp_t *)p_adev->comp_chain.head;
        for(i = 0; i < ncomp; i++)
        {
            if((p_comp->handle.id == comp_id) || (NULL == p_comp))
                break;

            p_comp = p_comp->next;
        }

        if(i == ncomp) 
            p_comp = NULL;
    }
    __xf_unlock(&p_adev->comp_chain.lock);

    if (p_comp)
    {
        xf_app_event_channel_t   *p_channel_curr = NULL; 
        
        p_channel_curr = xaf_sync_chain_find_node_by_param(&p_adev->event_chain, p_comp, event_id, XF_EVENT_DST_CFG_PARAM_UNDEFINED); 

        XF_CHK_ERR((p_channel_curr != NULL), XAF_INVALIDPTR_ERR);
        
        p_channel_curr->pending_buffers--;

        if ((xaf_sync_chain_get_node_state(&p_adev->event_chain, p_channel_curr)) == XF_EVENT_CHANNEL_STATE_ACTIVE)
        {
            /* ...submit the event to application via callback */
            if (xf_g_ap->app_event_handler_cb)
            {
                xf_g_ap->app_event_handler_cb((void *)p_comp, event_id, (void *)((event_buf + sizeof(event_id))), (buf_size - sizeof(event_id)), ((event_id == XF_CFG_COMP_ERR_FATAL) || (event_id == XF_CFG_COMP_ERR_ALL)));
            }

            /* ...resend buffer to DSP, but check again to ensure channel is still active(rare case) */
            if ((xaf_sync_chain_get_node_state(&p_adev->event_chain, p_channel_curr)) == XF_EVENT_CHANNEL_STATE_ACTIVE)
            {
                XF_CHK_API(xf_command(&p_comp->handle, 0, XF_EVENT, event_buf, buf_size));
                p_channel_curr->pending_buffers++;
            }
        }
    }
    else
    {
        TRACE(WARNING, _b("event-relay: Unknown event, ignored, event_id: %08x"), event_id);
    }

    return XAF_NO_ERR;
}
#endif /* XA_DISABLE_EVENT */

XAF_ERR_CODE xaf_malloc(xf_ap_t *xf_g_ap, void **buf_ptr, int size, int id)
{

    XAF_CHK_PTR(buf_ptr);

    *buf_ptr = xf_g_ap->xf_mem_malloc_fxn(xf_g_ap->g_mem_obj, size, id);
    XAF_CHK_PTR(*buf_ptr);
    memset(*buf_ptr, 0, size);

    if((unsigned long)*buf_ptr & (XAF_4BYTE_ALIGN - 1))
    {
       TRACE(INFO, _b("Memory allocation failed : %p is not 4-byte aligned"), *buf_ptr);
       return XAF_INVALIDPTR_ERR;
    }

    xf_g_ap->xaf_memory_used += size;

    return XAF_NO_ERR;
}

void xaf_free(xf_ap_t *xf_g_ap, void *buf, int id)
{
    xf_g_ap->xf_mem_free_fxn(xf_g_ap->g_mem_obj, buf, id);
}

XAF_ERR_CODE xaf_get_verinfo(pUWORD8 ver_info[3])
{
    static const xa_api_info_t xa_af_info_tbl = 
    {
        "Copyright (c) 2016 by Cadence Design Systems, Inc.  ALL RIGHTS RESERVED", /* Copyright string */
        LIBNAME " (Hostless)",            /* Library name */
        LIBVERSION,         /* Library version */
        LIB_APIVERSION      /* API version */
    };

    XAF_CHK_PTR(ver_info);

    ver_info[0] = (pUWORD8)xa_af_info_tbl.p_lib_name;
    ver_info[1] = (pUWORD8)xa_af_info_tbl.p_lib_ver;
    ver_info[2] = (pUWORD8)xa_af_info_tbl.p_api_ver;

    XAF_CHK_PTR(*ver_info[0]);
    XAF_CHK_PTR(*ver_info[1]);
    XAF_CHK_PTR(*ver_info[2]);

    return XAF_NO_ERR;
}

static void xaf_comp_response(xf_handle_t *h, xf_user_msg_t *msg)
{
    /* ...submit response to asynchronous delivery queue */
    xf_response_put(h, msg);
}

static XAF_ERR_CODE xaf_comp_post_init_config(xaf_adev_t *p_adev, xaf_comp_t *p_comp, void *p_msg)
{
    xf_proxy_t *p_proxy = &p_adev->proxy; 
    xf_start_msg_t *smsg = p_msg;
    UWORD32 i;

    p_comp->out_format.sample_rate   = smsg->sample_rate;
    p_comp->out_format.channels      = smsg->channels;
    p_comp->out_format.pcm_width     = smsg->pcm_width;

    TRACE(INFO, _b("Component[%x] Params: f=%d, c=%d, w=%d"), p_comp->handle.id, smsg->sample_rate, smsg->channels, smsg->pcm_width);
    for (i=0; i<p_comp->inp_ports; i++)
    {
        p_comp->out_format.input_length[i]  = smsg->input_length[i];
        TRACE(INFO, _b("Component[%x] Params: i[%d]=%d"), p_comp->handle.id, i, smsg->input_length[i]);
    }
    for (i=0; i<p_comp->out_ports; i++)
    {
        p_comp->out_format.output_length[i] = smsg->output_length[i];
        TRACE(INFO, _b("Component[%x] Params: o[%d]=%d"), p_comp->handle.id, i, smsg->output_length[i]);
    }

    if (p_comp->noutbuf)
    { 
        XF_CHK_API(xf_pool_alloc(p_proxy, p_comp->noutbuf, smsg->output_length[0], XF_POOL_OUTPUT, &p_comp->outpool, XAF_MEM_ID_COMP)); 
#if 1 //DISCONNECT
        for (i=0; i<p_comp->noutbuf; i++)
        {
            xf_buffer_t *p_buf;
            p_buf = xf_buffer_get(p_comp->outpool);
            p_comp->pout_buf[i] = xf_buffer_data(p_buf);
        }
#endif
    }

    if (p_comp->probe_enabled)
    {
        xf_buffer_t *p_buf;

        p_comp->probe_length = smsg->probe_length;
        XF_CHK_API(xf_pool_alloc(p_proxy, 1, smsg->probe_length, XF_POOL_OUTPUT, &p_comp->probepool, XAF_MEM_ID_COMP));

        p_buf = xf_buffer_get(p_comp->probepool);
        p_comp->probe_buf = xf_buffer_data(p_buf);
    }

    p_comp->init_done   = 1;
    p_comp->comp_status = XAF_INIT_DONE;

    return XAF_NO_ERR;
}


XAF_ERR_CODE xaf_adev_config_default_init(xaf_adev_config_t *padev_config)
{
    XAF_CHK_PTR(padev_config);
    UWORD32 i;

    memset(padev_config, 0, sizeof(xaf_adev_config_t));

    /* ...initialize adev default config params */
    padev_config->audio_component_buffer_size = 512*1024;

    padev_config->audio_framework_buffer_size = 256*1024;

    padev_config->proxy_thread_priority = XAF_PROXY_THREAD_PRIORITY;

    padev_config->dsp_thread_priority = XAF_DSP_THREAD_PRIORITY;

    for(i=0; i<XAF_MAX_WORKER_THREADS; i++)
    {
	    padev_config->worker_thread_scratch_size[i] = XF_CFG_CODEC_SCRATCHMEM_SIZE;
    }

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_adev_open(pVOID *pp_adev, xaf_adev_config_t *pconfig)
{
    int ret, size;
    xaf_adev_t *p_adev;
    void * pTmp;
    xf_proxy_t *p_proxy; 
    xf_ap_t    *xf_g_ap = NULL;

    XAF_CHK_PTR(pp_adev);
    XF_CHK_ERR((xf_g_ap == NULL), XAF_INVALIDPTR_ERR);

    UWORD32 dsp_thread_priority;
    UWORD32 proxy_thread_priority;
    UWORD32 audio_frmwk_buf_size, audio_comp_buf_size;

    xaf_mem_malloc_fxn_t *mem_malloc;
    xaf_mem_free_fxn_t *mem_free;
    mem_obj_t  *g_mem_obj;

    XAF_CHK_PTR(pconfig);
    audio_frmwk_buf_size = pconfig->audio_framework_buffer_size;
    audio_comp_buf_size = pconfig->audio_component_buffer_size;
    mem_malloc = pconfig->pmem_malloc;
    mem_free = pconfig->pmem_free;
    g_mem_obj = &pconfig->g_mem_obj;
    dsp_thread_priority = pconfig->dsp_thread_priority;
    proxy_thread_priority = pconfig->proxy_thread_priority;

#ifndef XA_DISABLE_EVENT
    xaf_app_event_handler_fxn_t app_event_handler_cb = pconfig->app_event_handler_cb;
#endif

    XAF_CHK_PTR(mem_malloc);
    XAF_CHK_PTR(mem_free);

#if 1 //TENA_2352, TENA_2191
    XAF_CHK_RANGE(audio_comp_buf_size, XA_AUDIO_COMP_BUF_SIZE_MIN, XA_AUDIO_COMP_BUF_SIZE_MAX);
    XAF_CHK_ALIGN(audio_comp_buf_size, XAF_32BYTE_ALIGN);
#endif

#if 1 //TENA_2351, TENA_2193
    XAF_CHK_RANGE(audio_frmwk_buf_size, XA_AUDIO_FRMWK_BUF_SIZE_MIN, XA_AUDIO_FRMWK_BUF_SIZE_MAX);
    XAF_CHK_ALIGN(audio_frmwk_buf_size, XAF_32BYTE_ALIGN);
#endif

    /* ...Thumb rule: DSP-thread priority should be less than proxy-thread priority */
#if defined(HAVE_XOS)
    XAF_CHK_RANGE(proxy_thread_priority, 1, (XOS_NUM_PRIORITY-1));
#endif
#if defined(HAVE_FREERTOS)
    XAF_CHK_RANGE(proxy_thread_priority, 1, configMAX_PRIORITIES);
#endif
    XAF_CHK_RANGE(dsp_thread_priority, 0, (proxy_thread_priority-1));

    //Memory allocation for adev struct pointer
    size = (sizeof(xaf_adev_t) +(XAF_4BYTE_ALIGN-1));
    pTmp = mem_malloc(g_mem_obj, size, XAF_MEM_ID_DEV);
    XAF_CHK_PTR(pTmp);
    memset(pTmp, 0, size);

    p_adev = (xaf_adev_t *) (((unsigned long)pTmp + (XAF_4BYTE_ALIGN-1))& ~(XAF_4BYTE_ALIGN-1));
    p_adev->adev_ptr = pTmp;
    *pp_adev = (void *)p_adev;
    
    p_adev->adev_state = XAF_ADEV_INIT;

    // App Interface Layer memory allocation (BSS)
    size = sizeof(xf_ap_t)+(XAF_8BYTE_ALIGN-1);
    p_adev->p_apMem = mem_malloc(g_mem_obj, size, XAF_MEM_ID_DEV);
    XAF_CHK_PTR(p_adev->p_apMem);
    memset(p_adev->p_apMem, 0, size);


    xf_g_ap = (xf_ap_t *) (((unsigned long)p_adev->p_apMem + (XAF_8BYTE_ALIGN-1))& ~(XAF_8BYTE_ALIGN-1));

    p_adev->xf_g_ap = xf_g_ap;

    xf_g_ap->xf_mem_malloc_fxn = mem_malloc;
    xf_g_ap->xf_mem_free_fxn = mem_free;
    xf_g_ap->g_mem_obj = g_mem_obj;

#ifndef XA_DISABLE_EVENT
    xf_g_ap->cdata = &p_adev->cdata;
    xf_g_ap->app_event_handler_cb = app_event_handler_cb;

    p_adev->cdata.cb = xaf_event_relay;
#endif

#if 0 /* by S.J*/
    // DSP Interface Layer memory allocation (BSS)
    size = sizeof(xf_dsp_t)+(XAF_8BYTE_ALIGN-1);
    ret = xaf_malloc(&(p_adev->p_dspMem), size, XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;

    xf_g_dsp = (xf_dsp_t *) (((UWORD64)p_adev->p_dspMem + (XAF_8BYTE_ALIGN-1)) & ~(XAF_8BYTE_ALIGN-1));


    size = audio_frmwk_buf_size + (XAF_32BYTE_ALIGN-1); 
    ret = xaf_malloc(&(p_adev->p_apSharedMem), size, XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;
    xf_g_dsp->xf_ap_shmem_buffer = (UWORD8 *) (((UWORD64)p_adev->p_apSharedMem + (XAF_32BYTE_ALIGN-1)) & ~(XAF_32BYTE_ALIGN-1));
    xf_g_dsp->xf_ap_shmem_buffer_size = audio_frmwk_buf_size;


    size = (audio_comp_buf_size*XF_CFG_CORES_NUM_DSP) + (XAF_32BYTE_ALIGN-1); 
    ret = xaf_malloc(&(p_adev->p_dspLocalBuff), size, XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;
    xf_g_dsp->xf_dsp_local_buffer = (UWORD8 *) (((UWORD64)p_adev->p_dspLocalBuff + (XAF_32BYTE_ALIGN-1)) & ~(XAF_32BYTE_ALIGN-1));
    xf_g_dsp->xf_dsp_local_buffer_size = audio_comp_buf_size*XF_CFG_CORES_NUM_DSP;
    

#if XF_CFG_CORES_NUM_DSP > 1
    ret = xaf_malloc(&(xf_g_dsp->xf_dsp_shmem_buffer), (audio_comp_buf_size), XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;    
    xf_g_dsp->xf_dsp_shmem_buffer_size = audio_comp_buf_size;
#endif    // #if XF_CFG_CORES_NUM_DSP > 1

    __xf_lock_init(&xf_g_ap->g_msgq_lock);

    xf_g_dsp->dsp_thread_args[XF_DSP_THREAD_ARG_IDX_WORKER_SCRATCH] = xf_g_dsp->worker_thread_scratch_size;
    {
		UWORD32 *p_worker_scratch, i;

        p_worker_scratch = (UWORD32 *)xf_g_dsp->dsp_thread_args[XF_DSP_THREAD_ARG_IDX_WORKER_SCRATCH];
        for(i=0; i<XAF_MAX_WORKER_THREADS; i++)
        {
		    p_worker_scratch[i] = pconfig->worker_thread_scratch_size[i];
        }
    }
    p_adev->dsp_thread_priority = dsp_thread_priority;
#if defined(HAVE_FREERTOS)
    ret = __xf_thread_create(&xf_g_ap->dsp_thread, dsp_thread_entry, (void *)xf_g_dsp->dsp_thread_args, "DSP-thread", NULL, STACK_SIZE, dsp_thread_priority);
#else
    ret = __xf_thread_create(&xf_g_ap->dsp_thread, dsp_thread_entry, (void *)xf_g_dsp->dsp_thread_args, "DSP-thread", xf_g_ap->dsp_stack, STACK_SIZE, dsp_thread_priority);
#endif
    if (ret != 0)
        return XAF_RTOS_ERR;
#else
    /* avoid set priority error */
    p_adev->dsp_thread_priority = dsp_thread_priority;
#endif

    p_proxy = &p_adev->proxy;

    p_proxy->proxy_thread_priority = proxy_thread_priority;

    /* ...open DSP proxy - specify "DSP#0" */
    XF_CHK_API(xf_proxy_init(p_proxy, 0));

    /* ...create auxiliary buffers pool for control commands */
    ret = xf_pool_alloc(p_proxy, XAF_AUX_POOL_SIZE, XAF_AUX_POOL_MSG_LENGTH, XF_POOL_AUX, &p_proxy->aux, XAF_MEM_ID_DEV);
    if (ret != 0) {
        xf_proxy_close(p_proxy);
	return ret;
    }

#if TENA_2356
    /* ...mutex for orderly comp deletion. */
    __xf_lock_init(&xf_g_ap->g_comp_delete_lock);
#endif

#ifndef XA_DISABLE_EVENT
    xaf_sync_chain_init(&p_adev->event_chain, (UWORD32)offset_of(xf_app_event_channel_t, next));
#endif
    xaf_sync_chain_init(&p_adev->comp_chain, (UWORD32)offset_of(xaf_comp_t, next));

    return XAF_NO_ERR;
}

#ifndef XA_DISABLE_DEPRECATED_API
XAF_ERR_CODE xaf_adev_open_deprecated(pVOID *pp_adev, WORD32 audio_frmwk_buf_size, WORD32 audio_comp_buf_size, xaf_mem_malloc_fxn_t mem_malloc, xaf_mem_free_fxn_t mem_free)
{
    int ret, size;
    xaf_adev_t *p_adev;
    void * pTmp;
    xf_proxy_t *p_proxy; 

    XAF_CHK_PTR(pp_adev);
    XF_CHK_ERR((xf_g_ap == NULL), XAF_INVALIDPTR_ERR);

    UWORD32 dsp_thread_priority = XAF_DSP_THREAD_PRIORITY;
    UWORD32 proxy_thread_priority = XAF_PROXY_THREAD_PRIORITY;

    XAF_CHK_PTR(mem_malloc);
    XAF_CHK_PTR(mem_free);

#if 1 //TENA_2352, TENA_2191
    XAF_CHK_RANGE(audio_comp_buf_size, XA_AUDIO_COMP_BUF_SIZE_MIN, XA_AUDIO_COMP_BUF_SIZE_MAX);
    XAF_CHK_ALIGN(audio_comp_buf_size, XAF_32BYTE_ALIGN);
#endif

#if 1 //TENA_2351, TENA_2193
    XAF_CHK_RANGE(audio_frmwk_buf_size, XA_AUDIO_FRMWK_BUF_SIZE_MIN, XA_AUDIO_FRMWK_BUF_SIZE_MAX);
    XAF_CHK_ALIGN(audio_frmwk_buf_size, XAF_32BYTE_ALIGN);
#endif

    /* ...Thumb rule: DSP-thread priority should be less than proxy-thread priority */
#if defined(HAVE_XOS)
    XAF_CHK_RANGE(proxy_thread_priority, 1, (XOS_NUM_PRIORITY-1));
#endif
#if defined(HAVE_FREERTOS)
    XAF_CHK_RANGE(proxy_thread_priority, 1, configMAX_PRIORITIES);
#endif
    XAF_CHK_RANGE(dsp_thread_priority, 0, (proxy_thread_priority-1));

    //Memory allocation for adev struct pointer
    size = (sizeof(xaf_adev_t) +(XAF_4BYTE_ALIGN-1));
    pTmp = mem_malloc(size, XAF_MEM_ID_DEV);
    XAF_CHK_PTR(pTmp);
    memset(pTmp, 0, size);

    p_adev = (xaf_adev_t *) (((unsigned long)pTmp + (XAF_4BYTE_ALIGN-1))& ~(XAF_4BYTE_ALIGN-1));
    p_adev->adev_ptr = pTmp;
    *pp_adev = (void *)p_adev;
    
    p_adev->adev_state = XAF_ADEV_INIT;

    // App Interface Layer memory allocation (BSS)
    size = sizeof(xf_ap_t)+(XAF_8BYTE_ALIGN-1);
    p_adev->p_apMem = mem_malloc(size, XAF_MEM_ID_DEV);
    XAF_CHK_PTR(p_adev->p_apMem);
    memset(p_adev->p_apMem, 0, size);

    xf_g_ap = (xf_ap_t *) (((unsigned long)p_adev->p_apMem + (XAF_8BYTE_ALIGN-1))& ~(XAF_8BYTE_ALIGN-1));

    xf_g_ap->xf_mem_malloc_fxn = mem_malloc;
    xf_g_ap->xf_mem_free_fxn = mem_free;
#ifndef XA_DISABLE_EVENT
    xf_g_ap->cdata = &p_adev->cdata;
    xf_g_ap->app_event_handler_cb = NULL;

    p_adev->cdata.cb = xaf_event_relay;
#endif

    // DSP Interface Layer memory allocation (BSS)
    size = sizeof(xf_dsp_t)+(XAF_8BYTE_ALIGN-1);
    ret = xaf_malloc(p_adev->xf_g_ap, &(p_adev->p_dspMem), size, XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;

    xf_g_dsp = (xf_dsp_t *) (((unsigned long)p_adev->p_dspMem + (XAF_8BYTE_ALIGN-1)) & ~(XAF_8BYTE_ALIGN-1));


    size = audio_frmwk_buf_size + (XAF_32BYTE_ALIGN-1); 
    ret = xaf_malloc(p_adev->xf_g_ap, &(p_adev->p_apSharedMem), size, XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;
    xf_g_dsp->xf_ap_shmem_buffer = (UWORD8 *) (((unsigned long)p_adev->p_apSharedMem + (XAF_32BYTE_ALIGN-1)) & ~(XAF_32BYTE_ALIGN-1));
    xf_g_dsp->xf_ap_shmem_buffer_size = audio_frmwk_buf_size;


    size = (audio_comp_buf_size*XF_CFG_CORES_NUM_DSP) + (XAF_32BYTE_ALIGN-1); 
    ret = xaf_malloc(p_adev->xf_g_ap, &(p_adev->p_dspLocalBuff), size, XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;
    xf_g_dsp->xf_dsp_local_buffer = (UWORD8 *) (((unsigned long)p_adev->p_dspLocalBuff + (XAF_32BYTE_ALIGN-1)) & ~(XAF_32BYTE_ALIGN-1));
    xf_g_dsp->xf_dsp_local_buffer_size = audio_comp_buf_size*XF_CFG_CORES_NUM_DSP;
    

#if XF_CFG_CORES_NUM_DSP > 1
    ret = xaf_malloc(p_adev->xf_g_ap, &(xf_g_dsp->xf_dsp_shmem_buffer), (audio_comp_buf_size), XAF_MEM_ID_DEV);
    if(ret != XAF_NO_ERR)
        return ret;    
    xf_g_dsp->xf_dsp_shmem_buffer_size = audio_comp_buf_size;
#endif    // #if XF_CFG_CORES_NUM_DSP > 1

    __xf_lock_init(&xf_g_ap->g_msgq_lock);

    xf_g_dsp->dsp_thread_args[XF_DSP_THREAD_ARG_IDX_WORKER_SCRATCH] = xf_g_dsp->worker_thread_scratch_size;
    {
		UWORD32 *p_worker_scratch, i;

		p_worker_scratch = (UWORD32 *)xf_g_dsp->dsp_thread_args[XF_DSP_THREAD_ARG_IDX_WORKER_SCRATCH];
        for(i=0; i<XAF_MAX_WORKER_THREADS; i++)
        {
		    p_worker_scratch[i] = XF_CFG_CODEC_SCRATCHMEM_SIZE;
        }
    }

    p_adev->dsp_thread_priority = dsp_thread_priority;
#if defined(HAVE_FREERTOS)
    ret = __xf_thread_create(&xf_g_ap->dsp_thread, dsp_thread_entry, (void *)xf_g_dsp->dsp_thread_args, "DSP-thread", NULL, STACK_SIZE, dsp_thread_priority);
#else
    ret = __xf_thread_create(&xf_g_ap->dsp_thread, dsp_thread_entry, (void *)xf_g_dsp->dsp_thread_args, "DSP-thread", xf_g_ap->dsp_stack, STACK_SIZE, dsp_thread_priority);
#endif
    if (ret != 0)
        return XAF_RTOS_ERR;
    
    p_proxy = &p_adev->proxy;

    p_proxy->proxy_thread_priority = proxy_thread_priority;

    /* ...open DSP proxy - specify "DSP#0" */
    XF_CHK_API(xf_proxy_init(p_proxy, 0));

    /* ...create auxiliary buffers pool for control commands */
    XF_CHK_API(xf_pool_alloc(p_proxy, XAF_AUX_POOL_SIZE, XAF_AUX_POOL_MSG_LENGTH, XF_POOL_AUX, &p_proxy->aux, XAF_MEM_ID_DEV));

#if TENA_2356
    /* ...mutex for orderly comp deletion. */
    __xf_lock_init(&xf_g_ap->g_comp_delete_lock);
#endif

#ifndef XA_DISABLE_EVENT
    xaf_sync_chain_init(&p_adev->event_chain, (UWORD32)offset_of(xf_app_event_channel_t, next));
#endif
    xaf_sync_chain_init(&p_adev->comp_chain, (UWORD32)offset_of(xaf_comp_t, next));

    return XAF_NO_ERR;
}
#endif

XAF_ERR_CODE xaf_adev_close(pVOID adev_ptr, xaf_adev_close_flag flag)
{
    xaf_adev_t *p_adev;
    xf_ap_t    *xf_g_ap;
    xf_proxy_t *p_proxy;
    xaf_comp_t *p_comp;
    WORD32 i, ncomp;
    mem_obj_t  *g_mem_obj;

    XF_CHK_ERR((adev_ptr != NULL), XAF_INVALIDPTR_ERR);

    p_adev = (xaf_adev_t *)adev_ptr;
    ncomp = p_adev->n_comp;
    xf_g_ap = p_adev->xf_g_ap;
    g_mem_obj = xf_g_ap->g_mem_obj;

    XAF_ADEV_STATE_CHK(p_adev, XAF_ADEV_RESET);
    p_adev->adev_state = XAF_ADEV_RESET;

    if(flag == XAF_ADEV_FORCE_CLOSE)
    {
        for(i = 0; i < ncomp; i++)
        {
            p_comp = (xaf_comp_t *) xaf_sync_chain_get_head(&p_adev->comp_chain);

            if(p_comp == NULL)
                break;

            XF_CHK_API(xaf_comp_delete(p_comp));
        }
    }

    XF_CHK_ERR((p_adev->n_comp == 0), XAF_API_ERR);

    p_proxy = &p_adev->proxy;

    if(p_proxy->aux != NULL)
    {
#if TENA_2356
        /* ...acquire lock. */
        __xf_lock(&xf_g_ap->g_comp_delete_lock);
#endif
        xf_pool_free(p_proxy->aux, XAF_MEM_ID_DEV);

#if TENA_2356
        /* ...release lock. */
        __xf_unlock(&xf_g_ap->g_comp_delete_lock);
#endif
    }
#if 0 /* by S.J*/
    TRACE(INFO, _b("dsp buffer usage(bytes): component=%d, framework=%d xaf=%d"),\
            xf_g_dsp->dsp_comp_buf_size_peak, xf_g_dsp->dsp_frmwk_buf_size_peak, \
            (xf_g_ap->xaf_memory_used + XAF_DEV_AND_AP_STRUCT_MEM_SIZE -  (xf_g_dsp->xf_dsp_local_buffer_size + xf_g_dsp->xf_ap_shmem_buffer_size)));
#endif
    if(xf_g_ap != NULL)
    {
#if 0 /*by S.j*/
        __xf_event_set(p_proxy->ipc.msgq_event, DSP_DIE_MSGQ_ENTRY);
        XF_CHK_API(__xf_thread_join(&xf_g_ap->dsp_thread, NULL));
        XF_CHK_API(__xf_thread_destroy(&xf_g_ap->dsp_thread));
#endif
        xf_proxy_close(p_proxy);

#if 0 /*by S.j*/
        xf_g_ap->xf_mem_free_fxn(p_adev->p_apSharedMem, XAF_MEM_ID_DEV);
        p_adev->p_apSharedMem = NULL;
#if XF_CFG_CORES_NUM_DSP > 1
        xf_g_ap->xf_mem_free_fxn(p_adev->xf_dsp_shmem_buffer, XAF_MEM_ID_DEV);
        p_adev->xf_dsp_shmem_buffer = NULL;
#endif    // #if XF_CFG_CORES_NUM_DSP > 1
        xf_g_ap->xf_mem_free_fxn(p_adev->p_dspLocalBuff, XAF_MEM_ID_DEV);
        p_adev->p_dspLocalBuff = NULL;
        xf_g_ap->xf_mem_free_fxn(p_adev->p_dspMem, XAF_MEM_ID_DEV);
        p_adev->p_dspMem = NULL;
#endif
#if TENA_2356
        __xf_lock_destroy(&xf_g_ap->g_comp_delete_lock);
#endif
#if 0 /*by S.j*/
        __xf_lock_destroy(&xf_g_ap->g_msgq_lock);
#endif
#ifndef XA_DISABLE_EVENT
        xaf_sync_chain_deinit(&p_adev->event_chain);
#endif
        xaf_sync_chain_deinit(&p_adev->comp_chain);
 
        {
          //ferret warning fix; not to use the memory allocated to function pointer xf_mem_free_fxn, after its freed(with free p_apMem).
          xaf_mem_free_fxn_t *pmem_free_fxn = xf_g_ap->xf_mem_free_fxn;
          
          pmem_free_fxn(g_mem_obj, p_adev->p_apMem, XAF_MEM_ID_DEV);
          p_adev->p_apMem = NULL;
          
          pmem_free_fxn(g_mem_obj, p_adev->adev_ptr, XAF_MEM_ID_DEV);
        }
        xf_g_ap = NULL;
    }

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_adev_set_priorities(pVOID adev_ptr, WORD32 n_rt_priorities,
                                     WORD32 rt_priority_base, WORD32 bg_priority)
{
    xaf_adev_t *p_adev = adev_ptr;
    
    /* ...Thumb rule: background-thread priority should be at-most DSP-thread priority */
    XAF_CHK_RANGE(bg_priority, 0, p_adev->dsp_thread_priority);

    /* ...Thumb rule: worker-thread priority should be at-most DSP-thread priority */
    XAF_CHK_RANGE(rt_priority_base, 0, p_adev->dsp_thread_priority);
    XAF_CHK_RANGE((rt_priority_base + n_rt_priorities-1), rt_priority_base, p_adev->dsp_thread_priority);

    return xf_set_priorities(&p_adev->proxy, 0,
                             n_rt_priorities, rt_priority_base, bg_priority);
}

XAF_ERR_CODE xaf_comp_config_default_init(xaf_comp_config_t *pcomp_config)
{
    XAF_CHK_PTR(pcomp_config);

    /* ...initialize component default config params */
    memset(pcomp_config, 0, sizeof(xaf_comp_config_t));

#ifndef XA_DISABLE_EVENT
    /* ...error-event channel is disabled by default */
    pcomp_config->error_channel_ctl = XAF_ERR_CHANNEL_DISABLE;

    pcomp_config->num_err_msg_buf = 2;
#endif

    /* ...configuration for a standalone pcm_gain component */
    pcomp_config->comp_id = "post-proc/pcm_gain";
    pcomp_config->comp_type = XAF_POST_PROC;
    pcomp_config->num_input_buffers = 2;
    pcomp_config->num_output_buffers = 1;

    return XAF_NO_ERR;
}

#ifndef XA_DISABLE_EVENT
static XAF_ERR_CODE xaf_setup_event_channel(xaf_comp_t *src_comp, UWORD32 src_config_param, xaf_comp_t *dest_comp, UWORD32 dst_config_param, UWORD32 nbuf, UWORD32 buf_size)
{
    xaf_adev_t  *p_adev;
    xf_app_event_channel_t *p_channel;
    void        *pTmp;

    p_adev    = (xaf_adev_t *) src_comp->p_adev;
    
    /* ...need to maintain app side housekeeping structure event channels */
    xaf_malloc(p_adev->xf_g_ap, &pTmp, sizeof(xf_app_event_channel_t), XAF_MEM_ID_DEV);
    memset(pTmp, 0, sizeof(xf_app_event_channel_t));

    p_channel = (xf_app_event_channel_t *)pTmp;
    p_channel->event_id_src     = src_config_param;
    p_channel->event_id_dst     = dst_config_param;
    p_channel->p_comp_src       = src_comp;
    p_channel->p_comp_dst       = dest_comp;
    p_channel->channel_state    = XF_EVENT_CHANNEL_STATE_ACTIVE;

    if (dest_comp != NULL)
    {
        /* ...channel for event communication between components */
        XF_CHK_API(xf_create_event_channel(&src_comp->handle, src_config_param, &dest_comp->handle, dst_config_param, nbuf, buf_size, XF_EVENT_BUFFER_ALIGNMENT));
    }
    else
    {
        /* ...channel for event communication from component/framwork to application */

        p_channel->event_id_dst = XF_CFG_ID_EVENT_TO_APP;

        /* ...create channel at DSP Interface Layer */
        XF_CHK_API(xf_create_event_channel(&src_comp->handle, src_config_param, NULL, XF_CFG_ID_EVENT_TO_APP, nbuf, buf_size, XF_EVENT_BUFFER_ALIGNMENT));

        UWORD32 i, length = buf_size + sizeof(src_config_param);
        xf_buffer_t *event_buf;
        void *p_buf;

        /* ...allocate and send buffers to DSP */
        XF_CHK_API(xf_pool_alloc(&p_adev->proxy, nbuf, length, XF_POOL_AUX, &p_channel->pool, XAF_MEM_ID_COMP));

        for (i=0; i<nbuf; i++)
        {
            event_buf = xf_buffer_get(p_channel->pool);
            p_buf = xf_buffer_data(event_buf);

            *(UWORD32*)p_buf = src_config_param;

            XF_CHK_API(xf_command(&src_comp->handle, 0, XF_EVENT, p_buf, length));
        
            p_channel->pending_buffers++;
        }
    }

    xaf_sync_chain_add_node(&p_adev->event_chain, p_channel);

    return XAF_NO_ERR;
}

static XAF_ERR_CODE xaf_destroy_event_channel(xaf_comp_t *src_comp, UWORD32 src_config_param, xaf_comp_t *dest_comp, UWORD32 dst_config_param)
{
    xaf_adev_t  *p_adev;
    xf_app_event_channel_t  *p_channel_curr = NULL;

    p_adev    = (xaf_adev_t *) src_comp->p_adev;
    
    dst_config_param = (dest_comp != NULL) ? dst_config_param : XF_CFG_ID_EVENT_TO_APP;

    p_channel_curr = xaf_sync_chain_find_node_by_param(&p_adev->event_chain, src_comp, src_config_param, dst_config_param); 
    
    XF_CHK_ERR((p_channel_curr != NULL), XAF_INVALIDPTR_ERR);

    xaf_sync_chain_set_node_state(&p_adev->event_chain, p_channel_curr, XF_EVENT_CHANNEL_STATE_INACTIVE);

    if (dest_comp != NULL)
    {
        /* ...inter-component event channel, delete at DSP Interface Layer */
        XF_CHK_API(xf_delete_event_channel(&src_comp->handle, src_config_param, &dest_comp->handle, dst_config_param));
    }
    else
    {
        /* ...channel for events to application, delete at DSP Interface Layer */
        XF_CHK_API(xf_delete_event_channel(&src_comp->handle, src_config_param, NULL, XF_CFG_ID_EVENT_TO_APP));

        /* ...free the event buffer pool for this chanel */
        xf_pool_free(p_channel_curr->pool, XAF_MEM_ID_COMP);
    }

    xaf_sync_chain_delete_node(&p_adev->event_chain, p_channel_curr);

    xaf_free(p_adev->xf_g_ap, (void *)p_channel_curr, XAF_MEM_ID_DEV);

    return XAF_NO_ERR;
}
#endif /* XA_DISABLE_EVENT */

XAF_ERR_CODE xaf_load_library(xaf_adev_t *p_adev, xaf_comp_t *p_comp, xf_id_t comp_id)
{
	xf_handle_t *p_handle;
	char lib_path[200];
	char lib_wrap_path[200];
	struct lib_info *codec_lib;
	struct lib_info *codec_wrap_lib;
	int dec_type;
	int ret;

	p_handle = &p_comp->handle;

	/* ...init codec lib and codec wrap lib */
	strcpy(lib_path, CORE_LIB_PATH);
	strcpy(lib_wrap_path, CORE_LIB_PATH);
	ret = xaf_malloc(p_adev->xf_g_ap, &p_comp->codec_lib, sizeof(struct lib_info), XAF_MEM_ID_COMP);
	codec_lib = (struct lib_info *)p_comp->codec_lib;
	ret = xaf_malloc(p_adev->xf_g_ap, &p_comp->codec_wrap_lib, sizeof(struct lib_info), XAF_MEM_ID_COMP);
	codec_wrap_lib = (struct lib_info *)p_comp->codec_wrap_lib;

	if (!strcmp(comp_id, "audio-decoder/mp3")) {
		strcat(lib_path, "lib_dsp_mp3_dec.so");
		dec_type = CODEC_MP3_DEC;
	} else if (!strcmp(comp_id, "audio-decoder/aac")) {
		strcat(lib_path, "lib_dsp_aac_dec.so");
		dec_type = CODEC_AAC_DEC;
	} else if (!strcmp(comp_id, "audio-decoder/bsac")) {
		strcat(lib_path, "lib_dsp_bsac_dec.so");
		dec_type = CODEC_BSAC_DEC;
	} else if (!strcmp(comp_id, "audio-decoder/dabplus")) {
		strcat(lib_path, "lib_dsp_dabplus_dec.so");
		dec_type = CODEC_DAB_DEC;
	} else if (!strcmp(comp_id, "audio-decoder/mp2")) {
		strcat(lib_path, "lib_dsp_mp2_dec.so");
		dec_type = CODEC_MP2_DEC;
	} else if (!strcmp(comp_id, "audio-decoder/drm")) {
		strcat(lib_path, "lib_dsp_drm_dec.so");
		dec_type = CODEC_DRM_DEC;
	} else if (!strcmp(comp_id, "audio-decoder/sbc")) {
		strcat(lib_path, "lib_dsp_sbc_dec.so");
		dec_type = CODEC_SBC_DEC;
	} else if (!strcmp(comp_id, "audio-encoder/sbc")) {
		strcat(lib_path, "lib_dsp_sbc_enc.so");
		dec_type = CODEC_SBC_ENC;
	} else if (!strcmp(comp_id, "audio-decoder/fsl-ogg")) {
		dec_type = CODEC_FSL_OGG_DEC;
		strcat(lib_wrap_path, "lib_vorbisd_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-mp3")) {
		dec_type = CODEC_FSL_MP3_DEC;
		strcat(lib_wrap_path, "lib_mp3d_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-aac")) {
		dec_type = CODEC_FSL_AAC_DEC;
		strcat(lib_wrap_path, "lib_aacd_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-aacplus")) {
		dec_type = CODEC_FSL_AAC_PLUS_DEC;
		strcat(lib_wrap_path, "lib_aacd_wrap_dsp.so");
	}else if (!strcmp(comp_id, "audio-decoder/fsl-ac3")) {
		dec_type = CODEC_FSL_AC3_DEC;
		strcat(lib_wrap_path, "lib_ac3d_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-ddp")) {
		dec_type = CODEC_FSL_DDP_DEC;
		strcat(lib_wrap_path, "lib_ddpd_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-nbamr")) {
		dec_type = CODEC_FSL_NBAMR_DEC;
		strcat(lib_wrap_path, "lib_nbamrd_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-wbamr")) {
		dec_type = CODEC_FSL_WBAMR_DEC;
		strcat(lib_wrap_path, "lib_wbamrd_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/fsl-wma")) {
		dec_type = CODEC_FSL_WMA_DEC;
		strcat(lib_wrap_path, "lib_wma10d_wrap_dsp.so");
	} else if (!strcmp(comp_id, "audio-decoder/opus")) {
		dec_type = CODEC_OPUS_DEC;
		strcat(lib_wrap_path, "lib_dsp_codec_opus_dec_wrap.so");
	} else if (!strcmp(comp_id, "post-proc/pcm_gain")) {
		dec_type = CODEC_PCM_GAIN;
		return XAF_NO_ERR;
	}

	if (dec_type <= CODEC_SBC_ENC) {
		strcat(lib_wrap_path, "lib_dsp_codec_wrap.so");
		codec_lib->filename = lib_path;
		codec_lib->lib_type = DSP_CODEC_LIB;
	} else {
		/* fsl codec wrap lib include codec */
		xaf_free(p_adev->xf_g_ap, codec_lib, XAF_MEM_ID_COMP);
		p_comp->codec_lib = NULL;
	}
	codec_wrap_lib->filename = lib_wrap_path;
	codec_wrap_lib->lib_type = (DSP_CODEC_WRAP_LIB | dec_type << 2);

	/* wrap load after codec lib */
	ret = xf_load_lib(p_comp, p_comp->codec_lib);
	if (ret)
		TRACE(REG, _b("load codec lib error\n"));

	ret = xf_load_lib(p_comp, p_comp->codec_wrap_lib);
	if (ret) {
		TRACE(REG, _b("load codec wrap lib error\n"));
		return ret;
	}

	TRACE(REG, _b("load library done\n"));
	return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_comp_create(pVOID adev_ptr, pVOID *pp_comp, xaf_comp_config_t *pcomp_config)
{
    xaf_adev_t *p_adev;
    xaf_comp_t *p_comp;
    xf_handle_t *p_handle;    
    void * pTmp;
    int ret, size;
    UWORD32 i;

    XAF_CHK_PTR(pcomp_config);

    xf_id_t comp_id = pcomp_config->comp_id;
    UWORD32 ninbuf = pcomp_config->num_input_buffers;
    UWORD32 noutbuf = pcomp_config->num_output_buffers;
    pVOID (*pp_inbuf)[XAF_MAX_INBUFS] = pcomp_config->pp_inbuf;
    xaf_comp_type comp_type = pcomp_config->comp_type;

    p_adev = (xaf_adev_t *)adev_ptr;
    XAF_CHK_PTR(p_adev);
    XAF_CHK_PTR(pp_comp);
    XAF_CHK_PTR(comp_id);
    if (ninbuf) XAF_CHK_PTR(pp_inbuf);

    XAF_CHK_RANGE(ninbuf, 0, XAF_MAX_INBUFS);
    XAF_CHK_RANGE(noutbuf, 0, 1);
    XAF_CHK_RANGE(comp_type, XAF_DECODER, XAF_MAX_COMPTYPE-1); 

#ifndef XA_DISABLE_EVENT
    XAF_CHK_RANGE(pcomp_config->error_channel_ctl, XAF_ERR_CHANNEL_DISABLE, XAF_ERR_CHANNEL_ALL);
    XAF_CHK_RANGE(pcomp_config->num_err_msg_buf, 1, 4);
#endif

    XAF_ADEV_STATE_CHK(p_adev, XAF_ADEV_RESET);

    //Memory allocation for component struct pointer
    size = (sizeof(xaf_comp_t) + (XAF_4BYTE_ALIGN-1));
    ret = xaf_malloc(p_adev->xf_g_ap, &pTmp, size, XAF_MEM_ID_COMP);
    if(ret != XAF_NO_ERR)
        return ret;
    p_comp = (xaf_comp_t *) (((unsigned long)pTmp + (XAF_4BYTE_ALIGN-1))& ~(XAF_4BYTE_ALIGN-1));

    p_comp->comp_ptr = pTmp;
    *pp_comp = (void*)p_comp;

    p_handle = &p_comp->handle;

    /* ...create component instance (select core-0) */
    XF_CHK_API(xf_open(&p_adev->proxy, p_handle, comp_id, 0, xaf_comp_response));

    XF_CHK_API(xaf_sync_chain_add_node(&p_adev->comp_chain, p_comp));

    // Temporary solution in place of component chain handling
    p_comp->p_adev = p_adev;
    p_adev->n_comp += 1;

    p_comp->ninbuf = ninbuf;

    /* ...allocate input buffer */
    if (ninbuf) 
    {
        xf_buffer_t *buf;
        XF_CHK_API(xf_pool_alloc(&p_adev->proxy, ninbuf, XAF_INBUF_SIZE, XF_POOL_INPUT, &p_comp->inpool, XAF_MEM_ID_COMP));
        
        for (i=0; i<ninbuf; i++)
        {
            buf         = xf_buffer_get(p_comp->inpool);
            p_comp->p_input[i] = xf_buffer_data(buf);   //TENA-2196 saving the address of buffer allocated.
            *(*pp_inbuf + i) = p_comp->p_input[i];
        }

    }
    p_comp->noutbuf = noutbuf;

    p_comp->comp_type   = comp_type;
    p_comp->comp_status = XAF_STARTING;
    p_comp->probe_enabled = 0;
    p_comp->probe_started = 0;

    for (i=0; i<MAX_IO_PORTS; i++)
    {
        p_comp->cmap[i].ptr  = NULL;
        p_comp->cmap[i].port = PORT_NOT_CONNECTED;
    }

    switch (comp_type)
    {
    case XAF_DECODER:
    case XAF_ENCODER:
    case XAF_PRE_PROC:
    case XAF_POST_PROC:
        p_comp->inp_ports = 1; p_comp->out_ports = 1;
        break;
    case XAF_MIXER:
        p_comp->inp_ports = 4; p_comp->out_ports = 1;
        break;
    case XAF_MIMO_PROC_12 ... (XAF_MAX_COMPTYPE-1):
#if 0 /* by S.J*/
        p_comp->inp_ports  = xf_io_ports[comp_type][0];
        p_comp->out_ports  = xf_io_ports[comp_type][1];
#endif
        break;
    case XAF_RENDERER:
        p_comp->inp_ports = 1; p_comp->out_ports = 1; /* optional outport */
        break;
#if 1 //TENA_2314
    case XAF_CAPTURER:
        p_comp->inp_ports = 0; p_comp->out_ports = 1;
        break;
#else
    case XAF_CAPTURER:
        p_comp->inp_ports = 1; p_comp->out_ports = 1;
        break;
#endif
    default :
        break;
    }

#ifndef XA_DISABLE_EVENT
    if(pcomp_config->error_channel_ctl)
    {
        UWORD32 error_channel_event_id = 0;

        p_comp->error_channel_ctl = pcomp_config->error_channel_ctl;

        if(p_comp->error_channel_ctl == XAF_ERR_CHANNEL_FATAL)
            error_channel_event_id = XF_CFG_COMP_ERR_FATAL;
        else if(p_comp->error_channel_ctl == XAF_ERR_CHANNEL_ALL)
            error_channel_event_id = XF_CFG_COMP_ERR_ALL;
        
        XF_CHK_API(xaf_setup_event_channel(p_comp, error_channel_event_id, NULL, 0, pcomp_config->num_err_msg_buf, XF_COMP_ERROR_CHANNEL_BUF_SIZE));
    }
#endif
    p_comp->comp_state = XAF_COMP_CREATE;

    return XAF_NO_ERR;
}

#ifndef XA_DISABLE_DEPRECATED_API
XAF_ERR_CODE xaf_comp_create_deprecated(pVOID adev_ptr, pVOID *pp_comp, xf_id_t comp_id, UWORD32 ninbuf, UWORD32 noutbuf, pVOID pp_inbuf[], xaf_comp_type comp_type)
{
    xaf_adev_t *p_adev;
    xaf_comp_t *p_comp;
    xf_handle_t *p_handle;    
    void * pTmp;
    int ret, size;
    UWORD32 i;

    p_adev = (xaf_adev_t *)adev_ptr;

    XAF_CHK_PTR(p_adev);
    XAF_CHK_PTR(pp_comp);
    XAF_CHK_PTR(comp_id);
    if (ninbuf) XAF_CHK_PTR(pp_inbuf);

    XAF_CHK_RANGE(ninbuf, 0, XAF_MAX_INBUFS);
    XAF_CHK_RANGE(noutbuf, 0, 1);
    XAF_CHK_RANGE(comp_type, XAF_DECODER, XAF_MAX_COMPTYPE-1); 

    XAF_ADEV_STATE_CHK(p_adev, XAF_ADEV_RESET);

    //Memory allocation for component struct pointer
    size = (sizeof(xaf_comp_t) + (XAF_4BYTE_ALIGN-1));
    ret = xaf_malloc(p_adev->xf_g_ap, &pTmp, size, XAF_MEM_ID_COMP);
    if(ret != XAF_NO_ERR)
        return ret;
    p_comp = (xaf_comp_t *) (((unsigned long)pTmp + (XAF_4BYTE_ALIGN-1))& ~(XAF_4BYTE_ALIGN-1));

    p_comp->comp_ptr = pTmp;
    *pp_comp = (void*)p_comp;

    p_handle = &p_comp->handle;

    /* ...create component instance (select core-0) */
    XF_CHK_API(xf_open(&p_adev->proxy, p_handle, comp_id, 0, xaf_comp_response));

    XF_CHK_API(xaf_sync_chain_add_node(&p_adev->comp_chain, p_comp));

    // Temporary solution in place of component chain handling
    p_comp->p_adev = p_adev;
    p_adev->n_comp += 1;

    p_comp->ninbuf = ninbuf;

    /* ...allocate input buffer */
    if (ninbuf) 
    {
        xf_buffer_t *buf;
        XF_CHK_API(xf_pool_alloc(&p_adev->proxy, ninbuf, XAF_INBUF_SIZE, XF_POOL_INPUT, &p_comp->inpool, XAF_MEM_ID_COMP));
        
        for (i=0; i<ninbuf; i++)
        {
            buf         = xf_buffer_get(p_comp->inpool);
            pp_inbuf[i] = xf_buffer_data(buf); 
            p_comp->p_input[i] = (void *)pp_inbuf[i];//TENA-2196 saving the address of buffer allocated.
        }

    }
    p_comp->noutbuf = noutbuf;

    p_comp->comp_type   = comp_type;
    p_comp->comp_status = XAF_STARTING;
    p_comp->probe_enabled = 0;
    p_comp->probe_started = 0;

    for (i=0; i<MAX_IO_PORTS; i++)
    {
        p_comp->cmap[i].ptr  = NULL;
        p_comp->cmap[i].port = PORT_NOT_CONNECTED;
    }

    switch (comp_type)
    {
    case XAF_DECODER:
    case XAF_ENCODER:
    case XAF_PRE_PROC:
    case XAF_POST_PROC:
        p_comp->inp_ports = 1; p_comp->out_ports = 1;
        break;
    case XAF_MIXER:
        p_comp->inp_ports = 4; p_comp->out_ports = 1;
        break;
    case XAF_MIMO_PROC_12 ... (XAF_MAX_COMPTYPE-1):
        p_comp->inp_ports  = xf_io_ports[comp_type][0];
        p_comp->out_ports  = xf_io_ports[comp_type][1];
        break;
    case XAF_RENDERER:
        p_comp->inp_ports = 1; p_comp->out_ports = 1; /* optional outport */
        break;
#if 1 //TENA_2314
    case XAF_CAPTURER:
        p_comp->inp_ports = 0; p_comp->out_ports = 1;
        break;
#else
    case XAF_CAPTURER:
        p_comp->inp_ports = 1; p_comp->out_ports = 1;
        break;
#endif
    default :
        break;
    }

    p_comp->comp_state = XAF_COMP_CREATE;

    return XAF_NO_ERR;
}
#endif /* XA_DISABLE_DEPRECATED_API */

XAF_ERR_CODE xaf_comp_delete(pVOID comp_ptr)
{
    xaf_comp_t *p_comp;
    xaf_adev_t *p_adev;
    xf_ap_t    *xf_g_ap;

    p_comp = (xaf_comp_t *)comp_ptr;
    p_adev = (xaf_adev_t *) p_comp->p_adev;
    xf_g_ap = p_adev->xf_g_ap;

    XAF_CHK_PTR(p_comp);

    XAF_COMP_STATE_CHK(p_comp);

    if (p_comp->codec_wrap_lib) xf_unload_lib(p_comp, p_comp->codec_wrap_lib);
    if (p_comp->codec_lib) xf_unload_lib(p_comp, p_comp->codec_lib);
    if (p_comp->codec_lib) xaf_free(p_adev->xf_g_ap, p_comp->codec_lib, XAF_MEM_ID_COMP);
    if (p_comp->codec_wrap_lib) xaf_free(p_adev->xf_g_ap, p_comp->codec_wrap_lib, XAF_MEM_ID_COMP);

    p_comp->comp_state = XAF_COMP_RESET;

    // Temporary solution in place of component chain handling
    XF_CHK_ERR((p_adev->n_comp > 0), XAF_API_ERR);

#ifndef XA_DISABLE_EVENT
    /* ...first delete any event channels associated with the component */
    {
        xf_app_event_channel_t   *p_channel_curr;

        while((p_channel_curr = xaf_sync_chain_find_node_by_comp(&p_adev->event_chain, comp_ptr)) != NULL)
        {
            XF_CHK_API(xaf_destroy_event_channel(p_channel_curr->p_comp_src, p_channel_curr->event_id_src, p_channel_curr->p_comp_dst, p_channel_curr->event_id_dst));
        }
    }
#endif

#if TENA_2356
    XAF_CHK_PTR(xf_g_ap);
    /* ...acquire lock. */
    __xf_lock(&xf_g_ap->g_comp_delete_lock);
#endif

    /* ...remove the component from chain */
    XF_CHK_API(xaf_sync_chain_delete_node(&p_adev->comp_chain, p_comp));

    p_adev->n_comp -= 1;

    if (p_comp->inpool)  xf_pool_free(p_comp->inpool, XAF_MEM_ID_COMP);
    if (p_comp->outpool) xf_pool_free(p_comp->outpool, XAF_MEM_ID_COMP);
    if (p_comp->probepool) xf_pool_free(p_comp->probepool, XAF_MEM_ID_COMP);

    xf_close(&p_comp->handle);

    xf_g_ap->xf_mem_free_fxn(xf_g_ap->g_mem_obj, p_comp->comp_ptr, XAF_MEM_ID_COMP);

#if TENA_2356
    /* ...release lock. */
    __xf_unlock(&xf_g_ap->g_comp_delete_lock);
#endif

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_comp_set_config(pVOID comp_ptr, WORD32 num_param, pWORD32 p_param)
{
    xaf_comp_t              *p_comp;
    xf_set_param_msg_t     *smsg;
    xf_handle_t            *p_handle;
    WORD32                     i, j;
    
    p_comp = (xaf_comp_t *)comp_ptr;    

    XAF_CHK_PTR(p_comp);
    XAF_CHK_PTR(p_param);
    XAF_CHK_RANGE(num_param, 1, XAF_MAX_CONFIG_PARAMS); 
    
    XAF_COMP_STATE_CHK(p_comp);

    p_handle = &p_comp->handle;
    XAF_CHK_PTR(p_handle);

    /* ...set persistent stream characteristics */
    smsg = xf_buffer_data(p_handle->aux);

    j = 0;
    for (i=0; i<num_param; i++)
    {
        smsg->item[i].id    = p_param[j++];
        smsg->item[i].value = p_param[j++];

        if ((smsg->item[i].id == XAF_COMP_CONFIG_PARAM_PROBE_ENABLE) && (smsg->item[i].value))
            p_comp->probe_enabled = 1;
    }
    
#if 0
    /* ...pass command to the component */
    /* ...tbd - command goes port 0 always, check if okay */
    XF_CHK_API(xf_command(p_handle, 0, XF_SET_PARAM, smsg, sizeof(xf_set_param_item_t)*num_param));

    /* ...wait until result is delivered */
    XF_CHK_API(xf_response_get(p_handle, &rmsg));

    /* ...make sure response is expected */
    XF_CHK_ERR((rmsg.opcode == XF_SET_PARAM && rmsg.buffer == smsg), XAF_API_ERR);
#else
    /* ...pass command to component through proxy */
    XF_CHK_API(xf_set_config(p_handle, smsg, sizeof(xf_set_param_item_t)*num_param));
#endif

    return XAF_NO_ERR;
}


XAF_ERR_CODE xaf_comp_get_config(pVOID comp_ptr, WORD32 num_param, pWORD32 p_param)
{
    xaf_comp_t             *p_comp;
    xf_get_param_msg_t     *smsg;
    xf_handle_t            *p_handle;
    WORD32                     i,j;

    p_comp = (xaf_comp_t *)comp_ptr;

    XAF_CHK_PTR(p_comp);
    XAF_CHK_PTR(p_param);
    XAF_CHK_RANGE(num_param, 1, XAF_MAX_CONFIG_PARAMS); 
    
    XAF_COMP_STATE_CHK(p_comp);

    p_handle = &p_comp->handle;
    XAF_CHK_PTR(p_handle);

    /* ...set persistent stream characteristics */
    smsg = xf_buffer_data(p_handle->aux);

    for (i=0,j=0; i<num_param; i++,j+=2)
    {
        smsg->c.id[i] = p_param[j];
        p_param[j+1] = 0;
    }
    
#if 0
    xf_user_msg_t           rmsg;
    /* ...pass command to the component */
    /* ...tbd - command goes port 0 always, check if okay */
    XF_CHK_API(xf_command(p_handle, 0, XF_GET_PARAM, smsg, XF_GET_PARAM_CMD_LEN(num_param)));

    /* ...wait until result is delivered */
    XF_CHK_API(xf_response_get(p_handle, &rmsg));

    /* ...make sure response is expected */
    XF_CHK_ERR((rmsg.opcode == XF_GET_PARAM && rmsg.buffer == smsg), XAF_API_ERR);
#else
    /* ...pass command to component through proxy */
    XF_CHK_API(xf_get_config(p_handle, smsg, XF_GET_PARAM_CMD_LEN(num_param)));
#endif

    for (i=0,j=1; i<num_param; i++,j+=2)
    {
        p_param[j] = smsg->r.value[i];
    }

    return XAF_NO_ERR;
}


XAF_ERR_CODE xaf_comp_get_status(pVOID adev_ptr, pVOID comp_ptr, xaf_comp_status *p_status, pVOID p_info)
{
    xaf_adev_t *p_adev;
    xaf_comp_t *p_comp;
    xf_handle_t *p_handle;

    p_adev = (xaf_adev_t *)adev_ptr;
    p_comp = (xaf_comp_t *)comp_ptr;

    XAF_CHK_PTR(p_comp);
    XAF_CHK_PTR(p_status);
    XAF_CHK_PTR(p_info);

    XAF_COMP_STATE_CHK(p_comp);

    if (!p_comp->init_done) XAF_CHK_PTR(p_adev);

    p_handle = &p_comp->handle;

    TRACE(INFO, _b("enter comp_get_status pending_resp=%d expect_out_cmd=%d"), p_comp->pending_resp, p_comp->expect_out_cmd);
    if (p_comp->pending_resp)
    {
        xf_user_msg_t rmsg;
        /* ...wait until result is delivered */
        XF_CHK_API(xf_response_get(p_handle, &rmsg)); 

        if (rmsg.opcode == XF_EVENT) {
            long *p_buf = (long *) p_info;
            p_buf[0] = (long) rmsg.buffer;
            return 1;
        }

        if (rmsg.opcode == XF_FILL_THIS_BUFFER) 
        {
            if (rmsg.buffer == p_comp->start_buf)
            {
                int num_out_ports = p_comp->out_ports;

                /* ... response on first output port is received */
                num_out_ports--;
                p_comp->pending_resp--;

                while (num_out_ports)
                {
                    /* ...wait until result is delivered */
                    XF_CHK_API(xf_response_get(p_handle, &rmsg)); 
            
                    /* ...make sure response is expected */
                    XF_CHK_ERR((rmsg.opcode == XF_FILL_THIS_BUFFER && rmsg.buffer == p_comp->start_buf), XAF_API_ERR);
            
                    num_out_ports--;
                    p_comp->pending_resp--;
                }

                XF_CHK_API(xaf_comp_post_init_config(p_adev, p_comp, p_comp->start_buf));
            }
            else 
            {
                long *p_buf = (long *) p_info;
                p_buf[0] = (long) rmsg.buffer;
                p_buf[1] = (long) rmsg.length;

                p_comp->pending_resp--;

                if (p_comp->inp_ports == XF_MSG_SRC_PORT(rmsg.id))
                {
                    if(rmsg.length == 0)
                    {
                    	p_comp->input_over = 0;
                        p_comp->exec_cmd_issued = 0;

                        TRACE(INFO, _b("FTB R[%08x]:(%08x,%u,%p)"), rmsg.id, rmsg.opcode, rmsg.length, rmsg.buffer);

                        /* ...collect pending responses before declaring exec_done */
                        if (p_comp->probe_enabled && p_comp->probe_started)
                        {
                            /* ...trigger probe buffer flush */
                            XF_CHK_API(xf_pause(p_handle, (p_comp->inp_ports + p_comp->out_ports)));

                            p_comp->probe_started = 0;
                        }

                        while (p_comp->pending_resp)
                        {
                        	XF_CHK_API(xf_response_get(p_handle, &rmsg));
                        	p_comp->pending_resp--;

                        	TRACE(INFO, _b("FTB R[%08x]:(%08x,%u,%p)"), rmsg.id, rmsg.opcode, rmsg.length, rmsg.buffer);
                        }

                        p_comp->comp_status = XAF_EXEC_DONE;
                    }
                    else
                        p_comp->comp_status = XAF_OUTPUT_READY;
                }
                else
                {
                    if(rmsg.length == 0)
                    {
                        p_comp->comp_status = XAF_PROBE_DONE;
                        TRACE(INFO, _b("PROBE R[%08x]:(%08x,%u,%p)"), rmsg.id, rmsg.opcode, rmsg.length, rmsg.buffer);
                    }
                    else
                        p_comp->comp_status = XAF_PROBE_READY;
                }

                p_comp->expect_out_cmd++;
            }
        }
        else
        {
            /* ...make sure response is expected */
            XF_CHK_ERR((rmsg.opcode == XF_EMPTY_THIS_BUFFER), XAF_API_ERR);
            long *p_buf = (long *) p_info;
            p_buf[0] = (long) rmsg.buffer;
            p_buf[1] = (long) rmsg.length;
            
            p_comp->pending_resp--;
            
            if (p_comp->input_over && rmsg.buffer == NULL)
            {
            	p_comp->input_over = 0;
            	p_comp->exec_cmd_issued = 0;

                TRACE(INFO, _b("ETB R[%08x]:(%08x,%u,%p)"), rmsg.id, rmsg.opcode, rmsg.length, rmsg.buffer);

                /* ...collect pending responses before declaring exec_done */
                if (p_comp->probe_enabled && p_comp->probe_started)
                {
                    /* ...trigger probe buffer flush */
                    XF_CHK_API(xf_pause(p_handle, (p_comp->inp_ports + p_comp->out_ports)));

                    p_comp->probe_started = 0;
                }

                while (p_comp->pending_resp)
                {
                	XF_CHK_API(xf_response_get(p_handle, &rmsg));
                	p_comp->pending_resp--;

                	TRACE(INFO, _b("ETB R[%08x]:(%08x,%u,%p)"), rmsg.id, rmsg.opcode, rmsg.length, rmsg.buffer);
                }

            	p_comp->comp_status = XAF_EXEC_DONE;
            }
            else p_comp->comp_status = XAF_NEED_INPUT;
        }
    }
    else if ((p_comp->comp_status == XAF_STARTING && p_comp->start_cmd_issued) ||
             (p_comp->comp_status == XAF_INIT_DONE && p_comp->exec_cmd_issued))
    {
        if (p_comp->inpool) p_comp->comp_status = XAF_NEED_INPUT;
    }
#if 1 //TENA_2355
    else
    {
        TRACE(INFO, _b("(no pending response)exit comp_get_status expect_out_cmd=%d comp_status:%d"), p_comp->expect_out_cmd, p_comp->comp_status);
        *p_status = p_comp->comp_status;

        return XAF_API_ERR;
    }
#endif
    
    TRACE(INFO, _b("exit comp_get_status pending_resp=%d expect_out_cmd=%d"), p_comp->pending_resp, p_comp->expect_out_cmd);
    *p_status = p_comp->comp_status;
    
    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_comp_process(pVOID adev_ptr, pVOID comp_ptr, pVOID p_buf, UWORD32 length, xaf_comp_flag flag)
{
    xaf_adev_t *p_adev;
    xaf_comp_t *p_comp;
    xf_handle_t *p_handle;

    p_adev = (xaf_adev_t *)adev_ptr;
    p_comp = (xaf_comp_t *)comp_ptr;    

    XAF_CHK_PTR(p_comp);

    XAF_COMP_STATE_CHK(p_comp);

    if (!p_comp->init_done) XAF_CHK_PTR(p_adev);
    XAF_CHK_RANGE(flag, XAF_START_FLAG, XAF_NEED_PROBE_FLAG);
    if (flag == XAF_INPUT_READY_FLAG) XAF_CHK_RANGE(length, 0, XAF_INBUF_SIZE);

    p_handle = &p_comp->handle;
    
    if((flag != XAF_START_FLAG) && (!p_comp->start_cmd_issued))
    {
        /* necessary to allow commands other than START only after start has been issued, TENA-2194 */
        return XAF_API_ERR;
    }

    switch (flag)
    {
    case XAF_START_FLAG:
        if (p_comp->start_cmd_issued)
            break;
        else
        {
            p_comp->start_buf = xf_buffer_data(p_handle->aux);
#if 1
        UWORD32 out_port_idx=0;
        for(out_port_idx=0;out_port_idx<p_comp->out_ports;out_port_idx++)
#endif
        {
              XF_CHK_API(xf_command(p_handle, (p_comp->inp_ports + out_port_idx), XF_FILL_THIS_BUFFER, p_comp->start_buf, 0));
              p_comp->pending_resp++;
        }
            p_comp->start_cmd_issued = 1;

        }
        break;
    
    case XAF_EXEC_FLAG:
        if (!p_comp->init_done || p_comp->exec_cmd_issued)
            break;
        p_comp->exec_cmd_issued = 1;
        if (p_comp->outpool)
        {
            UWORD32 i;
            void *p_data;

            for (i=0; i<p_comp->noutbuf; i++)
            {
#if 1 //DISCONNECT
                p_data = p_comp->pout_buf[i];
#else
                xf_buffer_t *p_buf;
                p_buf = xf_buffer_get(p_comp->outpool);
                p_data = xf_buffer_data(p_buf);
#endif

                XF_CHK_API(xf_command(&p_comp->handle, (p_comp->inp_ports), XF_FILL_THIS_BUFFER, p_data, p_comp->out_format.output_length[0]));
            }
            
            p_comp->pending_resp += p_comp->noutbuf;
        }
#if 0
        if (p_comp->probepool)
        {
            xf_buffer_t *p_buf;
            void *p_data;

            p_buf = xf_buffer_get(p_comp->probepool);
            p_data = xf_buffer_data(p_buf);

            XF_CHK_API(xf_command(&p_comp->handle, (p_comp->inp_ports + p_comp->out_ports), XF_FILL_THIS_BUFFER, p_data, p_comp->probe_length));

            p_comp->pending_resp++;
        }
#endif
        break;
 
    case XAF_INPUT_OVER_FLAG:
        if (!p_comp->input_over)
        {
            XF_CHK_API(xf_command(p_handle, 0, XF_EMPTY_THIS_BUFFER, NULL, 0));
            p_comp->input_over = 1;
            p_comp->pending_resp++;
        }
        break;

    case XAF_INPUT_READY_FLAG:
        {
            /* TENA-2196: To validate p_buf, allowing only pointers allocated by the library. */
            UWORD32 i, k;
            for(i=0, k=1; i<p_comp->ninbuf; i++)
            {
                if(p_buf == p_comp->p_input[i])
                {
                    k=0;
                    break;
                }
            }
            if(k) return XAF_INVALIDPTR_ERR;
        }
        if (!p_comp->input_over)
        {
            XAF_CHK_PTR(p_buf);
            XF_CHK_API(xf_command(p_handle, 0, XF_EMPTY_THIS_BUFFER, p_buf, length));
            p_comp->pending_resp++;
        }
        break;

    case XAF_NEED_OUTPUT_FLAG:
        if (p_comp->expect_out_cmd)
        {
            XAF_CHK_PTR(p_buf);
            XF_CHK_API(xf_command(p_handle, (p_comp->inp_ports), XF_FILL_THIS_BUFFER, p_buf, length));
            p_comp->expect_out_cmd--;

            p_comp->pending_resp++;
        }
        break;

    case XAF_NEED_PROBE_FLAG:
        if (p_comp->expect_out_cmd)
        {
            XAF_CHK_PTR(p_buf);
            XF_CHK_API(xf_command(p_handle, (p_comp->inp_ports + p_comp->out_ports), XF_FILL_THIS_BUFFER, p_buf, length));
            p_comp->expect_out_cmd--;

            p_comp->pending_resp++;
        }
    }
    
    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_connect(pVOID p_src, WORD32 src_out_port, pVOID p_dest, WORD32 dest_in_port, WORD32 num_buf)
{
    xaf_comp_t *src_comp;
    xaf_comp_t *dest_comp;

    src_comp  = (xaf_comp_t *) p_src;
    dest_comp = (xaf_comp_t *) p_dest;
    
    XAF_CHK_PTR(src_comp);
    XAF_CHK_PTR(dest_comp);
    XAF_CHK_RANGE(num_buf, 1, 1024);    

    XAF_COMP_STATE_CHK(src_comp);
    XAF_COMP_STATE_CHK(dest_comp);

    if (!src_comp->init_done)
        return XAF_ROUTING_ERR;

    /* ...src output port number validity check */
    if (src_out_port < (WORD32)src_comp->inp_ports || src_out_port >= (WORD32)(src_comp->inp_ports + src_comp->out_ports))
        return XAF_ROUTING_ERR;

    /* ...dest input port number validity check */
    if (dest_in_port < 0 || dest_in_port >= (WORD32)dest_comp->inp_ports)
        return XAF_ROUTING_ERR;

    /* ...src component connection validity check */
    if (src_comp->cmap[src_out_port].ptr != NULL || src_comp->cmap[src_out_port].port != PORT_NOT_CONNECTED)
        return XAF_ROUTING_ERR;

    /* ...dest component connection validity check */
    if (dest_comp->cmap[dest_in_port].ptr != NULL || dest_comp->cmap[dest_in_port].port != PORT_NOT_CONNECTED)
        return XAF_ROUTING_ERR;

                   
    XF_CHK_API(xf_route(&src_comp->handle, src_out_port, &dest_comp->handle, dest_in_port, num_buf, src_comp->out_format.output_length[src_out_port - src_comp->inp_ports], 8));
    
    /* ...update src component map */
    src_comp->cmap[src_out_port].ptr  = dest_comp;
    src_comp->cmap[src_out_port].port = dest_in_port;

    /* ...update dest component map */
    dest_comp->cmap[dest_in_port].ptr  = src_comp;
    dest_comp->cmap[dest_in_port].port = src_out_port;

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_disconnect(pVOID p_src, WORD32 src_out_port, pVOID p_dest, WORD32 dest_in_port)
{
    xaf_comp_t *src_comp; 
    xaf_comp_t *dest_comp;
    
    src_comp  = (xaf_comp_t *) p_src;
    dest_comp = (xaf_comp_t *) p_dest;
    
    XAF_CHK_PTR(src_comp);
    XAF_CHK_PTR(dest_comp);
    
    XAF_COMP_STATE_CHK(src_comp);

    if (!src_comp->init_done)
        return XAF_ROUTING_ERR;

    /* ...src output port number validity check */
    if (src_out_port < (WORD32)src_comp->inp_ports || src_out_port >= (WORD32)(src_comp->inp_ports + src_comp->out_ports))
        return XAF_ROUTING_ERR;

    /* ...dest input port number validity check */
    if (dest_in_port < 0 || dest_in_port >= (WORD32)dest_comp->inp_ports)
        return XAF_ROUTING_ERR;

    /* ...src component connection validity check */
    if (src_comp->cmap[src_out_port].ptr != dest_comp || (WORD32)src_comp->cmap[src_out_port].port != dest_in_port)
        return XAF_ROUTING_ERR;

    /* ...dest component connection validity check */
    if (dest_comp->cmap[dest_in_port].ptr != src_comp || (WORD32)dest_comp->cmap[dest_in_port].port != src_out_port)
        return XAF_ROUTING_ERR;

    XF_CHK_API(xf_unroute(&src_comp->handle, src_out_port));

    /* ...update src component map */
    src_comp->cmap[src_out_port].ptr  = NULL;
    src_comp->cmap[src_out_port].port = PORT_NOT_CONNECTED;

    /* ...update dest component map */
    dest_comp->cmap[dest_in_port].ptr  = NULL;
    dest_comp->cmap[dest_in_port].port = PORT_NOT_CONNECTED;

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_get_mem_stats(pVOID adev_ptr, WORD32 *pmem_info)
{
    xaf_adev_t *p_adev;

    XAF_CHK_PTR(pmem_info);
    XAF_CHK_PTR(adev_ptr);

    p_adev = (xaf_adev_t *)adev_ptr;

    if((p_adev->adev_state < XAF_ADEV_INIT))
    {
        return XAF_API_ERR;
    }

#if 0
    if(p_adev->n_comp)
    {
        xaf_comp_t *p_comp, *p_next;
        WORD32 i, ncomp;
        WORD32 init_done = 0, comp_state_gt_setcfg = 0;

        /* check if init has been done for all the components, return error if not */
        ncomp = p_adev->n_comp;
        p_comp = p_adev->comp_chain;
        p_next = p_comp->next;
        for(i = 0; i < ncomp; i++)
        {
            p_comp->comp_state;
            if(p_comp->comp_status >= XAF_INIT_DONE) /* this fails for renderer */
                init_done++;
            if(p_comp->comp_state > XAF_COMP_SET_CONFIG)
                comp_state_gt_setcfg++;

            if(p_next == NULL)
                break;

            p_comp = p_next;
            p_next = p_comp->next;
        }
        /* renderer doesnt have p_comp->comp_status. It only has p_comp->comp_state. 
         * Based on these states together, decide if it is appropriate to return mem stats */
        if(!(init_done && (comp_state_gt_setcfg >= ncomp)))
                return XAF_API_ERR;
    }
#endif

    /* mem stats info is complete only after components are initialzed. 
     * Recommended to capture stats before device is closed. */
#if 0 /* by S.J */
    *((WORD32 *)pmem_info + 0) = xf_g_dsp->dsp_comp_buf_size_peak;
    *((WORD32 *)pmem_info + 1) = xf_g_dsp->dsp_frmwk_buf_size_peak;
    *((WORD32 *)pmem_info + 2) = xf_g_ap->xaf_memory_used + XAF_DEV_AND_AP_STRUCT_MEM_SIZE -  (xf_g_dsp->xf_dsp_local_buffer_size + xf_g_dsp->xf_ap_shmem_buffer_size);
#endif
#if 0
    *((WORD32 *)pmem_info + 3) = xf_g_dsp->dsp_comp_buf_size_curr;
    *((WORD32 *)pmem_info + 4) = xf_g_dsp->dsp_frmwk_buf_size_curr;
#endif    

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_pause(pVOID comp_ptr, WORD32 port)
{
    xaf_comp_t    *p_comp;
    
    p_comp = (xaf_comp_t *)comp_ptr;    

    XAF_CHK_PTR(p_comp);
    XAF_CHK_RANGE(port, 0, (p_comp->inp_ports + p_comp->out_ports - 1));
 
    XAF_COMP_STATE_CHK(p_comp);

    XF_CHK_API(xf_pause(&p_comp->handle, port));

    /* ...tbd note paused port */

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_resume(pVOID comp_ptr, WORD32 port)
{
    xaf_comp_t    *p_comp;
    
    p_comp = (xaf_comp_t *)comp_ptr;    

    XAF_CHK_PTR(p_comp);
    XAF_CHK_RANGE(port, 0, (p_comp->inp_ports + p_comp->out_ports - 1));
 
    XAF_COMP_STATE_CHK(p_comp);
   
    /* ...tbd check if the port is paused, else return */ 

    XF_CHK_API(xf_resume(&p_comp->handle, port));

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_flush(pVOID comp_ptr, WORD32 port)
{
    xaf_comp_t    *p_comp;

    p_comp = (xaf_comp_t *)comp_ptr;

    XAF_CHK_PTR(p_comp);
    XAF_CHK_PTR(p_comp->init_done);
    XAF_CHK_RANGE(port, 0, (p_comp->inp_ports + p_comp->out_ports - 1));

    XAF_COMP_STATE_CHK(p_comp);

    XF_CHK_API(xf_flush(&p_comp->handle, port));

    return XAF_NO_ERR;
}


XAF_ERR_CODE xaf_probe_start(pVOID comp_ptr)
{
    xaf_comp_t    *p_comp;
    xf_handle_t   *p_handle;

    p_comp = (xaf_comp_t *)comp_ptr;

    XAF_CHK_PTR(p_comp);
    if (!p_comp->probe_enabled) return XAF_INVALIDVAL_ERR;
    if (p_comp->probe_started)  return XAF_API_ERR;

    p_handle = &p_comp->handle;
    XAF_CHK_PTR(p_handle);

    XAF_COMP_STATE_CHK(p_comp);

    XF_CHK_API(xf_resume(p_handle, (p_comp->inp_ports + p_comp->out_ports)));

    p_comp->probe_started = 1;

    XF_CHK_API(xf_command(p_handle, (p_comp->inp_ports + p_comp->out_ports), XF_FILL_THIS_BUFFER, p_comp->probe_buf, p_comp->probe_length));

    p_comp->pending_resp++;

    return XAF_NO_ERR;
}

XAF_ERR_CODE xaf_probe_stop(pVOID comp_ptr)
{
    xaf_comp_t    *p_comp;
    xf_handle_t   *p_handle;

    p_comp = (xaf_comp_t *)comp_ptr;

    XAF_CHK_PTR(p_comp);
    if (!p_comp->probe_enabled) return XAF_INVALIDVAL_ERR;
    /* if (!p_comp->probe_started) return XAF_API_ERR; */

    p_handle = &p_comp->handle;
    XAF_CHK_PTR(p_handle);

    XAF_COMP_STATE_CHK(p_comp);

    XF_CHK_API(xf_pause(p_handle, (p_comp->inp_ports + p_comp->out_ports)));

    p_comp->probe_started = 0;

    return XAF_NO_ERR;
}

#ifndef XA_DISABLE_EVENT
XAF_ERR_CODE xaf_create_event_channel(pVOID p_src, UWORD32 src_config_param, pVOID p_dest, UWORD32 dst_config_param, UWORD32 nbuf, UWORD32 buf_size)
{
    xaf_comp_t  *src_comp, *dest_comp;

    src_comp  = (xaf_comp_t *) p_src;
    dest_comp = (xaf_comp_t *) p_dest;
    
    XAF_CHK_PTR(src_comp);
    XAF_COMP_STATE_CHK(src_comp);
    XAF_CHK_RANGE(nbuf, 1, 16);    

    return xaf_setup_event_channel(src_comp, src_config_param, dest_comp, dst_config_param, nbuf, buf_size);
}

XAF_ERR_CODE xaf_delete_event_channel(pVOID p_src, UWORD32 src_config_param, pVOID p_dest, UWORD32 dst_config_param)
{
    xaf_comp_t *src_comp, *dest_comp;

    src_comp  = (xaf_comp_t *) p_src;
    dest_comp = (xaf_comp_t *) p_dest;
    
    XAF_CHK_PTR(src_comp);
    XAF_COMP_STATE_CHK(src_comp);
    
    return xaf_destroy_event_channel(src_comp, src_config_param, dest_comp, dst_config_param);
}
#endif
