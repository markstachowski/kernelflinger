/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Authors: Jeremy Compostella <jeremy.compostella@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <efi.h>
#include <efiapi.h>
#include <lib.h>
#include <fastboot.h>
#include <vars.h>
#ifdef CRASHMODE_USE_ADB
#include <adb.h>
#endif

#include "options.h"
#if defined(IOC_USE_SLCAN) || defined(IOC_USE_CBC)
#include "ioc_can.h"
#endif
#include "android.h"
#include "slot.h"
#include "timer.h"
#ifdef USE_AVB
#include "avb_init.h"
#include "libavb/libavb.h"
#include "libavb/uefi_avb_ops.h"
#include "libavb_ab/libavb_ab.h"
#endif
#include "security.h"
#include "security_interface.h"
#ifdef RPMB_STORAGE
#include <openssl/hkdf.h>
#include "rpmb.h"
#include "rpmb_storage.h"
#endif
#ifdef USE_TRUSTY
#include "trusty_interface.h"
#include "trusty_common.h"
#endif

typedef union {
	uint32_t raw;
	struct {
		uint32_t patch_M:4;
		uint32_t patch_Y:7;
		uint32_t version_C:7;
		uint32_t version_B:7;
		uint32_t version_A:7;
	};
} os_version_t;

#define MAX_CMD_BUF 0x1000
static CHAR8 cmd_buf[MAX_CMD_BUF];
struct rot_data_t g_rot_data = {0};

#ifdef CRASHMODE_USE_ADB
static EFI_STATUS enter_crashmode(enum boot_target *target)
{
	EFI_STATUS ret;

#ifdef USER
#error "adb in crashmode MUST be disabled on a USER build"
#endif

	ret = adb_init();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to initialize adb");
		return ret;
	}

	debug(L"adb implementation is initialized");
	for (;;) {
		ret = adb_run();
		if (EFI_ERROR(ret))
			break;

		*target = adb_get_boot_target();
		if (*target != UNKNOWN_TARGET)
			break;
	}
	adb_exit();

	return ret;
}
#endif
#ifndef __FORCE_FASTBOOT
static enum boot_target check_bcb(CHAR16 **target_path, BOOLEAN *oneshot)
{
	EFI_STATUS ret;
	struct bootloader_message bcb;
	CHAR16 *target = NULL;
	enum boot_target t;
	CHAR8 *bcb_cmd;
	BOOLEAN dirty;

	*oneshot = FALSE;
	*target_path = NULL;

	ret = read_bcb(MISC_LABEL, &bcb);
	if (EFI_ERROR(ret)) {
		error(L"Unable to read BCB");
		t = NORMAL_BOOT;
		goto out;
	}

	dirty = bcb.status[0] != '\0';
	/* We own the status field; clear it in case there is any stale data */
	bcb.status[0] = '\0';
	bcb_cmd = (CHAR8 *)bcb.command;
	if (!strncmpa(bcb_cmd, (CHAR8 *)"boot-", 5)) {
		target = stra_to_str(bcb_cmd + 5);
		debug(L"BCB boot target: '%s'", target);
	} else if (!strncmpa(bcb_cmd, (CHAR8 *)"bootonce-", 9)) {
		target = stra_to_str(bcb_cmd + 9);
		bcb_cmd[0] = '\0';
		dirty = TRUE;
		debug(L"BCB oneshot boot target: '%s'", target);
		*oneshot = TRUE;
	}

	if (dirty) {
		ret = write_bcb(MISC_LABEL, &bcb);
		if (EFI_ERROR(ret))
			error(L"Unable to update BCB contents!");
	}

	if (!target) {
		t = NORMAL_BOOT;
		goto out;
	}

	t = name_to_boot_target(target);
	if (t != UNKNOWN_TARGET && t != CHARGER)
		goto out;

	error(L"Unknown/Unsupported boot target in BCB: '%s'", target);
	t = NORMAL_BOOT;

out:
	FreePool(target);
	return t;
}
#endif

static EFI_STATUS process_bootimage(void *bootimage, UINTN imagesize)
{
	EFI_STATUS ret;

	if (bootimage) {
		/* 'fastboot boot' case, only allowed on unlocked devices.*/
		if (device_is_unlocked()) {
			UINT32 crc;

			ret = uefi_call_wrapper(BS->CalculateCrc32, 3, bootimage, imagesize, &crc);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"CalculateCrc32 failed");
				return ret;
			}

			ret = android_image_start_buffer(NULL, bootimage,
								NORMAL_BOOT, BOOT_STATE_GREEN, NULL,
								NULL, (const CHAR8 *)cmd_buf);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"Couldn't load Boot image");
				return ret;
			}
		}
	}

	return EFI_SUCCESS;
}

static EFI_STATUS enter_fastboot_mode(enum boot_target *target)
{
	EFI_STATUS ret;
	void *efiimage, *bootimage;
	UINTN imagesize;

#if defined(IOC_USE_SLCAN) || defined(IOC_USE_CBC)
	ret = notify_ioc_ready();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"notify ioc ready failed");
	}
#endif

	for (;;) {
		*target = UNKNOWN_TARGET;
		bootimage = NULL;
		efiimage = NULL;

		ret = fastboot_start(&bootimage, &efiimage, &imagesize, target);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Fastboot mode failed");
			break;
		}

		ret = process_bootimage(bootimage, imagesize);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Process bootimage failed");
			if (bootimage) {
				FreePool(bootimage);
				bootimage = NULL;
			}
			break;
		}

		if (*target == UNKNOWN_TARGET)
			continue;

		if ((*target == NORMAL_BOOT) || (*target == FASTBOOT))
			reboot_to_target(*target, EfiResetCold);
		break;
	}

	return ret;
}

/*
 *  Boot mode field definitions.
 */
static union bootMode
{
	UINT16 _bits;
	struct {
		UINT16 target           : 5; /* [4:0] */
		UINT16 do_mrc_training  : 1; /* [5] */
		UINT16 do_save_mrc_data : 1; /* [6] */
		UINT16 do_flash_update  : 1; /* [7] */
		UINT16 silent           : 1; /* [8] */
		UINT16 _reserved        : 1; /* [9] */
		UINT16 action           : 2; /* [11:10] 0:boot,1:CLI,2:halt,3:reset */
		UINT16 dipsw            : 4; /* [15:12] */
	};
} bootMode;

static enum boot_target check_command_line(EFI_HANDLE image, CHAR8 *cmd_buf, UINTN max_cmd_size)
{
	EFI_STATUS ret;
	enum boot_target target = FASTBOOT;
	static EFI_LOADED_IMAGE *limg;
	UINTN argc, i, j;
	CHAR16 **argv;
	UINTN cmd_len = 0;
	CHAR8 arg8[256] = "";
	UINTN arglen;
#if defined(USE_TRUSTY) || defined(RPMB_STORAGE)
	UINTN num;
#endif

	enum CmdType
	{
		RESET,
		BOOT_TARGET,
		BOOT,
		TRUSTY_PARAM,
		SECUREBOOT,
		BOOTVERSION,
		SERIALNO,
		DEV_SEC_INFO
	};

	struct Cmdline
	{
		CHAR8 *name;
		UINTN length;
		enum CmdType type;
	};

	struct Cmdline CmdlineArray[] = {
		{
			(CHAR8 *)"ABL.reset=",
			strlen((CHAR8 *)"ABL.reset="),
			RESET
		},
		{
			(CHAR8 *)"ABL.boot_target=",
			strlen((CHAR8 *)"ABL.boot_target="),
			BOOT_TARGET
		},
		{
			(CHAR8 *)"ABL.boot=",
			strlen((CHAR8 *)"ABL.boot="),
			BOOT
		},
		{
			(CHAR8 *)"trusty.param_addr=",
			strlen((CHAR8 *)"trusty.param_addr="),
			TRUSTY_PARAM
		},
		{
			(CHAR8 *)"ABL.secureboot=",
			strlen((CHAR8 *)"ABL.secureboot="),
			SECUREBOOT
		},
		{
			(CHAR8 *)"androidboot.bootloader=",
			strlen((CHAR8 *)"androidboot.bootloader="),
			BOOTVERSION
		},
		{
			(CHAR8 *)"androidboot.serialno=",
			strlen((CHAR8 *)"androidboot.serialno="),
			SERIALNO
		},
		{
			(CHAR8 *)"dev_sec_info.param_addr=",
			strlen((CHAR8 *)"dev_sec_info.param_addr="),
			DEV_SEC_INFO
		},
	};

	CHAR8 *nptr = NULL;
	ret = uefi_call_wrapper(BS->OpenProtocol, 6, image,
				&LoadedImageProtocol, (VOID **)&limg,
				image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to open LoadedImageProtocol");
		return FASTBOOT;
	}

	ret = get_argv(limg, &argc, &argv);
	if (EFI_ERROR(ret))
		return FASTBOOT;

	cmd_buf[0] = 0;

	for (i = 0; i < argc; i++) {
		debug(L" abl cmd %02d: %s", i, argv[i]);
		arglen = StrLen(argv[i]);

		if (arglen > (int)sizeof(arg8) - 2)
			arglen = sizeof(arg8) - 2;
		debug(L" abl cmd %02d length: %d", i, arglen);

		ret = str_to_stra((CHAR8 *)arg8, argv[i], arglen + 1);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Non-ascii characters in command line");
			return FASTBOOT;
		}

		if (cmd_len + arglen + 1 < max_cmd_size) {
			if (cmd_buf[0] != 0) {
				strncpy((CHAR8 *)(cmd_buf + cmd_len), (const CHAR8 *)" ", 1);
				cmd_len++;
			}
			for (j = 0; j < sizeof(CmdlineArray)/sizeof(CmdlineArray[0]); j++) {
				if((arglen > CmdlineArray[j].length) && !strncmp(arg8, CmdlineArray[j].name, CmdlineArray[j].length))
					break;
			}
			if (j < sizeof(CmdlineArray)/sizeof(CmdlineArray[0])) {
				switch(CmdlineArray[j].type) {
				/* Parse "ABL.reset=xxx" */
				case RESET:
					set_reboot_reason(argv[i] + CmdlineArray[j].length);
					continue;

				/* Parse "ABL.boot_target=xxxx" */
				case BOOT_TARGET:
					nptr = (CHAR8 *)(arg8 + CmdlineArray[j].length);
					/* Only handle CRASHMODE case, other mode should be decided by "ABL.boot". */
					if (!strcmp(nptr, (CHAR8 *)"CRASHMODE")) {
						target = CRASHMODE;
						goto out;
					}
					continue;

				/* Parse "ABL.boot=xx" */
				case BOOT:
					nptr = (CHAR8 *)(arg8 + CmdlineArray[j].length);
					bootMode._bits = (UINT16)strtoul((char *)nptr, 0, 16);
					target = bootMode.target;
					break;
#ifdef USE_TRUSTY
				/* Parse "trusty.param_addr=xxxxx" */
				case TRUSTY_PARAM:
					nptr = (CHAR8 *)(arg8 + CmdlineArray[j].length);
					num = strtoul((char *)nptr, 0, 16);
					debug(L"Parsed trusty param addr is 0x%x", num);
					set_trusty_param((VOID *)num);
					continue;
#endif //USE_TRUSTY
#ifdef RPMB_STORAGE
				/* Parse "dev_sec_info.param_addr=" */
				case DEV_SEC_INFO:
					nptr = (CHAR8 *)(arg8 + CmdlineArray[j].length);
					num = strtoul((char *)nptr, 0, 16);
					debug(L"Parsed device security information addr is 0x%x", num);
					set_device_security_info((VOID *)num);
					continue;
#endif //RPMB_STORAGE
				/* Parse "ABL.secureboot=x" */
				case SECUREBOOT: {
					UINT8 val;
					nptr = (CHAR8 *)(arg8 + CmdlineArray[j].length);
					val = (UINT8)strtoul((char *)nptr, 0, 10);
					ret = set_platform_secure_boot(val);
					if (EFI_ERROR(ret))
						efi_perror(ret, L"Failed to set secure boot");
					break;
				}

				/* Parse "android.bootloader=xxxxx" */
				case BOOTVERSION:
					continue;

				/* Parse "android.serialno=xxxxx " */
				case SERIALNO:
					continue;

				default:
					continue;
				}
			}
			strncpy((CHAR8 *)(cmd_buf + cmd_len), (const CHAR8 *)arg8, arglen);
			cmd_len += arglen;
		}
	}

out:
	debug(L"boot target: %d", target);
	FreePool(argv);
	return target;
}

#ifndef USE_AVB
/* Load a boot image into RAM.
 *
 * boot_target  - Boot image to load. Values supported are NORMAL_BOOT, RECOVERY,
 *                and ESP_BOOTIMAGE (for 'fastboot boot')
 * target_path  - Path to load boot image from for ESP_BOOTIMAGE case, ignored
 *                otherwise.
 * bootimage    - Returned allocated pointer value for the loaded boot image.
 * oneshot      - For ESP_BOOTIMAGE case, flag indicating that the image should
 *                be deleted.
 *
 * Return values:
 * EFI_INVALID_PARAMETER - Unsupported boot target type, key is not well-formed,
 *                         or loaded boot image was missing or corrupt
 * EFI_ACCESS_DENIED     - Validation failed against OEM or embedded certificate,
 *                         boot image still usable
 */
static EFI_STATUS load_boot_image(
				IN enum boot_target boot_target,
				IN CHAR16 *target_path,
				OUT VOID **bootimage,
				IN BOOLEAN oneshot)
{
	EFI_STATUS ret;

	switch (boot_target) {
	case NORMAL_BOOT:
		ret = EFI_NOT_FOUND;
		if (use_slot() && !slot_get_active())
			break;
		do {
			const CHAR16 *label = slot_label(BOOT_LABEL);
			ret = android_image_load_partition(label, bootimage);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"Failed to load boot image from %s partition",
						label);
				if (use_slot())
					slot_boot_failed(boot_target);
			}
		} while (EFI_ERROR(ret) && slot_get_active());
		break;

	case RECOVERY:
		if (recovery_in_boot_partition()) {
			ret = load_boot_image(NORMAL_BOOT, target_path, bootimage, oneshot);
			break;
		}
		if (use_slot() && !slot_recovery_tries_remaining()) {
			ret = EFI_NOT_FOUND;
			break;
		}
		ret = android_image_load_partition(RECOVERY_LABEL, bootimage);
		break;
	default:
		*bootimage = NULL;
		return EFI_INVALID_PARAMETER;
	}

	if (!EFI_ERROR(ret))
		debug(L"boot image loaded");

	return ret;
}
#endif


#ifdef USE_AVB
static EFI_STATUS start_boot_image(VOID *bootimage, UINT8 boot_state,
				enum boot_target boot_target,
				AvbSlotVerifyData *slot_data,
				CHAR8 *abl_cmd_line)
#else
static EFI_STATUS start_boot_image(VOID *bootimage, UINT8 boot_state,
				enum boot_target boot_target,
				X509 *verifier_cert,
				CHAR8 *abl_cmd_line)
#endif
{
	EFI_STATUS ret;
#ifdef USER
	/* per bootloaderequirements.pdf */
	if (boot_state == BOOT_STATE_ORANGE) {
		ret = android_clear_memory();
		if (EFI_ERROR(ret)) {
			error(L"Failed to clear memory. Load image aborted.");
			return ret;
		}
	}
#endif

#ifdef USER
	if (boot_state == BOOT_STATE_RED) {
		if (is_platform_secure_boot_enabled()) {
			return EFI_SECURITY_VIOLATION;
		}
	}
#endif

	set_efi_variable(&fastboot_guid, BOOT_STATE_VAR, sizeof(boot_state),
					&boot_state, FALSE, TRUE);

#ifdef OS_SECURE_BOOT
	ret = set_os_secure_boot(boot_state == BOOT_STATE_GREEN);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to set os secure boot");
#endif

	if (!use_slot()) {
		ret = slot_boot(boot_target);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to write slot boot");
			return ret;
		}
	}

	log(L"chainloading boot image, boot state is %s\n",
	boot_state_to_string(boot_state));
#ifdef USE_AVB
	ret = android_image_start_buffer(NULL, bootimage,
					 boot_target, boot_state, NULL,
					 slot_data, (const CHAR8 *)abl_cmd_line);
#else
	ret = android_image_start_buffer(NULL, bootimage,
					 boot_target, boot_state, NULL,
					 verifier_cert, (const CHAR8 *)abl_cmd_line);
#endif
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Couldn't load Boot image");

	ret = slot_boot_failed(boot_target);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to write slot failure");

	return ret;
}

#ifndef USE_AVB
/* Validate an image.
 *
 * Parameters:
 * boot_target    - Boot image to load. Values supported are NORMAL_BOOT,
 *                  RECOVERY, and ESP_BOOTIMAGE (for 'fastboot boot')
 * bootimage      - Bootimage to validate
 * verifier_cert  - Return the certificate that validated the boot image
 *
 * Return values:
 * BOOT_STATE_GREEN  - Boot image is valid against provided certificate
 * BOOT_STATE_YELLOW - Boot image is valid against embedded certificate
 * BOOT_STATE_RED    - Boot image is not valid
 */
static UINT8 validate_bootimage(
		IN enum boot_target boot_target,
		IN VOID *bootimage,
		OUT X509 **verifier_cert)
{
	CHAR16 target[BOOT_TARGET_SIZE];
	CHAR16 *expected;
	CHAR16 *expected2 = NULL;
	UINT8 boot_state;

	boot_state = verify_android_boot_image(bootimage, oem_cert,
						oem_cert_size, target,
						verifier_cert);

	if (boot_state == BOOT_STATE_RED) {
		error(L"boot image doesn't verify");
		return boot_state;
	}

	switch (boot_target) {
	case NORMAL_BOOT:
		expected = L"/boot";
		/* in case of multistage ota */
		expected2 = L"/recovery";
		break;
	case RECOVERY:
		if (recovery_in_boot_partition())
			expected = L"/boot";
		else
			expected = L"/recovery";
		break;
	default:
		expected = NULL;
	}

	if ((!expected || StrCmp(expected, target)) &&
		(!expected2 || StrCmp(expected2, target))) {
		error(L"boot image has unexpected target name");
		return BOOT_STATE_RED;
	}

	return boot_state;
}
#endif

#ifdef RPMB_STORAGE
EFI_STATUS  osloader_rpmb_key_init(VOID)
{
	UINT8 key[RPMB_KEY_SIZE] = {0};
	UINT8 *out_key;
	UINT8 number_derived_key = 0;
	UINT16 i;
	RPMB_RESPONSE_RESULT result;
	EFI_STATUS ret = EFI_SUCCESS;

	if (is_eom_and_secureboot_enabled()) {
		ret = clear_teedata_flag();
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Clear teedata flag failed");
			return ret;
		}
	}

	ret = get_rpmb_derived_key(&out_key, &number_derived_key);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"get_rpmb_derived_key failed");
		return ret;
	}

	for (i = 0; i < number_derived_key; i++) {
		memcpy(key, out_key + i * RPMB_KEY_SIZE, RPMB_KEY_SIZE);
		ret = rpmb_read_counter(key, &result);
		if (ret == EFI_SUCCESS)
			break;

		if (result == RPMB_RES_NO_AUTH_KEY_PROGRAM) {
			efi_perror(ret, L"key is not programmed, use the first derived key.");
			break;
		}

		if (result != RPMB_RES_AUTH_FAILURE) {
			efi_perror(ret, L"rpmb_read_counter unexpected error: %d.", result);
			goto err_get_rpmb_key;
		}
	}

	if (i >= number_derived_key) {
		error(L"All keys are not match!");
		goto err_get_rpmb_key;
	}

	if (i != 0)
		error(L"seed/key changed to %d ", i);

	if (!is_rpmb_programed()) {
		debug(L"rpmb not programmed");
		ret = program_rpmb_key(key);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"rpmb key program failed");
			return ret;
		}
	} else {
		debug(L"rpmb already programmed");
		set_rpmb_key(key);
	}

err_get_rpmb_key:
	memset(key, 0, sizeof(key));

	return ret;
}
#endif

#ifdef USE_AVB
EFI_STATUS avb_boot_android(enum boot_target boot_target, CHAR8 *abl_cmd_line)
{
	AvbOps *ops;
	AvbPartitionData *boot;
	AvbSlotVerifyData *slot_data = NULL;
#ifndef USE_SLOT
	const char *slot_suffix = "";
	AvbSlotVerifyResult verify_result;
#else
	AvbABFlowResult flow_result;
#endif
	const char *requested_partitions[] = {"boot", NULL};
	EFI_STATUS ret;
	VOID *bootimage = NULL;
	UINT8 boot_state = BOOT_STATE_GREEN;
	bool allow_verification_error = FALSE;
	AvbSlotVerifyFlags flags;
	const uint8_t *vbmeta_pub_key;
	UINTN vbmeta_pub_key_len;

	debug(L"Loading boot image");
	if (!use_slot()) {
		if (boot_target == RECOVERY) {
			requested_partitions[0] = "recovery";
		}
	}

	set_boottime_stamp(TM_AVB_START);
	ops = avb_init();
	if (ops) {
		if (ops->read_is_device_unlocked(ops, &allow_verification_error) != AVB_IO_RESULT_OK) {
			avb_fatal("Error determining whether device is unlocked.\n");
			return EFI_ABORTED;
		}
	} else {
		return EFI_OUT_OF_RESOURCES;
	}

	flags = AVB_SLOT_VERIFY_FLAGS_NONE;
	if (allow_verification_error) {
		flags |= AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR;
	}

#ifdef USE_SLOT
	flow_result = avb_ab_flow(&ab_ops, requested_partitions, flags, AVB_HASHTREE_ERROR_MODE_RESTART, &slot_data);
	ret = get_avb_flow_result(slot_data,
			    allow_verification_error,
			    flow_result,
			    &boot_state);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get avb slot a/b flow result for boot");
		goto fail;
	}
	slot_set_active_cached(slot_data->ab_suffix);

#ifdef  __SUPPORT_ABL_BOOT
	if (slot_data->ab_suffix) {
		CHAR8 *capsule_buf;
		UINTN capsule_buf_len = 0;
		CHAR16 *AB_SUFFIX = NULL;
		CHAR16 *ABL_AB_SUFFIX = NULL;

		AB_SUFFIX = stra_to_str((const CHAR8 *)slot_data->ab_suffix);
		if (!AB_SUFFIX) {
			error(L"Cannot get a valid AVB flow suffix: %s", slot_data->ab_suffix);
			goto fail;
		}
		ABL_AB_SUFFIX = stra_to_str((const CHAR8 *)abl_cmd_line);
		if (!ABL_AB_SUFFIX) {
			error(L"Cannot get a valid ABL suffix: %s", abl_cmd_line);
			goto fail;
		}
		if (!(StrStr(ABL_AB_SUFFIX, L"ABL.suffix"))) {
			debug(L"ABL.suffix is null");
		} else if (!(StrCmp(AB_SUFFIX, L"_a")) && (!(StrStr(ABL_AB_SUFFIX, L"ABL.suffix=0")))) {
			capsule_buf = (CHAR8 *)"m1:@0";
			capsule_buf_len = strlen(capsule_buf);
		} else if (!(StrCmp(AB_SUFFIX, L"_b")) && (!(StrStr(ABL_AB_SUFFIX, L"ABL.suffix=1")))) {
			capsule_buf = (CHAR8 *)"m2:@0";
			capsule_buf_len = strlen(capsule_buf);
		}

		if (capsule_buf_len != 0 ) {
			error(L"Avb flow suffix %a doesn't equal to ABL suffix, reboot and update ABL.", slot_data->ab_suffix);
			ret = set_efi_variable(&loader_guid, IFWI_CAPSULE_UPDATE, capsule_buf_len + 1,
						   capsule_buf, TRUE, TRUE);
			if (EFI_ERROR(ret)) {
				error(L"Unable to set slot %a into %a", slot_data->ab_suffix, IFWI_CAPSULE_UPDATE);
				goto fail;
			}
			reboot_to_target(NORMAL_BOOT, EfiResetCold);
		}
	}
#endif
#else
	verify_result = avb_slot_verify(ops,
					requested_partitions,
					slot_suffix,
					flags,
					AVB_HASHTREE_ERROR_MODE_RESTART,
					&slot_data);
	ret = get_avb_result(slot_data,
				allow_verification_error,
				verify_result,
				&boot_state);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get avb result for boot");
		goto fail;
	}
#endif

	boot = &slot_data->loaded_partitions[0];
	bootimage = boot->data;
	set_boottime_stamp(TM_VERIFY_BOOT_DONE);
	ret = avb_vbmeta_image_verify(slot_data->vbmeta_images[0].vbmeta_data,
			slot_data->vbmeta_images[0].vbmeta_size,
			&vbmeta_pub_key,
			&vbmeta_pub_key_len);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get the vbmeta_pub_key");
		goto fail;
	}

	ret = get_rot_data(bootimage, boot_state, vbmeta_pub_key, vbmeta_pub_key_len, &g_rot_data);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to init rot params");
		goto fail;
	}

#ifdef USE_TRUSTY
	if (boot_target == NORMAL_BOOT) {
		VOID *tosimage = NULL;
		ret = load_tos_image(&tosimage);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Load tos image failed");
			goto fail;
		}
		ret = start_trusty(tosimage);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Unable to start trusty: stop");
			goto fail;
		}
		set_boottime_stamp(TM_VERIFY_TOS_DONE);
	}
#endif

	if (boot_state == BOOT_STATE_GREEN) {
		avb_update_stored_rollback_indexes_for_slot(ops, slot_data);
	}

	ret = start_boot_image(bootimage, boot_state, boot_target, slot_data, abl_cmd_line);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to start boot image");
		goto fail;
	}

fail:
	if (slot_data)
		avb_slot_verify_data_free(slot_data);

	return ret;
}
#endif

#ifndef USE_AVB
EFI_STATUS boot_android(enum boot_target boot_target, CHAR8 *abl_cmd_line)
{
	EFI_STATUS ret;
	CHAR16 *target_path = NULL;
	VOID *bootimage = NULL;
	BOOLEAN oneshot = FALSE;
	UINT8 boot_state = BOOT_STATE_GREEN;
	X509 *verifier_cert = NULL;

	debug(L"Loading boot image");
	ret = load_boot_image(boot_target, target_path, &bootimage, oneshot);
	FreePool(target_path);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to load boot image");
		return ret;
	}
	boot_state = validate_bootimage(boot_target, bootimage, &verifier_cert);

	/*  keymaster interface always use the g_rot_data as its input param */
	ret = get_rot_data(bootimage, boot_state, verifier_cert, &g_rot_data);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to init rot params");
		goto exit;
	}

#ifdef USE_TRUSTY
	if (boot_target == NORMAL_BOOT) {
		VOID *tosimage = NULL;
		ret = load_tos_image(&tosimage);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Load tos image failed");
			goto exit;
		}
		ret = start_trusty(tosimage);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Unable to start trusty: stop");
			goto exit;
		}
	}
#endif

	ret = start_boot_image(bootimage, boot_state, boot_target, verifier_cert, abl_cmd_line);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to start boot image");
		goto exit;
	}

	ret = EFI_INVALID_PARAMETER;
exit:
	return ret;
}
#endif

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table)
{
	enum boot_target target;
	EFI_STATUS ret;

#ifndef __FORCE_FASTBOOT
	BOOLEAN oneshot = FALSE;
	CHAR16 *target_path = NULL;
	enum boot_target bcb_target;
#endif

	set_boottime_stamp(TM_EFI_MAIN);
	InitializeLib(image, sys_table);
	target = check_command_line(image, cmd_buf, sizeof(cmd_buf) - 1);

#ifdef RPMB_STORAGE
	emmc_rpmb_init(NULL);
	rpmb_storage_init(is_eom_and_secureboot_enabled());
#endif

	ret = slot_init();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Slot management initialization failed");
		return ret;
	}

#ifdef __FORCE_FASTBOOT
	target = FASTBOOT;
#endif

#ifndef __FORCE_FASTBOOT
	debug(L"Before Check BCB target is %d", target);
	bcb_target = check_bcb(&target_path, &oneshot);
	debug(L"BCB target is %d", bcb_target);
	if (bcb_target == RECOVERY)
		target = bcb_target;
	debug(L"After Check BCB target is %d", target);
#endif

	debug(L"target=%d", target);

#ifdef RPMB_STORAGE
	if (target != CRASHMODE) {
		ret = osloader_rpmb_key_init();
		if (EFI_ERROR(ret))
			error(L"rpmb key init failure for osloader");
	}
#endif

	for (;;) {
		switch (target) {
		case NORMAL_BOOT:
		case RECOVERY:
#ifdef USE_AVB
			ret = avb_boot_android(target, cmd_buf);
#else
			ret = boot_android(target, cmd_buf);
#endif
			if (EFI_ERROR(ret))
				target = FASTBOOT;
			break;
		case UNKNOWN_TARGET:
#ifndef CRASHMODE_USE_ADB
		case CRASHMODE:
#endif
		case FASTBOOT:
			enter_fastboot_mode(&target);
			break;
#ifdef CRASHMODE_USE_ADB
		case CRASHMODE:
			enter_crashmode(&target);
			break;
#endif
		default:
			reboot_to_target(target, EfiResetCold);
		}
	}

	return EFI_SUCCESS;
}
