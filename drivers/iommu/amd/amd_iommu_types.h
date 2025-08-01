/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2007-2010 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 *         Leo Duran <leo.duran@amd.com>
 */

#ifndef _ASM_X86_AMD_IOMMU_TYPES_H
#define _ASM_X86_AMD_IOMMU_TYPES_H

#include <linux/bitfield.h>
#include <linux/iommu.h>
#include <linux/types.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/msi.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <linux/irqreturn.h>
#include <linux/io-pgtable.h>

/*
 * Maximum number of IOMMUs supported
 */
#define MAX_IOMMUS	32

/*
 * some size calculation constants
 */
#define DEV_TABLE_ENTRY_SIZE		32

/* Capability offsets used by the driver */
#define MMIO_CAP_HDR_OFFSET	0x00
#define MMIO_RANGE_OFFSET	0x0c
#define MMIO_MISC_OFFSET	0x10

/* Masks, shifts and macros to parse the device range capability */
#define MMIO_RANGE_LD_MASK	0xff000000
#define MMIO_RANGE_FD_MASK	0x00ff0000
#define MMIO_RANGE_BUS_MASK	0x0000ff00
#define MMIO_RANGE_LD_SHIFT	24
#define MMIO_RANGE_FD_SHIFT	16
#define MMIO_RANGE_BUS_SHIFT	8
#define MMIO_GET_LD(x)  (((x) & MMIO_RANGE_LD_MASK) >> MMIO_RANGE_LD_SHIFT)
#define MMIO_GET_FD(x)  (((x) & MMIO_RANGE_FD_MASK) >> MMIO_RANGE_FD_SHIFT)
#define MMIO_GET_BUS(x) (((x) & MMIO_RANGE_BUS_MASK) >> MMIO_RANGE_BUS_SHIFT)
#define MMIO_MSI_NUM(x)	((x) & 0x1f)

/* Flag masks for the AMD IOMMU exclusion range */
#define MMIO_EXCL_ENABLE_MASK 0x01ULL
#define MMIO_EXCL_ALLOW_MASK  0x02ULL

/* Used offsets into the MMIO space */
#define MMIO_DEV_TABLE_OFFSET   0x0000
#define MMIO_CMD_BUF_OFFSET     0x0008
#define MMIO_EVT_BUF_OFFSET     0x0010
#define MMIO_CONTROL_OFFSET     0x0018
#define MMIO_EXCL_BASE_OFFSET   0x0020
#define MMIO_EXCL_LIMIT_OFFSET  0x0028
#define MMIO_EXT_FEATURES	0x0030
#define MMIO_PPR_LOG_OFFSET	0x0038
#define MMIO_GA_LOG_BASE_OFFSET	0x00e0
#define MMIO_GA_LOG_TAIL_OFFSET	0x00e8
#define MMIO_MSI_ADDR_LO_OFFSET	0x015C
#define MMIO_MSI_ADDR_HI_OFFSET	0x0160
#define MMIO_MSI_DATA_OFFSET	0x0164
#define MMIO_INTCAPXT_EVT_OFFSET	0x0170
#define MMIO_INTCAPXT_PPR_OFFSET	0x0178
#define MMIO_INTCAPXT_GALOG_OFFSET	0x0180
#define MMIO_EXT_FEATURES2	0x01A0
#define MMIO_CMD_HEAD_OFFSET	0x2000
#define MMIO_CMD_TAIL_OFFSET	0x2008
#define MMIO_EVT_HEAD_OFFSET	0x2010
#define MMIO_EVT_TAIL_OFFSET	0x2018
#define MMIO_STATUS_OFFSET	0x2020
#define MMIO_PPR_HEAD_OFFSET	0x2030
#define MMIO_PPR_TAIL_OFFSET	0x2038
#define MMIO_GA_HEAD_OFFSET	0x2040
#define MMIO_GA_TAIL_OFFSET	0x2048
#define MMIO_CNTR_CONF_OFFSET	0x4000
#define MMIO_CNTR_REG_OFFSET	0x40000
#define MMIO_REG_END_OFFSET	0x80000



/* Extended Feature Bits */
#define FEATURE_PREFETCH	BIT_ULL(0)
#define FEATURE_PPR		BIT_ULL(1)
#define FEATURE_X2APIC		BIT_ULL(2)
#define FEATURE_NX		BIT_ULL(3)
#define FEATURE_GT		BIT_ULL(4)
#define FEATURE_IA		BIT_ULL(6)
#define FEATURE_GA		BIT_ULL(7)
#define FEATURE_HE		BIT_ULL(8)
#define FEATURE_PC		BIT_ULL(9)
#define FEATURE_HATS		GENMASK_ULL(11, 10)
#define FEATURE_GATS		GENMASK_ULL(13, 12)
#define FEATURE_GLX		GENMASK_ULL(15, 14)
#define FEATURE_GAM_VAPIC	BIT_ULL(21)
#define FEATURE_PASMAX		GENMASK_ULL(36, 32)
#define FEATURE_GIOSUP		BIT_ULL(48)
#define FEATURE_HASUP		BIT_ULL(49)
#define FEATURE_EPHSUP		BIT_ULL(50)
#define FEATURE_HDSUP		BIT_ULL(52)
#define FEATURE_SNP		BIT_ULL(63)


/* Extended Feature 2 Bits */
#define FEATURE_SNPAVICSUP	GENMASK_ULL(7, 5)
#define FEATURE_SNPAVICSUP_GAM(x) \
	(FIELD_GET(FEATURE_SNPAVICSUP, x) == 0x1)
#define FEATURE_HT_RANGE_IGNORE		BIT_ULL(11)

#define FEATURE_NUM_INT_REMAP_SUP	GENMASK_ULL(9, 8)
#define FEATURE_NUM_INT_REMAP_SUP_2K(x) \
	(FIELD_GET(FEATURE_NUM_INT_REMAP_SUP, x) == 0x1)

/* Note:
 * The current driver only support 16-bit PASID.
 * Currently, hardware only implement upto 16-bit PASID
 * even though the spec says it could have upto 20 bits.
 */
#define PASID_MASK		0x0000ffff

/* MMIO status bits */
#define MMIO_STATUS_EVT_OVERFLOW_MASK		BIT(0)
#define MMIO_STATUS_EVT_INT_MASK		BIT(1)
#define MMIO_STATUS_COM_WAIT_INT_MASK		BIT(2)
#define MMIO_STATUS_EVT_RUN_MASK		BIT(3)
#define MMIO_STATUS_PPR_OVERFLOW_MASK		BIT(5)
#define MMIO_STATUS_PPR_INT_MASK		BIT(6)
#define MMIO_STATUS_PPR_RUN_MASK		BIT(7)
#define MMIO_STATUS_GALOG_RUN_MASK		BIT(8)
#define MMIO_STATUS_GALOG_OVERFLOW_MASK		BIT(9)
#define MMIO_STATUS_GALOG_INT_MASK		BIT(10)

/* event logging constants */
#define EVENT_ENTRY_SIZE	0x10
#define EVENT_TYPE_SHIFT	28
#define EVENT_TYPE_MASK		0xf
#define EVENT_TYPE_ILL_DEV	0x1
#define EVENT_TYPE_IO_FAULT	0x2
#define EVENT_TYPE_DEV_TAB_ERR	0x3
#define EVENT_TYPE_PAGE_TAB_ERR	0x4
#define EVENT_TYPE_ILL_CMD	0x5
#define EVENT_TYPE_CMD_HARD_ERR	0x6
#define EVENT_TYPE_IOTLB_INV_TO	0x7
#define EVENT_TYPE_INV_DEV_REQ	0x8
#define EVENT_TYPE_INV_PPR_REQ	0x9
#define EVENT_TYPE_RMP_FAULT	0xd
#define EVENT_TYPE_RMP_HW_ERR	0xe
#define EVENT_DEVID_MASK	0xffff
#define EVENT_DEVID_SHIFT	0
#define EVENT_DOMID_MASK_LO	0xffff
#define EVENT_DOMID_MASK_HI	0xf0000
#define EVENT_FLAGS_MASK	0xfff
#define EVENT_FLAGS_SHIFT	0x10
#define EVENT_FLAG_RW		0x020
#define EVENT_FLAG_I		0x008

/* feature control bits */
#define CONTROL_IOMMU_EN	0
#define CONTROL_HT_TUN_EN	1
#define CONTROL_EVT_LOG_EN	2
#define CONTROL_EVT_INT_EN	3
#define CONTROL_COMWAIT_EN	4
#define CONTROL_INV_TIMEOUT	5
#define CONTROL_PASSPW_EN	8
#define CONTROL_RESPASSPW_EN	9
#define CONTROL_COHERENT_EN	10
#define CONTROL_ISOC_EN		11
#define CONTROL_CMDBUF_EN	12
#define CONTROL_PPRLOG_EN	13
#define CONTROL_PPRINT_EN	14
#define CONTROL_PPR_EN		15
#define CONTROL_GT_EN		16
#define CONTROL_GA_EN		17
#define CONTROL_GAM_EN		25
#define CONTROL_GALOG_EN	28
#define CONTROL_GAINT_EN	29
#define CONTROL_NUM_INT_REMAP_MODE	43
#define CONTROL_NUM_INT_REMAP_MODE_MASK	0x03
#define CONTROL_NUM_INT_REMAP_MODE_2K	0x01
#define CONTROL_EPH_EN		45
#define CONTROL_XT_EN		50
#define CONTROL_INTCAPXT_EN	51
#define CONTROL_IRTCACHEDIS	59
#define CONTROL_SNPAVIC_EN	61

#define CTRL_INV_TO_MASK	7
#define CTRL_INV_TO_NONE	0
#define CTRL_INV_TO_1MS		1
#define CTRL_INV_TO_10MS	2
#define CTRL_INV_TO_100MS	3
#define CTRL_INV_TO_1S		4
#define CTRL_INV_TO_10S		5
#define CTRL_INV_TO_100S	6

/* command specific defines */
#define CMD_COMPL_WAIT          0x01
#define CMD_INV_DEV_ENTRY       0x02
#define CMD_INV_IOMMU_PAGES	0x03
#define CMD_INV_IOTLB_PAGES	0x04
#define CMD_INV_IRT		0x05
#define CMD_COMPLETE_PPR	0x07
#define CMD_INV_ALL		0x08

#define CMD_COMPL_WAIT_STORE_MASK	0x01
#define CMD_COMPL_WAIT_INT_MASK		0x02
#define CMD_INV_IOMMU_PAGES_SIZE_MASK	0x01
#define CMD_INV_IOMMU_PAGES_PDE_MASK	0x02
#define CMD_INV_IOMMU_PAGES_GN_MASK	0x04

#define PPR_STATUS_MASK			0xf
#define PPR_STATUS_SHIFT		12

#define CMD_INV_IOMMU_ALL_PAGES_ADDRESS	0x7fffffffffffffffULL

/* macros and definitions for device table entries */
#define DEV_ENTRY_VALID         0x00
#define DEV_ENTRY_TRANSLATION   0x01
#define DEV_ENTRY_HAD           0x07
#define DEV_ENTRY_PPR           0x34
#define DEV_ENTRY_IR            0x3d
#define DEV_ENTRY_IW            0x3e
#define DEV_ENTRY_NO_PAGE_FAULT	0x62
#define DEV_ENTRY_EX            0x67
#define DEV_ENTRY_SYSMGT1       0x68
#define DEV_ENTRY_SYSMGT2       0x69
#define DTE_DATA1_SYSMGT_MASK	GENMASK_ULL(41, 40)

#define DEV_ENTRY_IRQ_TBL_EN	0x80
#define DEV_ENTRY_INIT_PASS     0xb8
#define DEV_ENTRY_EINT_PASS     0xb9
#define DEV_ENTRY_NMI_PASS      0xba
#define DEV_ENTRY_LINT0_PASS    0xbe
#define DEV_ENTRY_LINT1_PASS    0xbf
#define DEV_ENTRY_MODE_MASK	0x07
#define DEV_ENTRY_MODE_SHIFT	0x09

#define MAX_DEV_TABLE_ENTRIES	0xffff

/* constants to configure the command buffer */
#define CMD_BUFFER_SIZE    8192
#define CMD_BUFFER_UNINITIALIZED 1
#define CMD_BUFFER_ENTRIES 512
#define MMIO_CMD_SIZE_SHIFT 56
#define MMIO_CMD_SIZE_512 (0x9ULL << MMIO_CMD_SIZE_SHIFT)

/* constants for event buffer handling */
#define EVT_BUFFER_SIZE		8192 /* 512 entries */
#define EVT_LEN_MASK		(0x9ULL << 56)

/* Constants for PPR Log handling */
#define PPR_LOG_ENTRIES		512
#define PPR_LOG_SIZE_SHIFT	56
#define PPR_LOG_SIZE_512	(0x9ULL << PPR_LOG_SIZE_SHIFT)
#define PPR_ENTRY_SIZE		16
#define PPR_LOG_SIZE		(PPR_ENTRY_SIZE * PPR_LOG_ENTRIES)

/* PAGE_SERVICE_REQUEST PPR Log Buffer Entry flags */
#define PPR_FLAG_EXEC		0x002	/* Execute permission requested */
#define PPR_FLAG_READ		0x004	/* Read permission requested */
#define PPR_FLAG_WRITE		0x020	/* Write permission requested */
#define PPR_FLAG_US		0x040	/* 1: User, 0: Supervisor */
#define PPR_FLAG_RVSD		0x080	/* Reserved bit not zero */
#define PPR_FLAG_GN		0x100	/* GVA and PASID is valid */

#define PPR_REQ_TYPE(x)		(((x) >> 60) & 0xfULL)
#define PPR_FLAGS(x)		(((x) >> 48) & 0xfffULL)
#define PPR_DEVID(x)		((x) & 0xffffULL)
#define PPR_TAG(x)		(((x) >> 32) & 0x3ffULL)
#define PPR_PASID1(x)		(((x) >> 16) & 0xffffULL)
#define PPR_PASID2(x)		(((x) >> 42) & 0xfULL)
#define PPR_PASID(x)		((PPR_PASID2(x) << 16) | PPR_PASID1(x))

#define PPR_REQ_FAULT		0x01

/* Constants for GA Log handling */
#define GA_LOG_ENTRIES		512
#define GA_LOG_SIZE_SHIFT	56
#define GA_LOG_SIZE_512		(0x8ULL << GA_LOG_SIZE_SHIFT)
#define GA_ENTRY_SIZE		8
#define GA_LOG_SIZE		(GA_ENTRY_SIZE * GA_LOG_ENTRIES)

#define GA_TAG(x)		(u32)(x & 0xffffffffULL)
#define GA_DEVID(x)		(u16)(((x) >> 32) & 0xffffULL)
#define GA_REQ_TYPE(x)		(((x) >> 60) & 0xfULL)

#define GA_GUEST_NR		0x1

#define IOMMU_IN_ADDR_BIT_SIZE  52
#define IOMMU_OUT_ADDR_BIT_SIZE 52

/*
 * This bitmap is used to advertise the page sizes our hardware support
 * to the IOMMU core, which will then use this information to split
 * physically contiguous memory regions it is mapping into page sizes
 * that we support.
 *
 * 512GB Pages are not supported due to a hardware bug
 * Page sizes >= the 52 bit max physical address of the CPU are not supported.
 */
#define AMD_IOMMU_PGSIZES	(GENMASK_ULL(51, 12) ^ SZ_512G)

/* Special mode where page-sizes are limited to 4 KiB */
#define AMD_IOMMU_PGSIZES_4K	(PAGE_SIZE)

/* 4K, 2MB, 1G page sizes are supported */
#define AMD_IOMMU_PGSIZES_V2	(PAGE_SIZE | (1ULL << 21) | (1ULL << 30))

/* Bit value definition for dte irq remapping fields*/
#define DTE_IRQ_PHYS_ADDR_MASK		GENMASK_ULL(51, 6)
#define DTE_IRQ_REMAP_INTCTL_MASK	(0x3ULL << 60)
#define DTE_IRQ_REMAP_INTCTL    (2ULL << 60)
#define DTE_IRQ_REMAP_ENABLE    1ULL

#define DTE_INTTAB_ALIGNMENT    128
#define DTE_INTTABLEN_MASK      (0xfULL << 1)
#define DTE_INTTABLEN_VALUE_512 9ULL
#define DTE_INTTABLEN_512       (DTE_INTTABLEN_VALUE_512 << 1)
#define MAX_IRQS_PER_TABLE_512  BIT(DTE_INTTABLEN_VALUE_512)
#define DTE_INTTABLEN_VALUE_2K	11ULL
#define DTE_INTTABLEN_2K	(DTE_INTTABLEN_VALUE_2K << 1)
#define MAX_IRQS_PER_TABLE_2K	BIT(DTE_INTTABLEN_VALUE_2K)

#define PAGE_MODE_NONE    0x00
#define PAGE_MODE_1_LEVEL 0x01
#define PAGE_MODE_2_LEVEL 0x02
#define PAGE_MODE_3_LEVEL 0x03
#define PAGE_MODE_4_LEVEL 0x04
#define PAGE_MODE_5_LEVEL 0x05
#define PAGE_MODE_6_LEVEL 0x06
#define PAGE_MODE_7_LEVEL 0x07

#define GUEST_PGTABLE_4_LEVEL	0x00
#define GUEST_PGTABLE_5_LEVEL	0x01

#define PM_LEVEL_SHIFT(x)	(12 + ((x) * 9))
#define PM_LEVEL_SIZE(x)	(((x) < 6) ? \
				  ((1ULL << PM_LEVEL_SHIFT((x))) - 1): \
				   (0xffffffffffffffffULL))
#define PM_LEVEL_INDEX(x, a)	(((a) >> PM_LEVEL_SHIFT((x))) & 0x1ffULL)
#define PM_LEVEL_ENC(x)		(((x) << 9) & 0xe00ULL)
#define PM_LEVEL_PDE(x, a)	((a) | PM_LEVEL_ENC((x)) | \
				 IOMMU_PTE_PR | IOMMU_PTE_IR | IOMMU_PTE_IW)
#define PM_PTE_LEVEL(pte)	(((pte) >> 9) & 0x7ULL)

#define PM_MAP_4k		0
#define PM_ADDR_MASK		0x000ffffffffff000ULL
#define PM_MAP_MASK(lvl)	(PM_ADDR_MASK & \
				(~((1ULL << (12 + ((lvl) * 9))) - 1)))
#define PM_ALIGNED(lvl, addr)	((PM_MAP_MASK(lvl) & (addr)) == (addr))

/*
 * Returns the page table level to use for a given page size
 * Pagesize is expected to be a power-of-two
 */
#define PAGE_SIZE_LEVEL(pagesize) \
		((__ffs(pagesize) - 12) / 9)
/*
 * Returns the number of ptes to use for a given page size
 * Pagesize is expected to be a power-of-two
 */
#define PAGE_SIZE_PTE_COUNT(pagesize) \
		(1ULL << ((__ffs(pagesize) - 12) % 9))

/*
 * Aligns a given io-virtual address to a given page size
 * Pagesize is expected to be a power-of-two
 */
#define PAGE_SIZE_ALIGN(address, pagesize) \
		((address) & ~((pagesize) - 1))
/*
 * Creates an IOMMU PTE for an address and a given pagesize
 * The PTE has no permission bits set
 * Pagesize is expected to be a power-of-two larger than 4096
 */
#define PAGE_SIZE_PTE(address, pagesize)		\
		(((address) | ((pagesize) - 1)) &	\
		 (~(pagesize >> 1)) & PM_ADDR_MASK)

/*
 * Takes a PTE value with mode=0x07 and returns the page size it maps
 */
#define PTE_PAGE_SIZE(pte) \
	(1ULL << (1 + ffz(((pte) | 0xfffULL))))

/*
 * Takes a page-table level and returns the default page-size for this level
 */
#define PTE_LEVEL_PAGE_SIZE(level)			\
	(1ULL << (12 + (9 * (level))))

/*
 * The IOPTE dirty bit
 */
#define IOMMU_PTE_HD_BIT (6)

/*
 * Bit value definition for I/O PTE fields
 */
#define IOMMU_PTE_PR	BIT_ULL(0)
#define IOMMU_PTE_HD	BIT_ULL(IOMMU_PTE_HD_BIT)
#define IOMMU_PTE_U	BIT_ULL(59)
#define IOMMU_PTE_FC	BIT_ULL(60)
#define IOMMU_PTE_IR	BIT_ULL(61)
#define IOMMU_PTE_IW	BIT_ULL(62)

/*
 * Bit value definition for DTE fields
 */
#define DTE_FLAG_V	BIT_ULL(0)
#define DTE_FLAG_TV	BIT_ULL(1)
#define DTE_FLAG_HAD	(3ULL << 7)
#define DTE_FLAG_GIOV	BIT_ULL(54)
#define DTE_FLAG_GV	BIT_ULL(55)
#define DTE_GLX		GENMASK_ULL(57, 56)
#define DTE_FLAG_IR	BIT_ULL(61)
#define DTE_FLAG_IW	BIT_ULL(62)

#define DTE_FLAG_IOTLB	BIT_ULL(32)
#define DTE_FLAG_MASK	(0x3ffULL << 32)
#define DEV_DOMID_MASK	0xffffULL

#define DTE_GCR3_14_12	GENMASK_ULL(60, 58)
#define DTE_GCR3_30_15	GENMASK_ULL(31, 16)
#define DTE_GCR3_51_31	GENMASK_ULL(63, 43)

#define DTE_GPT_LEVEL_SHIFT	54
#define DTE_GPT_LEVEL_MASK	GENMASK_ULL(55, 54)

#define GCR3_VALID		0x01ULL

/* DTE[128:179] | DTE[184:191] */
#define DTE_DATA2_INTR_MASK	~GENMASK_ULL(55, 52)

#define IOMMU_PAGE_MASK (((1ULL << 52) - 1) & ~0xfffULL)
#define IOMMU_PTE_PRESENT(pte) ((pte) & IOMMU_PTE_PR)
#define IOMMU_PTE_DIRTY(pte) ((pte) & IOMMU_PTE_HD)
#define IOMMU_PTE_PAGE(pte) (iommu_phys_to_virt((pte) & IOMMU_PAGE_MASK))
#define IOMMU_PTE_MODE(pte) (((pte) >> 9) & 0x07)

#define IOMMU_PROT_MASK 0x03
#define IOMMU_PROT_IR 0x01
#define IOMMU_PROT_IW 0x02

#define IOMMU_UNITY_MAP_FLAG_EXCL_RANGE	(1 << 2)

/* IOMMU capabilities */
#define IOMMU_CAP_IOTLB   24
#define IOMMU_CAP_NPCACHE 26
#define IOMMU_CAP_EFR     27

/* IOMMU IVINFO */
#define IOMMU_IVINFO_OFFSET     36
#define IOMMU_IVINFO_EFRSUP     BIT(0)
#define IOMMU_IVINFO_DMA_REMAP  BIT(1)

/* IOMMU Feature Reporting Field (for IVHD type 10h */
#define IOMMU_FEAT_GASUP_SHIFT	6

/* IOMMU HATDIS for IVHD type 11h and 40h */
#define IOMMU_IVHD_ATTR_HATDIS_SHIFT	0

/* IOMMU Extended Feature Register (EFR) */
#define IOMMU_EFR_XTSUP_SHIFT	2
#define IOMMU_EFR_GASUP_SHIFT	7
#define IOMMU_EFR_MSICAPMMIOSUP_SHIFT	46

#define MAX_DOMAIN_ID 65536

/* Timeout stuff */
#define LOOP_TIMEOUT		100000
#define MMIO_STATUS_TIMEOUT	2000000

extern bool amd_iommu_dump;
#define DUMP_printk(format, arg...)				\
	do {							\
		if (amd_iommu_dump)				\
			pr_info(format, ## arg);	\
	} while(0);

/* global flag if IOMMUs cache non-present entries */
extern bool amd_iommu_np_cache;
/* Only true if all IOMMUs support device IOTLBs */
extern bool amd_iommu_iotlb_sup;

struct irq_remap_table {
	raw_spinlock_t lock;
	unsigned min_index;
	u32 *table;
};

/* Interrupt remapping feature used? */
extern bool amd_iommu_irq_remap;

extern const struct iommu_ops amd_iommu_ops;

/* IVRS indicates that pre-boot remapping was enabled */
extern bool amdr_ivrs_remap_support;

#define PCI_SBDF_TO_SEGID(sbdf)		(((sbdf) >> 16) & 0xffff)
#define PCI_SBDF_TO_DEVID(sbdf)		((sbdf) & 0xffff)
#define PCI_SEG_DEVID_TO_SBDF(seg, devid)	((((u32)(seg) & 0xffff) << 16) | \
						 ((devid) & 0xffff))

/* Make iterating over all pci segment easier */
#define for_each_pci_segment(pci_seg) \
	list_for_each_entry((pci_seg), &amd_iommu_pci_seg_list, list)
#define for_each_pci_segment_safe(pci_seg, next) \
	list_for_each_entry_safe((pci_seg), (next), &amd_iommu_pci_seg_list, list)
/*
 * Make iterating over all IOMMUs easier
 */
#define for_each_iommu(iommu) \
	list_for_each_entry((iommu), &amd_iommu_list, list)
#define for_each_iommu_safe(iommu, next) \
	list_for_each_entry_safe((iommu), (next), &amd_iommu_list, list)
/* Making iterating over protection_domain->dev_data_list easier */
#define for_each_pdom_dev_data(pdom_dev_data, pdom) \
	list_for_each_entry(pdom_dev_data, &pdom->dev_data_list, list)
#define for_each_pdom_dev_data_safe(pdom_dev_data, next, pdom) \
	list_for_each_entry_safe((pdom_dev_data), (next), &pdom->dev_data_list, list)

#define for_each_ivhd_dte_flags(entry) \
	list_for_each_entry((entry), &amd_ivhd_dev_flags_list, list)

struct amd_iommu;
struct iommu_domain;
struct irq_domain;
struct amd_irte_ops;

#define AMD_IOMMU_FLAG_TRANS_PRE_ENABLED      (1 << 0)

#define io_pgtable_to_data(x) \
	container_of((x), struct amd_io_pgtable, pgtbl)

#define io_pgtable_ops_to_data(x) \
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

#define io_pgtable_ops_to_domain(x) \
	container_of(io_pgtable_ops_to_data(x), \
		     struct protection_domain, iop)

#define io_pgtable_cfg_to_data(x) \
	container_of((x), struct amd_io_pgtable, pgtbl.cfg)

struct gcr3_tbl_info {
	u64	*gcr3_tbl;	/* Guest CR3 table */
	int	glx;		/* Number of levels for GCR3 table */
	u32	pasid_cnt;	/* Track attached PASIDs */
	u16	domid;		/* Per device domain ID */
};

struct amd_io_pgtable {
	struct io_pgtable	pgtbl;
	int			mode;
	u64			*root;
	u64			*pgd;		/* v2 pgtable pgd pointer */
};

enum protection_domain_mode {
	PD_MODE_NONE,
	PD_MODE_V1,
	PD_MODE_V2,
};

/* Track dev_data/PASID list for the protection domain */
struct pdom_dev_data {
	/* Points to attached device data */
	struct iommu_dev_data *dev_data;
	/* PASID attached to the protection domain */
	ioasid_t pasid;
	/* For protection_domain->dev_data_list */
	struct list_head list;
};

/* Keeps track of the IOMMUs attached to protection domain */
struct pdom_iommu_info {
	struct amd_iommu *iommu; /* IOMMUs attach to protection domain */
	u32 refcnt;	/* Count of attached dev/pasid per domain/IOMMU */
};

/*
 * This structure contains generic data for  IOMMU protection domains
 * independent of their use.
 */
struct protection_domain {
	struct list_head dev_list; /* List of all devices in this domain */
	struct iommu_domain domain; /* generic domain handle used by
				       iommu core code */
	struct amd_io_pgtable iop;
	spinlock_t lock;	/* mostly used to lock the page table*/
	u16 id;			/* the domain id written to the device table */
	enum protection_domain_mode pd_mode; /* Track page table type */
	bool dirty_tracking;	/* dirty tracking is enabled in the domain */
	struct xarray iommu_array;	/* per-IOMMU reference count */

	struct mmu_notifier mn;	/* mmu notifier for the SVA domain */
	struct list_head dev_data_list; /* List of pdom_dev_data */
};

/*
 * This structure contains information about one PCI segment in the system.
 */
struct amd_iommu_pci_seg {
	/* List with all PCI segments in the system */
	struct list_head list;

	/* List of all available dev_data structures */
	struct llist_head dev_data_list;

	/* PCI segment number */
	u16 id;

	/* Largest PCI device id we expect translation requests for */
	u16 last_bdf;

	/* Size of the device table */
	u32 dev_table_size;

	/*
	 * device table virtual address
	 *
	 * Pointer to the per PCI segment device table.
	 * It is indexed by the PCI device id or the HT unit id and contains
	 * information about the domain the device belongs to as well as the
	 * page table root pointer.
	 */
	struct dev_table_entry *dev_table;

	/*
	 * The rlookup iommu table is used to find the IOMMU which is
	 * responsible for a specific device. It is indexed by the PCI
	 * device id.
	 */
	struct amd_iommu **rlookup_table;

	/*
	 * This table is used to find the irq remapping table for a given
	 * device id quickly.
	 */
	struct irq_remap_table **irq_lookup_table;

	/*
	 * Pointer to a device table which the content of old device table
	 * will be copied to. It's only be used in kdump kernel.
	 */
	struct dev_table_entry *old_dev_tbl_cpy;

	/*
	 * The alias table is a driver specific data structure which contains the
	 * mappings of the PCI device ids to the actual requestor ids on the IOMMU.
	 * More than one device can share the same requestor id.
	 */
	u16 *alias_table;

	/*
	 * A list of required unity mappings we find in ACPI. It is not locked
	 * because as runtime it is only read. It is created at ACPI table
	 * parsing time.
	 */
	struct list_head unity_map;
};

/*
 * Structure where we save information about one hardware AMD IOMMU in the
 * system.
 */
struct amd_iommu {
	struct list_head list;

	/* Index within the IOMMU array */
	int index;

	/* locks the accesses to the hardware */
	raw_spinlock_t lock;

	/* Pointer to PCI device of this IOMMU */
	struct pci_dev *dev;

	/* Cache pdev to root device for resume quirks */
	struct pci_dev *root_pdev;

	/* physical address of MMIO space */
	u64 mmio_phys;

	/* physical end address of MMIO space */
	u64 mmio_phys_end;

	/* virtual address of MMIO space */
	u8 __iomem *mmio_base;

	/* capabilities of that IOMMU read from ACPI */
	u32 cap;

	/* flags read from acpi table */
	u8 acpi_flags;

	/* Extended features */
	u64 features;

	/* Extended features 2 */
	u64 features2;

	/* PCI device id of the IOMMU device */
	u16 devid;

	/*
	 * Capability pointer. There could be more than one IOMMU per PCI
	 * device function if there are more than one AMD IOMMU capability
	 * pointers.
	 */
	u16 cap_ptr;

	/* pci domain of this IOMMU */
	struct amd_iommu_pci_seg *pci_seg;

	/* start of exclusion range of that IOMMU */
	u64 exclusion_start;
	/* length of exclusion range of that IOMMU */
	u64 exclusion_length;

	/* command buffer virtual address */
	u8 *cmd_buf;
	u32 cmd_buf_head;
	u32 cmd_buf_tail;

	/* event buffer virtual address */
	u8 *evt_buf;

	/* Name for event log interrupt */
	unsigned char evt_irq_name[16];

	/* Base of the PPR log, if present */
	u8 *ppr_log;

	/* Name for PPR log interrupt */
	unsigned char ppr_irq_name[16];

	/* Base of the GA log, if present */
	u8 *ga_log;

	/* Name for GA log interrupt */
	unsigned char ga_irq_name[16];

	/* Tail of the GA log, if present */
	u8 *ga_log_tail;

	/* true if interrupts for this IOMMU are already enabled */
	bool int_enabled;

	/* if one, we need to send a completion wait command */
	bool need_sync;

	/* true if disable irte caching */
	bool irtcachedis_enabled;

	/* Handle for IOMMU core code */
	struct iommu_device iommu;

	/*
	 * We can't rely on the BIOS to restore all values on reinit, so we
	 * need to stash them
	 */

	/* The iommu BAR */
	u32 stored_addr_lo;
	u32 stored_addr_hi;

	/*
	 * Each iommu has 6 l1s, each of which is documented as having 0x12
	 * registers
	 */
	u32 stored_l1[6][0x12];

	/* The l2 indirect registers */
	u32 stored_l2[0x83];

	/* The maximum PC banks and counters/bank (PCSup=1) */
	u8 max_banks;
	u8 max_counters;
#ifdef CONFIG_IRQ_REMAP
	struct irq_domain *ir_domain;

	struct amd_irte_ops *irte_ops;
#endif

	u32 flags;
	volatile u64 *cmd_sem;
	atomic64_t cmd_sem_val;

#ifdef CONFIG_AMD_IOMMU_DEBUGFS
	/* DebugFS Info */
	struct dentry *debugfs;
	int dbg_mmio_offset;
	int dbg_cap_offset;
#endif

	/* IOPF support */
	struct iopf_queue *iopf_queue;
	unsigned char iopfq_name[32];
};

static inline struct amd_iommu *dev_to_amd_iommu(struct device *dev)
{
	struct iommu_device *iommu = dev_to_iommu_device(dev);

	return container_of(iommu, struct amd_iommu, iommu);
}

#define ACPIHID_UID_LEN 256
#define ACPIHID_HID_LEN 9

struct acpihid_map_entry {
	struct list_head list;
	u8 uid[ACPIHID_UID_LEN];
	u8 hid[ACPIHID_HID_LEN];
	u32 devid;
	u32 root_devid;
	bool cmd_line;
	struct iommu_group *group;
};

struct devid_map {
	struct list_head list;
	u8 id;
	u32 devid;
	bool cmd_line;
};

#define AMD_IOMMU_DEVICE_FLAG_ATS_SUP     0x1    /* ATS feature supported */
#define AMD_IOMMU_DEVICE_FLAG_PRI_SUP     0x2    /* PRI feature supported */
#define AMD_IOMMU_DEVICE_FLAG_PASID_SUP   0x4    /* PASID context supported */
/* Device may request execution on memory pages */
#define AMD_IOMMU_DEVICE_FLAG_EXEC_SUP    0x8
/* Device may request super-user privileges */
#define AMD_IOMMU_DEVICE_FLAG_PRIV_SUP   0x10

/*
 * This struct contains device specific data for the IOMMU
 */
struct iommu_dev_data {
	/*Protect against attach/detach races */
	struct mutex mutex;
	spinlock_t dte_lock;              /* DTE lock for 256-bit access */

	struct list_head list;		  /* For domain->dev_list */
	struct llist_node dev_data_list;  /* For global dev_data_list */
	struct protection_domain *domain; /* Domain the device is bound to */
	struct gcr3_tbl_info gcr3_info;   /* Per-device GCR3 table */
	struct device *dev;
	u16 devid;			  /* PCI Device ID */

	unsigned int max_irqs;		  /* Maximum IRQs supported by device */
	u32 max_pasids;			  /* Max supported PASIDs */
	u32 flags;			  /* Holds AMD_IOMMU_DEVICE_FLAG_<*> */
	int ats_qdep;
	u8 ats_enabled  :1;		  /* ATS state */
	u8 pri_enabled  :1;		  /* PRI state */
	u8 pasid_enabled:1;		  /* PASID state */
	u8 pri_tlp      :1;		  /* PASID TLB required for
					     PPR completions */
	u8 ppr          :1;		  /* Enable device PPR support */
	bool use_vapic;			  /* Enable device to use vapic mode */
	bool defer_attach;

	struct ratelimit_state rs;        /* Ratelimit IOPF messages */
};

/* Map HPET and IOAPIC ids to the devid used by the IOMMU */
extern struct list_head ioapic_map;
extern struct list_head hpet_map;
extern struct list_head acpihid_map;

/*
 * List with all PCI segments in the system. This list is not locked because
 * it is only written at driver initialization time
 */
extern struct list_head amd_iommu_pci_seg_list;

/*
 * List with all IOMMUs in the system. This list is not locked because it is
 * only written and read at driver initialization or suspend time
 */
extern struct list_head amd_iommu_list;

/*
 * Structure defining one entry in the device table
 */
struct dev_table_entry {
	union {
		u64 data[4];
		u128 data128[2];
	};
};

/*
 * Structure defining one entry in the command buffer
 */
struct iommu_cmd {
	u32 data[4];
};

/*
 * Structure to sture persistent DTE flags from IVHD
 */
struct ivhd_dte_flags {
	struct list_head list;
	u16 segid;
	u16 devid_first;
	u16 devid_last;
	struct dev_table_entry dte;
};

/*
 * One entry for unity mappings parsed out of the ACPI table.
 */
struct unity_map_entry {
	struct list_head list;

	/* starting device id this entry is used for (including) */
	u16 devid_start;
	/* end device id this entry is used for (including) */
	u16 devid_end;

	/* start address to unity map (including) */
	u64 address_start;
	/* end address to unity map (including) */
	u64 address_end;

	/* required protection */
	int prot;
};

/*
 * Data structures for device handling
 */

extern bool amd_iommu_force_isolation;

/* Max levels of glxval supported */
extern int amd_iommu_max_glx_val;

/* IDA to track protection domain IDs */
extern struct ida pdom_ids;

/* Global EFR and EFR2 registers */
extern u64 amd_iommu_efr;
extern u64 amd_iommu_efr2;

static inline int get_ioapic_devid(int id)
{
	struct devid_map *entry;

	list_for_each_entry(entry, &ioapic_map, list) {
		if (entry->id == id)
			return entry->devid;
	}

	return -EINVAL;
}

static inline int get_hpet_devid(int id)
{
	struct devid_map *entry;

	list_for_each_entry(entry, &hpet_map, list) {
		if (entry->id == id)
			return entry->devid;
	}

	return -EINVAL;
}

enum amd_iommu_intr_mode_type {
	AMD_IOMMU_GUEST_IR_LEGACY,

	/* This mode is not visible to users. It is used when
	 * we cannot fully enable vAPIC and fallback to only support
	 * legacy interrupt remapping via 128-bit IRTE.
	 */
	AMD_IOMMU_GUEST_IR_LEGACY_GA,
	AMD_IOMMU_GUEST_IR_VAPIC,
};

#define AMD_IOMMU_GUEST_IR_GA(x)	(x == AMD_IOMMU_GUEST_IR_VAPIC || \
					 x == AMD_IOMMU_GUEST_IR_LEGACY_GA)

#define AMD_IOMMU_GUEST_IR_VAPIC(x)	(x == AMD_IOMMU_GUEST_IR_VAPIC)

union irte {
	u32 val;
	struct {
		u32 valid	: 1,
		    no_fault	: 1,
		    int_type	: 3,
		    rq_eoi	: 1,
		    dm		: 1,
		    rsvd_1	: 1,
		    destination	: 8,
		    vector	: 8,
		    rsvd_2	: 8;
	} fields;
};

#define APICID_TO_IRTE_DEST_LO(x)    (x & 0xffffff)
#define APICID_TO_IRTE_DEST_HI(x)    ((x >> 24) & 0xff)

union irte_ga_lo {
	u64 val;

	/* For int remapping */
	struct {
		u64 valid	: 1,
		    no_fault	: 1,
		    /* ------ */
		    int_type	: 3,
		    rq_eoi	: 1,
		    dm		: 1,
		    /* ------ */
		    guest_mode	: 1,
		    destination	: 24,
		    ga_tag	: 32;
	} fields_remap;

	/* For guest vAPIC */
	struct {
		u64 valid	: 1,
		    no_fault	: 1,
		    /* ------ */
		    ga_log_intr	: 1,
		    rsvd1	: 3,
		    is_run	: 1,
		    /* ------ */
		    guest_mode	: 1,
		    destination	: 24,
		    ga_tag	: 32;
	} fields_vapic;
};

union irte_ga_hi {
	u64 val;
	struct {
		u64 vector	: 8,
		    rsvd_1	: 4,
		    ga_root_ptr	: 40,
		    rsvd_2	: 4,
		    destination : 8;
	} fields;
};

struct irte_ga {
	union {
		struct {
			union irte_ga_lo lo;
			union irte_ga_hi hi;
		};
		u128 irte;
	};
};

struct irq_2_irte {
	u16 devid; /* Device ID for IRTE table */
	u16 index; /* Index into IRTE table*/
};

struct amd_ir_data {
	u32 cached_ga_tag;
	struct amd_iommu *iommu;
	struct irq_2_irte irq_2_irte;
	struct msi_msg msi_entry;
	void *entry;    /* Pointer to union irte or struct irte_ga */

	/**
	 * Store information for activate/de-activate
	 * Guest virtual APIC mode during runtime.
	 */
	struct irq_cfg *cfg;
	int ga_vector;
	u64 ga_root_ptr;
	u32 ga_tag;
};

struct amd_irte_ops {
	void (*prepare)(void *, u32, bool, u8, u32, int);
	void (*activate)(struct amd_iommu *iommu, void *, u16, u16);
	void (*deactivate)(struct amd_iommu *iommu, void *, u16, u16);
	void (*set_affinity)(struct amd_iommu *iommu, void *, u16, u16, u8, u32);
	void *(*get)(struct irq_remap_table *, int);
	void (*set_allocated)(struct irq_remap_table *, int);
	bool (*is_allocated)(struct irq_remap_table *, int);
	void (*clear_allocated)(struct irq_remap_table *, int);
};

#ifdef CONFIG_IRQ_REMAP
extern struct amd_irte_ops irte_32_ops;
extern struct amd_irte_ops irte_128_ops;
#endif

#endif /* _ASM_X86_AMD_IOMMU_TYPES_H */
