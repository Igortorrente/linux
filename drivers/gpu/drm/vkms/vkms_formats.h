// SPDX-License-Identifier: GPL-2.0+

#ifndef _VKMS_FORMATS_H_
#define _VKMS_FORMATS_H_

#include "vkms_drv.h"

plane_format_transform_func get_plane_fmt_transform_function(u32 format);

wb_format_transform_func get_wb_fmt_transform_function(u32 format);

#endif /* _VKMS_FORMATS_H_ */
