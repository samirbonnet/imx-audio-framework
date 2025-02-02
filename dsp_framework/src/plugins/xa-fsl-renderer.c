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
/*******************************************************************************
 * xa-renderer.c
 *
 * dummy (dumping data to file)renderer implementation
 *
 * Copyright (c) 2012 Tensilica Inc. ALL RIGHTS RESERVED.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#define MODULE_TAG                      RENDERER

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include "osal-timer.h"
#include <stdio.h>
#include "audio/xa-renderer-api.h"
#include "xf-debug.h"
#include <string.h>

#include "mydefs.h"
#include "hardware.h"
#include "dsp_irq_handler.h"
#include "debug.h"

#ifdef XAF_PROFILE
#include "xaf-clk-test.h"
extern clk_t renderer_cycles;
#endif

/*******************************************************************************
 * Codec parameters
 ******************************************************************************/

/* ...total length of HW FIFO in bytes */
#define HW_FIFO_LENGTH                  8192

/* maximum allowed framesize in bytes per channel. This is the default framesize */
#define MAX_FRAME_SIZE_IN_BYTES_DEFAULT    ( HW_FIFO_LENGTH / 4 )

/* minimum allowed framesize in bytes per channel */
#define MIN_FRAME_SIZE_IN_BYTES    ( 128 )

#define HW_I2S_SF (44100)

#define READ_FIFO(payload) {\
        if(d->output)\
        {\
            /* ...write to optional output buffer */\
            memcpy(d->output, d->pfifo_r, payload);\
            d->bytes_produced = payload;\
        }\
        /* ...write to output file and increment read pointer */\
        d->pfifo_r += payload;\
        if((UWORD32)d->pfifo_r >= (UWORD32)&d->g_fifo_renderer[2*payload])\
        {\
            d->pfifo_r = (void*)d->g_fifo_renderer;\
        }\
    }

#define UPDATE_WPTR(offset, payload) {\
        d->pfifo_w += offset;\
        if((UWORD32)d->pfifo_w >= (UWORD32)&d->g_fifo_renderer[2*payload])\
        {\
            d->pfifo_w = (void*)d->g_fifo_renderer;\
        }\
    }

/*******************************************************************************
 * Local data definition
 ******************************************************************************/

typedef struct XARenderer
{
    /***************************************************************************
     * Internal stuff
     **************************************************************************/

    /* ...component state */
    UWORD32                     state;

    /* ...notification callback pointer */
    xa_renderer_cb_t       *cdata;

    /* ...input buffer pointer */
    void                   *input;

    /* ...output buffer pointer */
    void                   *output;

    /* ...estimation of amount of samples that can be written into FIFO */
    UWORD32                     fifo_avail;

    /* ...number of samples consumed */
    UWORD32                     consumed;
    /* ...number of bytes copied in fifo*/
    UWORD32                     submited_inbytes;
    /***************************************************************************
     * Run-time data
     **************************************************************************/
    
    /* ...size of PCM sample in bytes  */
    UWORD32                     sample_size;

    /* ...number of channels */
    UWORD32                     channels;

    /* ...sample width */
    UWORD32                     pcm_width;
    
    /* ...framesize in bytes per channel */
    UWORD32                     frame_size_bytes;     

    /* ...current sampling rate */
    UWORD32                     rate;
    
    /* ...flag for detecting underrun..made to non zero over submit */
    UWORD32              submit_flag;

    FILE * fw;

    /* ...cumulative output bytes produced*/
    UWORD64             cumulative_bytes_produced;

    /* ...output bytes produced*/
    UWORD32             bytes_produced;

    /* ... FIFO read pointer */
    void    *pfifo_r;

    /* ... FIFO write pointer */
    void    *pfifo_w;

    UWORD8 *g_fifo_renderer;

    /* ...input over flag */
    UWORD32             input_over;

    /* ...execution complete flag */
    UWORD32     exec_done;

    /* ...framesize in samples per channel */
    UWORD32     frame_size;

	void                  *dev_addr;
	void                  *fe_dev_addr;

	void                  *edma_addr;
	void                  *sdma_addr;
	void                  *fe_edma_addr;

	void                  *irqstr_addr;

	/* struct nxp_edma_hw_tcd  tcd[MAX_PERIOD_COUNT];*/
	void                  *tcd;
	void                  *tcd_align32;

	void                  *fe_tcd;
	void                  *fe_tcd_align32;

	void                  (*dev_init)(volatile void * dev_addr, int mode,
					  int channel, int rate, int width, int mclk_rate);
	void                  (*dev_start)(volatile void * dev_addr, int tx);
	void                  (*dev_stop)(volatile void * dev_addr, int tx);
	void                  (*dev_isr)(volatile void * dev_addr);
	void                  (*dev_suspend)(volatile void * dev_addr, u32 *cache_addr);
	void                  (*dev_resume)(volatile void * dev_addr, u32 *cache_addr);
	void                  (*fe_dev_isr)(volatile void * dev_addr);


	void                  (*fe_dev_init)(volatile void * dev_addr, int mode,
					     int channel, int rate, int width, int mclk_rate);
	void                  (*fe_dev_start)(volatile void * dev_addr, int tx);
	void                  (*fe_dev_stop)(volatile void * dev_addr, int tx);
	void                  (*fe_dev_suspend)(volatile void * dev_addr, u32 *cache_addr);
	void                  (*fe_dev_resume)(volatile void * dev_addr, u32 *cache_addr);
	int                   (*fe_dev_hw_params)(volatile void * dev_addr, int channel,
						  int rate, int in_format, volatile void * private_data);

	u32                   dev_Int;
	u32                   dev_fifo_off;
	u32                   dma_Int;

	u32                   fe_dev_Int;
	u32                   fe_dma_Int;
	u32                   fe_dev_fifo_in_off;
	u32                   fe_dev_fifo_out_off;
	u32                   irq_2_dsp;

	u32                   dev_cache[40];
	u32                   fe_dev_cache[120];
	u32                   edma_cache[40];
	u32                   fe_edma_cache[40];

	void                  *dma;
	dmac_t                *dmac[2];

	struct fsl_easrc      easrc;
	struct fsl_easrc_context   ctx;

}   XARenderer;

#define MAX_UWORD32 ((UWORD64)0xFFFFFFFF)
/*******************************************************************************
 * Operating flags
 ******************************************************************************/

#define XA_RENDERER_FLAG_PREINIT_DONE   (1 << 0)
#define XA_RENDERER_FLAG_POSTINIT_DONE  (1 << 1)
#define XA_RENDERER_FLAG_IDLE           (1 << 2)
#define XA_RENDERER_FLAG_RUNNING        (1 << 3)
#define XA_RENDERER_FLAG_PAUSED         (1 << 4)
/*******************************************************************************
 * global variables
 ******************************************************************************/

static inline int xa_hw_renderer_deinit(struct XARenderer *d);

/* ...start HW-renderer operation */
static inline int xa_hw_renderer_start(struct XARenderer *d)
{
	LOG(("HW-renderer started\n"));

	irqstr_start(d->irqstr_addr, d->fe_dev_Int, d->fe_dma_Int);
	dma_chan_start(d->dmac[0]);
	dma_chan_start(d->dmac[1]);
	d->fe_dev_start(d->fe_dev_addr, 1);
	d->dev_start(d->dev_addr, 1);
	return 0;
}

/* ...close hardware renderer */
static inline void xa_hw_renderer_close(struct XARenderer *d)
{
	LOG(("HW-renderer closed\n"));
	if (!d->irqstr_addr)
		return;
	dma_chan_stop(d->dmac[0]);
	dma_chan_stop(d->dmac[1]);
	d->dev_stop(d->dev_addr, 1);
	d->fe_dev_stop(d->fe_dev_addr, 1);
}

/* ...emulation of renderer interrupt service routine */
static void xa_hw_renderer_callback(void *arg)
{
	XARenderer *d = (XARenderer *)arg;
	s32     avail;
	u32     status;
	u32     num;

	READ_FIFO(d->frame_size_bytes * d->channels);
	d->fifo_avail = d->fifo_avail + (d->frame_size_bytes * d->channels);
	LOG2("fifo_avail %x, fifo_ptr_r %x\n", d->fifo_avail, d->pfifo_r);
	/* ...notify user on input-buffer (idx = 0) consumption */
	if((d->fifo_avail) >= d->frame_size_bytes * d->channels * 2)
	{
		LOG("isr under run\n");
		/*under run case*/
		d->state ^= XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_IDLE;
		d->fifo_avail = d->frame_size_bytes * d->channels * 2;
		xa_hw_renderer_close(d);
	} else if(((int)d-> fifo_avail) <= 0) {
		/* over run */
		LOG("isr over run\n");
#if 0
		d->state ^= XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_IDLE;
		d->fifo_avail=HW_FIFO_LENGTH;
		xa_hw_renderer_close(d);
#endif
	}

	d->cdata->cb(d->cdata, 0);
}

/*******************************************************************************
 * Codec access functions
 ******************************************************************************/

static inline void xa_fw_renderer_close(XARenderer *d)
{
    fclose(d->fw);
    //__xf_disable_interrupt(d->irq_2_dsp);
    //__xf_unset_threaded_irq_handler(d->irq_2_dsp);
}

/* ...submit data (in bytes) into internal renderer ring-buffer */
static inline UWORD32 xa_fw_renderer_submit(XARenderer *d, void *b, UWORD32 bytes_write)
{
    FILE *fp = NULL;
    UWORD32 avail;
    UWORD32 k;
    UWORD32 zfill;
    UWORD32 payload;

    fp      = d ->fw;
    payload = d->frame_size_bytes*d->channels;
    avail   = d->fifo_avail;
    k       = 0;
    zfill   = 0;    

    /* ...reset optional output-bytes produced */
    d->bytes_produced = 0;

    if(avail >= payload)
    {
        k             = (payload > bytes_write) ? bytes_write : payload;
        zfill         = payload - k;
        d->fifo_avail = (avail -= payload);

        /* ...write one frame worth data to FIFO */
        memcpy((char *)d->pfifo_w, (char *)b, k);

        if (zfill)
        {
            /* ...write zeros to complete one frame worth data to FIFO */
            memset((char *)d->pfifo_w + k, 0, zfill);

            TRACE(OUTPUT, _b("submitted zero-fill bytes:%d"), zfill);
        }

        /* ...update the write pointer */
        UPDATE_WPTR(payload, payload);

        /* ...process buffer start-up */
        if (d->state & XA_RENDERER_FLAG_IDLE)
        {
            /* ...start-up transmission if FIFO gets at least 2 frames */
            //if (avail <= (HW_FIFO_LENGTH - (2 * payload)))
            if (avail == 0)
            {
		/* trigger start*/
		xa_hw_renderer_start(d);
                d->state ^= XA_RENDERER_FLAG_IDLE | XA_RENDERER_FLAG_RUNNING;

                /* ...write one frame worth data written to FIFO to output file */
                //READ_FIFO(payload);

                TRACE(OUTPUT, _b("FIFO/timer started after buffer full:IDLE->RUNNING"));
            }
        }
        else
        {
            /* ...write one frame worth data written to FIFO to output file */
            //READ_FIFO(payload);

        }

        /* ...declare exec done on input over and if no more valid data is available */
        d->exec_done = (d->input_over && (bytes_write == 0));

        if(d->exec_done) 
        {
            /* ... stop interrupts as soon as exec is done */
	    xa_hw_renderer_close(d);
            d->state ^= XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_IDLE;
        
            TRACE(OUTPUT, _b("exec done, timer stopped"));
        }
    }

    return k;
}

/*******************************************************************************
 * API command hooks
 ******************************************************************************/

/* ...standard codec initialization routine */
static XA_ERRORCODE xa_renderer_get_api_size(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...check parameters are sane */
    XF_CHK_ERR(pv_value, XA_API_FATAL_INVALID_CMD_TYPE);
    /* ...retrieve API structure size */
    *(WORD32 *)pv_value = sizeof(*d);
    return XA_NO_ERROR;
}

/* ...initialize hardware renderer */
static inline int xa_hw_renderer_init(struct XARenderer *d)
{
	struct dsp_main_struct *dsp;
	int             r;
	int             board_type;
	dmac_cfg_t      audio_cfg;
	int dev_type;

	board_type = BOARD_TYPE;

	dsp = get_main_struct();
	dma_probe(dsp);

	d->dma = dsp->dma_device;
	dma_init(d->dma);

	/*initially FIFO will be empty so fifo_avail is 2x framesize bytes for ping and pong */
	d->fifo_avail = d->frame_size_bytes * d->channels * 2;
	/* ...make sure that the frame_size_bytes is within the FIFO length */
	XF_CHK_ERR(d->fifo_avail <= HW_FIFO_LENGTH, XA_RENDERER_CONFIG_NONFATAL_RANGE);

	/* alloc internal buffer for DMA/SAI/ESAI*/
	xaf_malloc((void **)&d->g_fifo_renderer, d->frame_size_bytes * d->channels * 2, 0);

	/* ...initialize FIFO params, zero fill FIFO and init pointers to start of FIFO */
	d->pfifo_w = d->pfifo_r = d->g_fifo_renderer;

	/*It is better to send address through the set_param */
	if (board_type == DSP_IMX8QXP_TYPE) {
		d->dev_addr     = (void *)ESAI_ADDR;
		d->dev_Int      = ESAI_INT;
		d->dev_fifo_off = REG_ESAI_ETDR;

		d->fe_dma_Int   = EDMA_ASRC_INT_NUM;
		d->fe_dev_Int   = ASRC_INT;
		d->fe_dev_addr  = (void *)ASRC_ADDR;
		d->fe_edma_addr = (void *)EDMA_ADDR_ASRC_RXA;
		d->fe_dev_fifo_in_off  = REG_ASRDIA;
		d->fe_dev_fifo_out_off = REG_ASRDOA;

		d->irqstr_addr =  (void *)IRQ_STR_ADDR;

		d->dev_init     = esai_init;
		d->dev_start    = esai_start;
		d->dev_stop     = esai_stop;
		d->dev_isr      = esai_irq_handler;
		d->dev_suspend  = esai_suspend;
		d->dev_resume   = esai_resume;

		d->fe_dev_init  = asrc_init;
		d->fe_dev_start = asrc_start;
		d->fe_dev_stop  = asrc_stop;
		d->fe_dev_isr   = asrc_irq_handler;
		d->fe_dev_suspend  = asrc_suspend;
		d->fe_dev_resume   = asrc_resume;
		d->fe_dev_hw_params = asrc_hw_params;

		d->irq_2_dsp = INT_NUM_IRQSTR_DSP_6;

		/* dma channel configuration */
		audio_cfg.period_len = d->frame_size_bytes * d->channels;
		audio_cfg.period_count = 2;
		audio_cfg.direction = DMA_MEM_TO_DEV;
		audio_cfg.src_addr = d->g_fifo_renderer;
		audio_cfg.dest_addr = (void *)(ASRC_ADDR + REG_ASRDIA);
		audio_cfg.callback = xa_hw_renderer_callback;
		audio_cfg.comp = (void *)d;
		audio_cfg.peripheral_config = NULL;
		audio_cfg.peripheral_size = 0;

		dev_type = EDMA_ASRC_RX;
		d->dmac[0] = request_dma_chan(d->dma, dev_type);
		if (!d->dmac[0])
			return XA_FATAL_ERROR;
		dma_chan_config(d->dmac[0], &audio_cfg);

		audio_cfg.period_len = d->frame_size_bytes * d->channels;
		audio_cfg.period_count = 2;
		audio_cfg.direction = DMA_DEV_TO_DEV;
		audio_cfg.src_addr = (void *)(ASRC_ADDR + REG_ASRDOA);
		audio_cfg.dest_addr = (void *)(ESAI_ADDR + REG_ESAI_ETDR);
		audio_cfg.callback = NULL;
		audio_cfg.comp = (void *)d;
		audio_cfg.peripheral_config = NULL;
		audio_cfg.peripheral_size = 0;

		dev_type = EDMA_ESAI_TX;
		d->dmac[1] = request_dma_chan(d->dma, dev_type);
		if (!d->dmac[1])
			return XA_FATAL_ERROR;
		dma_chan_config(d->dmac[1], &audio_cfg);
	} else {
		sdmac_cfg_t sdmac_cfg;
		memset(&d->easrc, 0, sizeof(struct fsl_easrc));
		memset(&d->ctx, 0, sizeof(struct fsl_easrc_context));
		d->easrc.paddr = (unsigned char *)EASRC_ADDR;

		d->dev_addr     = (void *)SAI_ADDR;
		d->dev_Int      = SAI_INT;
		d->dev_fifo_off = FSL_SAI_TDR0;

		d->fe_dma_Int   = SDMA_INT;
		/* not enable easrc Int and enable sai Int */
		d->fe_dev_Int   = SAI_INT;
		d->fe_dev_addr  = &d->easrc;
		d->fe_edma_addr = NULL;
		d->fe_dev_fifo_in_off  = REG_EASRC_WRFIFO(0);
		d->fe_dev_fifo_out_off = REG_EASRC_RDFIFO(0);

		d->irqstr_addr =  (void *)IRQ_STR_ADDR;

		d->dev_init     = sai_init;
		d->dev_start    = sai_start;
		d->dev_stop     = sai_stop;
		d->dev_isr      = sai_irq_handler;
		d->dev_suspend  = sai_suspend;
		d->dev_resume   = sai_resume;
		d->fe_dev_init  = easrc_init;
		d->fe_dev_start = easrc_start;
		d->fe_dev_stop  = easrc_stop;
		d->fe_dev_isr   = easrc_irq_handler;
		d->fe_dev_suspend  = easrc_suspend;
		d->fe_dev_resume   = easrc_resume;
		d->fe_dev_hw_params = fsl_easrc_hw_params;

		d->irq_2_dsp = INT_NUM_IRQSTR_DSP_1;

		/* dma channels configuration */
		audio_cfg.period_len = d->frame_size_bytes * d->channels;
		audio_cfg.period_count = 2;
		audio_cfg.direction = DMA_MEM_TO_DEV;
		audio_cfg.src_addr = d->g_fifo_renderer;
		audio_cfg.dest_addr = (void *)(EASRC_ADDR + REG_EASRC_WRFIFO(0));
		audio_cfg.callback = xa_hw_renderer_callback;
		audio_cfg.comp = (void *)d;
		/* event 16: ASRC Context 0 receive DMA request */
		sdmac_cfg.events[0] = 16;
		sdmac_cfg.events[1] = -1;
		sdmac_cfg.watermark = 0xc;

		audio_cfg.peripheral_config = &sdmac_cfg;
		audio_cfg.peripheral_size = sizeof(sdmac_cfg_t);

		d->dmac[0] = request_dma_chan(d->dma, 0);
		dma_chan_config(d->dmac[0], &audio_cfg);

		audio_cfg.period_len = d->frame_size_bytes * d->channels;
		audio_cfg.period_count = 2;
		audio_cfg.direction = DMA_DEV_TO_DEV;
		audio_cfg.src_addr = (void *)(EASRC_ADDR + REG_EASRC_RDFIFO(0));
		audio_cfg.dest_addr = (void *)(SAI_ADDR + FSL_SAI_TDR0);
		audio_cfg.callback = xa_hw_renderer_callback;
		audio_cfg.comp = (void *)d;
		/* event 5:  SAI-3 transmit DMA request
		 * event 17: ASRC Context 0 transmit DMA request */
		sdmac_cfg.events[0] = 17;
		sdmac_cfg.events[1] = 5;
		sdmac_cfg.watermark = 0x80061806;

		audio_cfg.peripheral_config = &sdmac_cfg;
		audio_cfg.peripheral_size = sizeof(sdmac_cfg_t);

		d->dmac[1] = request_dma_chan(d->dma, 0);
		dma_chan_config(d->dmac[1], &audio_cfg);
	}

	irqstr_init(d->irqstr_addr, d->fe_dev_Int, d->fe_dma_Int);

	d->fe_dev_init(d->fe_dev_addr, 1, d->channels,  d->rate, d->pcm_width, 24576000);
	d->fe_dev_hw_params(&d->easrc, d->channels, d->rate, 2, &d->ctx);

	d->dev_init(d->dev_addr, 1, d->channels,  d->rate, d->pcm_width, 24576000);

	xos_register_interrupt_handler(d->irq_2_dsp, (XosIntFunc *)xa_hw_comp_isr, 0);
	xos_interrupt_enable(d->irq_2_dsp);

	if (board_type == DSP_IMX8MP_TYPE)
		WM8960_Init();

	LOG("hw_init finished\n");
	return 0;
}

static inline int xa_hw_renderer_deinit(struct XARenderer *d)
{
	release_dma_chan(d->dmac[0]);
	release_dma_chan(d->dmac[1]);
	dma_release(d->dma);

	if (d->tcd) {
		xaf_free(d->tcd, 0);
		d->tcd = NULL;
	}

	if (d->fe_tcd) {
		xaf_free(d->fe_tcd, 0);
		d->fe_tcd = NULL;
	}

	if (d->g_fifo_renderer) {
		xaf_free(d->g_fifo_renderer, 0);
		d->g_fifo_renderer = NULL;
		d->pfifo_w = d->pfifo_r = NULL;
	}
}

static XA_ERRORCODE xa_fw_renderer_init (XARenderer *d)
{
   d->consumed = 0;
   d->fw = NULL;

   XF_CHK_ERR(xa_hw_renderer_init(d) == 0, XA_RENDERER_CONFIG_FATAL_HW);
   return XA_NO_ERROR;
}

/* ...standard codec initialization routine */
static XA_ERRORCODE xa_renderer_init(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...sanity check - pointer must be valid */
    XF_CHK_ERR(d, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...process particular initialization type */
    switch (i_idx)
    {
    case XA_CMD_TYPE_INIT_API_PRE_CONFIG_PARAMS:
    {
        /* ...pre-configuration initialization; reset internal data */
        memset(d, 0, sizeof(*d));
        /* ...set default renderer parameters - 16-bit little-endian stereo @ 48KHz */        
        d->channels = 2;
        d->pcm_width = 16;
        d->rate = 48000;
        d->sample_size = ( d->pcm_width >> 3 ); /* convert bits to bytes */ 
        d->frame_size_bytes = MAX_FRAME_SIZE_IN_BYTES_DEFAULT;
        d->frame_size = MAX_FRAME_SIZE_IN_BYTES_DEFAULT/d->sample_size; 
        
        /* ...and mark renderer has been created */
        d->state = XA_RENDERER_FLAG_PREINIT_DONE;
        return XA_NO_ERROR;
    }
    case XA_CMD_TYPE_INIT_API_POST_CONFIG_PARAMS:
    {
        /* ...post-configuration initialization (all parameters are set) */
        XF_CHK_ERR(d->state & XA_RENDERER_FLAG_PREINIT_DONE, XA_API_FATAL_INVALID_CMD_TYPE);

        XF_CHK_ERR(xa_fw_renderer_init(d) == 0, XA_RENDERER_CONFIG_FATAL_HW);

        /* ...mark post-initialization is complete */
        d->state |= XA_RENDERER_FLAG_POSTINIT_DONE;
        return XA_NO_ERROR;
    }

    case XA_CMD_TYPE_INIT_PROCESS:
    {
        /* ...kick run-time initialization process; make sure setup is complete */
        XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_API_FATAL_INVALID_CMD_TYPE);
        /* ...mark renderer is in idle state */
        d->state |= XA_RENDERER_FLAG_IDLE;
        return XA_NO_ERROR;
    }

    case XA_CMD_TYPE_INIT_DONE_QUERY:
    {
        /* ...check if initialization is done; make sure pointer is sane */
        XF_CHK_ERR(pv_value, XA_API_FATAL_INVALID_CMD_TYPE);
        /* ...put current status */
        *(WORD32 *)pv_value = (d->state & XA_RENDERER_FLAG_IDLE ? 1 : 0);
        return XA_NO_ERROR;
    }

    default:
        /* ...unrecognized command type */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

static XA_ERRORCODE xa_renderer_deinit(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    xa_hw_renderer_deinit(d);
    LOG("xa_renderer_deinit\n");
}

/* ...HW-renderer control function */
static inline XA_ERRORCODE xa_hw_renderer_control(XARenderer *d, UWORD32 state)
{
    switch (state)
    {

    case XA_RENDERER_STATE_START:
        /* ...process buffer start-up, on trigger from application */
        if ((d->state & XA_RENDERER_FLAG_IDLE))
        {
            UWORD32 payload = d->frame_size_bytes * d->channels;

            /* ...start the FIFO from the pong buffer, hence adjust the read pointer and make it follow write pointer */
            d->pfifo_r = d->pfifo_w;

            /* ...write one frame worth data written to FIFO to output file */
            //READ_FIFO(payload);

            /* ...to always start with full FIFO worth data */
            d->fifo_avail = 0;

            /* ...start-up transmission with zero filled FIFO */
            //__xf_timer_start(&rend_timer, __xf_timer_ratio_to_period((d->frame_size_bytes / d->sample_size ), d->rate));
	    xa_hw_renderer_start(d);

            /* ...change state to Running */
            d->state ^= (XA_RENDERER_FLAG_IDLE | XA_RENDERER_FLAG_RUNNING);
            
            TRACE(INIT, _b("FIFO/timer started, state:IDLE to RUNNING, fifo_avail:%d"), d->fifo_avail);
        }
        else
        {
            TRACE(INIT, _b("no change in state:RUNNING"));
        }
        return XA_NO_ERROR;

    case XA_RENDERER_STATE_RUN:
        /* ...renderer must be in paused state */
        XF_CHK_ERR(d->state & XA_RENDERER_FLAG_PAUSED, XA_RENDERER_EXEC_NONFATAL_STATE);
        /* ...mark renderer is running */
        d->state ^= XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_PAUSED;

        xa_hw_renderer_start(d);

        return XA_NO_ERROR;

    case XA_RENDERER_STATE_PAUSE:
        /* ...renderer must be in running state */
        XF_CHK_ERR(d->state & XA_RENDERER_FLAG_RUNNING, XA_RENDERER_EXEC_NONFATAL_STATE);
        /* ...pause renderer operation */
        xa_hw_renderer_close(d);
        /* ...mark renderer is paused */
        d->state ^= XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_PAUSED;
        return XA_NO_ERROR;

    case XA_RENDERER_STATE_IDLE:
        /* ...command is valid in any active state; stop renderer operation */
        xa_fw_renderer_close(d);

        /* ...reset renderer flags */
        d->state &= ~(XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_PAUSED);
        return XA_NO_ERROR;

    case XA_RENDERER_STATE_SUSPEND:
	XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_EXEC_NONFATAL_STATE);

	d->dev_suspend(d->dev_addr, d->dev_cache);
	d->fe_dev_suspend(d->fe_dev_addr, d->fe_dev_cache);
	dma_suspend(d->dma);
	return XA_NO_ERROR;

    case XA_RENDERER_STATE_SUSPEND_RESUME:
	XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_EXEC_NONFATAL_STATE);

	irqstr_init(d->irqstr_addr, d->fe_dev_Int, d->fe_dma_Int);
	d->dev_resume(d->dev_addr, d->dev_cache);
	d->fe_dev_resume(d->fe_dev_addr, d->fe_dev_cache);
	dma_resume(d->dma);
	xos_register_interrupt_handler(d->irq_2_dsp, (XosIntFunc *)xa_hw_comp_isr, 0);
	xos_interrupt_enable(d->irq_2_dsp);
	return XA_NO_ERROR;


    default:
        /* ...unrecognized command */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

/* ...set renderer configuration parameter */
static XA_ERRORCODE xa_renderer_set_config_param(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    UWORD32     i_value;

    /* ...sanity check - pointers must be sane */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);
    /* ...pre-initialization must be completed */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_PREINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);
    /* ...process individual configuration parameter */
    switch (i_idx)
    {
    case XA_RENDERER_CONFIG_PARAM_PCM_WIDTH:
        /* ...command is valid only in configuration state */
        XF_CHK_ERR((d->state & XA_RENDERER_FLAG_POSTINIT_DONE) == 0, XA_RENDERER_CONFIG_FATAL_STATE);
        /* ...get requested PCM width */
        i_value = (UWORD32) *(WORD32 *)pv_value;
        /* ...check value is permitted (16 bits only) */
        XF_CHK_ERR(i_value == 16, XA_RENDERER_CONFIG_NONFATAL_RANGE);
        /* ...apply setting */
        d->pcm_width = i_value;
        d->sample_size = ( d->pcm_width >> 3 ); /* convert bits to bytes */ 

        /* ...update internal variable frame_size_bytes */
        d->frame_size_bytes = d->frame_size * d->sample_size;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_CHANNELS:
        /* ...command is valid only in configuration state */
        XF_CHK_ERR((d->state & XA_RENDERER_FLAG_POSTINIT_DONE) == 0, XA_RENDERER_CONFIG_FATAL_STATE);
        /* ...get requested channel number */
        i_value = (UWORD32) *(WORD32 *)pv_value;
        /* ...allow stereo only */
        XF_CHK_ERR((i_value == 2) || (i_value == 1), XA_RENDERER_CONFIG_NONFATAL_RANGE);
        /* ...apply setting */
        d->channels = (UWORD32)i_value;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_SAMPLE_RATE:
        /* ...command is valid only in configuration state */
        XF_CHK_ERR((d->state & XA_RENDERER_FLAG_POSTINIT_DONE) == 0, XA_RENDERER_CONFIG_FATAL_STATE);
        /* ...get requested sampling rate */
        i_value = (UWORD32) *(WORD32 *)pv_value;
        
        /* ...allow 16 , 44.1 or 48KHz only  */
        //XF_CHK_ERR(i_value == 16000 || i_value == 44100 || i_value == 48000, XA_RENDERER_CONFIG_NONFATAL_RANGE);
        /* ...apply setting */
        d->rate = (UWORD32)i_value;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_FRAME_SIZE:
        /* ...command is valid only in configuration state */
        XF_CHK_ERR((d->state & XA_RENDERER_FLAG_POSTINIT_DONE) == 0, XA_RENDERER_CONFIG_FATAL_STATE);

        /* ...check it is valid framesize or not */
        XF_CHK_ERR( ( ( *(WORD32 *)pv_value >= MIN_FRAME_SIZE_IN_BYTES) && ( *(WORD32 *)pv_value <= MAX_FRAME_SIZE_IN_BYTES_DEFAULT ) ), XA_RENDERER_CONFIG_NONFATAL_RANGE);
        
        /* ...check frame_size_bytes is multiple of 4 or not */
        XF_CHK_ERR( ( (*(WORD32 *)pv_value & 0x3) == 0 ), XA_RENDERER_CONFIG_NONFATAL_RANGE);    
        
        /* ...get requested frame size */
        d->frame_size_bytes = (UWORD32) *(WORD32 *)pv_value;        

        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_CB:
        /* ...set opaque callback data function */
        d->cdata = (xa_renderer_cb_t *)pv_value;

        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_STATE:
        /* ...runtime state control parameter valid only in execution state */
        XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

        /* ...get requested state */
        i_value = (UWORD32) *(WORD32 *)pv_value;

        /* ...pass to state control hook */
        return xa_hw_renderer_control(d, i_value);

    case XA_RENDERER_CONFIG_PARAM_FRAME_SIZE_IN_SAMPLES:
        {
            WORD32 frame_size_bytes = *(WORD32 *)pv_value * d->sample_size;

            /* ...command is valid only in configuration state */
            XF_CHK_ERR((d->state & XA_RENDERER_FLAG_POSTINIT_DONE) == 0, XA_RENDERER_CONFIG_FATAL_STATE);
            
            /* ...check it is valid framesize or not */
            XF_CHK_ERR( ( ( frame_size_bytes >= MIN_FRAME_SIZE_IN_BYTES) && ( frame_size_bytes <= MAX_FRAME_SIZE_IN_BYTES_DEFAULT ) ), XA_RENDERER_CONFIG_NONFATAL_RANGE);
            
            /* ...check frame_size_bytes is multiple of 4 or not */
            XF_CHK_ERR( ( (frame_size_bytes & 0x3) == 0 ), XA_RENDERER_CONFIG_NONFATAL_RANGE);    
            
            /* ...get requested frame size */
            d->frame_size  = (UWORD32) *(WORD32 *)pv_value;

            /* ...update internal variable frame_size_bytes */
            d->frame_size_bytes = d->frame_size * d->sample_size;
            
            TRACE(INIT, _b("frame_size:%d"), d->frame_size);
            
            return XA_NO_ERROR;
        }
    default:
        /* ...unrecognized parameter */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

/* ...state retrieval function */
static inline UWORD32 xa_hw_renderer_get_state(XARenderer *d)
{
    if (d->state & XA_RENDERER_FLAG_RUNNING)
        return XA_RENDERER_STATE_RUN;
    else if (d->state & XA_RENDERER_FLAG_PAUSED)
        return XA_RENDERER_STATE_PAUSE;
    else
        return XA_RENDERER_STATE_IDLE;
}

/* ...retrieve configuration parameter */
static XA_ERRORCODE xa_renderer_get_config_param(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...sanity check - renderer must be initialized */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...make sure pre-initialization is completed */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_PREINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

    /* ...process individual configuration parameter */
    switch (i_idx)
    {
    case XA_RENDERER_CONFIG_PARAM_PCM_WIDTH:
        /* ...return current PCM width */
        *(WORD32 *)pv_value = d->pcm_width;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_CHANNELS:
        /* ...return current channel number */
        *(WORD32 *)pv_value = d->channels;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_SAMPLE_RATE:
        /* ...return current sampling rate */
        *(WORD32 *)pv_value = d->rate;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_FRAME_SIZE: /* ...deprecated */
        /* ...return current audio frame length (in bytes) */
        *(WORD32 *)pv_value = d->frame_size_bytes;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_STATE:
        /* ...return current execution state */
        *(WORD32 *)pv_value = xa_hw_renderer_get_state(d);
        return XA_NO_ERROR;
    case XA_RENDERER_CONFIG_PARAM_BYTES_PRODUCED:
        /* ...return current execution state */
        *(UWORD32 *)pv_value = (UWORD32)(d->cumulative_bytes_produced > MAX_UWORD32 ? MAX_UWORD32 : d->cumulative_bytes_produced) ;
        return XA_NO_ERROR;

    case XA_RENDERER_CONFIG_PARAM_FRAME_SIZE_IN_SAMPLES:
        /* ...return current audio frame length (in samples) */
        *(WORD32 *)pv_value = d->frame_size;
        return XA_NO_ERROR;

    default:
        /* ...unrecognized parameter */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

static XA_ERRORCODE xa_renderer_do_exec(XARenderer *d)
{
    d->consumed = xa_fw_renderer_submit(d, d->input, d->submited_inbytes);

    d->cumulative_bytes_produced += d->consumed;

    return XA_NO_ERROR;
}

/* ...execution command */
static XA_ERRORCODE xa_renderer_execute(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    XA_ERRORCODE ret;

    /* ...sanity check - pointer must be valid */
    XF_CHK_ERR(d, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...renderer must be in running state */
    XF_CHK_ERR(d->state & (XA_RENDERER_FLAG_RUNNING | XA_RENDERER_FLAG_IDLE), XA_RENDERER_EXEC_FATAL_STATE);

    /* ...process individual command type */
    switch (i_idx)
    {
    case XA_CMD_TYPE_DO_EXECUTE:
        ret = xa_renderer_do_exec(d);
        return ret;

    case XA_CMD_TYPE_DONE_QUERY:
        XF_CHK_ERR(pv_value, XA_API_FATAL_INVALID_CMD_TYPE);
        
        *(UWORD32 *)pv_value = d->exec_done;

        return XA_NO_ERROR;

    case XA_CMD_TYPE_DO_RUNTIME_INIT:
        /* ...silently ignore */
        return XA_NO_ERROR;

    default:
        /* ...unrecognized command */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

/* ...set number of input bytes */
static XA_ERRORCODE xa_renderer_set_input_bytes(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    UWORD32     size=0;
   
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...make sure it is an input port  */
    XF_CHK_ERR(i_idx == 0, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...renderer must be initialized */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_EXEC_FATAL_STATE);

    /* ...input buffer pointer must be valid */
    XF_CHK_ERR(d->input, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...check buffer size is sane */
    XF_CHK_ERR((size = *(UWORD32 *)pv_value / (d->sample_size * d->channels)) >= 0, XA_RENDERER_EXEC_FATAL_INPUT);

    /* ...make sure we have integral amount of samples */
    XF_CHK_ERR((size * d->sample_size * d->channels) == *(UWORD32 *)pv_value, XA_RENDERER_EXEC_FATAL_INPUT);
   
    d->submited_inbytes = *(UWORD32 *)pv_value;

    /* ...all is correct */
    return XA_NO_ERROR;
}

/* ...get number of output bytes */
static XA_ERRORCODE xa_renderer_get_output_bytes(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...sanity check - check parameters */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...track index must be valid */
    XF_CHK_ERR(i_idx == 1, XA_API_FATAL_INVALID_CMD_TYPE);
    
    /* ...pcm gain component must be running */
    //XF_CHK_ERR(d->state & XA_RENDERER_FLAG_RUNNING, XA_API_FATAL_INVALID_CMD_TYPE);
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_API_FATAL_INVALID_CMD_TYPE);
    
    /* ...output buffer must exist */
    XF_CHK_ERR(d->output, XA_RENDERER_EXEC_NONFATAL_OUTPUT);

    /* ...return number of produced bytes */
    *(WORD32 *)pv_value = d->bytes_produced;

    return XA_NO_ERROR;
}

/* ...get number of consumed bytes */
static XA_ERRORCODE xa_renderer_get_curidx_input_buf(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...sanity check - check parameters */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...input buffer index must be valid */
    XF_CHK_ERR(i_idx == 0, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...renderer must be in post-init state */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_EXEC_FATAL_STATE);

    /* ...input buffer must exist */
    XF_CHK_ERR(d->input, XA_RENDERER_EXEC_FATAL_INPUT);

    /* ...return number of bytes consumed */
    *(WORD32 *)pv_value = d->consumed;
    d->consumed = 0;
    return XA_NO_ERROR;
}

/*******************************************************************************
 * Memory information API
 ******************************************************************************/

/* ..get total amount of data for memory tables */
static XA_ERRORCODE xa_renderer_get_memtabs_size(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity checks */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...check renderer is pre-initialized */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_PREINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

    /* ...we have all our tables inside API structure */
    *(WORD32 *)pv_value = 0;

    return XA_NO_ERROR;
}

/* ..set memory tables pointer */
static XA_ERRORCODE xa_renderer_set_memtabs_ptr(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity checks */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...check renderer is pre-initialized */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_PREINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

    /* ...do not do anything; just return success - tbd */
    return XA_NO_ERROR;
}

/* ...return total amount of memory buffers */
static XA_ERRORCODE xa_renderer_get_n_memtabs(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity checks */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...we have 1 input buffer and 1 optional output buffer */
    *(WORD32 *)pv_value = 2;

    return XA_NO_ERROR;
}

/* ...return memory buffer data */
static XA_ERRORCODE xa_renderer_get_mem_info_size(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    UWORD32     i_value;

    /* ...basic sanity check */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...command valid only after post-initialization step */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

    switch (i_idx)
    {
    case 0:
        /* ...input buffer specification; accept exact audio frame */
        i_value = d->frame_size_bytes * d->channels;
        break;

    case 1:
        /* ...output buffer specification; accept exact audio frame */
        i_value = 0;
        break;

    default:
        /* ...invalid index */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }

    /* ...return buffer size to caller */
    *(WORD32 *)pv_value = (WORD32) i_value;

    return XA_NO_ERROR;
}

/* ...return memory alignment data */
static XA_ERRORCODE xa_renderer_get_mem_info_alignment(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity check */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...command valid only after post-initialization step */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

    /* ...all buffers are at least 4-bytes aligned */
    *(WORD32 *)pv_value = 4;

    return XA_NO_ERROR;
}

/* ...return memory type data */
static XA_ERRORCODE xa_renderer_get_mem_info_type(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity check */
    XF_CHK_ERR(d && pv_value, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...command valid only after post-initialization step */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_RENDERER_CONFIG_FATAL_STATE);

    switch (i_idx)
    {
    case 0:
        /* ...input buffers */
        *(WORD32 *)pv_value = XA_MEMTYPE_INPUT;
        return XA_NO_ERROR;

    case 1:
        /* ...output buffers */
        *(WORD32 *)pv_value = XA_MEMTYPE_OUTPUT;
        return XA_NO_ERROR;

    default:
        /* ...invalid index */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

/* ...set memory pointer */
static XA_ERRORCODE xa_renderer_set_mem_ptr(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity check */
    XF_CHK_ERR(d, XA_API_FATAL_INVALID_CMD_TYPE);

    /* ...codec must be initialized */
    XF_CHK_ERR(d->state & XA_RENDERER_FLAG_POSTINIT_DONE, XA_API_FATAL_INVALID_CMD_TYPE);

    TRACE(INIT, _b("xa_renderer_set_mem_ptr[%u]: %p"), i_idx, pv_value);

    /* ...select memory buffer */
    switch (i_idx)
    {
    case 0:
        /* ...basic sanity check */
        XF_CHK_ERR(pv_value, XA_API_FATAL_INVALID_CMD_TYPE);
        /* ...input buffer */
        d->input = pv_value;
        return XA_NO_ERROR;

    case 1:
        /* ...output buffer(optional). Can be NULL as this is optional output. */
        d->output = NULL;
        return XA_NO_ERROR;

    default:
        /* ...invalid index */
        return XF_CHK_ERR(0, XA_API_FATAL_INVALID_CMD_TYPE);
    }
}

/* ...set input over */
static XA_ERRORCODE xa_renderer_input_over(XARenderer *d, WORD32 i_idx, pVOID pv_value)
{
    /* ...basic sanity check */
    XF_CHK_ERR(d, XA_API_FATAL_INVALID_CMD_TYPE);

    d->input_over = 1;

    return XA_NO_ERROR;
}

/*******************************************************************************
 * API command hooks
 ******************************************************************************/

static XA_ERRORCODE (* const xa_renderer_api[])(XARenderer *, WORD32, pVOID) =
{
    [XA_API_CMD_GET_API_SIZE]           = xa_renderer_get_api_size,
    [XA_API_CMD_INIT]                   = xa_renderer_init,
    [XA_API_CMD_DEINIT]                 = xa_renderer_deinit,
    [XA_API_CMD_SET_CONFIG_PARAM]       = xa_renderer_set_config_param,
    [XA_API_CMD_GET_CONFIG_PARAM]       = xa_renderer_get_config_param,
    [XA_API_CMD_EXECUTE]                = xa_renderer_execute,
    [XA_API_CMD_SET_INPUT_BYTES]        = xa_renderer_set_input_bytes,
    [XA_API_CMD_GET_CURIDX_INPUT_BUF]   = xa_renderer_get_curidx_input_buf,
    [XA_API_CMD_GET_MEMTABS_SIZE]       = xa_renderer_get_memtabs_size,
    [XA_API_CMD_SET_MEMTABS_PTR]        = xa_renderer_set_memtabs_ptr,
    [XA_API_CMD_GET_N_MEMTABS]          = xa_renderer_get_n_memtabs,
    [XA_API_CMD_GET_MEM_INFO_SIZE]      = xa_renderer_get_mem_info_size,
    [XA_API_CMD_GET_MEM_INFO_ALIGNMENT] = xa_renderer_get_mem_info_alignment,
    [XA_API_CMD_GET_MEM_INFO_TYPE]      = xa_renderer_get_mem_info_type,
    [XA_API_CMD_SET_MEM_PTR]            = xa_renderer_set_mem_ptr,
    [XA_API_CMD_INPUT_OVER]             = xa_renderer_input_over,
    [XA_API_CMD_GET_OUTPUT_BYTES]       = xa_renderer_get_output_bytes,
};

/* ...total numer of commands supported */
#define XA_RENDERER_API_COMMANDS_NUM   (sizeof(xa_renderer_api) / sizeof(xa_renderer_api[0]))

/*******************************************************************************
 * API entry point
 ******************************************************************************/

XA_ERRORCODE xa_renderer(xa_codec_handle_t p_xa_module_obj, WORD32 i_cmd, WORD32 i_idx, pVOID pv_value)
{
    XA_ERRORCODE Rend_ret = 0;
    XARenderer *renderer = (XARenderer *) p_xa_module_obj;
#ifdef XAF_PROFILE
    clk_t comp_start, comp_stop;
#endif
    /* ...check if command index is sane */
    XF_CHK_ERR(i_cmd < XA_RENDERER_API_COMMANDS_NUM, XA_API_FATAL_INVALID_CMD);

    /* ...see if command is defined */
    XF_CHK_ERR(xa_renderer_api[i_cmd], XA_API_FATAL_INVALID_CMD);

    /* ...execute requested command */
#ifdef XAF_PROFILE
     if(XA_API_CMD_INIT != i_cmd)
     {
         comp_start = clk_read_start(CLK_SELN_THREAD);
     }
#endif
  
    Rend_ret = xa_renderer_api[i_cmd](renderer, i_idx, pv_value);
#ifdef XAF_PROFILE
    if(XA_API_CMD_INIT != i_cmd)
     {
        comp_stop = clk_read_stop(CLK_SELN_THREAD);
        renderer_cycles += clk_diff(comp_stop, comp_start);
     }
#endif
    return Rend_ret;
}
