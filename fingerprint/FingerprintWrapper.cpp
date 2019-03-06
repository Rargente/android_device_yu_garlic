/*
 * Copyright (C) 2018, The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "FingerprintWrapper"

#include <dlfcn.h>
#include <cstdio>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <hardware/fingerprint.h>
#include <utils/threads.h>

#define CHIP_ID_NONE -1
#define CHIP_ID_SILEAD 1
#define CHIP_ID_GOODIX 2
#define CHIP_ID_ELAN 3
#define CHIP_ID_CHIPONE 4
int chip_id = -1;

typedef struct {
    fingerprint_device_t base;
    union {
        fingerprint_device_t *device;
        hw_device_t *hw_device;
    } vendor;
} device_t;

static android::Mutex vendor_mutex;

static union {
    const fingerprint_module_t *module;
    const hw_module_t *hw_module;
} vendor;

int getChipID()
{
    if (chip_id == -1) {
        int fd = open("/sys/devices/platform/fp_drv/fp_drv_info", 0);
        if (fd <= 0) {
            ALOGE("Cannot open: %s", "/sys/devices/platform/fp_drv/fp_drv_info");
            return chip_id;
		    }
        char fp_drv[50];
        memset(fp_drv, 0, sizeof(fp_drv));
        int result = read(fd, fp_drv, sizeof(fp_drv));
        ALOGD("read file result:%d,fp_drv: %s", result, fp_drv);

        if (!strcmp(fp_drv, "silead_fp")) {
            chip_id = CHIP_ID_SILEAD;
            ALOGD("match: /drv/%s Ok!\n", "silead_fp");
        } else if (!strcmp(fp_drv, "goodix_fp")) {
            chip_id = CHIP_ID_GOODIX;
            ALOGD("match: /drv/%s Ok!\n", "goodix_fp");
        } else if (!strcmp(fp_drv, "elan_fp")) {
            chip_id = CHIP_ID_ELAN;
            ALOGD("match: /drv/%s Ok!\n", "elan_fp");
        } else if (!strcmp(fp_drv, "chipone_fp")) {
            chip_id = CHIP_ID_CHIPONE;
            ALOGD("match: /drv/%s Ok!\n", "chipone_fp");
        } else
            ALOGD("not mach ANY fp drv !\n");

        close(fd);
    }

	return chip_id;
}

static bool ensure_vendor_module_is_loaded(void)
{
    android::Mutex::Autolock lock(vendor_mutex);

    if (!vendor.module) {
        int rv;
        int id = getChipID();

        if (id == CHIP_ID_SILEAD) {
            if (0 != (rv = hw_get_module("fingerprint.silead", &vendor.hw_module)))
                ALOGE("Can't open fingerprint HW Module, error: %d", rv);
            property_set("persist.sys.fp.goodix", "0");
            property_set("persist.sys.fp.silead", "1");
        } else if (id == CHIP_ID_GOODIX) {
            if (0 != (rv = hw_get_module("fingerprint.goodix", &vendor.hw_module)))
                ALOGE("Can't open fingerprint HW Module, error: %d", rv);
            property_set("persist.sys.fp.goodix", "1");
            property_set("persist.sys.fp.silead", "0");
        } else if (id == CHIP_ID_CHIPONE) {
            if (0 != (rv = hw_get_module("fingerprint.chipone", &vendor.hw_module)))
                ALOGE("Can't open fingerprint HW Module, error: %d", rv);
            property_set("persist.sys.fp.goodix", "0");
            property_set("persist.sys.fp.silead", "0");
        } else {
            if (0 != (rv = hw_get_module("fingerprint.elan", &vendor.hw_module)))
                ALOGE("Can't open fingerprint HW Module, error: %d", rv);
            property_set("persist.sys.fp.goodix", "0");
            property_set("persist.sys.fp.silead", "0");
        }
        if (rv) {
            ALOGE("failed to open vendor module, error %d", rv);
            vendor.module = NULL;
        } else {
            ALOGI("loaded vendor module: %s version %x", vendor.module->common.name,
                vendor.module->common.module_api_version);
        }
    }

    return vendor.module != NULL;
}

static int set_notify(struct fingerprint_device *dev, fingerprint_notify_t notify)
{
    device_t *device = (device_t *) dev;

    device->base.notify = notify;
    return device->vendor.device->set_notify(device->vendor.device, notify);
}

static uint64_t pre_enroll(struct fingerprint_device *dev)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->pre_enroll(device->vendor.device);
}

static int enroll(struct fingerprint_device *dev, const hw_auth_token_t *hat, uint32_t gid,
                uint32_t timeout_sec)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->enroll(device->vendor.device, hat, gid, timeout_sec);
}

static int post_enroll(struct fingerprint_device *dev)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->post_enroll(device->vendor.device);
}

static uint64_t get_authenticator_id(struct fingerprint_device *dev)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->get_authenticator_id(device->vendor.device);
}

static int cancel(struct fingerprint_device *dev)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->cancel(device->vendor.device);
}

#define MAX_FINGERPRINTS 100

typedef int (*enumerate_2_0)(struct fingerprint_device *dev, fingerprint_finger_id_t *results,
        uint32_t *max_size);

static int enumerate_pre_2_1(struct fingerprint_device *dev)
{
    device_t *device = (device_t *) dev;
    fingerprint_finger_id_t results[MAX_FINGERPRINTS];
    uint32_t n = MAX_FINGERPRINTS;
    enumerate_2_0 enumerate = (enumerate_2_0) device->vendor.device->enumerate;
    int rv = enumerate(device->vendor.device, results, &n);

    if (rv == 0) {
        uint32_t i;
        fingerprint_msg_t msg;

        msg.type = FINGERPRINT_TEMPLATE_ENUMERATING;
        for (i = 0; i < n; i++) {
            msg.data.enumerated.finger = results[i];
            msg.data.enumerated.remaining_templates = n - i - 1;
            device->base.notify(&msg);
        }
    }

    return rv;
}

static int enumerate(struct fingerprint_device *dev)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->enumerate(device->vendor.device);
}

static int remove(struct fingerprint_device *dev, uint32_t gid, uint32_t fid)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->remove(device->vendor.device, gid, fid);
}

static int set_active_group(struct fingerprint_device *dev, uint32_t gid, const char *store_path)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->set_active_group(device->vendor.device, gid, store_path);
}

static int authenticate(struct fingerprint_device *dev, uint64_t operation_id, uint32_t gid)
{
    device_t *device = (device_t *) dev;

    return device->vendor.device->authenticate(device->vendor.device, operation_id, gid);
}

static int device_close(hw_device_t *hw_device)
{
    device_t *device = (device_t *) hw_device;
    int rv = device->base.common.close(device->vendor.hw_device);
    free(device);
    return rv;
}

static int device_open(const hw_module_t *module, const char *name, hw_device_t **device_out)
{
    int rv;
    device_t *device;

    if (!ensure_vendor_module_is_loaded()) {
        return -EINVAL;
    }

    device = (device_t *) calloc(sizeof(*device), 1);
    if (!device) {
        ALOGE("%s: Failed to allocate memory", __func__);
        return -ENOMEM;
    }

    rv = vendor.module->common.methods->open(vendor.hw_module, name, &device->vendor.hw_device);
    if (rv) {
        ALOGE("%s: failed to open, error %d\n", __func__, rv);
        free(device);
        return rv;
    }

    device->base.common.tag = HARDWARE_DEVICE_TAG;
    device->base.common.version = device->vendor.device->common.version;
    device->base.common.module = (hw_module_t *) module;
    device->base.common.close = device_close;

    device->base.set_notify = set_notify;
    device->base.pre_enroll = pre_enroll;
    device->base.enroll = enroll;
    device->base.post_enroll = post_enroll;
    device->base.get_authenticator_id = get_authenticator_id;
    device->base.cancel = cancel;
    if (vendor.module->common.module_api_version >= FINGERPRINT_MODULE_API_VERSION_2_1) {
        device->base.enumerate = enumerate;
    } else {
        device->base.enumerate = enumerate_pre_2_1;
    }
    device->base.remove = remove;
    device->base.set_active_group = set_active_group;
    device->base.authenticate = authenticate;

    *device_out = (hw_device_t *) device;
    return 0;
}

static struct hw_module_methods_t module_methods = {
    .open = device_open
};

fingerprint_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = FINGERPRINT_MODULE_API_VERSION_2_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = FINGERPRINT_HARDWARE_MODULE_ID,
        .name = "Lineage Fingerprint Wrapper",
        .author = "The LineageOS Project",
        .methods = &module_methods,
        .dso = NULL, /* remove compilation warnings */
        .reserved = {0}, /* remove compilation warnings */
    },
};
