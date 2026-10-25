/* gxd-device.h - /net/reactivated/Fprint/Device/0
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "gxd.h"
#include "gxd-sensor.h"

#define GXD_TYPE_DEVICE (gxd_device_get_type ())
G_DECLARE_FINAL_TYPE (GxdDevice, gxd_device, GXD, DEVICE, FprintDBusDeviceSkeleton)

GxdDevice *gxd_device_new (GxdSensor *sensor, GError **error);
