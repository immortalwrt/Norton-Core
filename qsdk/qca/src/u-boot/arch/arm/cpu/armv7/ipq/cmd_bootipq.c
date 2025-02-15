/*
 * Copyright (c) 2013-2016 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <common.h>
#include <command.h>
#include <image.h>
#include <nand.h>
#include <errno.h>
#include <asm/arch-ipq806x/smem.h>
#include <asm/arch-ipq806x/scm.h>
#include <linux/mtd/ubi.h>
#include <part.h>

#define img_addr		((void *)CONFIG_SYS_LOAD_ADDR)
#define CE1_REG_USAGE		(0)
#define CE1_ADM_USAGE		(1)
#define CE1_RESOURCE		(1)

/*
 * SOC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.  For example:
 *   1.0 -> 0x00010000
 *   2.3 -> 0x00020003
 */
#define SOCINFO_VERSION_MAJOR(ver) ((ver & 0xffff0000) >> 16)
#define SOCINFO_VERSION_MINOR(ver) (ver & 0x0000ffff)

DECLARE_GLOBAL_DATA_PTR;
static int debug = 0;
static ipq_smem_flash_info_t *sfi = &ipq_smem_flash_info;
int ipq_fs_on_nand, rootfs_part_avail = 1;
static int soc_version = 1;
extern board_ipq806x_params_t *gboard_param;
#ifdef CONFIG_IPQ_MMC
static ipq_mmc *host = &mmc_host;
#endif

#define DTB_CFG_LEN		64
static char dtb_config_name[DTB_CFG_LEN];

typedef struct {
	unsigned int image_type;
	unsigned int header_vsn_num;
	unsigned int image_src;
	unsigned char *image_dest_ptr;
	unsigned int image_size;
	unsigned int code_size;
	unsigned char *signature_ptr;
	unsigned int signature_size;
	unsigned char *cert_chain_ptr;
	unsigned int cert_chain_size;
} mbn_header_t;

typedef struct {
	unsigned int kernel_load_addr;
	unsigned int kernel_load_size;
} kernel_img_info_t;

typedef struct {
	unsigned int resource;
	unsigned int channel_id;
} switch_ce_chn_buf_t;

kernel_img_info_t kernel_img_info;

static void update_dtb_config_name(uint32_t addr)
{
	struct fdt_property *imginfop;
	int nodeoffset;

	/*
	 * construt the dtb config name upon image info property
	 */
	nodeoffset = fdt_path_offset((const void *)addr, "/image-info");

	if(nodeoffset >= 0) {
		imginfop = ( struct fdt_property *)fdt_get_property((const void *)addr, nodeoffset,
					"type", NULL);
		if(imginfop) {
			if (strcmp(imginfop->data, "multiplatform") != 0) {
				printf("node property is not set, using default dtb config\n");
				snprintf((char *)dtb_config_name,
					sizeof(dtb_config_name),"%s","");
			}
		} else {
			printf("node property is unavailable, using default dtb config\n");
			snprintf((char *)dtb_config_name,
				sizeof(dtb_config_name),"%s","");
		}

	} else {
		snprintf((char *)dtb_config_name,
			sizeof(dtb_config_name),"#config@%d",SOCINFO_VERSION_MAJOR(soc_version));
		/*
		 * Using dtb_config_name + 1 to remove '#' from dtb_config_name.
		 */
		if (fit_conf_get_node((void *)addr, (dtb_config_name + 1)) < 0) {
			/*
			 * Fetching the dtb_config_name based on the soc version
			 * of the board.
			 */
			snprintf((char *)dtb_config_name, sizeof(dtb_config_name), "#config@%s",
				gboard_param->dtb_config_name
				[(SOCINFO_VERSION_MAJOR(soc_version) - 1)]);
			if (fit_conf_get_node((void *)addr, (dtb_config_name + 1)) < 0) {
				/*
				 * For .itb of a specific soc version in a board.
				 */
				snprintf((char *)dtb_config_name, sizeof(dtb_config_name),
					"#config@1");
			}
		}
	}
}

#ifdef CONFIG_IPQ_LOAD_NSS_FW
/**
 * check if the image and its header is valid and move it to
 * load address as specified in the header
 */
static int load_nss_img(const char *runcmd, char *args, int argslen,
						int nsscore)
{
	char cmd[128];
	int ret;

	if (debug)
		printf(runcmd);

	if ((ret = run_command(runcmd, 0)) != CMD_RET_SUCCESS) {
		return ret;
	}

	snprintf(cmd, sizeof(cmd), "bootm start 0x%x; bootm loados", CONFIG_SYS_LOAD_ADDR);

	if (debug)
		printf(cmd);

	if ((ret = run_command(cmd, 0)) != CMD_RET_SUCCESS) {
		return ret;
	}

	if (args) {
		snprintf(args, argslen, "qca-nss-drv.load%d=0x%x,"
				"qca-nss-drv.entry%d=0x%x,"
				"qca-nss-drv.string%d=\"%.*s\"",
				nsscore, image_get_load(img_addr),
				nsscore, image_get_ep(img_addr),
				nsscore, IH_NMLEN, image_get_name(img_addr));
	}

	return ret;
}

#endif /* CONFIG_IPQ_LOAD_NSS_FW */

/*
 * Set the root device and bootargs for mounting root filesystem.
 */
static int set_fs_bootargs(int *fs_on_nand)
{
	char *bootargs = NULL;
#ifdef CONFIG_IPQ_MMC
#define EMMC_MAX_ARGS 48
	char emmc_rootfs[EMMC_MAX_ARGS];
	block_dev_desc_t *blk_dev = mmc_get_dev(host->dev_num);
	disk_partition_t disk_info;
	int pos;
#endif
	unsigned int active_part = 0;

#define nand_rootfs	"ubi.mtd=" IPQ_ROOT_FS_PART_NAME " root=mtd:ubi_rootfs rootfstype=squashfs"
#define nor_rootfs	"root=mtd:" IPQ_ROOT_FS_PART_NAME " rootfstype=squashfs"

	if (sfi->flash_type == SMEM_BOOT_SPI_FLASH &&
			sfi->flash_secondary_type != SMEM_BOOT_MMC_FLASH) {

		if (sfi->rootfs.offset == 0xBAD0FF5E) {
			rootfs_part_avail = 0;
			/*
			 * While booting out of SPI-NOR, not having a
			 * 'rootfs' entry in the partition table implies
			 * that the Root FS is available in the NAND flash
			 */
			bootargs = nand_rootfs;
			*fs_on_nand = 1;
			active_part = get_rootfs_active_partition();
			sfi->rootfs.offset = active_part * IPQ_NAND_ROOTFS_SIZE;
			sfi->rootfs.size = IPQ_NAND_ROOTFS_SIZE;
		} else {
			bootargs = nor_rootfs;
			*fs_on_nand = 0;
		}
	} else if (sfi->flash_type == SMEM_BOOT_NAND_FLASH) {
		bootargs = nand_rootfs;
		*fs_on_nand = 1;
#ifdef CONFIG_IPQ_MMC
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH ||
			sfi->flash_secondary_type == SMEM_BOOT_MMC_FLASH) {
		active_part = get_rootfs_active_partition();
		if (active_part) {
			pos = find_part_efi(blk_dev, IPQ_ROOT_FS_ALT_PART_NAME, &disk_info);
		} else {
			pos = find_part_efi(blk_dev, IPQ_ROOT_FS_PART_NAME, &disk_info);
		}
		if (pos > 0) {
			snprintf(emmc_rootfs, sizeof(emmc_rootfs),
				"root=/dev/mmcblk0p%d rootwait", pos);
			bootargs = emmc_rootfs;
			*fs_on_nand = 0;
		}
#endif
	} else {
		printf("bootipq: unsupported boot flash type\n");
		return -EINVAL;
	}

	if ((getenv("fsbootargs") == NULL) && (bootargs != NULL))
		setenv("fsbootargs", bootargs);

	return run_command("setenv bootargs ${bootargs} ${fsbootargs}", 0);
}

/**
 * Inovke the dump routine and in case of failure, do not stop unless the user
 * requested to stop
 */
static int inline do_dumpipq_data(void)
{
	uint64_t etime;

	if (run_command("dumpipq_data", 0) != CMD_RET_SUCCESS) {
		printf("\nAuto crashdump saving failed!"
			"\nPress any key within 10s to take control of U-Boot");

		etime = get_timer_masked() + (10 * CONFIG_SYS_HZ);
		while (get_timer_masked() < etime) {
			if (tstc())
				break;
		}

		if (get_timer_masked() < etime)
			return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

static int switch_ce_channel_buf(unsigned int channel_id)
{
	int ret;
	switch_ce_chn_buf_t ce1_chn_buf;

	ce1_chn_buf.resource   = CE1_RESOURCE;
	ce1_chn_buf.channel_id = channel_id;

	ret = scm_call(SCM_SVC_TZ, CE_CHN_SWITCH_CMD, &ce1_chn_buf,
		sizeof(switch_ce_chn_buf_t), NULL, 0);

	return ret;
}

/**
 * Load the Kernel image in mbn format and transfer control to kernel.
 */
static int do_boot_signedimg(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
#ifdef CONFIG_IPQ_LOAD_NSS_FW
	char bootargs[IH_NMLEN+32];
#endif
	char runcmd[256] = {0};
	int ret, count;
	unsigned int request;
#ifdef CONFIG_IPQ_MMC
	block_dev_desc_t *blk_dev = mmc_get_dev(host->dev_num);
	disk_partition_t disk_info;
	unsigned int active_part = 0;
#endif

#ifdef CONFIG_IPQ_APPSBL_DLOAD
	unsigned long * dmagic1 = (unsigned long *) 0x2A03F000;
	unsigned long * dmagic2 = (unsigned long *) 0x2A03F004;
	unsigned long * dmagic3 = (unsigned long *) 0x2A03F010;
#endif

	if (argc == 2 && strncmp(argv[1], "debug", 5) == 0)
		debug = 1;

#ifdef CONFIG_IPQ_APPSBL_DLOAD
	/* check if we are in download mode */
	if (*dmagic1 == 0xE47B337D && *dmagic2 == 0x0501CAB0) {
		/* clear the magic and run the dump command */
		*dmagic3 = *dmagic1;

		printf("\nCrashdump magic found. 1:%#08x 2:%#08x\n",
				*dmagic1, *dmagic2);

		*dmagic1 = 0;
		*dmagic2 = 0;

#if CONFIG_IPQ_DEV_FIRMWARE	/* Development FW would dump a crash over TFTP */
		uint64_t etime = get_timer_masked() + (10 * CONFIG_SYS_HZ);
		printf("\nHit any key within 10s to stop dump activity...");
		while (!tstc()) {       /* while no incoming data */
			if (get_timer_masked() >= etime) {
				if (do_dumpipq_data() == CMD_RET_FAILURE)
					return CMD_RET_FAILURE;
				break;
			}
		}

#endif
		/* reset the system, some images might not be loaded
		 * when crashmagic is found
		 */
		run_command("reset", 0);
	} else if(*dmagic1 != 0 && *dmagic2 != 0 ) {
		/* It is power up,  (*dmagic1,*dmagic2) = 0xFFFFFFFF */
		printf("\nPower up: 1:%#08x 2:%#08x\n", *dmagic1, *dmagic2);
		run_command("setenv bootargs ${bootargs} NortonDump=0", 0);
	} else {
		/* Regular reboot or Reboot after crash */
		if (*dmagic3 == 0xE47B337D) {
			/* Reboot after crash */
			*dmagic3 = 0xFFFFFFFF;
			printf("\nCrashdump processed: 1:%#08x 2:%#08x\n", *dmagic1, *dmagic2 );
			run_command("setenv bootargs ${bootargs} NortonDump=1", 0);
		} else {
			/* Reboot */
			printf("\nReboot: 1:%#08x 2:%#08x\n", *dmagic1, *dmagic2 );
		}
	}
#endif

#ifdef CONFIG_IPQ806X_PCI
	board_pci_deinit();
#endif /* CONFIG_IPQ806X_PCI */

	if ((ret = set_fs_bootargs(&ipq_fs_on_nand)))
		return ret;

	/* check the smem info to see which flash used for booting */
	if (sfi->flash_type == SMEM_BOOT_SPI_FLASH) {
		if (debug) {
			printf("Using nand device 1\n");
		}
		run_command("nand device 1", 0);
	} else if (sfi->flash_type == SMEM_BOOT_NAND_FLASH) {
		if (debug) {
			printf("Using nand device 0\n");
		}
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH) {
		if (debug) {
			printf("Using MMC device\n");
		}
	} else {
		printf("Unsupported BOOT flash type\n");
		return -1;
	}

#ifdef CONFIG_IPQ_LOAD_NSS_FW
	/* check the smem info to see whether the partition size is valid.
	 * refer board/qcom/ipq806x_cdp/ipq806x_cdp.c:ipq_get_part_details
	 * for more details
	 */
	if (sfi->nss[0].size != 0xBAD0FF5E) {
		snprintf(runcmd, sizeof(runcmd), "nand read 0x%x 0x%llx 0x%llx",
				CONFIG_SYS_LOAD_ADDR,
				sfi->nss[0].offset, sfi->nss[0].size);

		if (load_nss_img(runcmd, bootargs, sizeof(bootargs), 0)
				!= CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		if (getenv("nssbootargs0") == NULL)
			setenv("nssbootargs0", bootargs);

		run_command("setenv bootargs ${bootargs} ${nssbootargs0}", 0);
	}

	if (sfi->nss[1].size != 0xBAD0FF5E) {
		snprintf(runcmd, sizeof(runcmd), "nand read 0x%x 0x%llx 0x%llx",
				CONFIG_SYS_LOAD_ADDR,
				sfi->nss[1].offset, sfi->nss[1].size);

		if (load_nss_img(runcmd, bootargs, sizeof(bootargs), 1)
				!= CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		if (getenv("nssbootargs1") == NULL)
			setenv("nssbootargs1", bootargs);

		run_command("setenv bootargs ${bootargs} ${nssbootargs1}", 0);
	}
#endif /* CONFIG_IPQ_LOAD_NSS_FW */

	if (debug) {
		run_command("printenv bootargs", 0);
		printf("Booting from flash\n");
	}

	request = CONFIG_SYS_LOAD_ADDR;
	kernel_img_info.kernel_load_addr = request;

	if (ipq_fs_on_nand) {

		/*
		 * The kernel will be available inside a UBI volume
		 */
		snprintf(runcmd, sizeof(runcmd),
			"set mtdids nand0=nand0 && "
			"set mtdparts mtdparts=nand0:0x%llx@0x%llx(fs),${msmparts} && "
			"ubi part fs && "
			"ubi read 0x%x kernel && ", sfi->rootfs.size, sfi->rootfs.offset,
			request);


		if (debug)
			printf("%s\n", runcmd);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		kernel_img_info.kernel_load_size =
			(unsigned int)ubi_get_volume_size("kernel");

#ifdef CONFIG_IPQ_MMC
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH ||
			sfi->flash_secondary_type == SMEM_BOOT_MMC_FLASH) {
		active_part = get_rootfs_active_partition();
		if (active_part) {
			ret = find_part_efi(blk_dev, "0:HLOS_1", &disk_info);
		} else {
			ret = find_part_efi(blk_dev, "0:HLOS", &disk_info);
		}

		if (ret > 0) {
			snprintf(runcmd, sizeof(runcmd), "mmc read 0x%x 0x%X 0x%X",
					CONFIG_SYS_LOAD_ADDR,
					(uint)disk_info.start, (uint)disk_info.size);

			if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
				return CMD_RET_FAILURE;

			kernel_img_info.kernel_load_size = disk_info.size * disk_info.blksz;
		}
#endif
	} else {

		/*
		 * Kernel is in a separate partition
		 */
		snprintf(runcmd, sizeof(runcmd),
			/* NOR is treated as psuedo NAND */
			"set mtdids nand1=nand1 && "
			"set mtdparts mtdparts=nand1:${msmparts} && "
			"nand read 0x%x 0x%llx 0x%llx",
			request, sfi->hlos.offset, sfi->hlos.size);

		if (debug)
			printf("%s\n", runcmd);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		kernel_img_info.kernel_load_size =  sfi->hlos.size;
	}

	request += sizeof(mbn_header_t);

	/* This sys call will switch the CE1 channel to register usage */
	ret = switch_ce_channel_buf(CE1_REG_USAGE);

	if (ret)
		return CMD_RET_FAILURE;

	ret = scm_call(SCM_SVC_BOOT, KERNEL_AUTH_CMD, &kernel_img_info,
		sizeof(kernel_img_info_t), NULL, 0);

	if (ret) {
		printf("Kernel image authentication failed \n");
		BUG();
	}

	/*
	 * This sys call will switch the CE1 channel to ADM usage
	 * so that HLOS can use it.
	 */
	ret = switch_ce_channel_buf(CE1_ADM_USAGE);

	if (ret)
		return CMD_RET_FAILURE;

	update_dtb_config_name(request);
	snprintf(runcmd, sizeof(runcmd), "bootm 0x%x%s\n", request,
		dtb_config_name);

	if (debug)
		printf("%s\n", runcmd);

#ifdef CONFIG_IPQ_MMC
	board_mmc_deinit();
#endif

	if (run_command(runcmd, 0) != CMD_RET_SUCCESS) {
#ifdef CONFIG_IPQ_MMC
		mmc_initialize(gd->bd);
#endif
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}


/**
 * Load the NSS images and Kernel image and transfer control to kernel
 */
static int do_boot_unsignedimg(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
#ifdef CONFIG_IPQ_LOAD_NSS_FW
	char bootargs[IH_NMLEN+32];
#endif
	char runcmd[256] = {0};
	int ret, count;
	unsigned int active_part = 0;

#ifdef CONFIG_IPQ_MMC
	block_dev_desc_t *blk_dev = mmc_get_dev(host->dev_num);
	disk_partition_t disk_info;
#endif

#ifdef CONFIG_IPQ_APPSBL_DLOAD
	unsigned long * dmagic1 = (unsigned long *) 0x2A03F000;
	unsigned long * dmagic2 = (unsigned long *) 0x2A03F004;
	unsigned long * dmagic3 = (unsigned long *) 0x2A03F010;
#endif
	if (argc == 2 && strncmp(argv[1], "debug", 5) == 0)
		debug = 1;

#ifdef CONFIG_IPQ_APPSBL_DLOAD
	/* check if we are in download mode */
	if (*dmagic1 == 0xE47B337D && *dmagic2 == 0x0501CAB0) {
		/* clear the magic and run the dump command */
		*dmagic3 = *dmagic1;

		printf("\nCrashdump magic found. 1:%#08x 2:%#08x\n",
				*dmagic1, *dmagic2);

		*dmagic1 = 0;
		*dmagic2 = 0;

#if CONFIG_IPQ_DEV_FIRMWARE	/* Development FW would dump a crash over TFTP */
		uint64_t etime = get_timer_masked() + (10 * CONFIG_SYS_HZ);
		printf("\nHit any key within 10s to stop dump activity...");
		while (!tstc()) {       /* while no incoming data */
			if (get_timer_masked() >= etime) {
				if (do_dumpipq_data() == CMD_RET_FAILURE)
					return CMD_RET_FAILURE;
				break;
			}
		}
#endif
		/* reset the system, some images might not be loaded
		 * when crash-magic is found
		 */
		run_command("reset", 0);

	} else if(*dmagic1 != 0 && *dmagic2 != 0 ) {
		/* It is power up,  (*dmagic1,*dmagic2) = 0xFFFFFFFF */
		printf("\nPower up: 1:%#08x 2:%#08x\n", *dmagic1, *dmagic2);
		run_command("setenv bootargs ${bootargs} NortonDump=0", 0);
	} else {
		/* Regular reboot or Reboot after crash */
		if (*dmagic3 == 0xE47B337D) {
			/* Reboot after crash */
			*dmagic3 = 0xFFFFFFFF;
			printf("\nCrashdump processed: 1:%#08x 2:%#08x\n", *dmagic1, *dmagic2 );
			run_command("setenv bootargs ${bootargs} NortonDump=1", 0);
		} else {
			/* Reboot */
			printf("\nReboot: 1:%#08x 2:%#08x\n", *dmagic1, *dmagic2 );
		}
	}

#endif

#ifdef CONFIG_IPQ806X_PCI
	board_pci_deinit();
#endif /* CONFIG_IPQ806X_PCI */

	if ((ret = set_fs_bootargs(&ipq_fs_on_nand)))
		return ret;

	/* check the smem info to see which flash used for booting */
	if (sfi->flash_type == SMEM_BOOT_SPI_FLASH) {
		if (debug) {
			printf("Using nand device 1\n");
		}
		run_command("nand device 1", 0);
	} else if (sfi->flash_type == SMEM_BOOT_NAND_FLASH) {
		if (debug) {
			printf("Using nand device 0\n");
		}
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH) {
		if (debug) {
			printf("Using MMC device\n");
		}
	} else {
		printf("Unsupported BOOT flash type\n");
		return -1;
	}

#ifdef CONFIG_IPQ_LOAD_NSS_FW
	/* check the smem info to see whether the partition size is valid.
	 * refer board/qcom/ipq806x_cdp/ipq806x_cdp.c:ipq_get_part_details
	 * for more details
	 */
	if (sfi->nss[0].size != 0xBAD0FF5E) {
		snprintf(runcmd, sizeof(runcmd), "nand read 0x%x 0x%llx 0x%llx",
				CONFIG_SYS_LOAD_ADDR,
				sfi->nss[0].offset, sfi->nss[0].size);

		if (load_nss_img(runcmd, bootargs, sizeof(bootargs), 0)
				!= CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		if (getenv("nssbootargs0") == NULL)
			setenv("nssbootargs0", bootargs);

		run_command("setenv bootargs ${bootargs} ${nssbootargs0}", 0);
	}

	if (sfi->nss[1].size != 0xBAD0FF5E) {
		snprintf(runcmd, sizeof(runcmd), "nand read 0x%x 0x%llx 0x%llx",
				CONFIG_SYS_LOAD_ADDR,
				sfi->nss[1].offset, sfi->nss[1].size);

		if (load_nss_img(runcmd, bootargs, sizeof(bootargs), 1)
				!= CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		if (getenv("nssbootargs1") == NULL)
			setenv("nssbootargs1", bootargs);

		run_command("setenv bootargs ${bootargs} ${nssbootargs1}", 0);
	}
#endif /* CONFIG_IPQ_LOAD_NSS_FW */

	if (debug) {
		run_command("printenv bootargs", 0);
		printf("Booting from flash\n");
	}

	if (ipq_fs_on_nand) {

		/*
		 * The kernel will be available inside a UBI volume
		 */
		snprintf(runcmd, sizeof(runcmd),
			"set mtdids nand0=nand0 && "
			"set mtdparts mtdparts=nand0:0x%llx@0x%llx(fs),${msmparts} && "
			"ubi part fs && "
			"ubi read 0x%x kernel && ",
				sfi->rootfs.size, sfi->rootfs.offset,
				CONFIG_SYS_LOAD_ADDR);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		update_dtb_config_name((uint32_t)img_addr);
		snprintf(runcmd, sizeof(runcmd),"bootm 0x%x%s\n",
				CONFIG_SYS_LOAD_ADDR,
				dtb_config_name);
#ifdef CONFIG_IPQ_MMC
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH ||
			sfi->flash_secondary_type == SMEM_BOOT_MMC_FLASH) {
		active_part = get_rootfs_active_partition();
		if (active_part) {
			ret = find_part_efi(blk_dev, "0:HLOS_1", &disk_info);
		} else {
			ret = find_part_efi(blk_dev, "0:HLOS", &disk_info);
		}

		if (ret > 0) {
			snprintf(runcmd, sizeof(runcmd), "mmc read 0x%x 0x%X 0x%X",
					CONFIG_SYS_LOAD_ADDR,
					(uint)disk_info.start, (uint)disk_info.size);

			if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
				return CMD_RET_FAILURE;
                        /* We need to skip the MBN header added by the signing code. Original ITB image does not have it. */
                        uint32_t actual_addr = img_addr + sizeof(mbn_header_t);
                        uint32_t actual_load_addr = CONFIG_SYS_LOAD_ADDR + sizeof(mbn_header_t);

                        update_dtb_config_name((uint32_t)actual_addr);
                        snprintf(runcmd, sizeof(runcmd),"bootm 0x%x%s\n",
                                actual_load_addr,
                                dtb_config_name);
		}

#endif
	} else {

		/*
		 * Kernel is in a separate partition
		 */
		snprintf(runcmd, sizeof(runcmd),
			/* NOR is treated as psuedo NAND */
			"set mtdids nand1=nand1 && "
			"set mtdparts mtdparts=nand1:${msmparts} && "
			"nand read 0x%x 0x%llx 0x%llx",
			CONFIG_SYS_LOAD_ADDR, sfi->hlos.offset, sfi->hlos.size);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		update_dtb_config_name((uint32_t)img_addr);

		snprintf(runcmd, sizeof(runcmd),"bootm 0x%x%s\n",
			CONFIG_SYS_LOAD_ADDR,
			dtb_config_name);

	}

	if (debug)
		printf("%s\n", runcmd);

#ifdef CONFIG_IPQ_MMC
	board_mmc_deinit();
#endif

	if (run_command(runcmd, 0) != CMD_RET_SUCCESS) {
#ifdef CONFIG_IPQ_MMC
		mmc_initialize(gd->bd);
#endif
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

static int run_usb_auto_ops(void);

static int do_bootipq(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	int ret;
	char buf;

	/* flash images and blow fuses from files on USB storage device */
	if (run_usb_auto_ops() != 0) {
		goto FAIL;
	}

	/*
	 * set fdt_high parameter to all ones, so that u-boot will pass the
	 * loaded in-place fdt address to kernel instead of relocating the fdt.
	 */
	if (setenv_addr("fdt_high", (void *)CONFIG_IPQ_FDT_HIGH)
			!= CMD_RET_SUCCESS) {
		printf("Cannot set fdt_high to %x to avoid relocation\n",
			CONFIG_IPQ_FDT_HIGH);
	}

	if(!ipq_smem_get_socinfo_version((uint32_t *)&soc_version))
		debug("Soc version is = %x \n", SOCINFO_VERSION_MAJOR(soc_version));
	else
		printf("Cannot get socinfo, using defaults\n");

	ret = scm_call(SCM_SVC_FUSE, QFPROM_IS_AUTHENTICATE_CMD,
			NULL, 0, &buf, sizeof(char));

	snprintf((char *)dtb_config_name, sizeof(dtb_config_name),
		"#config@%d_%d", gboard_param->machid, SOCINFO_VERSION_MAJOR(soc_version));

	if (ret == 0 && buf == 1) {
		ret = do_boot_signedimg(cmdtp, flag, argc, argv);
	} else if (ret == 0 || ret == -EOPNOTSUPP) {
		ret = do_boot_unsignedimg(cmdtp, flag, argc, argv);
	}

	if (ret == CMD_RET_SUCCESS)
		return CMD_RET_SUCCESS;

FAIL:
	// led indicates u-boot failure. To get out of this, cycle the power.
	printf("Contact manufacturer.\n");
	while (1) {
		// Turn off amber
		run_command("i2c led 2 0", 0);
		// delay 100 ms
		udelay(100000);
		// Turn on amber
		run_command("i2c led 2 10", 0);
	}

	return CMD_RET_FAILURE;
}

U_BOOT_CMD(bootipq, 2, 0, do_bootipq,
	   "bootipq from flash device",
	   "bootipq [debug] - Load image(s) and boots the kernel\n");

//======= Symantec-specific functions ===================================================================

#define START_USB_CMD                "usb start"
#define LOAD_IMG_FROM_USB_CMD        "fatload usb 0:1 0x42000000 emmc-ipq806x-single.img.uboot"
#define CHECK_IMG_CMD                "checkimg 0x42000000"
#define CHECK_VER_CMD                "checkver 0x42000000"
#define FLASH_IMG_CMD                "imgaddr=0x42000000 && source ${imgaddr}:script"
#define LOAD_DEV_SECDAT_FROM_USB_CMD "fatload usb 0:1 0x42000000 dev_sec.dat"
#define LOAD_PVT_SECDAT_FROM_USB_CMD "fatload usb 0:1 0x42000000 pvt_sec.dat"
#define WRITE_FUSES_CMD              "fuseipq 0x42000000"

#define MAX_FLASH_ATTEMPTS 30
#define FLASHING_ATTEMPT_DELAY 1000000

static int flash_from_usb(void) {
	/* If an image is available on USB, flash it before bootipq */
	if (run_command(LOAD_IMG_FROM_USB_CMD, 0) != CMD_RET_SUCCESS) {
		return 0;
	}

	if (run_command(CHECK_IMG_CMD, 0) != CMD_RET_SUCCESS) {
		printf("Failed to verify FW image signature.\n");
		return 0;
	}

	if (run_command(CHECK_VER_CMD, 0) != CMD_RET_SUCCESS) {
		printf("Failed to verify FW image version.\n");
		return 0;
	}

	printf("Flashing FW image from USB storage device...\n");
	int count = 0;
	int status = CMD_RET_FAILURE;
	/* If flashing fails, re-flash until succeed or the limit is reached */
	while ((status = run_command(FLASH_IMG_CMD, 0)) != CMD_RET_SUCCESS && count++ < MAX_FLASH_ATTEMPTS) {
		printf("Failed to flashed image loaded from USB storage device.\n");
		udelay(FLASHING_ATTEMPT_DELAY);
	}
	/* If flashing fails repeatedly, go to LED flashing loop that indicates failure */
	if (status != CMD_RET_SUCCESS) {
		return -1;
	}

	return 0;
}

#define SEC_HASH_VAR_NAME "sec_hash"
#define CMD_SIZE 64
#define HASH_SIZE 40
enum { UNK_SECDAT, DEV_SECDAT, PVT_SECDAT, PROD_SECDAT } secdat_type;
// Expected size and sha1 hash of the sec.dat file
int  expected_size = 0x24c;
char expected_devsec_hash[HASH_SIZE] = "4567024570b634277ba05dffd085026281edde4a";
char expected_pvtsec_hash[HASH_SIZE] = "c2a41db0a8f32e73017cd423c8faee600d1a1aa4";

static int check_secdat(void)
{
	int size;
	char cmd[CMD_SIZE];
	char * s;

	if ((s = getenv("filesize")) == NULL) {
		printf("No filesize env.");
		return -1;
	}

	size = simple_strtoul(s, NULL, 16);

	if (size != expected_size) {
		printf("sec dat file size %d is invalid.", size);
		return -1;
	}

	snprintf(cmd, CMD_SIZE, "sha1sum 0x42000000 0x%x", size);

	printf("%s\n", cmd);

	if (run_command(cmd, 0) != CMD_RET_SUCCESS) {
		printf("Can not run %s.\n", cmd);
		return -1;
	}

	if ((s = getenv("sha1")) == NULL) {
		printf("sha1 environment variable is not set.\n");
		return -1;
	}

	switch (secdat_type) {
	case DEV_SECDAT:
        	if (strncmp(s, expected_devsec_hash, HASH_SIZE) != 0) {
			printf("DEV sec data sha1 hash is invalid.\n");
			return -1;
		}
		break;
	case PVT_SECDAT:
		if (strncmp(s, expected_pvtsec_hash, HASH_SIZE) != 0) {
			printf("PVT sec data sha1 hash is invalid.\n");
			return -1;
		}
		break;
	default:
		printf("Unknown sec dat type.\n");
		return -1;
	}

	// Save hash to u-boot env variable.
	setenv (SEC_HASH_VAR_NAME, s);
	saveenv ();

	printf("sec data hash verified!\n");

	return 0;
}

// Check if emmc partitions contain signed images, return 0 if signed, -1 otherwise
#define READ_SIZE 64
#define LOAD_ADDRESS   0x42000000
#define SIG_CERT_SIZE  6400

static int is_emmc_image_signed()
{
	block_dev_desc_t *blk_dev = mmc_get_dev(host->dev_num);
	disk_partition_t disk_info;
	int ret;
	unsigned int active_part = 0;
	mbn_header_t *mbn_hdr;
	int sig_cert_size;
	char cmd[CMD_SIZE];

	// Get kernel partition info
	active_part = get_rootfs_active_partition();
	if (active_part) {
		ret = find_part_efi(blk_dev, "0:HLOS_1", &disk_info);
	} else {
		ret = find_part_efi(blk_dev, "0:HLOS", &disk_info);
        }

	if (ret <= 0) {
		printf("Can't find partition %s.\n", active_part ? "0:HLOS_1": "0:HLOS");
		return -1;
	}

	// Load kernel image to mem to check certificate
	if (snprintf(cmd, CMD_SIZE, "mmc read 0x%x 0x%X 0x%X", LOAD_ADDRESS, (uint)disk_info.start, READ_SIZE) >= CMD_SIZE) {
		printf("cmd buffer is not big enough.\n");
		return -1;
	}

	if (run_command(cmd, 0) != CMD_RET_SUCCESS) {
		printf("Failed to run cmd %s.\n", cmd);
		return -1;
	}

	mbn_hdr = (mbn_header_t *) LOAD_ADDRESS;
	// Check the presence of certificate
	sig_cert_size = mbn_hdr->image_size - mbn_hdr->code_size;

	if (sig_cert_size != SIG_CERT_SIZE) {
		printf("Image %s is not signed.\n", active_part ? "0:HLOS_1": "0:HLOS");
		return -1;
	}

	printf("Image %s is signed.\n", active_part ? "0:HLOS_1": "0:HLOS");

	return 0;
}

static void write_fuses_from_usb(void) {
	/* Check if the image is signed and the status of Secure Boot first,
		skip if the image is not signed or hw is a secure device already */
	int ret;
	char buf;

	if (is_emmc_image_signed() != 0) {
		return;
	}

	ret = scm_call(SCM_SVC_FUSE, QFPROM_IS_AUTHENTICATE_CMD,
				NULL, 0, &buf, sizeof(char));
	if (ret != 0) {
		printf("Failed to check the status of Secure Boot\n");
		return;
	}

	if (buf == 1) {
		printf("Secure Boot is already enabled\n");
		return;
	}

	/* Load sec.dat from USB storage */
	if (run_command(LOAD_DEV_SECDAT_FROM_USB_CMD, 0) == CMD_RET_SUCCESS) {
		secdat_type = DEV_SECDAT;
	} else if (run_command(LOAD_PVT_SECDAT_FROM_USB_CMD, 0) == CMD_RET_SUCCESS) {
		secdat_type = PVT_SECDAT;
	} else {
		secdat_type = UNK_SECDAT;
		printf("Could not find or load any SEC.DAT files from USB storage\n");
		return;
	}

	/* Check the size and sha1 hash of the loaded sec.dat file before using it */
	if (check_secdat() != 0) {
		return;
	}

	/* Write fuses if everything was good */
	if (run_command(WRITE_FUSES_CMD, 0) == CMD_RET_SUCCESS) {
		printf("Successfully configured QFPROM\n");
	} else {
		printf("Failed to write to QFPROM\n");
	}
}

static int run_usb_auto_ops(void) {
	int ret;

	if ((ret = run_command(START_USB_CMD, 0)) == CMD_RET_SUCCESS) {
		if (flash_from_usb() != 0) {
			return -1;
		}
		write_fuses_from_usb();
	} else {
		printf("Failed to initialize USB\n");
	}

	return 0;
}

