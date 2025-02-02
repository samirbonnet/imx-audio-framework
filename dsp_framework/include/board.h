/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2021 NXP
 */

#ifndef _BOARD_H_
#define _BOARD_H_

enum {
	DSP_IMX8QXP_TYPE = 0,
	DSP_IMX8QM_TYPE,
	DSP_IMX8MP_TYPE,
	DSP_IMX8ULP_TYPE,
};

/*
 * Memory allocation for reserved memory:
 * We alway reserve 32M memory from DRAM
 * The DRAM reserved memory is split into three parts currently.
 * The front part is used to keep the dsp firmware, the other part is
 * considered as scratch memory for dsp framework.
 *
 *---------------------------------------------------------------------------
 *| Offset                |  Size    |   Usage                              |
 *---------------------------------------------------------------------------
 *| 0x0 ~ 0xEFFFFF        |  15M     |   Code memory of firmware            |
 *---------------------------------------------------------------------------
 *| 0xF00000 ~ 0xFFFFFF   |  1M      |   Message buffer + Globle dsp struct |
 *---------------------------------------------------------------------------
 *| 0x1000000 ~ 0x1FFFFFF |  16M     |   Scratch memory                     |
 *---------------------------------------------------------------------------
 */

/* Cache definition
 * Every 512M in 4GB space has dedicate cache attribute.
 * 1: write through
 * 2: cache bypass
 * 4: write back
 * F: invalid access
 */

#define FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL (1)

#define RP_MBOX_SUSPEND_SYSTEM  (0xFF11)
#define RP_MBOX_SUSPEND_ACK     (0xFF12)
#define RP_MBOX_RESUME_SYSTEM   (0xFF13)
#define RP_MBOX_RESUME_ACK      (0xFF14)

/* PLATF_8M */
#ifdef PLATF_8M

#define BOARD_TYPE (DSP_IMX8MP_TYPE)

#define I_CACHE_ATTRIBUTE      0x22242224     //write back mode
#define D_CACHE_ATTRIBUTE      0x22212221     //write through mode
#define INT_NUM_MU             7
#define MU_PADDR               0x30E70000

#define VDEV0_VRING_SA_BASE   (0x942f0000U)
#define VDEV0_VRING_DA_BASE   (0x942f0000U)
#define VDEV0_VRING_SIZE      (0x00008000U)
#define VDEV0_VRING_NUM       (0x00000002U)

#define RESERVED_MEM_ADDR     (0x92400000U)
#define RESERVED_MEM_SIZE     (0x02000000U)
#define GLOBAL_DSP_MEM_ADDR   (RESERVED_MEM_ADDR + 0xF00000U)
#define GLOBAL_DSP_MEM_SIZE   (0x100000U)
#define SCRATCH_MEM_ADDR      (RESERVED_MEM_ADDR + 0x1000000U)
#define SCRATCH_MEM_SIZE      (0xef0000U)

#define RPMSG_LITE_SRTM_SHMEM_BASE  (VDEV0_VRING_DA_BASE)
#define RPMSG_LITE_SRTM_LINK_ID     (0U)

#define MUB_BASE              (MU_PADDR)
#define SYSTEM_CLOCK          (800000000UL)

#define UART_BASE             (0x30890000)
#define UART_CLK_ROOT         (24000000)

#define LPUART_BASE           (~0U)

#define I2C3_ADDR             0x30A40000
#define I2C_ADDR              I2C3_ADDR
#define I2C_CLK               (24000000UL)
/*
 * This limit caused by an i.MX7D hardware issue(e7805 in Errata).
 * If there is no limit, when the bitrate set up to 400KHz, it will
 * cause the SCK low level period less than 1.3us.
 */
#define I2C_BITRATE           (375000)

#define MICFIL_ADDR		0x30CA0000
#define MICFIL_VAD_INT		44
#define MICFIL_VADE_INT		45
#define MICFIL_INT		109
#define MICFIL_INTE		110

#define IRQSTR_MP_ADDR		0x30A80000
#define IRQ_STR_ADDR		IRQSTR_MP_ADDR

#define SAI_MP_ADDR		0x30c30000
#define SAI_MP_INT_NUM		50
#define SAI_ADDR		SAI_MP_ADDR
#define SAI_INT			SAI_MP_INT_NUM

#define EASRC_MP_ADDR		0x30c90000
#define EASRC_MP_INT_NUM	122
#define EASRC_ADDR		EASRC_MP_ADDR
#define EASRC_INT		EASRC_MP_INT_NUM

/* sdma2 not used in dsp */
#define SDMA2_ADDR		0x30e10000
#define SDMA2_INT_NUM		103
#define SDMA2_MICFIL_EVENTID	24

#define SDMA3_ADDR		0x30e00000
#define SDMA3_INT_NUM		34
#define SDMA3_MICFIL_EVENTID	24
#define SDMA_ADDR		SDMA3_ADDR
#define SDMA_INT		SDMA3_INT_NUM
#define SDMA_MICFIL_EVENT	SDMA3_MICFIL_EVENTID

/* not exist or not used hw */
#define EDMA_ADDR_ESAI_TX	0
#define EDMA_ADDR_ESAI_RX	0
#define EDMA_ADDR_ASRC_RXA	0
#define EDMA_ADDR_ASRC_TXA	0
#define EDMA_SAI_INT_NUM	0
#define EDMA_ESAI_INT_NUM	0
#define EDMA_ASRC_INT_NUM	0

#define ESAI_ADDR		0
#define ESAI_INT		0

#define ASRC_ADDR		0
#define ASRC_INT		0

#else /* PLATF_8M */
#ifdef PLATF_8ULP

#define BOARD_TYPE (DSP_IMX8ULP_TYPE)

#define I_CACHE_ATTRIBUTE      0x22222224     //write back mode
#define D_CACHE_ATTRIBUTE      0x22222221     //write through mode
#define INT_NUM_MU             15
#define MU_PADDR               0x2DA20000

/* remapping  0x8def0000U   -    0x19ef0000 */
#define VDEV0_VRING_SA_BASE   (0x8fef0000U)
#define VDEV0_VRING_DA_BASE   (0x1bef0000U)
#define VDEV0_VRING_SIZE      (0x00008000U)
#define VDEV0_VRING_NUM       (0x00000002U)

/* remapping  0x8e000000U   -    0x1a000000 */
#define RESERVED_MEM_ADDR     (0x1a000000U)
#define RESERVED_MEM_SIZE     (0x02000000U)
#define GLOBAL_DSP_MEM_ADDR   (RESERVED_MEM_ADDR + 0xF00000U)
#define GLOBAL_DSP_MEM_SIZE   (0x100000U)
#define SCRATCH_MEM_ADDR      (RESERVED_MEM_ADDR + 0x1000000U)
#define SCRATCH_MEM_SIZE      (0xef0000U)

#define RPMSG_LITE_SRTM_SHMEM_BASE  (VDEV0_VRING_DA_BASE)
#define RPMSG_LITE_SRTM_LINK_ID     (0U)

#define MUB_BASE              (MU_PADDR)
#define SYSTEM_CLOCK          (528000000UL)

/*lpuart6 for debug */
#define LPUART_BASE           (0x29860000)
#define UART_BASE             (~0U)
#define UART_CLK_ROOT         (48000000)

/* not exist or not used hw */
#define I2C_ADDR		0
#define I2C_CLK			0
#define I2C_BITRATE		(375000)

#define EDMA_ADDR_ESAI_TX	0
#define EDMA_ADDR_ESAI_RX	0
#define EDMA_ADDR_ASRC_RXA	0
#define EDMA_ADDR_ASRC_TXA	0
#define EDMA_SAI_INT_NUM	0
#define EDMA_ESAI_INT_NUM	0
#define EDMA_ASRC_INT_NUM	0

#define IRQ_STR_ADDR		0

#define SAI_ADDR		0
#define SAI_INT			0

#define ESAI_ADDR		0
#define ESAI_INT		0

#define SDMA_ADDR		0
#define SDMA_INT		0
#define SDMA_MICFIL_EVENT	0

#define EASRC_ADDR		0
#define EASRC_INT		0

#define ASRC_ADDR		0
#define ASRC_INT		0

#define MICFIL_ADDR		0
#define MICFIL_INT		0

#else /* !PLATF_8ULP && ! PLATF_8M  (8QXP || 8QM)*/

#define BOARD_TYPE (DSP_IMX8QXP_TYPE)

#define I_CACHE_ATTRIBUTE      0x22242224     //write back mode
#define D_CACHE_ATTRIBUTE      0x22212221     //write through mode
#define INT_NUM_MU             7
#define MU_PADDR               0x5D310000

#define VDEV0_VRING_SA_BASE   (0x942f0000U)
#define VDEV0_VRING_DA_BASE   (0x942f0000U)
#define VDEV0_VRING_SIZE      (0x00008000U)
#define VDEV0_VRING_NUM       (0x00000002U)

#define RESERVED_MEM_ADDR     (0x92400000U)
#define RESERVED_MEM_SIZE     (0x02000000U)
#define GLOBAL_DSP_MEM_ADDR   (RESERVED_MEM_ADDR + 0xF00000U)
#define GLOBAL_DSP_MEM_SIZE   (0x100000U)
#define SCRATCH_MEM_ADDR      (RESERVED_MEM_ADDR + 0x1000000U)
#define SCRATCH_MEM_SIZE      (0xef0000U)

#define RPMSG_LITE_SRTM_SHMEM_BASE  (VDEV0_VRING_DA_BASE)
#define RPMSG_LITE_SRTM_LINK_ID     (0U)

#define MUB_BASE              (MU_PADDR)
#define SYSTEM_CLOCK          (600000000UL)

#define LPUART_BASE           (0x5a090000)
#define UART_BASE             (~0U)
#define UART_CLK_ROOT         (80000000)

#define IRQSTR_QXP_ADDR		0x51080000
#define IRQSTR_QM_ADDR		0x510A0000
#define IRQ_STR_ADDR		IRQSTR_QXP_ADDR

#define SAI0_ADDR		0x59040000
#define SAI0_INT		314
#define SAI_ADDR		SAI0_ADDR
#define SAI_INT			SAI0_INT

#define ESAI_ADDR		0x59010000
#define ESAI_INT		409

#define EDMA_ADDR_ESAI_TX	0x59270000
#define EDMA_ADDR_ESAI_RX	0x59260000
#define EDMA_ADDR_ASRC_RXA	0x59200000
#define EDMA_ADDR_ASRC_TXA	0x59230000
#define EDMA_SAI_INT_NUM	315
#define EDMA_ESAI_INT_NUM	410
#define EDMA_ASRC_INT_NUM	374

#define ASRC_ADDR		0x59000000
#define ASRC_INT		372

/* not exist or not used hw */
#define I2C_ADDR		0
#define I2C_CLK			0
#define I2C_BITRATE		(375000)

#define EASRC_ADDR		0
#define EASRC_INT		0

#define MICFIL_ADDR		0
#define MICFIL_INT		0

#define SDMA_ADDR		0
#define SDMA_INT		0
#define SDMA_MICFIL_EVENT	0

#endif /*PLATF_8ULP */
#endif /*PLATF_8M */

#define INT_NUM_IRQSTR_DSP_0   19
#define INT_NUM_IRQSTR_DSP_1   20
#define INT_NUM_IRQSTR_DSP_2   21
#define INT_NUM_IRQSTR_DSP_3   22
#define INT_NUM_IRQSTR_DSP_4   23
#define INT_NUM_IRQSTR_DSP_5   24
#define INT_NUM_IRQSTR_DSP_6   25
#define INT_NUM_IRQSTR_DSP_7   26

#endif /* _BOARD_H_ */
