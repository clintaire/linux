/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IOMMU API for ARM architected SMMUv3 implementations.
 *
 * Copyright (C) 2015 ARM Limited
 */

#ifndef _ARM_SMMU_V3_H
#define _ARM_SMMU_V3_H

#include <linux/bitfield.h>
#include <linux/iommu.h>
#include <linux/iommufd.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/sizes.h>

struct arm_smmu_device;

/* MMIO registers */
#define ARM_SMMU_IDR0			0x0
#define IDR0_ST_LVL			GENMASK(28, 27)
#define IDR0_ST_LVL_2LVL		1
#define IDR0_STALL_MODEL		GENMASK(25, 24)
#define IDR0_STALL_MODEL_STALL		0
#define IDR0_STALL_MODEL_FORCE		2
#define IDR0_TTENDIAN			GENMASK(22, 21)
#define IDR0_TTENDIAN_MIXED		0
#define IDR0_TTENDIAN_LE		2
#define IDR0_TTENDIAN_BE		3
#define IDR0_CD2L			(1 << 19)
#define IDR0_VMID16			(1 << 18)
#define IDR0_PRI			(1 << 16)
#define IDR0_SEV			(1 << 14)
#define IDR0_MSI			(1 << 13)
#define IDR0_ASID16			(1 << 12)
#define IDR0_ATS			(1 << 10)
#define IDR0_HYP			(1 << 9)
#define IDR0_HTTU			GENMASK(7, 6)
#define IDR0_HTTU_ACCESS		1
#define IDR0_HTTU_ACCESS_DIRTY		2
#define IDR0_COHACC			(1 << 4)
#define IDR0_TTF			GENMASK(3, 2)
#define IDR0_TTF_AARCH64		2
#define IDR0_TTF_AARCH32_64		3
#define IDR0_S1P			(1 << 1)
#define IDR0_S2P			(1 << 0)

#define ARM_SMMU_IDR1			0x4
#define IDR1_TABLES_PRESET		(1 << 30)
#define IDR1_QUEUES_PRESET		(1 << 29)
#define IDR1_REL			(1 << 28)
#define IDR1_ATTR_TYPES_OVR		(1 << 27)
#define IDR1_CMDQS			GENMASK(25, 21)
#define IDR1_EVTQS			GENMASK(20, 16)
#define IDR1_PRIQS			GENMASK(15, 11)
#define IDR1_SSIDSIZE			GENMASK(10, 6)
#define IDR1_SIDSIZE			GENMASK(5, 0)

#define ARM_SMMU_IDR3			0xc
#define IDR3_FWB			(1 << 8)
#define IDR3_RIL			(1 << 10)
#define IDR3_BBM			GENMASK(12, 11)

#define ARM_SMMU_IDR5			0x14
#define IDR5_STALL_MAX			GENMASK(31, 16)
#define IDR5_GRAN64K			(1 << 6)
#define IDR5_GRAN16K			(1 << 5)
#define IDR5_GRAN4K			(1 << 4)
#define IDR5_OAS			GENMASK(2, 0)
#define IDR5_OAS_32_BIT			0
#define IDR5_OAS_36_BIT			1
#define IDR5_OAS_40_BIT			2
#define IDR5_OAS_42_BIT			3
#define IDR5_OAS_44_BIT			4
#define IDR5_OAS_48_BIT			5
#define IDR5_OAS_52_BIT			6
#define IDR5_VAX			GENMASK(11, 10)
#define IDR5_VAX_52_BIT			1

#define ARM_SMMU_IIDR			0x18
#define IIDR_PRODUCTID			GENMASK(31, 20)
#define IIDR_VARIANT			GENMASK(19, 16)
#define IIDR_REVISION			GENMASK(15, 12)
#define IIDR_IMPLEMENTER		GENMASK(11, 0)

#define ARM_SMMU_AIDR			0x1C

#define ARM_SMMU_CR0			0x20
#define CR0_ATSCHK			(1 << 4)
#define CR0_CMDQEN			(1 << 3)
#define CR0_EVTQEN			(1 << 2)
#define CR0_PRIQEN			(1 << 1)
#define CR0_SMMUEN			(1 << 0)

#define ARM_SMMU_CR0ACK			0x24

#define ARM_SMMU_CR1			0x28
#define CR1_TABLE_SH			GENMASK(11, 10)
#define CR1_TABLE_OC			GENMASK(9, 8)
#define CR1_TABLE_IC			GENMASK(7, 6)
#define CR1_QUEUE_SH			GENMASK(5, 4)
#define CR1_QUEUE_OC			GENMASK(3, 2)
#define CR1_QUEUE_IC			GENMASK(1, 0)
/* CR1 cacheability fields don't quite follow the usual TCR-style encoding */
#define CR1_CACHE_NC			0
#define CR1_CACHE_WB			1
#define CR1_CACHE_WT			2

#define ARM_SMMU_CR2			0x2c
#define CR2_PTM				(1 << 2)
#define CR2_RECINVSID			(1 << 1)
#define CR2_E2H				(1 << 0)

#define ARM_SMMU_GBPA			0x44
#define GBPA_UPDATE			(1 << 31)
#define GBPA_ABORT			(1 << 20)

#define ARM_SMMU_IRQ_CTRL		0x50
#define IRQ_CTRL_EVTQ_IRQEN		(1 << 2)
#define IRQ_CTRL_PRIQ_IRQEN		(1 << 1)
#define IRQ_CTRL_GERROR_IRQEN		(1 << 0)

#define ARM_SMMU_IRQ_CTRLACK		0x54

#define ARM_SMMU_GERROR			0x60
#define GERROR_SFM_ERR			(1 << 8)
#define GERROR_MSI_GERROR_ABT_ERR	(1 << 7)
#define GERROR_MSI_PRIQ_ABT_ERR		(1 << 6)
#define GERROR_MSI_EVTQ_ABT_ERR		(1 << 5)
#define GERROR_MSI_CMDQ_ABT_ERR		(1 << 4)
#define GERROR_PRIQ_ABT_ERR		(1 << 3)
#define GERROR_EVTQ_ABT_ERR		(1 << 2)
#define GERROR_CMDQ_ERR			(1 << 0)
#define GERROR_ERR_MASK			0x1fd

#define ARM_SMMU_GERRORN		0x64

#define ARM_SMMU_GERROR_IRQ_CFG0	0x68
#define ARM_SMMU_GERROR_IRQ_CFG1	0x70
#define ARM_SMMU_GERROR_IRQ_CFG2	0x74

#define ARM_SMMU_STRTAB_BASE		0x80
#define STRTAB_BASE_RA			(1UL << 62)
#define STRTAB_BASE_ADDR_MASK		GENMASK_ULL(51, 6)

#define ARM_SMMU_STRTAB_BASE_CFG	0x88
#define STRTAB_BASE_CFG_FMT		GENMASK(17, 16)
#define STRTAB_BASE_CFG_FMT_LINEAR	0
#define STRTAB_BASE_CFG_FMT_2LVL	1
#define STRTAB_BASE_CFG_SPLIT		GENMASK(10, 6)
#define STRTAB_BASE_CFG_LOG2SIZE	GENMASK(5, 0)

#define ARM_SMMU_CMDQ_BASE		0x90
#define ARM_SMMU_CMDQ_PROD		0x98
#define ARM_SMMU_CMDQ_CONS		0x9c

#define ARM_SMMU_EVTQ_BASE		0xa0
#define ARM_SMMU_EVTQ_PROD		0xa8
#define ARM_SMMU_EVTQ_CONS		0xac
#define ARM_SMMU_EVTQ_IRQ_CFG0		0xb0
#define ARM_SMMU_EVTQ_IRQ_CFG1		0xb8
#define ARM_SMMU_EVTQ_IRQ_CFG2		0xbc

#define ARM_SMMU_PRIQ_BASE		0xc0
#define ARM_SMMU_PRIQ_PROD		0xc8
#define ARM_SMMU_PRIQ_CONS		0xcc
#define ARM_SMMU_PRIQ_IRQ_CFG0		0xd0
#define ARM_SMMU_PRIQ_IRQ_CFG1		0xd8
#define ARM_SMMU_PRIQ_IRQ_CFG2		0xdc

#define ARM_SMMU_REG_SZ			0xe00

/* Common MSI config fields */
#define MSI_CFG0_ADDR_MASK		GENMASK_ULL(51, 2)
#define MSI_CFG2_SH			GENMASK(5, 4)
#define MSI_CFG2_MEMATTR		GENMASK(3, 0)

/* Common memory attribute values */
#define ARM_SMMU_SH_NSH			0
#define ARM_SMMU_SH_OSH			2
#define ARM_SMMU_SH_ISH			3
#define ARM_SMMU_MEMATTR_DEVICE_nGnRE	0x1
#define ARM_SMMU_MEMATTR_OIWB		0xf

#define Q_IDX(llq, p)			((p) & ((1 << (llq)->max_n_shift) - 1))
#define Q_WRP(llq, p)			((p) & (1 << (llq)->max_n_shift))
#define Q_OVERFLOW_FLAG			(1U << 31)
#define Q_OVF(p)			((p) & Q_OVERFLOW_FLAG)
#define Q_ENT(q, p)			((q)->base +			\
					 Q_IDX(&((q)->llq), p) *	\
					 (q)->ent_dwords)

#define Q_BASE_RWA			(1UL << 62)
#define Q_BASE_ADDR_MASK		GENMASK_ULL(51, 5)
#define Q_BASE_LOG2SIZE			GENMASK(4, 0)

/* Ensure DMA allocations are naturally aligned */
#ifdef CONFIG_CMA_ALIGNMENT
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + CONFIG_CMA_ALIGNMENT)
#else
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + MAX_PAGE_ORDER)
#endif

/*
 * Stream table.
 *
 * Linear: Enough to cover 1 << IDR1.SIDSIZE entries
 * 2lvl: 128k L1 entries,
 *       256 lazy entries per table (each table covers a PCI bus)
 */
#define STRTAB_SPLIT			8

#define STRTAB_L1_DESC_SPAN		GENMASK_ULL(4, 0)
#define STRTAB_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 6)

#define STRTAB_STE_DWORDS		8

struct arm_smmu_ste {
	__le64 data[STRTAB_STE_DWORDS];
};

#define STRTAB_NUM_L2_STES		(1 << STRTAB_SPLIT)
struct arm_smmu_strtab_l2 {
	struct arm_smmu_ste stes[STRTAB_NUM_L2_STES];
};

struct arm_smmu_strtab_l1 {
	__le64 l2ptr;
};
#define STRTAB_MAX_L1_ENTRIES		(1 << 17)

static inline u32 arm_smmu_strtab_l1_idx(u32 sid)
{
	return sid / STRTAB_NUM_L2_STES;
}

static inline u32 arm_smmu_strtab_l2_idx(u32 sid)
{
	return sid % STRTAB_NUM_L2_STES;
}

#define STRTAB_STE_0_V			(1UL << 0)
#define STRTAB_STE_0_CFG		GENMASK_ULL(3, 1)
#define STRTAB_STE_0_CFG_ABORT		0
#define STRTAB_STE_0_CFG_BYPASS		4
#define STRTAB_STE_0_CFG_S1_TRANS	5
#define STRTAB_STE_0_CFG_S2_TRANS	6
#define STRTAB_STE_0_CFG_NESTED		7

#define STRTAB_STE_0_S1FMT		GENMASK_ULL(5, 4)
#define STRTAB_STE_0_S1FMT_LINEAR	0
#define STRTAB_STE_0_S1FMT_64K_L2	2
#define STRTAB_STE_0_S1CTXPTR_MASK	GENMASK_ULL(51, 6)
#define STRTAB_STE_0_S1CDMAX		GENMASK_ULL(63, 59)

#define STRTAB_STE_1_S1DSS		GENMASK_ULL(1, 0)
#define STRTAB_STE_1_S1DSS_TERMINATE	0x0
#define STRTAB_STE_1_S1DSS_BYPASS	0x1
#define STRTAB_STE_1_S1DSS_SSID0	0x2

#define STRTAB_STE_1_S1C_CACHE_NC	0UL
#define STRTAB_STE_1_S1C_CACHE_WBRA	1UL
#define STRTAB_STE_1_S1C_CACHE_WT	2UL
#define STRTAB_STE_1_S1C_CACHE_WB	3UL
#define STRTAB_STE_1_S1CIR		GENMASK_ULL(3, 2)
#define STRTAB_STE_1_S1COR		GENMASK_ULL(5, 4)
#define STRTAB_STE_1_S1CSH		GENMASK_ULL(7, 6)

#define STRTAB_STE_1_MEV		(1UL << 19)
#define STRTAB_STE_1_S2FWB		(1UL << 25)
#define STRTAB_STE_1_S1STALLD		(1UL << 27)

#define STRTAB_STE_1_EATS		GENMASK_ULL(29, 28)
#define STRTAB_STE_1_EATS_ABT		0UL
#define STRTAB_STE_1_EATS_TRANS		1UL
#define STRTAB_STE_1_EATS_S1CHK		2UL

#define STRTAB_STE_1_STRW		GENMASK_ULL(31, 30)
#define STRTAB_STE_1_STRW_NSEL1		0UL
#define STRTAB_STE_1_STRW_EL2		2UL

#define STRTAB_STE_1_SHCFG		GENMASK_ULL(45, 44)
#define STRTAB_STE_1_SHCFG_INCOMING	1UL

#define STRTAB_STE_2_S2VMID		GENMASK_ULL(15, 0)
#define STRTAB_STE_2_VTCR		GENMASK_ULL(50, 32)
#define STRTAB_STE_2_VTCR_S2T0SZ	GENMASK_ULL(5, 0)
#define STRTAB_STE_2_VTCR_S2SL0		GENMASK_ULL(7, 6)
#define STRTAB_STE_2_VTCR_S2IR0		GENMASK_ULL(9, 8)
#define STRTAB_STE_2_VTCR_S2OR0		GENMASK_ULL(11, 10)
#define STRTAB_STE_2_VTCR_S2SH0		GENMASK_ULL(13, 12)
#define STRTAB_STE_2_VTCR_S2TG		GENMASK_ULL(15, 14)
#define STRTAB_STE_2_VTCR_S2PS		GENMASK_ULL(18, 16)
#define STRTAB_STE_2_S2AA64		(1UL << 51)
#define STRTAB_STE_2_S2ENDI		(1UL << 52)
#define STRTAB_STE_2_S2PTW		(1UL << 54)
#define STRTAB_STE_2_S2S		(1UL << 57)
#define STRTAB_STE_2_S2R		(1UL << 58)

#define STRTAB_STE_3_S2TTB_MASK		GENMASK_ULL(51, 4)

/* These bits can be controlled by userspace for STRTAB_STE_0_CFG_NESTED */
#define STRTAB_STE_0_NESTING_ALLOWED                                         \
	cpu_to_le64(STRTAB_STE_0_V | STRTAB_STE_0_CFG | STRTAB_STE_0_S1FMT | \
		    STRTAB_STE_0_S1CTXPTR_MASK | STRTAB_STE_0_S1CDMAX)
#define STRTAB_STE_1_NESTING_ALLOWED                            \
	cpu_to_le64(STRTAB_STE_1_S1DSS | STRTAB_STE_1_S1CIR |   \
		    STRTAB_STE_1_S1COR | STRTAB_STE_1_S1CSH |   \
		    STRTAB_STE_1_S1STALLD | STRTAB_STE_1_EATS)

/*
 * Context descriptors.
 *
 * Linear: when less than 1024 SSIDs are supported
 * 2lvl: at most 1024 L1 entries,
 *       1024 lazy entries per table.
 */
#define CTXDESC_L2_ENTRIES		1024

#define CTXDESC_L1_DESC_V		(1UL << 0)
#define CTXDESC_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 12)

#define CTXDESC_CD_DWORDS		8

struct arm_smmu_cd {
	__le64 data[CTXDESC_CD_DWORDS];
};

struct arm_smmu_cdtab_l2 {
	struct arm_smmu_cd cds[CTXDESC_L2_ENTRIES];
};

struct arm_smmu_cdtab_l1 {
	__le64 l2ptr;
};

static inline unsigned int arm_smmu_cdtab_l1_idx(unsigned int ssid)
{
	return ssid / CTXDESC_L2_ENTRIES;
}

static inline unsigned int arm_smmu_cdtab_l2_idx(unsigned int ssid)
{
	return ssid % CTXDESC_L2_ENTRIES;
}

#define CTXDESC_CD_0_TCR_T0SZ		GENMASK_ULL(5, 0)
#define CTXDESC_CD_0_TCR_TG0		GENMASK_ULL(7, 6)
#define CTXDESC_CD_0_TCR_IRGN0		GENMASK_ULL(9, 8)
#define CTXDESC_CD_0_TCR_ORGN0		GENMASK_ULL(11, 10)
#define CTXDESC_CD_0_TCR_SH0		GENMASK_ULL(13, 12)
#define CTXDESC_CD_0_TCR_EPD0		(1ULL << 14)
#define CTXDESC_CD_0_TCR_EPD1		(1ULL << 30)

#define CTXDESC_CD_0_ENDI		(1UL << 15)
#define CTXDESC_CD_0_V			(1UL << 31)

#define CTXDESC_CD_0_TCR_IPS		GENMASK_ULL(34, 32)
#define CTXDESC_CD_0_TCR_TBI0		(1ULL << 38)

#define CTXDESC_CD_0_TCR_HA            (1UL << 43)
#define CTXDESC_CD_0_TCR_HD            (1UL << 42)

#define CTXDESC_CD_0_AA64		(1UL << 41)
#define CTXDESC_CD_0_S			(1UL << 44)
#define CTXDESC_CD_0_R			(1UL << 45)
#define CTXDESC_CD_0_A			(1UL << 46)
#define CTXDESC_CD_0_ASET		(1UL << 47)
#define CTXDESC_CD_0_ASID		GENMASK_ULL(63, 48)

#define CTXDESC_CD_1_TTB0_MASK		GENMASK_ULL(51, 4)

/*
 * When the SMMU only supports linear context descriptor tables, pick a
 * reasonable size limit (64kB).
 */
#define CTXDESC_LINEAR_CDMAX		ilog2(SZ_64K / sizeof(struct arm_smmu_cd))

/* Command queue */
#define CMDQ_ENT_SZ_SHIFT		4
#define CMDQ_ENT_DWORDS			((1 << CMDQ_ENT_SZ_SHIFT) >> 3)
#define CMDQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - CMDQ_ENT_SZ_SHIFT)

#define CMDQ_CONS_ERR			GENMASK(30, 24)
#define CMDQ_ERR_CERROR_NONE_IDX	0
#define CMDQ_ERR_CERROR_ILL_IDX		1
#define CMDQ_ERR_CERROR_ABT_IDX		2
#define CMDQ_ERR_CERROR_ATC_INV_IDX	3

#define CMDQ_PROD_OWNED_FLAG		Q_OVERFLOW_FLAG

/*
 * This is used to size the command queue and therefore must be at least
 * BITS_PER_LONG so that the valid_map works correctly (it relies on the
 * total number of queue entries being a multiple of BITS_PER_LONG).
 */
#define CMDQ_BATCH_ENTRIES		BITS_PER_LONG

#define CMDQ_0_OP			GENMASK_ULL(7, 0)
#define CMDQ_0_SSV			(1UL << 11)

#define CMDQ_PREFETCH_0_SID		GENMASK_ULL(63, 32)
#define CMDQ_PREFETCH_1_SIZE		GENMASK_ULL(4, 0)
#define CMDQ_PREFETCH_1_ADDR_MASK	GENMASK_ULL(63, 12)

#define CMDQ_CFGI_0_SSID		GENMASK_ULL(31, 12)
#define CMDQ_CFGI_0_SID			GENMASK_ULL(63, 32)
#define CMDQ_CFGI_1_LEAF		(1UL << 0)
#define CMDQ_CFGI_1_RANGE		GENMASK_ULL(4, 0)

#define CMDQ_TLBI_0_NUM			GENMASK_ULL(16, 12)
#define CMDQ_TLBI_RANGE_NUM_MAX		31
#define CMDQ_TLBI_0_SCALE		GENMASK_ULL(24, 20)
#define CMDQ_TLBI_0_VMID		GENMASK_ULL(47, 32)
#define CMDQ_TLBI_0_ASID		GENMASK_ULL(63, 48)
#define CMDQ_TLBI_1_LEAF		(1UL << 0)
#define CMDQ_TLBI_1_TTL			GENMASK_ULL(9, 8)
#define CMDQ_TLBI_1_TG			GENMASK_ULL(11, 10)
#define CMDQ_TLBI_1_VA_MASK		GENMASK_ULL(63, 12)
#define CMDQ_TLBI_1_IPA_MASK		GENMASK_ULL(51, 12)

#define CMDQ_ATC_0_SSID			GENMASK_ULL(31, 12)
#define CMDQ_ATC_0_SID			GENMASK_ULL(63, 32)
#define CMDQ_ATC_0_GLOBAL		(1UL << 9)
#define CMDQ_ATC_1_SIZE			GENMASK_ULL(5, 0)
#define CMDQ_ATC_1_ADDR_MASK		GENMASK_ULL(63, 12)

#define CMDQ_PRI_0_SSID			GENMASK_ULL(31, 12)
#define CMDQ_PRI_0_SID			GENMASK_ULL(63, 32)
#define CMDQ_PRI_1_GRPID		GENMASK_ULL(8, 0)
#define CMDQ_PRI_1_RESP			GENMASK_ULL(13, 12)

#define CMDQ_RESUME_0_RESP_TERM		0UL
#define CMDQ_RESUME_0_RESP_RETRY	1UL
#define CMDQ_RESUME_0_RESP_ABORT	2UL
#define CMDQ_RESUME_0_RESP		GENMASK_ULL(13, 12)
#define CMDQ_RESUME_0_SID		GENMASK_ULL(63, 32)
#define CMDQ_RESUME_1_STAG		GENMASK_ULL(15, 0)

#define CMDQ_SYNC_0_CS			GENMASK_ULL(13, 12)
#define CMDQ_SYNC_0_CS_NONE		0
#define CMDQ_SYNC_0_CS_IRQ		1
#define CMDQ_SYNC_0_CS_SEV		2
#define CMDQ_SYNC_0_MSH			GENMASK_ULL(23, 22)
#define CMDQ_SYNC_0_MSIATTR		GENMASK_ULL(27, 24)
#define CMDQ_SYNC_0_MSIDATA		GENMASK_ULL(63, 32)
#define CMDQ_SYNC_1_MSIADDR_MASK	GENMASK_ULL(51, 2)

/* Event queue */
#define EVTQ_ENT_SZ_SHIFT		5
#define EVTQ_ENT_DWORDS			((1 << EVTQ_ENT_SZ_SHIFT) >> 3)
#define EVTQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - EVTQ_ENT_SZ_SHIFT)

#define EVTQ_0_ID			GENMASK_ULL(7, 0)

#define EVT_ID_BAD_STREAMID_CONFIG	0x02
#define EVT_ID_STE_FETCH_FAULT		0x03
#define EVT_ID_BAD_STE_CONFIG		0x04
#define EVT_ID_STREAM_DISABLED_FAULT	0x06
#define EVT_ID_BAD_SUBSTREAMID_CONFIG	0x08
#define EVT_ID_CD_FETCH_FAULT		0x09
#define EVT_ID_BAD_CD_CONFIG		0x0a
#define EVT_ID_TRANSLATION_FAULT	0x10
#define EVT_ID_ADDR_SIZE_FAULT		0x11
#define EVT_ID_ACCESS_FAULT		0x12
#define EVT_ID_PERMISSION_FAULT		0x13
#define EVT_ID_VMS_FETCH_FAULT		0x25

#define EVTQ_0_SSV			(1UL << 11)
#define EVTQ_0_SSID			GENMASK_ULL(31, 12)
#define EVTQ_0_SID			GENMASK_ULL(63, 32)
#define EVTQ_1_STAG			GENMASK_ULL(15, 0)
#define EVTQ_1_STALL			(1UL << 31)
#define EVTQ_1_PnU			(1UL << 33)
#define EVTQ_1_InD			(1UL << 34)
#define EVTQ_1_RnW			(1UL << 35)
#define EVTQ_1_S2			(1UL << 39)
#define EVTQ_1_CLASS			GENMASK_ULL(41, 40)
#define EVTQ_1_CLASS_TT			0x01
#define EVTQ_1_TT_READ			(1UL << 44)
#define EVTQ_2_ADDR			GENMASK_ULL(63, 0)
#define EVTQ_3_IPA			GENMASK_ULL(51, 12)
#define EVTQ_3_FETCH_ADDR		GENMASK_ULL(51, 3)

/* PRI queue */
#define PRIQ_ENT_SZ_SHIFT		4
#define PRIQ_ENT_DWORDS			((1 << PRIQ_ENT_SZ_SHIFT) >> 3)
#define PRIQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - PRIQ_ENT_SZ_SHIFT)

#define PRIQ_0_SID			GENMASK_ULL(31, 0)
#define PRIQ_0_SSID			GENMASK_ULL(51, 32)
#define PRIQ_0_PERM_PRIV		(1UL << 58)
#define PRIQ_0_PERM_EXEC		(1UL << 59)
#define PRIQ_0_PERM_READ		(1UL << 60)
#define PRIQ_0_PERM_WRITE		(1UL << 61)
#define PRIQ_0_PRG_LAST			(1UL << 62)
#define PRIQ_0_SSID_V			(1UL << 63)

#define PRIQ_1_PRG_IDX			GENMASK_ULL(8, 0)
#define PRIQ_1_ADDR_MASK		GENMASK_ULL(63, 12)

/* High-level queue structures */
#define ARM_SMMU_POLL_TIMEOUT_US	1000000 /* 1s! */
#define ARM_SMMU_POLL_SPIN_COUNT	10

#define MSI_IOVA_BASE			0x8000000
#define MSI_IOVA_LENGTH			0x100000

enum pri_resp {
	PRI_RESP_DENY = 0,
	PRI_RESP_FAIL = 1,
	PRI_RESP_SUCC = 2,
};

struct arm_smmu_cmdq_ent {
	/* Common fields */
	u8				opcode;
	bool				substream_valid;

	/* Command-specific fields */
	union {
		#define CMDQ_OP_PREFETCH_CFG	0x1
		struct {
			u32			sid;
		} prefetch;

		#define CMDQ_OP_CFGI_STE	0x3
		#define CMDQ_OP_CFGI_ALL	0x4
		#define CMDQ_OP_CFGI_CD		0x5
		#define CMDQ_OP_CFGI_CD_ALL	0x6
		struct {
			u32			sid;
			u32			ssid;
			union {
				bool		leaf;
				u8		span;
			};
		} cfgi;

		#define CMDQ_OP_TLBI_NH_ALL     0x10
		#define CMDQ_OP_TLBI_NH_ASID	0x11
		#define CMDQ_OP_TLBI_NH_VA	0x12
		#define CMDQ_OP_TLBI_NH_VAA	0x13
		#define CMDQ_OP_TLBI_EL2_ALL	0x20
		#define CMDQ_OP_TLBI_EL2_ASID	0x21
		#define CMDQ_OP_TLBI_EL2_VA	0x22
		#define CMDQ_OP_TLBI_S12_VMALL	0x28
		#define CMDQ_OP_TLBI_S2_IPA	0x2a
		#define CMDQ_OP_TLBI_NSNH_ALL	0x30
		struct {
			u8			num;
			u8			scale;
			u16			asid;
			u16			vmid;
			bool			leaf;
			u8			ttl;
			u8			tg;
			u64			addr;
		} tlbi;

		#define CMDQ_OP_ATC_INV		0x40
		#define ATC_INV_SIZE_ALL	52
		struct {
			u32			sid;
			u32			ssid;
			u64			addr;
			u8			size;
			bool			global;
		} atc;

		#define CMDQ_OP_PRI_RESP	0x41
		struct {
			u32			sid;
			u32			ssid;
			u16			grpid;
			enum pri_resp		resp;
		} pri;

		#define CMDQ_OP_RESUME		0x44
		struct {
			u32			sid;
			u16			stag;
			u8			resp;
		} resume;

		#define CMDQ_OP_CMD_SYNC	0x46
		struct {
			u64			msiaddr;
		} sync;
	};
};

struct arm_smmu_ll_queue {
	union {
		u64			val;
		struct {
			u32		prod;
			u32		cons;
		};
		struct {
			atomic_t	prod;
			atomic_t	cons;
		} atomic;
		u8			__pad[SMP_CACHE_BYTES];
	} ____cacheline_aligned_in_smp;
	u32				max_n_shift;
};

struct arm_smmu_queue {
	struct arm_smmu_ll_queue	llq;
	int				irq; /* Wired interrupt */

	__le64				*base;
	dma_addr_t			base_dma;
	u64				q_base;

	size_t				ent_dwords;

	u32 __iomem			*prod_reg;
	u32 __iomem			*cons_reg;
};

struct arm_smmu_queue_poll {
	ktime_t				timeout;
	unsigned int			delay;
	unsigned int			spin_cnt;
	bool				wfe;
};

struct arm_smmu_cmdq {
	struct arm_smmu_queue		q;
	atomic_long_t			*valid_map;
	atomic_t			owner_prod;
	atomic_t			lock;
	bool				(*supports_cmd)(struct arm_smmu_cmdq_ent *ent);
};

static inline bool arm_smmu_cmdq_supports_cmd(struct arm_smmu_cmdq *cmdq,
					      struct arm_smmu_cmdq_ent *ent)
{
	return cmdq->supports_cmd ? cmdq->supports_cmd(ent) : true;
}

struct arm_smmu_cmdq_batch {
	u64				cmds[CMDQ_BATCH_ENTRIES * CMDQ_ENT_DWORDS];
	struct arm_smmu_cmdq		*cmdq;
	int				num;
};

struct arm_smmu_evtq {
	struct arm_smmu_queue		q;
	struct iopf_queue		*iopf;
	u32				max_stalls;
};

struct arm_smmu_priq {
	struct arm_smmu_queue		q;
};

/* High-level stream table and context descriptor structures */
struct arm_smmu_ctx_desc {
	u16				asid;
};

struct arm_smmu_ctx_desc_cfg {
	union {
		struct {
			struct arm_smmu_cd *table;
			unsigned int num_ents;
		} linear;
		struct {
			struct arm_smmu_cdtab_l1 *l1tab;
			struct arm_smmu_cdtab_l2 **l2ptrs;
			unsigned int num_l1_ents;
		} l2;
	};
	dma_addr_t			cdtab_dma;
	unsigned int			used_ssids;
	u8				in_ste;
	u8				s1fmt;
	/* log2 of the maximum number of CDs supported by this table */
	u8				s1cdmax;
};

static inline bool
arm_smmu_cdtab_allocated(struct arm_smmu_ctx_desc_cfg *cfg)
{
	return cfg->linear.table || cfg->l2.l1tab;
}

/* True if the cd table has SSIDS > 0 in use. */
static inline bool arm_smmu_ssids_in_use(struct arm_smmu_ctx_desc_cfg *cd_table)
{
	return cd_table->used_ssids;
}

struct arm_smmu_s2_cfg {
	u16				vmid;
};

struct arm_smmu_strtab_cfg {
	union {
		struct {
			struct arm_smmu_ste *table;
			dma_addr_t ste_dma;
			unsigned int num_ents;
		} linear;
		struct {
			struct arm_smmu_strtab_l1 *l1tab;
			struct arm_smmu_strtab_l2 **l2ptrs;
			dma_addr_t l1_dma;
			unsigned int num_l1_ents;
		} l2;
	};
};

struct arm_smmu_impl_ops {
	int (*device_reset)(struct arm_smmu_device *smmu);
	void (*device_remove)(struct arm_smmu_device *smmu);
	int (*init_structures)(struct arm_smmu_device *smmu);
	struct arm_smmu_cmdq *(*get_secondary_cmdq)(
		struct arm_smmu_device *smmu, struct arm_smmu_cmdq_ent *ent);
};

/* An SMMUv3 instance */
struct arm_smmu_device {
	struct device			*dev;
	struct device			*impl_dev;
	const struct arm_smmu_impl_ops	*impl_ops;

	void __iomem			*base;
	void __iomem			*page1;

#define ARM_SMMU_FEAT_2_LVL_STRTAB	(1 << 0)
#define ARM_SMMU_FEAT_2_LVL_CDTAB	(1 << 1)
#define ARM_SMMU_FEAT_TT_LE		(1 << 2)
#define ARM_SMMU_FEAT_TT_BE		(1 << 3)
#define ARM_SMMU_FEAT_PRI		(1 << 4)
#define ARM_SMMU_FEAT_ATS		(1 << 5)
#define ARM_SMMU_FEAT_SEV		(1 << 6)
#define ARM_SMMU_FEAT_MSI		(1 << 7)
#define ARM_SMMU_FEAT_COHERENCY		(1 << 8)
#define ARM_SMMU_FEAT_TRANS_S1		(1 << 9)
#define ARM_SMMU_FEAT_TRANS_S2		(1 << 10)
#define ARM_SMMU_FEAT_STALLS		(1 << 11)
#define ARM_SMMU_FEAT_HYP		(1 << 12)
#define ARM_SMMU_FEAT_STALL_FORCE	(1 << 13)
#define ARM_SMMU_FEAT_VAX		(1 << 14)
#define ARM_SMMU_FEAT_RANGE_INV		(1 << 15)
#define ARM_SMMU_FEAT_BTM		(1 << 16)
#define ARM_SMMU_FEAT_SVA		(1 << 17)
#define ARM_SMMU_FEAT_E2H		(1 << 18)
#define ARM_SMMU_FEAT_NESTING		(1 << 19)
#define ARM_SMMU_FEAT_ATTR_TYPES_OVR	(1 << 20)
#define ARM_SMMU_FEAT_HA		(1 << 21)
#define ARM_SMMU_FEAT_HD		(1 << 22)
#define ARM_SMMU_FEAT_S2FWB		(1 << 23)
#define ARM_SMMU_FEAT_BBML2		(1 << 24)
	u32				features;

#define ARM_SMMU_OPT_SKIP_PREFETCH	(1 << 0)
#define ARM_SMMU_OPT_PAGE0_REGS_ONLY	(1 << 1)
#define ARM_SMMU_OPT_MSIPOLL		(1 << 2)
#define ARM_SMMU_OPT_CMDQ_FORCE_SYNC	(1 << 3)
#define ARM_SMMU_OPT_TEGRA241_CMDQV	(1 << 4)
	u32				options;

	struct arm_smmu_cmdq		cmdq;
	struct arm_smmu_evtq		evtq;
	struct arm_smmu_priq		priq;

	int				gerr_irq;
	int				combined_irq;

	unsigned long			ias; /* IPA */
	unsigned long			oas; /* PA */
	unsigned long			pgsize_bitmap;

#define ARM_SMMU_MAX_ASIDS		(1 << 16)
	unsigned int			asid_bits;

#define ARM_SMMU_MAX_VMIDS		(1 << 16)
	unsigned int			vmid_bits;
	struct ida			vmid_map;

	unsigned int			ssid_bits;
	unsigned int			sid_bits;

	struct arm_smmu_strtab_cfg	strtab_cfg;

	/* IOMMU core code handle */
	struct iommu_device		iommu;

	struct rb_root			streams;
	struct mutex			streams_mutex;
};

struct arm_smmu_stream {
	u32				id;
	struct arm_smmu_master		*master;
	struct rb_node			node;
};

struct arm_smmu_vmaster {
	struct arm_vsmmu		*vsmmu;
	unsigned long			vsid;
};

struct arm_smmu_event {
	u8				stall : 1,
					ssv : 1,
					privileged : 1,
					instruction : 1,
					s2 : 1,
					read : 1,
					ttrnw : 1,
					class_tt : 1;
	u8				id;
	u8				class;
	u16				stag;
	u32				sid;
	u32				ssid;
	u64				iova;
	u64				ipa;
	u64				fetch_addr;
	struct device			*dev;
};

/* SMMU private data for each master */
struct arm_smmu_master {
	struct arm_smmu_device		*smmu;
	struct device			*dev;
	struct arm_smmu_stream		*streams;
	struct arm_smmu_vmaster		*vmaster; /* use smmu->streams_mutex */
	/* Locked by the iommu core using the group mutex */
	struct arm_smmu_ctx_desc_cfg	cd_table;
	unsigned int			num_streams;
	bool				ats_enabled : 1;
	bool				ste_ats_enabled : 1;
	bool				stall_enabled;
	unsigned int			ssid_bits;
	unsigned int			iopf_refcount;
};

/* SMMU private data for an IOMMU domain */
enum arm_smmu_domain_stage {
	ARM_SMMU_DOMAIN_S1 = 0,
	ARM_SMMU_DOMAIN_S2,
};

struct arm_smmu_domain {
	struct arm_smmu_device		*smmu;

	struct io_pgtable_ops		*pgtbl_ops;
	atomic_t			nr_ats_masters;

	enum arm_smmu_domain_stage	stage;
	union {
		struct arm_smmu_ctx_desc	cd;
		struct arm_smmu_s2_cfg		s2_cfg;
	};

	struct iommu_domain		domain;

	/* List of struct arm_smmu_master_domain */
	struct list_head		devices;
	spinlock_t			devices_lock;
	bool				enforce_cache_coherency : 1;
	bool				nest_parent : 1;

	struct mmu_notifier		mmu_notifier;
};

struct arm_smmu_nested_domain {
	struct iommu_domain domain;
	struct arm_vsmmu *vsmmu;
	bool enable_ats : 1;

	__le64 ste[2];
};

/* The following are exposed for testing purposes. */
struct arm_smmu_entry_writer_ops;
struct arm_smmu_entry_writer {
	const struct arm_smmu_entry_writer_ops *ops;
	struct arm_smmu_master *master;
};

struct arm_smmu_entry_writer_ops {
	void (*get_used)(const __le64 *entry, __le64 *used);
	void (*sync)(struct arm_smmu_entry_writer *writer);
};

void arm_smmu_make_abort_ste(struct arm_smmu_ste *target);
void arm_smmu_make_s2_domain_ste(struct arm_smmu_ste *target,
				 struct arm_smmu_master *master,
				 struct arm_smmu_domain *smmu_domain,
				 bool ats_enabled);

#if IS_ENABLED(CONFIG_KUNIT)
void arm_smmu_get_ste_used(const __le64 *ent, __le64 *used_bits);
void arm_smmu_write_entry(struct arm_smmu_entry_writer *writer, __le64 *cur,
			  const __le64 *target);
void arm_smmu_get_cd_used(const __le64 *ent, __le64 *used_bits);
void arm_smmu_make_bypass_ste(struct arm_smmu_device *smmu,
			      struct arm_smmu_ste *target);
void arm_smmu_make_cdtable_ste(struct arm_smmu_ste *target,
			       struct arm_smmu_master *master, bool ats_enabled,
			       unsigned int s1dss);
void arm_smmu_make_sva_cd(struct arm_smmu_cd *target,
			  struct arm_smmu_master *master, struct mm_struct *mm,
			  u16 asid);
#endif

struct arm_smmu_master_domain {
	struct list_head devices_elm;
	struct arm_smmu_master *master;
	/*
	 * For nested domains the master_domain is threaded onto the S2 parent,
	 * this points to the IOMMU_DOMAIN_NESTED to disambiguate the masters.
	 */
	struct iommu_domain *domain;
	ioasid_t ssid;
	bool nested_ats_flush : 1;
	bool using_iopf : 1;
};

static inline struct arm_smmu_domain *to_smmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_domain, domain);
}

static inline struct arm_smmu_nested_domain *
to_smmu_nested_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_nested_domain, domain);
}

extern struct xarray arm_smmu_asid_xa;
extern struct mutex arm_smmu_asid_lock;

struct arm_smmu_domain *arm_smmu_domain_alloc(void);

void arm_smmu_clear_cd(struct arm_smmu_master *master, ioasid_t ssid);
struct arm_smmu_cd *arm_smmu_get_cd_ptr(struct arm_smmu_master *master,
					u32 ssid);
void arm_smmu_make_s1_cd(struct arm_smmu_cd *target,
			 struct arm_smmu_master *master,
			 struct arm_smmu_domain *smmu_domain);
void arm_smmu_write_cd_entry(struct arm_smmu_master *master, int ssid,
			     struct arm_smmu_cd *cdptr,
			     const struct arm_smmu_cd *target);

int arm_smmu_set_pasid(struct arm_smmu_master *master,
		       struct arm_smmu_domain *smmu_domain, ioasid_t pasid,
		       struct arm_smmu_cd *cd, struct iommu_domain *old);

void arm_smmu_tlb_inv_asid(struct arm_smmu_device *smmu, u16 asid);
void arm_smmu_tlb_inv_range_asid(unsigned long iova, size_t size, int asid,
				 size_t granule, bool leaf,
				 struct arm_smmu_domain *smmu_domain);
int arm_smmu_atc_inv_domain(struct arm_smmu_domain *smmu_domain,
			    unsigned long iova, size_t size);

void __arm_smmu_cmdq_skip_err(struct arm_smmu_device *smmu,
			      struct arm_smmu_cmdq *cmdq);
int arm_smmu_init_one_queue(struct arm_smmu_device *smmu,
			    struct arm_smmu_queue *q, void __iomem *page,
			    unsigned long prod_off, unsigned long cons_off,
			    size_t dwords, const char *name);
int arm_smmu_cmdq_init(struct arm_smmu_device *smmu,
		       struct arm_smmu_cmdq *cmdq);

static inline bool arm_smmu_master_canwbs(struct arm_smmu_master *master)
{
	return dev_iommu_fwspec_get(master->dev)->flags &
	       IOMMU_FWSPEC_PCI_RC_CANWBS;
}

struct arm_smmu_attach_state {
	/* Inputs */
	struct iommu_domain *old_domain;
	struct arm_smmu_master *master;
	bool cd_needs_ats;
	bool disable_ats;
	ioasid_t ssid;
	/* Resulting state */
	struct arm_smmu_vmaster *vmaster;
	bool ats_enabled;
};

int arm_smmu_attach_prepare(struct arm_smmu_attach_state *state,
			    struct iommu_domain *new_domain);
void arm_smmu_attach_commit(struct arm_smmu_attach_state *state);
void arm_smmu_install_ste_for_dev(struct arm_smmu_master *master,
				  const struct arm_smmu_ste *target);

int arm_smmu_cmdq_issue_cmdlist(struct arm_smmu_device *smmu,
				struct arm_smmu_cmdq *cmdq, u64 *cmds, int n,
				bool sync);

#ifdef CONFIG_ARM_SMMU_V3_SVA
bool arm_smmu_sva_supported(struct arm_smmu_device *smmu);
void arm_smmu_sva_notifier_synchronize(void);
struct iommu_domain *arm_smmu_sva_domain_alloc(struct device *dev,
					       struct mm_struct *mm);
#else /* CONFIG_ARM_SMMU_V3_SVA */
static inline bool arm_smmu_sva_supported(struct arm_smmu_device *smmu)
{
	return false;
}

static inline void arm_smmu_sva_notifier_synchronize(void) {}

#define arm_smmu_sva_domain_alloc NULL

#endif /* CONFIG_ARM_SMMU_V3_SVA */

#ifdef CONFIG_TEGRA241_CMDQV
struct arm_smmu_device *tegra241_cmdqv_probe(struct arm_smmu_device *smmu);
#else /* CONFIG_TEGRA241_CMDQV */
static inline struct arm_smmu_device *
tegra241_cmdqv_probe(struct arm_smmu_device *smmu)
{
	return ERR_PTR(-ENODEV);
}
#endif /* CONFIG_TEGRA241_CMDQV */

struct arm_vsmmu {
	struct iommufd_viommu core;
	struct arm_smmu_device *smmu;
	struct arm_smmu_domain *s2_parent;
	u16 vmid;
};

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_IOMMUFD)
void *arm_smmu_hw_info(struct device *dev, u32 *length, u32 *type);
struct iommufd_viommu *arm_vsmmu_alloc(struct device *dev,
				       struct iommu_domain *parent,
				       struct iommufd_ctx *ictx,
				       unsigned int viommu_type);
int arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				    struct arm_smmu_nested_domain *nested_domain);
void arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state);
void arm_smmu_master_clear_vmaster(struct arm_smmu_master *master);
int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster, u64 *evt);
#else
#define arm_smmu_hw_info NULL
#define arm_vsmmu_alloc NULL

static inline int
arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				struct arm_smmu_nested_domain *nested_domain)
{
	return 0;
}

static inline void
arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state)
{
}

static inline void
arm_smmu_master_clear_vmaster(struct arm_smmu_master *master)
{
}

static inline int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster,
					   u64 *evt)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_ARM_SMMU_V3_IOMMUFD */

#endif /* _ARM_SMMU_V3_H */
