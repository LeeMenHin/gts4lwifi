/* Copyright (c) 2011-2016, The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/crc32.h>
#include <linux/lzo.h>
#include <linux/vmalloc.h>
#include "msm_sd.h"
#include "msm_cci.h"
#include "msm_eeprom.h"
#include "msm_actuator_fpga.h"

#undef CDBG
#define CDBG(fmt, args...) pr_debug(fmt, ##args)

DEFINE_MSM_MUTEX(msm_eeprom_mutex);
#ifdef CONFIG_COMPAT
static struct v4l2_file_operations msm_eeprom_v4l2_subdev_fops;
#endif

extern uint32_t front_af_cal_pan;
extern uint32_t front_af_cal_macro;
#if defined(CONFIG_SAMSUNG_MULTI_CAMERA)
extern uint8_t rear_dual_cal[FROM_REAR_DUAL_CAL_SIZE + 1];
extern int rear2_af_cal[FROM_REAR_AF_CAL_SIZE + 1];
#endif
extern int rear_af_cal[FROM_REAR_AF_CAL_SIZE + 1];
extern char rear_sensor_id[FROM_SENSOR_ID_SIZE + 1];
extern char front_sensor_id[FROM_SENSOR_ID_SIZE + 1];
#if defined(CONFIG_SAMSUNG_MULTI_CAMERA)
extern char rear2_sensor_id[FROM_SENSOR_ID_SIZE + 1];
extern int rear2_dual_tilt_x;
extern int rear2_dual_tilt_y;
extern int rear2_dual_tilt_z;
extern int rear2_dual_tilt_sx;
extern int rear2_dual_tilt_sy;
extern int rear2_dual_tilt_range;
extern int rear2_dual_tilt_max_err;
extern int rear2_dual_tilt_avg_err;
extern int rear2_dual_tilt_dll_ver;
#endif

extern uint8_t rear_module_id[FROM_MODULE_ID_SIZE + 1];
extern uint8_t front_module_id[FROM_MODULE_ID_SIZE + 1];

extern char front_mtf_exif[FROM_MTF_SIZE + 1];
extern char rear_mtf_exif[FROM_MTF_SIZE + 1];
#if defined(CONFIG_SAMSUNG_MULTI_CAMERA)
extern char rear2_mtf_exif[FROM_MTF_SIZE + 1];
#endif

static int msm_eeprom_get_dt_data(struct msm_eeprom_ctrl_t *e_ctrl);
static long msm_eeprom_subdev_fops_ioctl32(struct file *file, unsigned int cmd, unsigned long arg);
/**
  * msm_eeprom_verify_sum - verify crc32 checksum
  * @mem:			data buffer
  * @size:			size of data buffer
  * @sum:			expected checksum
  * @rev_endian:	compare reversed endian (0:little, 1:big)
  *
  * Returns 0 if checksum match, -EINVAL otherwise.
  */
static int msm_eeprom_verify_sum(const char *mem, uint32_t size, uint32_t sum, uint32_t rev_endian)
{
	uint32_t crc = ~0;
	uint32_t cmp_crc = 0;

	/* check overflow */
	if (size > crc - sizeof(uint32_t))
		return -EINVAL;

	crc = crc32_le(crc, mem, size);

	crc = ~crc;
	if(rev_endian == 1)	{
		cmp_crc = (((crc) & 0xFF) << 24)
				| (((crc) & 0xFF00) << 8)
				| (((crc) >> 8) & 0xFF00)
				| ((crc) >> 24);
	} else {
		cmp_crc = crc;
	}

	if (cmp_crc != sum) {
		pr_err("%s: rev %d, expect 0x%x, result 0x%x\n", __func__, rev_endian, sum, cmp_crc);
		return -EINVAL;
	}

	CDBG("%s: checksum pass 0x%x\n", __func__, sum);
	return 0;
}

/**
  * msm_eeprom_match_crc - verify multiple regions using crc
  * @data:	data block to be verified
  *
  * Iterates through all regions stored in @data.  Regions with odd index
  * are treated as data, and its next region is treated as checksum.  Thus
  * regions of even index must have valid_size of 4 or 0 (skip verification).
  * Returns a bitmask of verified regions, starting from LSB.  1 indicates
  * a checksum match, while 0 indicates checksum mismatch or not verified.
  */
static uint32_t msm_eeprom_match_crc(struct msm_eeprom_memory_block_t *data, uint32_t subdev_id)
{
	int j, rc;
	uint32_t *sum;
	uint32_t ret = 0;
	uint8_t *memptr, *memptr_crc;
	uint8_t map_ver = 0;
	struct msm_eeprom_memory_map_t *map;

	if (!data) {
		pr_err("%s data is NULL \n", __func__);
		return -EINVAL;
	}
	map = data->map;

	if (subdev_id == 1) // Front cam
		map_ver = data->mapdata[FRONT_CAM_MAP_VERSION_ADDR];
	else
		map_ver = data->mapdata[REAR_CAM_MAP_VERSION_ADDR];

	pr_err("%s: map version = %c [0x%x]\n", __func__, map_ver, map_ver);
	for (j = 0; j + 1 < data->num_map; j += 2) {
		memptr = data->mapdata + map[j].mem.addr;
		memptr_crc = data->mapdata + map[j+1].mem.addr;

		/* empty table or no checksum */
		if (!map[j].mem.valid_size || !map[j+1].mem.valid_size) {
			continue;
		}
		if (map[j+1].mem.valid_size != sizeof(uint32_t)) {
			CDBG("%s: malformatted data mapping\n", __func__);
			return -EINVAL;
		}
		sum = (uint32_t *) (memptr_crc);
		rc = msm_eeprom_verify_sum(memptr, map[j].mem.valid_size, *sum, 0);

		if (!rc)
		{
			ret |= 1 << (j/2);
		}
	}

	//  if PAF cal data has error(even though CRC is correct),
	//  set crc value of PAF cal data to 0.
	if(subdev_id == 0 && data->mapdata[0x4A] == 'M')
	{
		uint32_t PAF_err = *((uint32_t *)(&data->mapdata[0x2804]));

		CDBG("%s: PAF_err = 0x%08X, ret = 0x%04X\n", __func__, PAF_err, ret);

		if(PAF_err != 0)
		{
			ret &= 0xFB;  //  exclude 3rd bit(map index : 6)
			pr_err("%s: PAF_err = 0x%08X, ret = 0x%04X\n", __func__, PAF_err, ret);
		}
	}

	return ret;
}

static int msm_eeprom_get_cmm_data(struct msm_eeprom_ctrl_t *e_ctrl,
				       struct msm_eeprom_cfg_data *cdata)
{
	int rc = 0;
	struct msm_eeprom_cmm_t *cmm_data = &e_ctrl->eboard_info->cmm_data;
	cdata->cfg.get_cmm_data.cmm_support = cmm_data->cmm_support;
	cdata->cfg.get_cmm_data.cmm_compression = cmm_data->cmm_compression;
	cdata->cfg.get_cmm_data.cmm_size = cmm_data->cmm_size;
	return rc;
}

static int eeprom_config_read_cal_data(struct msm_eeprom_ctrl_t *e_ctrl,
	struct msm_eeprom_cfg_data *cdata)
{
	int rc;

	/* check range */
	if (cdata->cfg.read_data.num_bytes >
		e_ctrl->cal_data.num_data) {
		CDBG("%s: Invalid size. exp %u, req %u\n", __func__,
			e_ctrl->cal_data.num_data,
			cdata->cfg.read_data.num_bytes);
		return -EINVAL;
	}
	if (!e_ctrl->cal_data.mapdata) {
		pr_err("%s : is NULL \n", __func__);
		return -EFAULT;
	}

	CDBG("%s:%d: subdevid: %d\n",__func__,__LINE__,e_ctrl->subdev_id);
	rc = copy_to_user(cdata->cfg.read_data.dbuffer,
		e_ctrl->cal_data.mapdata,
		cdata->cfg.read_data.num_bytes);

	return rc;
}

static int msm_eeprom_config(struct msm_eeprom_ctrl_t *e_ctrl,
	void __user *argp)
{
	struct msm_eeprom_cfg_data *cdata =
		(struct msm_eeprom_cfg_data *)argp;
	int rc = 0;
	size_t length = 0;

	CDBG("%s E\n", __func__);
	switch (cdata->cfgtype) {
	case CFG_EEPROM_GET_INFO:
		CDBG("%s E CFG_EEPROM_GET_INFO\n", __func__);
		cdata->is_supported = e_ctrl->is_supported;
		length = strlen(e_ctrl->eboard_info->eeprom_name) + 1;
		if (length > MAX_EEPROM_NAME) {
			pr_err("%s:%d invalid eeprom_name length %d\n",
				__func__, __LINE__, (int)length);
			rc = -EINVAL;
			break;
		}
		memcpy(cdata->cfg.eeprom_name,
			e_ctrl->eboard_info->eeprom_name, length);
		break;
	case CFG_EEPROM_GET_CAL_DATA:
		CDBG("%s E CFG_EEPROM_GET_CAL_DATA\n", __func__);
		cdata->cfg.get_data.num_bytes =
			e_ctrl->cal_data.num_data;
		break;
	case CFG_EEPROM_READ_CAL_DATA:
		CDBG("%s E CFG_EEPROM_READ_CAL_DATA\n", __func__);
		rc = eeprom_config_read_cal_data(e_ctrl, cdata);
		break;
	case CFG_EEPROM_GET_MM_INFO:
		CDBG("%s E CFG_EEPROM_GET_MM_INFO\n", __func__);
		rc = msm_eeprom_get_cmm_data(e_ctrl, cdata);
		break;
	default:
		break;
	}

	CDBG("%s X rc: %d\n", __func__, rc);
	return rc;
}

static int msm_eeprom_get_subdev_id(struct msm_eeprom_ctrl_t *e_ctrl,
				    void *arg)
{
	uint32_t *subdev_id = (uint32_t *)arg;
	CDBG("%s E\n", __func__);
	if (!subdev_id) {
		pr_err("%s failed\n", __func__);
		return -EINVAL;
	}
	*subdev_id = e_ctrl->subdev_id;
	CDBG("subdev_id %d\n", *subdev_id);
	CDBG("%s X\n", __func__);
	return 0;
}

static long msm_eeprom_subdev_ioctl(struct v4l2_subdev *sd,
		unsigned int cmd, void *arg)
{
	struct msm_eeprom_ctrl_t *e_ctrl = v4l2_get_subdevdata(sd);
	void __user *argp = (void __user *)arg;
	CDBG("%s E\n", __func__);
	CDBG("%s:%d a_ctrl %pK argp %pK\n", __func__, __LINE__, e_ctrl, argp);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_eeprom_get_subdev_id(e_ctrl, argp);
	case VIDIOC_MSM_EEPROM_CFG:
		return msm_eeprom_config(e_ctrl, argp);
	default:
		return -ENOIOCTLCMD;
	}

	CDBG("%s X\n", __func__);
}

#if defined(CONFIG_USE_ACTUATOR_FPGA)
static struct msm_camera_i2c_fn_t msm_eeprom_fpga_iic_func_tbl = {
	.i2c_read = msm_camera_fpga_i2c_read,
	.i2c_write = msm_camera_fpga_i2c_write,
	.i2c_write_table_w_microdelay =
		msm_camera_fpga_i2c_write_table_w_microdelay,
	.i2c_read_seq = msm_camera_fpga_i2c_read_seq,
	.i2c_write_seq = msm_camera_fpga_i2c_write_seq,
	.i2c_write_table = msm_camera_fpga_i2c_write_table,
	.i2c_write_seq_table = msm_camera_fpga_i2c_write_seq_table,
	.i2c_poll = msm_camera_fpga_i2c_poll,
	.i2c_util = msm_camera_fpga_i2c_util,
};
#else
static struct msm_camera_i2c_fn_t msm_eeprom_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_seq = msm_camera_cci_i2c_write_seq,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
	msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
	.i2c_poll = msm_camera_cci_i2c_poll,
};
#endif

static struct msm_camera_i2c_fn_t msm_eeprom_qup_func_tbl = {
	.i2c_read = msm_camera_qup_i2c_read,
	.i2c_read_seq = msm_camera_qup_i2c_read_seq,
	.i2c_write = msm_camera_qup_i2c_write,
	.i2c_write_table = msm_camera_qup_i2c_write_table,
	.i2c_write_seq_table = msm_camera_qup_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
	msm_camera_qup_i2c_write_table_w_microdelay,
};

static struct msm_camera_i2c_fn_t msm_eeprom_spi_func_tbl = {
	.i2c_read = msm_camera_spi_read,
	.i2c_read_seq = msm_camera_spi_read_seq,
	.i2c_write_seq = msm_camera_spi_write_seq,
};

static int msm_eeprom_open(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_eeprom_ctrl_t *e_ctrl = v4l2_get_subdevdata(sd);
	CDBG("%s E\n", __func__);
	if (!e_ctrl) {
		pr_err("%s failed e_ctrl is NULL\n", __func__);
		return -EINVAL;
	}
	CDBG("%s X\n", __func__);
	return rc;
}

static int msm_eeprom_close(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_eeprom_ctrl_t *e_ctrl = v4l2_get_subdevdata(sd);
	CDBG("%s E\n", __func__);
	if (!e_ctrl) {
		pr_err("%s failed e_ctrl is NULL\n", __func__);
		return -EINVAL;
	}
	CDBG("%s X\n", __func__);
	return rc;
}

static const struct v4l2_subdev_internal_ops msm_eeprom_internal_ops = {
	.open = msm_eeprom_open,
	.close = msm_eeprom_close,
};

/**
  * read_eeprom_memory() - read map data into buffer
  * @e_ctrl:	eeprom control struct
  * @block:	block to be read
  *
  * This function iterates through blocks stored in block->map, reads each
  * region and concatenate them into the pre-allocated block->mapdata
  */
static int read_eeprom_memory(struct msm_eeprom_ctrl_t *e_ctrl,
			      struct msm_eeprom_memory_block_t *block)
{
	int rc = 0;
	int j;
	struct msm_eeprom_memory_map_t *emap = block->map;
	struct msm_eeprom_board_info *eb_info;
	uint8_t *memptr = block->mapdata;
	uint32_t read_addr, read_size, size, max_size;

	if (!e_ctrl) {
		pr_err("%s e_ctrl is NULL \n", __func__);
		return -EINVAL;
	}

	eb_info = e_ctrl->eboard_info;

	if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE) {
		max_size = I2C_BAM_MAX_SIZE;
#if defined(CONFIG_USE_ACTUATOR_FPGA)
	} else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		max_size = FPGA_I2C_MAX_SIZE;
#endif
	} else {
		max_size = SPI_TRANSFER_MAX_SIZE;
	}

	for (j = 0; j < block->num_map; j++) {
		if (emap[j].saddr.addr) {
			eb_info->i2c_slaveaddr = emap[j].saddr.addr;
			e_ctrl->i2c_client.cci_client->sid =
					eb_info->i2c_slaveaddr >> 1;
			pr_err("qcom,slave-addr = 0x%X\n",
				eb_info->i2c_slaveaddr);
		}

		if (emap[j].page.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].page.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].page.addr,
				emap[j].page.data, emap[j].page.data_t);
				msleep(emap[j].page.delay);
			if (rc < 0) {
				pr_err("%s: page write failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				emap[j].pageen.data, emap[j].pageen.data_t);
				msleep(emap[j].pageen.delay);
			if (rc < 0) {
				pr_err("%s: page enable failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].poll.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].poll.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_poll(
				&(e_ctrl->i2c_client), emap[j].poll.addr,
				emap[j].poll.data, emap[j].poll.data_t,
				emap[j].poll.delay);
			if (rc < 0) {
				pr_err("%s: poll failed\n", __func__);
				return rc;
			}
		}

		if (emap[j].mem.valid_size) {
			memptr = block->mapdata + emap[j].mem.addr;
			CDBG("%s: %d memptr = %p, addr = 0x%X, size = %d, subdev = %d\n", __func__, __LINE__, memptr, emap[j].mem.addr, emap[j].mem.valid_size, e_ctrl->subdev_id);

			if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE || e_ctrl->eeprom_device_type == MSM_CAMERA_SPI_DEVICE ||
				e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
				if(e_ctrl->eeprom_device_type == MSM_CAMERA_SPI_DEVICE && emap[j].mem.data_t == 0) {
					continue;
				}

				e_ctrl->i2c_client.addr_type = emap[j].mem.addr_t;
				read_addr = emap[j].mem.addr;
				read_size = emap[j].mem.valid_size;
				for (size = emap[j].mem.valid_size; size > 0; size -= read_size)
				{
					if (size > max_size) { // i2c bam max size
						read_size = max_size;
					} else {
						read_size = size;
					}
					rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
						&(e_ctrl->i2c_client), read_addr, memptr, read_size);
					if (rc < 0) {
						pr_err("%s: read failed\n", __func__);
						return rc;
					}

					if (size > max_size) { // i2c bam max size
						memptr += read_size;
						read_addr += read_size;
					}
				}
			} else {
				pr_err("%s: not supported device type\n", __func__);
			}
		}

		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				0, emap[j].pageen.data_t);
			if (rc < 0) {
				pr_err("%s: page disable failed\n", __func__);
				return rc;
			}
		}
	}
	return rc;
}
/**
  * msm_eeprom_parse_memory_map() - parse memory map in device node
  * @of:	device node
  * @data:	memory block for output
  *
  * This functions parses @of to fill @data.  It allocates map itself, parses
  * the @of node, calculate total data length, and allocates required buffer.
  * It only fills the map, but does not perform actual reading.
  */
static int msm_eeprom_parse_memory_map(struct device_node *of,
				       struct msm_eeprom_memory_block_t *data)
{
	int i, rc = 0;
	char property[PROPERTY_MAXSIZE];
	uint32_t count = 6;
	struct msm_eeprom_memory_map_t *map;
	uint32_t total_size = 0;

	snprintf(property, PROPERTY_MAXSIZE, "qcom,num-blocks");
	rc = of_property_read_u32(of, property, &data->num_map);
	CDBG("%s::%d  %s %d\n", __func__, __LINE__, property, data->num_map);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		return rc;
	}

	map = kzalloc((sizeof(*map) * data->num_map), GFP_KERNEL);
	if (!map) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		return -ENOMEM;
	}
	data->map = map;

	for (i = 0; i < data->num_map; i++) {
		CDBG("%s, %d: i = %d\n", __func__, __LINE__, i);

		snprintf(property, PROPERTY_MAXSIZE, "qcom,page%d", i);
		rc = of_property_read_u32_array(of, property,
				(uint32_t *) &map[i].page, count);
		if (rc < 0) {
			pr_err("%s: failed %d\n", __func__, __LINE__);
			goto ERROR;
		}
#if 0// TEMP_8998 : Temp eeprom
		snprintf(property, PROPERTY_MAXSIZE,
					"qcom,pageen%d", i);
		rc = of_property_read_u32_array(of, property,
			(uint32_t *) &map[i].pageen, count);
		if (rc < 0)
			CDBG("%s: pageen not needed\n", __func__);

		snprintf(property, PROPERTY_MAXSIZE, "qcom,saddr%d", i);
		rc = of_property_read_u32_array(of, property,
			(uint32_t *) &map[i].saddr.addr, 1);
		if (rc < 0)
			CDBG("%s: saddr not needed - block %d\n", __func__, i);
#endif
		snprintf(property, PROPERTY_MAXSIZE, "qcom,poll%d", i);
		rc = of_property_read_u32_array(of, property,
				(uint32_t *) &map[i].poll, count);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR;
		}

		snprintf(property, PROPERTY_MAXSIZE, "qcom,mem%d", i);
		rc = of_property_read_u32_array(of, property,
				(uint32_t *) &map[i].mem, count);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR;
		}

		if(map[i].mem.data_t == 1)
		{
			data->num_data += map[i].mem.valid_size;
		}
	}

	CDBG("%s::%d valid size = %d\n", __func__,__LINE__, data->num_data);

	// if total-size is defined at dtsi file.
	// set num_data as total-size
	snprintf(property, PROPERTY_MAXSIZE, "qcom,total-size");
	rc = of_property_read_u32(of, property, &total_size);
	CDBG("%s::%d  %s %d\n", __func__,__LINE__,property, total_size);

	// if "qcom,total-size" propoerty exists.
	if (rc >= 0) {
		pr_err("%s::%d set num_data as total-size (num_map : %d, total : %d, valid : %d)\n",
			__func__,__LINE__, data->num_map, total_size, data->num_data);
		data->num_data = total_size;
	}

	data->mapdata = kzalloc(data->num_data, GFP_KERNEL);
	if (!data->mapdata) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto ERROR;
	}
	return rc;

ERROR:
	kfree(data->map);
	memset(data, 0, sizeof(*data));
	return rc;
}

static struct msm_cam_clk_info cam_8960_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_clk", 24000000},
};

static struct msm_cam_clk_info cam_8974_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_src_clk", 24000000},
	[SENSOR_CAM_CLK] = {"cam_clk", 0},
};

static int msm_eeprom_match_id(struct msm_eeprom_ctrl_t *e_ctrl, bool bShowLog)
{
	int rc;
	struct msm_camera_i2c_client *client = &e_ctrl->i2c_client;
	uint8_t id[2];
	uint8_t read_data[4] = {0,};

	CDBG("%s: subdev_id[%d], eeprom_device_type[%d]\n", __func__, e_ctrl->subdev_id, e_ctrl->eeprom_device_type);
	if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE ||
	    e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(client, 0, read_data, sizeof(read_data));
		if (rc < 0)
			return rc;

		if(bShowLog == TRUE)
		{
			pr_err("%s: read 0x%02x 0x%02x 0x%02x 0x%02x\n",
				__func__, read_data[0], read_data[1], read_data[2], read_data[3]);
		}
	} else {
		rc = msm_camera_spi_query_id(client, 0, &id[0], 2);
		if (rc < 0)
			return rc;

		if(bShowLog == TRUE)
		{
			// Fidelix 32Mb:0xF815, Winbond 32Mb:0xEF15, Macronix 32Mb:0xC236, GigaDevice 32Mb:0xC815
			pr_err("%s: read 0x%02X%02X\n", __func__, id[0], id[1]);
		}

#if 1/* for DFMS test */
		if ((id[0] == client->spi_client->mfr_id0 && id[1] == client->spi_client->device_id0)
			|| (id[0] == client->spi_client->mfr_id1 && id[1] == client->spi_client->device_id1)
			|| (id[0] == client->spi_client->mfr_id2 && id[1] == client->spi_client->device_id2)
			|| (id[0] == client->spi_client->mfr_id3 && id[1] == client->spi_client->device_id3))
			return 0;

		return -ENODEV;
#else
		if (id[0] == 0x00 || id[1] == 0x00)
			return -ENODEV;
#endif
	}

	return 0;
}

/**
  * msm_eeprom_power_down() - power down eeprom
  * @e_ctrl:	control struct
  * @down:	indicate whether kernel powered up eeprom before
  *
  * This function powers down EEPROM only if it's powered on by calling
  * msm_eeprom_power_up() before.  If @down is false, no action will be
  * taken.  Otherwise, eeprom will be powered down.
  */
static int msm_eeprom_power_down(struct msm_eeprom_ctrl_t *e_ctrl, bool down)
{
	int rc = 0;

	CDBG("%s [POWER_DBG] : E \n", __func__);

	if (down) {
		rc = msm_camera_power_down(&e_ctrl->eboard_info->power_info,
			e_ctrl->eeprom_device_type, &e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
		if (rc < 0)
			pr_err("%s : msm_camera_power_down failed\n", __func__);
	}
	return rc;
}

/**
  * msm_eeprom_power_up() - power up eeprom if it's not on
  * @e_ctrl:	control struct
  * @down:	output to indicate whether power down is needed later
  *
  * This function powers up EEPROM only if it's not already on.  If power
  * up is performed here, @down will be set to true.  Caller should power
  * down EEPROM after transaction if @down is true.
  */
static int msm_eeprom_power_up(struct msm_eeprom_ctrl_t *e_ctrl, bool *down, bool bShowLog)
{
	int rc = 0;

	CDBG("%s [POWER_DBG] : E \n", __func__);

	if (down)
		*down = FALSE;

	rc = msm_camera_power_up(&e_ctrl->eboard_info->power_info,
		e_ctrl->eeprom_device_type, &e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
	if (rc < 0)
		pr_err("%s : msm_camera_power_up failed\n", __func__);
	else {
		if (rc != NO_POWER_OFF && down) {
			*down = TRUE;
		}
		usleep_range(1*1000, 2*1000);	//time margin after power up
		rc = msm_eeprom_match_id(e_ctrl, bShowLog);
		if (rc < 0) {
			pr_err("%s : msm_eeprom_match_id failed\n", __func__);
			if (down)
				msm_eeprom_power_down(e_ctrl, *down);
		}
	}
	return rc;
}

static int eeprom_config_decompress(struct msm_eeprom_ctrl_t *e_ctrl,
	struct msm_eeprom_cfg_data *cdata)
{
	int rc = 0;
	uint8_t *buf_comp = cdata->cfg.comp_read_data.cbuffer;
	uint8_t *buf_decomp = NULL;
	size_t comp_size;
	size_t decomp_size;
	static uint8_t isFWcheck2 = 0;

	buf_decomp = vmalloc(cdata->cfg.comp_read_data.num_bytes);
	if (!buf_decomp) {
		pr_err("%s: vmalloc fail \n", __func__);
		rc = -ENOMEM;
		goto FREE;
	}

	comp_size = cdata->cfg.comp_read_data.comp_size-4;

	pr_info("%s: address = 0x%x, comp = %d, decomp =%d, crc = 0x%08X\n",
		__func__, cdata->cfg.comp_read_data.addr, cdata->cfg.comp_read_data.comp_size,
		cdata->cfg.comp_read_data.num_bytes, *(uint32_t*)&buf_comp[comp_size]);

	//	compressed data(buf_comp) contains uncompressed crc32 value.
	rc = msm_eeprom_verify_sum(buf_comp, comp_size, *(uint32_t*)&buf_comp[comp_size], 1);

	if (rc < 0) {
		pr_err("%s: crc check error, rc %d\n", __func__, rc);
		if (isFWcheck2 == 0) {
			msm_camera_fw_check('F', CHECK_CAMERA_FW);//F: Fail
			isFWcheck2++;
		}
		goto FREE;
	}

	decomp_size = cdata->cfg.comp_read_data.num_bytes;
	rc = lzo1x_decompress_safe(buf_comp, comp_size, buf_decomp, &decomp_size);
	if (rc != LZO_E_OK) {
		pr_err("%s: decompression failed %d \n", __func__, rc);
		goto FREE;
	}
	rc = copy_to_user(cdata->cfg.comp_read_data.dbuffer, buf_decomp, decomp_size);

	if (rc < 0) {
		pr_err("%s: failed to copy to user\n", __func__);
		goto FREE;
	}

	CDBG("%s: done \n", __func__);

	FREE:
	if (buf_decomp) vfree(buf_decomp);

	return rc;
}

static int eeprom_config_read_compressed_data(struct msm_eeprom_ctrl_t *e_ctrl,
	struct msm_eeprom_cfg_data *cdata)
{
	int rc = 0;
#if 0 //  just once to power up when load lib
	bool down;
#endif

	uint8_t *buf_comp = NULL;
	uint8_t *buf_decomp = NULL;
	size_t decomp_size;
	static uint8_t isFWcheck = 0;

	int max_size = 0, read_size = 0, size = 0;
	uint32_t read_addr = 0, memptr = 0;

	pr_info("%s: address (0x%x) comp_size (%d) after decomp (%d) \n", __func__,
	cdata->cfg.read_data.addr,
	cdata->cfg.read_data.comp_size, cdata->cfg.read_data.num_bytes);

	buf_comp = vmalloc(cdata->cfg.read_data.comp_size);
	buf_decomp = vmalloc(cdata->cfg.read_data.num_bytes);
	if (!buf_decomp || !buf_comp) {
		pr_err("%s: vmalloc fail \n", __func__);
		rc = -ENOMEM;
		goto FREE;
	}

#if 0 //  just once to power up when load lib
	rc = msm_eeprom_power_up(e_ctrl, &down);
	if (rc < 0) {
		pr_err("%s: failed to power on eeprom\n", __func__);
		goto FREE;
	}
#endif

	if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE) {
		max_size = I2C_BAM_MAX_SIZE;
#if defined(CONFIG_USE_ACTUATOR_FPGA)
	} else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		max_size = FPGA_I2C_MAX_SIZE;
#endif
	} else {
		max_size = SPI_TRANSFER_MAX_SIZE;
	}

	read_size = cdata->cfg.read_data.comp_size;
	read_addr = cdata->cfg.read_data.addr;
	memptr = 0;
	for (size = cdata->cfg.read_data.comp_size; size > 0; size -= read_size)
	{
		if (size > max_size) {
			read_size = max_size;
		} else {
			read_size = size;
		}

		rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
			&(e_ctrl->i2c_client), read_addr, &buf_comp[memptr], read_size);
		if (rc < 0) {
			pr_err("%s: failed to read data, rc %d\n", __func__, rc);
			goto POWER_DOWN;
		}

		if (size > max_size) {
			memptr += read_size;
			read_addr += read_size;
		}
	}

	pr_info("%s: crc = 0x%08X\n", __func__, *(uint32_t*)&buf_comp[cdata->cfg.read_data.comp_size-4]);

	//  compressed data(buf_comp) contains uncompressed crc32 value.
	rc = msm_eeprom_verify_sum(buf_comp, cdata->cfg.read_data.comp_size-4,
		*(uint32_t*)&buf_comp[cdata->cfg.read_data.comp_size-4], 1);

	if (rc < 0) {
		pr_err("%s: crc check error, rc %d\n", __func__, rc);
		if (isFWcheck == 0) {
			msm_camera_fw_check('F', CHECK_CAMERA_FW);//F: Fail
			isFWcheck++;
		}
		goto POWER_DOWN;
	}

	decomp_size = cdata->cfg.read_data.num_bytes;
	rc = lzo1x_decompress_safe(buf_comp, cdata->cfg.read_data.comp_size-4,
	                     buf_decomp, &decomp_size);
	if (rc != LZO_E_OK) {
		pr_err("%s: decompression failed %d \n", __func__, rc);
		goto POWER_DOWN;
	}
	rc = copy_to_user(cdata->cfg.read_data.dbuffer, buf_decomp, decomp_size);

	if (rc < 0) {
		pr_err("%s: failed to copy to user\n", __func__);
		goto POWER_DOWN;
	}

	pr_info("%s: done \n", __func__);

	POWER_DOWN:
#if 0 //  just once to power up when load lib
	msm_eeprom_power_down(e_ctrl, down);
#endif

	FREE:
	if (buf_comp) vfree(buf_comp);
	if (buf_decomp) vfree(buf_decomp);

	return rc;
}

static int eeprom_config_compress_data(struct msm_eeprom_ctrl_t *e_ctrl,
				    struct msm_eeprom_cfg_data *cdata)
{
	int rc = 0;

	char *buf = NULL;
	void *work_mem = NULL;
	uint8_t *compressed_buf = NULL;
	size_t compressed_size = 0;
	struct eeprom_comp_write_t *data;
	uint32_t crc = ~0;
	uint32_t cmp_crc = 0;

	data = &cdata->cfg.comp_write_data;
	pr_info("%s: size %d \n", __func__, data->num_bytes);

	buf = vmalloc(data->num_bytes);
	if (!buf) {
		pr_err("%s: allocation failed 1 \n", __func__);
		return -ENOMEM;
	}
	rc = copy_from_user(buf, data->dbuffer, data->num_bytes);
	if (rc < 0) {
		pr_err("%s: failed to copy write data\n", __func__);
		goto FREE;
	}

	compressed_buf = vmalloc(data->num_bytes + data->num_bytes / 16 + 64 + 3);
	if (!compressed_buf) {
		pr_err("%s: allocation failed 2 \n", __func__);
		rc = -ENOMEM;
		goto FREE;
	}
	work_mem = vmalloc(LZO1X_1_MEM_COMPRESS);
	if (!work_mem) {
		pr_err("%s: allocation failed 3 \n", __func__);
		rc = -ENOMEM;
		goto FREE;
	}
	if (lzo1x_1_compress(buf, data->num_bytes,
			compressed_buf, &compressed_size, work_mem) != LZO_E_OK) {
		pr_err("%s: compression failed \n", __func__);
		goto FREE;
	}

	memcpy(data->cbuffer, compressed_buf, compressed_size);

	crc = crc32_le(crc, compressed_buf, compressed_size);
	crc = ~crc;

	cmp_crc = crc;
	// swap endian little to big
	crc = (((cmp_crc) & 0xFF) << 24)
		| (((cmp_crc) & 0xFF00) << 8)
		| (((cmp_crc) >> 8) & 0xFF00)
		| ((cmp_crc) >> 24);

	memcpy(&data->cbuffer[compressed_size], (uint8_t *)&crc, 4);
	pr_info("%s: compressed size %d, crc=0x%08X \n", __func__, (uint32_t)compressed_size, crc);
	*data->write_size = (uint32_t)(compressed_size + 4);  //  include CRC size

	CDBG("%s: done \n", __func__);
FREE:
	if (buf) vfree(buf);
	if (compressed_buf) vfree(compressed_buf);
	if (work_mem) vfree(work_mem);

	return rc;
}

static int eeprom_config_write_data(struct msm_eeprom_ctrl_t *e_ctrl,
				    struct msm_eeprom_cfg_data *cdata)
{
	int rc = 0;
	char *buf = NULL;
	bool down;
	bool bShowLog = TRUE;
	void *work_mem = NULL;
	uint8_t *compressed_buf = NULL;
	size_t compressed_size = 0;
	uint32_t crc = ~0;
  uint32_t cmp_crc = 0;

	int max_size = 0, write_size = 0, size = 0;
	uint32_t write_addr = 0, memptr = 0;

	pr_info("%s: compress ? %d size %d \n", __func__,
		cdata->cfg.write_data.compress, cdata->cfg.write_data.num_bytes);
	buf = vmalloc(cdata->cfg.write_data.num_bytes);
	if (!buf) {
		pr_err("%s: allocation failed 1 \n", __func__);
		return -ENOMEM;
	}
	rc = copy_from_user(buf, cdata->cfg.write_data.dbuffer,
			    cdata->cfg.write_data.num_bytes);
	if (rc < 0) {
		pr_err("%s: failed to copy write data\n", __func__);
		goto FREE;
	}
	/* compress */
	if (cdata->cfg.write_data.compress) {
		compressed_buf = vmalloc(cdata->cfg.write_data.num_bytes +
			cdata->cfg.write_data.num_bytes / 16 + 64 + 3);
		if (!compressed_buf) {
			pr_err("%s: allocation failed 2 \n", __func__);
			rc = -ENOMEM;
			goto FREE;
		}
		work_mem = vmalloc(LZO1X_1_MEM_COMPRESS);
		if (!work_mem) {
			pr_err("%s: allocation failed 3 \n", __func__);
			rc = -ENOMEM;
			goto FREE;
		}
		if (lzo1x_1_compress(buf, cdata->cfg.write_data.num_bytes,
				compressed_buf, &compressed_size, work_mem) != LZO_E_OK) {
			pr_err("%s: compression failed \n", __func__);
			goto FREE;
		}

		crc = crc32_le(crc, compressed_buf, compressed_size);
		crc = ~crc;

		cmp_crc = crc;
		// swap endian little to big
		crc = (((cmp_crc) & 0xFF) << 24)
			| (((cmp_crc) & 0xFF00) << 8)
			| (((cmp_crc) >> 8) & 0xFF00)
			| ((cmp_crc) >> 24);

		pr_info("%s: compressed size %d, crc=0x%08X device_type %d \n",
			__func__, (uint32_t)compressed_size, crc, e_ctrl->eeprom_device_type);
		*cdata->cfg.write_data.write_size = (uint32_t)(compressed_size + 4);  //  include CRC size
	}
	rc = msm_eeprom_power_up(e_ctrl, &down, bShowLog);
	if (rc < 0) {
		pr_err("%s: failed to power on eeprom\n", __func__);
		goto FREE;
	}
	usleep_range(10*1000, 11*1000);	//	time margin after power up for Winbond F-ROM

	if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE) {
		max_size = I2C_BAM_MAX_SIZE;
#if defined(CONFIG_USE_ACTUATOR_FPGA)
	} else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		max_size = FPGA_I2C_MAX_SIZE;
#endif
	} else {
		max_size = SPI_TRANSFER_MAX_SIZE;
	}

	if (cdata->cfg.write_data.compress) {
		write_size = compressed_size;
		write_addr = cdata->cfg.write_data.addr;
		memptr = 0;
		for (size = compressed_size; size > 0; size -= write_size)
		{
			if (size > max_size) {
				write_size = max_size;
			} else {
				write_size = size;
			}

			rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_write_seq(
				&(e_ctrl->i2c_client), write_addr, &compressed_buf[memptr], write_size);
			if (rc < 0) {
				pr_err("%s: failed to write data, rc %d\n", __func__, rc);
				goto POWER_DOWN;
			}

			if (size > max_size) {
				memptr += write_size;
				write_addr += write_size;
			}
		}

		//  write CRC32 for compressed data
		rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_write_seq(
			&(e_ctrl->i2c_client), cdata->cfg.write_data.addr+compressed_size,
			(uint8_t *)&crc, 4);
	} else {
		write_size = cdata->cfg.write_data.num_bytes;
		write_addr = cdata->cfg.write_data.addr;
		memptr = 0;
		for (size = cdata->cfg.write_data.num_bytes; size > 0; size -= write_size)
		{
			if (size > max_size) {
				write_size = max_size;
			} else {
				write_size = size;
			}

			rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_write_seq(
				&(e_ctrl->i2c_client), write_addr, &buf[memptr], write_size);
			if (rc < 0) {
				pr_err("%s: failed to write data, rc %d\n", __func__, rc);
				goto POWER_DOWN;
			}

			if (size > max_size) {
				memptr += write_size;
				write_addr += write_size;
			}
		}
	}

	if (rc < 0) {
		pr_err("%s: failed to write data, rc %d\n", __func__, rc);
		goto POWER_DOWN;
	}
	CDBG("%s: done \n", __func__);
POWER_DOWN:
	msm_eeprom_power_down(e_ctrl, down);
FREE:
	if (buf) vfree(buf);
	if (compressed_buf) vfree(compressed_buf);
	if (work_mem) vfree(work_mem);
	return rc;
}

static int eeprom_config_erase(struct msm_eeprom_ctrl_t *e_ctrl,
			       struct msm_eeprom_cfg_data *cdata)
{
	int rc;
	bool down;
	bool bShowLog = FALSE;

	pr_info("%s: erasing addr 0x%x, size %u\n", __func__,
		cdata->cfg.erase_data.addr, cdata->cfg.erase_data.num_bytes);
	rc = msm_eeprom_power_up(e_ctrl, &down, bShowLog);
	if (rc < 0) {
		pr_err("%s: failed to power on eeprom\n", __func__);
		return rc;
	}
	usleep_range(10*1000, 11*1000);	//	time margin after power up for Winbond F-ROM
	rc = msm_camera_spi_erase(&e_ctrl->i2c_client,
		cdata->cfg.erase_data.addr, cdata->cfg.erase_data.num_bytes);
	if (rc < 0)
		pr_err("%s: failed to erase eeprom\n", __func__);
	msm_eeprom_power_down(e_ctrl, down);
	return rc;
}

static int32_t msm_eeprom_read_eeprom_data(struct msm_eeprom_ctrl_t *e_ctrl)
{
	int32_t rc = 0;
	bool down;
	bool bShowLog = FALSE;
	int i;
	int normal_crc_value = 0;

	CDBG("%s:%d Enter\n", __func__, __LINE__);
	/* check eeprom id */
	rc = msm_eeprom_power_up(e_ctrl, &down, bShowLog);
	if (rc < 0) {
		pr_err("%s: failed to power on eeprom\n", __func__);
		return rc;
	}

	normal_crc_value = 0;
	for(i = 0; i < e_ctrl->cal_data.num_map>>1; i ++)
		normal_crc_value |= (1 << i);
	CDBG("num_map = %d, Normal CRC value = 0x%X\n", e_ctrl->cal_data.num_map, normal_crc_value);

	/* read eeprom */
	if (e_ctrl->cal_data.map) {
		rc = read_eeprom_memory(e_ctrl, &e_ctrl->cal_data);
		if (rc < 0) {
			pr_err("%s: read cal data failed\n", __func__);
			goto power_down;
		}
		e_ctrl->is_supported |= msm_eeprom_match_crc(
						&e_ctrl->cal_data, e_ctrl->subdev_id);

		if(e_ctrl->is_supported != normal_crc_value) {
			pr_err("%s : any CRC value(s) are not matched.\n", __func__);
		} else {
			pr_err("%s : All CRC values are matched.\n", __func__);
		}

		if(e_ctrl->subdev_id == 0) {
#if  defined(FROM_REAR_AF_CAL_MACRO_ADDR)
			memcpy(&rear_af_cal[0], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_MACRO_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_PAN_ADDR)
			memcpy(&rear_af_cal[9], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_PAN_ADDR], 4);
#endif
#if defined(FROM_REAR_SENSOR_ID_ADDR)
			/* read rear sensor id */
			memcpy(rear_sensor_id, &e_ctrl->cal_data.mapdata[FROM_REAR_SENSOR_ID_ADDR], FROM_SENSOR_ID_SIZE);
			rear_sensor_id[FROM_SENSOR_ID_SIZE] = '\0';

			CDBG("%s : %d rear id = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
				__func__, e_ctrl->subdev_id, rear_sensor_id[0], rear_sensor_id[1], rear_sensor_id[2], rear_sensor_id[3],
				rear_sensor_id[4], rear_sensor_id[5], rear_sensor_id[6], rear_sensor_id[7], rear_sensor_id[8], rear_sensor_id[9],
				rear_sensor_id[10], rear_sensor_id[11], rear_sensor_id[12], rear_sensor_id[13], rear_sensor_id[14], rear_sensor_id[15]);
#endif
#if defined(FROM_MODULE_ID_ADDR)
			/* read module id */
			memcpy(rear_module_id, &e_ctrl->cal_data.mapdata[FROM_MODULE_ID_ADDR], FROM_MODULE_ID_SIZE);
			rear_module_id[FROM_MODULE_ID_SIZE] = '\0';

			CDBG("%s : %d rear_module_id= %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
				__func__, e_ctrl->subdev_id, rear_module_id[0], rear_module_id[1], rear_module_id[2], rear_module_id[3],
				rear_module_id[4], rear_module_id[5], rear_module_id[6], rear_module_id[7], rear_module_id[8], rear_module_id[9]);
				//rear_module_id[10], rear_module_id[11], rear_module_id[12], rear_module_id[13], rear_module_id[14], rear_module_id[15]);
#endif

		} else if (e_ctrl->subdev_id == 1) { /* read front sensor id */
			memcpy(front_sensor_id, &e_ctrl->cal_data.mapdata[FROM_FRONT_SENSOR_ID_ADDR], FROM_SENSOR_ID_SIZE);
			front_sensor_id[FROM_SENSOR_ID_SIZE] = '\0';
			CDBG("%s : %d sensor id = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", __func__,
				e_ctrl->subdev_id, front_sensor_id[0], front_sensor_id[1], front_sensor_id[2], front_sensor_id[3], front_sensor_id[4],
				front_sensor_id[5], front_sensor_id[6], front_sensor_id[7], front_sensor_id[8], front_sensor_id[9], front_sensor_id[10],
				front_sensor_id[11], front_sensor_id[12], front_sensor_id[13], front_sensor_id[14], front_sensor_id[15]);

			/* front module id */
			memcpy(front_module_id, &e_ctrl->cal_data.mapdata[FROM_MODULE_ID_ADDR], FROM_MODULE_ID_SIZE);
			front_module_id[FROM_MODULE_ID_SIZE] = '\0';
			CDBG("%s : %d front_module_id = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", __func__, e_ctrl->subdev_id,
			front_module_id[0], front_module_id[1], front_module_id[2], front_module_id[3], front_module_id[4],
			front_module_id[5], front_module_id[6], front_module_id[7], front_module_id[8], front_module_id[9]);

			front_af_cal_pan = *((uint32_t *)&e_ctrl->cal_data.mapdata[FROM_FRONT_AF_CAL_PAN_ADDR]);
			front_af_cal_macro = *((uint32_t *)&e_ctrl->cal_data.mapdata[FROM_FRONT_AF_CAL_MACRO_ADDR]);
			CDBG("front_af_cal_pan: %d, front_af_cal_macro: %d\n", front_af_cal_pan, front_af_cal_macro);

			/* front mtf exif */
			memcpy(front_mtf_exif, &e_ctrl->cal_data.mapdata[FROM_FRONT_MTF_ADDR], FROM_MTF_SIZE);
			front_mtf_exif[FROM_MTF_SIZE] = '\0';
			CDBG("%s : %d front mtf exif = %s", __func__, e_ctrl->subdev_id, front_mtf_exif);
		}
	}

	e_ctrl->is_supported = (e_ctrl->is_supported << 1) | 1;
	pr_err("%s : is_supported = 0x%04X\n", __func__, e_ctrl->is_supported);
power_down:
	msm_eeprom_power_down(e_ctrl, down);
	CDBG("%s:%d Exit\n", __func__, __LINE__);
	return rc;
}

static int eeprom_config_read_data(struct msm_eeprom_ctrl_t *e_ctrl,
				struct msm_eeprom_cfg_data *cdata)
{
	char *buf;
	int rc = 0;
	bool down;
	bool bShowLog = FALSE;

	int max_size = 0, read_size = 0, size = 0;
	uint32_t read_addr = 0, memptr = 0;

	buf = vmalloc(cdata->cfg.read_data.num_bytes);
	if (!buf) {
		pr_err("%s : buf is NULL \n", __func__);
		return -ENOMEM;
	}
	rc = msm_eeprom_power_up(e_ctrl, &down, bShowLog);
	if (rc < 0) {
		pr_err("%s: failed to power on eeprom\n", __func__);
		goto FREE;
	}

	if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE) {
		max_size = I2C_BAM_MAX_SIZE;
#if defined(CONFIG_USE_ACTUATOR_FPGA)
	} else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		max_size = FPGA_I2C_MAX_SIZE;
#endif
	} else {
		max_size = SPI_TRANSFER_MAX_SIZE;
	}

	read_size = cdata->cfg.read_data.num_bytes;
	read_addr = cdata->cfg.read_data.addr;
	memptr = 0;
	for (size = cdata->cfg.read_data.num_bytes; size > 0; size -= read_size)
	{
		if (size > max_size) {
			read_size = max_size;
		} else {
			read_size = size;
		}

		rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
			&(e_ctrl->i2c_client), read_addr, &buf[memptr], read_size);
		if (rc < 0) {
			pr_err("%s: failed to read data, rc %d\n", __func__, rc);
			goto POWER_DOWN;
		}

		if (size > max_size) {
			memptr += read_size;
			read_addr += read_size;
		}
	}

	rc = copy_to_user(cdata->cfg.read_data.dbuffer, buf,
			  cdata->cfg.read_data.num_bytes);
POWER_DOWN:
	msm_eeprom_power_down(e_ctrl, down);
FREE:
	vfree(buf);
	return rc;
}
static struct v4l2_subdev_core_ops msm_eeprom_subdev_core_ops = {
	.ioctl = msm_eeprom_subdev_ioctl,
};

static struct v4l2_subdev_ops msm_eeprom_subdev_ops = {
	.core = &msm_eeprom_subdev_core_ops,
};

#define msm_eeprom_spi_parse_cmd(spic, str, name, out, size)		\
	{								\
		rc = of_property_read_u32_array(			\
			spic->spi_master->dev.of_node,			\
			str, out, size);				\
		if (rc < 0)						\
			return rc;					\
		spic->cmd_tbl.name.opcode = out[0];			\
		spic->cmd_tbl.name.addr_len = out[1];			\
		spic->cmd_tbl.name.dummy_len = out[2];			\
		spic->cmd_tbl.name.delay_intv = out[3];			\
		spic->cmd_tbl.name.delay_count = out[4];		\
	}

static int msm_eeprom_spi_parse_of(struct msm_camera_spi_client *spic)
{
	int rc = -EFAULT;
	uint32_t tmp[5];
	struct device_node *of = spic->spi_master->dev.of_node;
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-read", read, tmp, 5);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-readseq", read_seq, tmp, 5);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-queryid", query_id, tmp, 5);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-pprog",
				 page_program, tmp, 5);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-wenable",
				 write_enable, tmp, 5);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-readst",
				 read_status, tmp, 5);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop-erase", erase, tmp, 5);

	rc = of_property_read_u32(of, "qcom,spi-busy-mask", tmp);
	if (rc < 0) {
		pr_err("%s: Failed to get busy mask\n", __func__);
		return rc;
	}
	spic->busy_mask = tmp[0];
	rc = of_property_read_u32(of, "qcom,spi-page-size", tmp);
	if (rc < 0) {
		pr_err("%s: Failed to get page size\n", __func__);
		return rc;
	}
	spic->page_size = tmp[0];
	rc = of_property_read_u32(of, "qcom,spi-erase-size", tmp);
	if (rc < 0) {
		pr_err("%s: Failed to get erase size\n", __func__);
		return rc;
	}
	spic->erase_size = tmp[0];

	rc = of_property_read_u32_array(of, "qcom,eeprom-id0", tmp, 2);
	if (rc < 0) {
		pr_err("%s: Failed to get eeprom id 0\n", __func__);
		return rc;
	}
	spic->mfr_id0 = tmp[0];
	spic->device_id0 = tmp[1];

	rc = of_property_read_u32_array(of, "qcom,eeprom-id1", tmp, 2);
	if (rc < 0) {
		pr_err("%s: Failed to get eeprom id 1\n", __func__);
		return rc;
	}
	spic->mfr_id1 = tmp[0];
	spic->device_id1 = tmp[1];

	rc = of_property_read_u32_array(of, "qcom,eeprom-id2", tmp, 2);
	if (rc < 0) {
		pr_err("%s: Failed to get eeprom id 2\n", __func__);
		return rc;
	}
	spic->mfr_id2 = tmp[0];
	spic->device_id2 = tmp[1];

	rc = of_property_read_u32_array(of, "qcom,eeprom-id3", tmp, 2);
	if (rc < 0) {
		pr_err("%s: Failed to get eeprom id 3\n", __func__);
		return rc;
	}
	spic->mfr_id3 = tmp[0];
	spic->device_id3 = tmp[1];

	return 0;
}

static int msm_eeprom_get_dt_data(struct msm_eeprom_ctrl_t *e_ctrl)
{
	int rc = 0, i = 0;
	struct msm_eeprom_board_info *eb_info;
	struct msm_camera_power_ctrl_t *power_info =
		&e_ctrl->eboard_info->power_info;
	struct device_node *of_node = NULL;
	struct msm_camera_gpio_conf *gconf = NULL;
	int8_t gpio_array_size = 0;
	uint16_t *gpio_array = NULL;

	eb_info = e_ctrl->eboard_info;
	if (e_ctrl->eeprom_device_type == MSM_CAMERA_SPI_DEVICE)
		of_node = e_ctrl->i2c_client.
			spi_client->spi_master->dev.of_node;
	else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE)
		of_node = e_ctrl->pdev->dev.of_node;
	else if (e_ctrl->eeprom_device_type == MSM_CAMERA_I2C_DEVICE)
		of_node = e_ctrl->i2c_client.client->dev.of_node;

	if (!of_node) {
		pr_err("%s: %d of_node is NULL\n", __func__ , __LINE__);
		return -ENOMEM;
	}
	rc = msm_camera_get_dt_vreg_data(of_node, &power_info->cam_vreg,
					     &power_info->num_vreg);
	if (rc < 0)
		return rc;

	if (e_ctrl->userspace_probe == 0) {
		rc = msm_camera_get_dt_power_setting_data(of_node,
			power_info->cam_vreg, power_info->num_vreg,
			power_info);
		if (rc < 0)
			goto ERROR1;
	}

	power_info->gpio_conf = kzalloc(sizeof(struct msm_camera_gpio_conf),
					GFP_KERNEL);
	if (!power_info->gpio_conf) {
		rc = -ENOMEM;
		goto ERROR2;
	}
	gconf = power_info->gpio_conf;
	gpio_array_size = of_gpio_count(of_node);
	CDBG("%s gpio count %d\n", __func__, gpio_array_size);

	if (gpio_array_size > 0) {
		pr_err("%s table is creating.\n", __func__);
		gpio_array = kzalloc(sizeof(uint16_t) * gpio_array_size,
			GFP_KERNEL);
		if (!gpio_array) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR3;
		}
		for (i = 0; i < gpio_array_size; i++) {
			gpio_array[i] = of_get_gpio(of_node, i);
			CDBG("%s gpio_array[%d] = %d\n", __func__, i,
				gpio_array[i]);
		}

		rc = msm_camera_get_dt_gpio_req_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR4;
		}

		rc = msm_camera_init_gpio_pin_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR4;
		}
		kfree(gpio_array);
	}

	return rc;
ERROR4:
	kfree(gpio_array);
ERROR3:
	kfree(power_info->gpio_conf);
ERROR2:
	kfree(power_info->cam_vreg);
ERROR1:
	kfree(power_info->power_setting);
	return rc;
}

#ifdef CONFIG_COMPAT
#if 0// TEMP_8998 : Temp eeprom
static int eeprom_config_read_cal_data32(struct msm_eeprom_ctrl_t *e_ctrl,
	void __user *arg)
{
	int rc;
	uint8_t *ptr_dest = NULL;
	struct msm_eeprom_cfg_data32 *cdata32 =
		(struct msm_eeprom_cfg_data32 *) arg;
	struct msm_eeprom_cfg_data cdata;

	cdata.cfgtype = cdata32->cfgtype;
	cdata.is_supported = cdata32->is_supported;
	cdata.cfg.read_data.num_bytes = cdata32->cfg.read_data.num_bytes;
	/* check range */
	if (cdata.cfg.read_data.num_bytes >
	    e_ctrl->cal_data.num_data) {
		CDBG("%s: Invalid size. exp %u, req %u\n", __func__,
			e_ctrl->cal_data.num_data,
			cdata.cfg.read_data.num_bytes);
		return -EINVAL;
	}
	if (!e_ctrl->cal_data.mapdata)
		return -EFAULT;

	ptr_dest = (uint8_t *) compat_ptr(cdata32->cfg.read_data.dbuffer);

	rc = copy_to_user(ptr_dest, e_ctrl->cal_data.mapdata,
		cdata.cfg.read_data.num_bytes);

	return rc;
}
#endif
static int msm_eeprom_config32(struct msm_eeprom_ctrl_t *e_ctrl,
	void __user *argp)
{
	struct msm_eeprom_cfg_data32 *cdata32 = (struct msm_eeprom_cfg_data32 *)argp;
	struct msm_eeprom_cfg_data cdata;
	int rc = 0;
	size_t length = 0;

	CDBG("%s:%d E: subdevid: %d\n",__func__,__LINE__,e_ctrl->subdev_id);
	cdata.cfgtype = cdata32->cfgtype;
	CDBG("%s:%d cfgtype = %d\n", __func__, __LINE__, cdata.cfgtype);
	mutex_lock(e_ctrl->eeprom_mutex);
	switch (cdata.cfgtype) {
	case CFG_EEPROM_GET_INFO:
		CDBG("%s E CFG_EEPROM_GET_INFO: %d, %s\n",
			__func__, e_ctrl->is_supported, e_ctrl->eboard_info->eeprom_name);
		cdata32->is_supported = e_ctrl->is_supported;
		length = strlen(e_ctrl->eboard_info->eeprom_name) + 1;
		if (length > MAX_EEPROM_NAME) {
			pr_err("%s:%d invalid eeprom_name length %d\n",
				__func__, __LINE__, (int)length);
			rc = -EINVAL;
			break;
		}
		memcpy(cdata32->cfg.eeprom_name,
			e_ctrl->eboard_info->eeprom_name, length);
		break;
	case CFG_EEPROM_GET_CAL_DATA:
		CDBG("%s E CFG_EEPROM_GET_CAL_DATA: %d\n", __func__, e_ctrl->cal_data.num_data);
		cdata32->cfg.get_data.num_bytes =
			e_ctrl->cal_data.num_data;
		break;
	case CFG_EEPROM_READ_CAL_DATA:
		CDBG("%s E CFG_EEPROM_READ_CAL_DATA\n", __func__);
		cdata.cfg.read_data.num_bytes = cdata32->cfg.read_data.num_bytes;
		cdata.cfg.read_data.dbuffer = compat_ptr(cdata32->cfg.read_data.dbuffer);
		rc = eeprom_config_read_cal_data(e_ctrl, &cdata);
		break;
	case CFG_EEPROM_READ_DATA:
		CDBG("%s E CFG_EEPROM_READ_DATA\n", __func__);
		cdata.cfg.read_data.num_bytes = cdata32->cfg.read_data.num_bytes;
		cdata.cfg.read_data.addr = cdata32->cfg.read_data.addr;
		cdata.cfg.read_data.dbuffer = compat_ptr(cdata32->cfg.read_data.dbuffer);
		rc = eeprom_config_read_data(e_ctrl, &cdata);
		break;
	case CFG_EEPROM_DECOMPRESS_DATA:
		CDBG("%s E CFG_EEPROM_DECOMPRESS_DATA\n", __func__);
		cdata.cfg.comp_read_data.num_bytes = cdata32->cfg.comp_read_data.num_bytes;
		cdata.cfg.comp_read_data.addr = cdata32->cfg.comp_read_data.addr;
		cdata.cfg.comp_read_data.comp_size = cdata32->cfg.comp_read_data.comp_size;
		cdata.cfg.comp_read_data.cbuffer = compat_ptr(cdata32->cfg.comp_read_data.cbuffer);
		cdata.cfg.comp_read_data.dbuffer = compat_ptr(cdata32->cfg.comp_read_data.dbuffer);
		rc = eeprom_config_decompress(e_ctrl, &cdata);
		if (rc < 0)
			pr_err("%s : eeprom_config_decompress failed \n", __func__);
		break;
	case CFG_EEPROM_READ_COMPRESSED_DATA:
		CDBG("%s E CFG_EEPROM_READ_COMPRESSED_DATA\n", __func__);
		cdata.cfg.read_data.num_bytes = cdata32->cfg.read_data.num_bytes;
		cdata.cfg.read_data.addr = cdata32->cfg.read_data.addr;
		cdata.cfg.read_data.comp_size = cdata32->cfg.read_data.comp_size;
		cdata.cfg.read_data.dbuffer = compat_ptr(cdata32->cfg.read_data.dbuffer);
		rc = eeprom_config_read_compressed_data(e_ctrl, &cdata);
		if (rc < 0)
			pr_err("%s : eeprom_config_read_compressed_data failed \n", __func__);
		break;
	case CFG_EEPROM_WRITE_DATA:
		pr_warn("%s E CFG_EEPROM_WRITE_DATA\n", __func__);
		cdata.cfg.write_data.num_bytes = cdata32->cfg.write_data.num_bytes;
		cdata.cfg.write_data.addr = cdata32->cfg.write_data.addr;
		cdata.cfg.write_data.compress = cdata32->cfg.write_data.compress;
		cdata.cfg.write_data.write_size = compat_ptr(cdata32->cfg.write_data.write_size);
		cdata.cfg.write_data.dbuffer = compat_ptr(cdata32->cfg.write_data.dbuffer);
		rc = eeprom_config_write_data(e_ctrl, &cdata);
		break;
	case CFG_EEPROM_COMPRESS_DATA:
		pr_warn("%s E CFG_EEPROM_COMPRESS_DATA\n", __func__);
		cdata.cfg.comp_write_data.num_bytes = cdata32->cfg.comp_write_data.num_bytes;
		cdata.cfg.comp_write_data.addr = cdata32->cfg.comp_write_data.addr;
		cdata.cfg.comp_write_data.write_size = compat_ptr(cdata32->cfg.comp_write_data.write_size);
		cdata.cfg.comp_write_data.dbuffer = compat_ptr(cdata32->cfg.comp_write_data.dbuffer);
		cdata.cfg.comp_write_data.cbuffer = compat_ptr(cdata32->cfg.comp_write_data.cbuffer);
		rc = eeprom_config_compress_data(e_ctrl, &cdata);
		break;
	case CFG_EEPROM_READ_DATA_FROM_HW:
		e_ctrl->is_supported = 0x01;
		pr_err ("kernel is_supported before : 0x%04X\n", e_ctrl->is_supported);
		rc = msm_eeprom_read_eeprom_data(e_ctrl);
		pr_err ("kernel is_supported after : 0x%04X\n", e_ctrl->is_supported);

		cdata32->is_supported = e_ctrl->is_supported;
		cdata.cfg.read_data.num_bytes = cdata32->cfg.read_data.num_bytes;
		cdata.cfg.read_data.dbuffer = compat_ptr(cdata32->cfg.read_data.dbuffer);
		if (rc < 0) {
			pr_err("%s:%d failed rc %d\n", __func__, __LINE__,  rc);
			break;
		}
		rc = copy_to_user(cdata.cfg.read_data.dbuffer,
			e_ctrl->cal_data.mapdata,
			cdata.cfg.read_data.num_bytes);
		break;
	case CFG_EEPROM_GET_ERASESIZE:
		CDBG("%s E CFG_EEPROM_GET_ERASESIZE: %d\n",
			__func__, e_ctrl->i2c_client.spi_client->erase_size);
		cdata32->cfg.get_data.num_bytes =
			e_ctrl->i2c_client.spi_client->erase_size;
		break;
	case CFG_EEPROM_ERASE:
		pr_warn("%s E CFG_EEPROM_ERASE\n", __func__);
		cdata.cfg.erase_data.addr = cdata32->cfg.erase_data.addr;
		cdata.cfg.erase_data.num_bytes = cdata32->cfg.erase_data.num_bytes;
		rc = eeprom_config_erase(e_ctrl, &cdata);
		break;
	case CFG_EEPROM_POWER_ON:
		rc = msm_eeprom_power_up(e_ctrl, NULL, TRUE);
		if (rc < 0)
			pr_err("%s : msm_eeprom_power_up failed \n", __func__);
		break;
	case CFG_EEPROM_POWER_OFF:
		rc = msm_eeprom_power_down(e_ctrl, true);
		if (rc < 0)
			pr_err("%s : msm_eeprom_power_down failed \n", __func__);
		break;
	case CFG_EEPROM_GET_MM_INFO:
		CDBG("%s E CFG_EEPROM_GET_MM_INFO\n", __func__);
		rc = msm_eeprom_get_cmm_data(e_ctrl, &cdata);
		break;
	default:
		break;
	}
	mutex_unlock(e_ctrl->eeprom_mutex);
	CDBG("%s X rc: %d\n", __func__, rc);
	return rc;
}

static long msm_eeprom_subdev_ioctl32(struct v4l2_subdev *sd,
		unsigned int cmd, void *arg)
{
	struct msm_eeprom_ctrl_t *e_ctrl = v4l2_get_subdevdata(sd);
	void __user *argp = (void __user *)arg;

	CDBG("%s E\n", __func__);
	CDBG("%s:%d a_ctrl %pK argp %pK\n", __func__, __LINE__, e_ctrl, argp);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_eeprom_get_subdev_id(e_ctrl, argp);
	case VIDIOC_MSM_EEPROM_CFG32:
		return msm_eeprom_config32(e_ctrl, argp);
	default:
		return -ENOIOCTLCMD;
	}

	CDBG("%s X\n", __func__);
}

static long msm_eeprom_subdev_do_ioctl32(
	struct file *file, unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);

	return msm_eeprom_subdev_ioctl32(sd, cmd, arg);
}

static long msm_eeprom_subdev_fops_ioctl32(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_eeprom_subdev_do_ioctl32);
}

#endif

#if 0 // remove eebin. This is not used.
static int msm_eeprom_cmm_dts(struct msm_eeprom_board_info *eb_info,
				struct device_node *of_node)
{
	int rc = 0;
	struct msm_eeprom_cmm_t *cmm_data = &eb_info->cmm_data;

	cmm_data->cmm_support =
		of_property_read_bool(of_node, "qcom,cmm-data-support");
	if (!cmm_data->cmm_support)
		return -EINVAL;
	cmm_data->cmm_compression =
		of_property_read_bool(of_node, "qcom,cmm-data-compressed");
	if (!cmm_data->cmm_compression)
		CDBG("No MM compression data\n");

	rc = of_property_read_u32(of_node, "qcom,cmm-data-offset",
				  &cmm_data->cmm_offset);
	if (rc < 0)
		CDBG("No MM offset data\n");

	rc = of_property_read_u32(of_node, "qcom,cmm-data-size",
				  &cmm_data->cmm_size);
	if (rc < 0)
		CDBG("No MM size data\n");

	CDBG("cmm_support: cmm_compr %d, cmm_offset %d, cmm_size %d\n",
		cmm_data->cmm_compression,
		cmm_data->cmm_offset,
		cmm_data->cmm_size);
	return 0;
}
#endif

#define	MAX_RETRY		(3)
static int msm_eeprom_spi_setup(struct spi_device *spi)
{
	struct msm_eeprom_ctrl_t *e_ctrl = NULL;
	struct msm_camera_i2c_client *client = NULL;
	struct msm_camera_spi_client *spi_client;
	struct msm_eeprom_board_info *eb_info;
	struct msm_camera_power_ctrl_t *power_info = NULL;
	int rc = 0;
	int i, j, b_ok = 0;
	int normal_crc_value = 0;

	e_ctrl = kzalloc(sizeof(*e_ctrl), GFP_KERNEL);
	if (!e_ctrl) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	e_ctrl->eeprom_v4l2_subdev_ops = &msm_eeprom_subdev_ops;
	e_ctrl->eeprom_mutex = &msm_eeprom_mutex;
	client = &e_ctrl->i2c_client;
	e_ctrl->is_supported = 0;

	spi_client = kzalloc(sizeof(*spi_client), GFP_KERNEL);
	if (!spi_client) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		kfree(e_ctrl);
		return -ENOMEM;
	}

	rc = of_property_read_u32(spi->dev.of_node, "cell-index",
		&e_ctrl->subdev_id);
	CDBG("cell-index %d, rc %d\n", e_ctrl->subdev_id, rc);
	if (rc < 0) {
		pr_err("failed rc %d, cell-index %d\n", rc, e_ctrl->subdev_id);
		goto spi_free;
	}

	e_ctrl->eeprom_device_type = MSM_CAMERA_SPI_DEVICE;
	client->spi_client = spi_client;
	spi_client->spi_master = spi;
	client->i2c_func_tbl = &msm_eeprom_spi_func_tbl;
	client->addr_type = MSM_CAMERA_I2C_3B_ADDR;

	eb_info = kzalloc(sizeof(*eb_info), GFP_KERNEL);
	if (!eb_info) {
		pr_err("%s : eb_info is NULL \n", __func__);
		goto spi_free;
	}
	e_ctrl->eboard_info = eb_info;
	rc = of_property_read_string(spi->dev.of_node, "qcom,eeprom-name",
		&eb_info->eeprom_name);
	CDBG("%s qcom,eeprom-name %s, rc %d\n", __func__,
		eb_info->eeprom_name, rc);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto board_free;
	}
#if 0	//	remove eebin
	rc = msm_eeprom_cmm_dts(e_ctrl->eboard_info, spi->dev.of_node);
	if (rc < 0)
		CDBG("%s MM data miss:%d\n", __func__, __LINE__);
#endif
	power_info = &eb_info->power_info;

	power_info->clk_info = cam_8974_clk_info;
	power_info->clk_info_size = ARRAY_SIZE(cam_8974_clk_info);
	power_info->dev = &spi->dev;

	rc = msm_eeprom_get_dt_data(e_ctrl);
	if (rc < 0) {
		pr_err("%s : msm_eeprom_get_dt_data \n", __func__);
		goto board_free;
	}

	/* set spi instruction info */
	spi_client->retry_delay = 1;
	spi_client->retries = 0;

	rc = msm_eeprom_spi_parse_of(spi_client);
	if (rc < 0) {
		dev_err(&spi->dev,
			"%s: Error parsing device properties\n", __func__);
		goto board_free;
	}

	/* prepare memory buffer */
	rc = msm_eeprom_parse_memory_map(spi->dev.of_node,
		&e_ctrl->cal_data);
	if (rc < 0)
		pr_err("%s: no cal memory map\n", __func__);

	/* power up eeprom for reading */
	for(b_ok = 0, i = 0; i < MAX_RETRY; i ++)
	{
		rc = msm_camera_power_up(power_info, e_ctrl->eeprom_device_type,
			&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
		if (rc < 0) {
			pr_err("failed rc %d\n", rc);
			continue;
		} else {
			b_ok = 1;
			break;
		}
	}
	if(b_ok == 0)
		goto caldata_free;

	/* check eeprom id */
	for(b_ok = 0, i = 0; i < MAX_RETRY; i ++)
	{
		rc = msm_eeprom_match_id(e_ctrl, TRUE);
		if (rc < 0) {
			pr_err("%s: eeprom not matching %d\n", __func__, rc);
			continue;
			//goto power_down;
		} else {
			b_ok = 1;
			break;
		}
	}
	if(b_ok == 0)
		goto power_down;

	normal_crc_value = 0;
	for(i = 0; i < e_ctrl->cal_data.num_map>>1; i ++)
		normal_crc_value |= (1 << i);
	CDBG("num_map = %d, Normal CRC value = 0x%X\n", e_ctrl->cal_data.num_map, normal_crc_value);

	/* read eeprom */
	if (e_ctrl->cal_data.map)
	{
		for(i = 0; i < MAX_RETRY; i ++)
		{
			for(b_ok = 0, j = 0; j < MAX_RETRY; j ++)
			{
				rc = read_eeprom_memory(e_ctrl, &e_ctrl->cal_data);
				if (rc < 0) {
					pr_err("%s: read cal data failed\n", __func__);
					continue;
				} else {
					CDBG("%s: read cal data ok!!\n", __func__);
					b_ok = 1;
					break;
				}
			}
			if(b_ok == 0)
				goto power_down;

			e_ctrl->is_supported |= msm_eeprom_match_crc(&e_ctrl->cal_data, e_ctrl->subdev_id);

			if(e_ctrl->is_supported != normal_crc_value) {
				pr_err("%s : any CRC values at F-ROM are not matched. Read again(idx = %d, max = %d).\n", __func__, i+1, MAX_RETRY);
				continue;
			} else {
				pr_err("%s : All CRC values are matched.\n", __func__);
				break;
			}
		}

		if(b_ok == 1) {
			/* read rear sensor id */
			memcpy(rear_sensor_id, &e_ctrl->cal_data.mapdata[FROM_REAR_SENSOR_ID_ADDR], FROM_SENSOR_ID_SIZE);
			rear_sensor_id[FROM_SENSOR_ID_SIZE] = '\0';
			CDBG("%s : %d rear id = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
				__func__, e_ctrl->subdev_id, rear_sensor_id[0], rear_sensor_id[1], rear_sensor_id[2], rear_sensor_id[3],
				rear_sensor_id[4], rear_sensor_id[5], rear_sensor_id[6], rear_sensor_id[7], rear_sensor_id[8], rear_sensor_id[9],
				rear_sensor_id[10], rear_sensor_id[11], rear_sensor_id[12], rear_sensor_id[13], rear_sensor_id[14], rear_sensor_id[15]);

#if defined(CONFIG_SAMSUNG_MULTI_CAMERA)
			/* read rear2 sensor id */
			memcpy(rear2_sensor_id, &e_ctrl->cal_data.mapdata[FROM_REAR2_SENSOR_ID_ADDR], FROM_SENSOR_ID_SIZE);
			rear2_sensor_id[FROM_SENSOR_ID_SIZE] = '\0';
			CDBG("%s : %d rear2 id = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", __func__, e_ctrl->subdev_id, rear2_sensor_id[0], rear2_sensor_id[1], rear2_sensor_id[2], rear2_sensor_id[3], rear2_sensor_id[4], rear2_sensor_id[5], rear2_sensor_id[6], rear2_sensor_id[7], rear2_sensor_id[8], rear2_sensor_id[9], rear2_sensor_id[10], rear2_sensor_id[11], rear2_sensor_id[12], rear2_sensor_id[13], rear2_sensor_id[14], rear2_sensor_id[15]);
#endif

			/* read module id */
			memcpy(rear_module_id, &e_ctrl->cal_data.mapdata[FROM_MODULE_ID_ADDR], FROM_MODULE_ID_SIZE);
			rear_module_id[FROM_MODULE_ID_SIZE] = '\0';
			CDBG("%s : %d rear_module_id = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", __func__, e_ctrl->subdev_id,
			  rear_module_id[0], rear_module_id[1], rear_module_id[2], rear_module_id[3], rear_module_id[4],
			  rear_module_id[5], rear_module_id[6], rear_module_id[7], rear_module_id[8], rear_module_id[9]);

			/* rear mtf exif */
			memcpy(rear_mtf_exif, &e_ctrl->cal_data.mapdata[FROM_REAR_MTF_ADDR], FROM_MTF_SIZE);
			rear_mtf_exif[FROM_MTF_SIZE] = '\0';
			CDBG("%s : %d rear mtf exif = %s", __func__, e_ctrl->subdev_id, rear_mtf_exif);

			/*rear af cal*/
#if  defined(FROM_REAR_AF_CAL_MACRO_ADDR)
			memcpy(&rear_af_cal[0], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_MACRO_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_PAN_ADDR)
			memcpy(&rear_af_cal[9], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_PAN_ADDR], 4);
#endif

#if defined(CONFIG_SAMSUNG_MULTI_CAMERA)
			/* rear2 mtf exif */
			memcpy(rear2_mtf_exif, &e_ctrl->cal_data.mapdata[FROM_REAR2_MTF_ADDR], FROM_MTF_SIZE);
			rear2_mtf_exif[FROM_MTF_SIZE] = '\0';
			CDBG("%s : %d rear2 mtf exif = %s", __func__, e_ctrl->subdev_id, rear2_mtf_exif);

			/* rear dual cal */
			memcpy(rear_dual_cal, &e_ctrl->cal_data.mapdata[FROM_REAR_DUAL_CAL_ADDR], FROM_REAR_DUAL_CAL_SIZE);
			rear_dual_cal[FROM_REAR_DUAL_CAL_SIZE] = '\0';
			CDBG("%s : %d rear dual cal = %s", __func__, e_ctrl->subdev_id, rear_dual_cal);

			/* rear2 tilt */
			memcpy(&rear2_dual_tilt_x, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_X], 4);
			memcpy(&rear2_dual_tilt_y, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_Y], 4);
			memcpy(&rear2_dual_tilt_z, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_Z], 4);
			memcpy(&rear2_dual_tilt_sx, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_SX], 4);
			memcpy(&rear2_dual_tilt_sy, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_SY], 4);
			memcpy(&rear2_dual_tilt_range, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_RANGE], 4);
			memcpy(&rear2_dual_tilt_max_err, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_MAX_ERR], 4);
			memcpy(&rear2_dual_tilt_avg_err, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_AVG_ERR], 4);
			memcpy(&rear2_dual_tilt_dll_ver, &e_ctrl->cal_data.mapdata[FROM_REAR2_DUAL_TILT_DLL_VERSION], 4);
			CDBG("%s : %d rear dual tilt x = %d, y = %d, z = %d, sx = %d, sy = %d, range = %d, max_err = %d, avg_err = %d, dll_ver = %d\n",
				__func__, e_ctrl->subdev_id, rear2_dual_tilt_x, rear2_dual_tilt_y, rear2_dual_tilt_z, rear2_dual_tilt_sx, rear2_dual_tilt_sy,
				rear2_dual_tilt_range, rear2_dual_tilt_max_err, rear2_dual_tilt_avg_err, rear2_dual_tilt_dll_ver);

			/*rear af cal*/
#if  defined(FROM_REAR_AF_CAL_MACRO_ADDR)
			memcpy(&rear_af_cal[0], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_MACRO_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D10_ADDR)
			memcpy(&rear_af_cal[1], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D10_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D20_ADDR)
			memcpy(&rear_af_cal[2], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D20_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D30_ADDR)
			memcpy(&rear_af_cal[3], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D30_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D40_ADDR)
			memcpy(&rear_af_cal[4], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D40_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D50_ADDR)
			memcpy(&rear_af_cal[5], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D50_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D60_ADDR)
			memcpy(&rear_af_cal[6], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D60_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D70_ADDR)
			memcpy(&rear_af_cal[7], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D70_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_D80_ADDR)
			memcpy(&rear_af_cal[8], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_D80_ADDR], 4);
#endif
#if  defined(FROM_REAR_AF_CAL_PAN_ADDR)
			memcpy(&rear_af_cal[9], &e_ctrl->cal_data.mapdata[FROM_REAR_AF_CAL_PAN_ADDR], 4);
#endif

			/*rear2 af cal*/
#if  defined(FROM_REAR2_AF_CAL_MACRO_ADDR)
			memcpy(&rear2_af_cal[0], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_MACRO_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D10_ADDR)
			memcpy(&rear2_af_cal[1], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D10_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D20_ADDR)
			memcpy(&rear2_af_cal[2], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D20_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D30_ADDR)
			memcpy(&rear2_af_cal[3], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D30_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D40_ADDR)
			memcpy(&rear2_af_cal[4], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D40_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D50_ADDR)
			memcpy(&rear2_af_cal[5], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D50_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D60_ADDR)
			memcpy(&rear2_af_cal[6], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D60_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D70_ADDR)
			memcpy(&rear2_af_cal[7], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D70_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_D80_ADDR)
			memcpy(&rear2_af_cal[8], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_D80_ADDR], 4);
#endif
#if  defined(FROM_REAR2_AF_CAL_PAN_ADDR)
			memcpy(&rear2_af_cal[9], &e_ctrl->cal_data.mapdata[FROM_REAR2_AF_CAL_PAN_ADDR], 4);
#endif
#endif
		}
	}

	for(b_ok = 0, i = 0; i < MAX_RETRY; i ++)
	{
		rc = msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
			&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
		if (rc < 0) {
			pr_err("failed rc %d\n", rc);
			continue;
		} else {
			b_ok = 1;
			break;
		}
	}
	if(b_ok == 0)
		goto caldata_free;

	/* initiazlie subdev */
	v4l2_spi_subdev_init(&e_ctrl->msm_sd.sd,
		e_ctrl->i2c_client.spi_client->spi_master,
		e_ctrl->eeprom_v4l2_subdev_ops);
	v4l2_set_subdevdata(&e_ctrl->msm_sd.sd, e_ctrl);
	e_ctrl->msm_sd.sd.internal_ops = &msm_eeprom_internal_ops;
	e_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&e_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	e_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	e_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_EEPROM;
	msm_sd_register(&e_ctrl->msm_sd);

#ifdef CONFIG_COMPAT
	msm_eeprom_v4l2_subdev_fops = v4l2_subdev_fops;
	msm_eeprom_v4l2_subdev_fops.compat_ioctl32 =
		msm_eeprom_subdev_fops_ioctl32;
	e_ctrl->msm_sd.sd.devnode->fops = &msm_eeprom_v4l2_subdev_fops;
#endif

	e_ctrl->is_supported = (e_ctrl->is_supported << 1) | 1;
	pr_err("%s success result=%d supported=%x X\n", __func__, rc,
		e_ctrl->is_supported);

	return 0;

power_down:
	msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
caldata_free:
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
board_free:
	kfree(e_ctrl->eboard_info);
spi_free:
	kfree(spi_client);
	kfree(e_ctrl);
	return rc;
}

static int msm_eeprom_spi_probe(struct spi_device *spi)
{
	int irq, cs, cpha, cpol, cs_high;

	CDBG("%s\n", __func__);
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	spi_setup(spi);

	irq = spi->irq;
	cs = spi->chip_select;
	cpha = (spi->mode & SPI_CPHA) ? 1 : 0;
	cpol = (spi->mode & SPI_CPOL) ? 1 : 0;
	cs_high = (spi->mode & SPI_CS_HIGH) ? 1 : 0;
	pr_err("%s: irq[%d] cs[%x] CPHA[%x] CPOL[%x] CS_HIGH[%x]\n",
			__func__, irq, cs, cpha, cpol, cs_high);
	pr_err("%s: max_speed[%u]\n", __func__, spi->max_speed_hz);

	return msm_eeprom_spi_setup(spi);
}

static int msm_eeprom_spi_remove(struct spi_device *sdev)
{
	struct v4l2_subdev *sd = spi_get_drvdata(sdev);
	struct msm_eeprom_ctrl_t  *e_ctrl;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	e_ctrl = (struct msm_eeprom_ctrl_t *)v4l2_get_subdevdata(sd);
	if (!e_ctrl) {
		pr_err("%s: eeprom device is NULL\n", __func__);
		return 0;
	}

	kfree(e_ctrl->i2c_client.spi_client);
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
	if (e_ctrl->eboard_info) {
		kfree(e_ctrl->eboard_info->power_info.gpio_conf);
		kfree(e_ctrl->eboard_info);
	}
	kfree(e_ctrl);
	return 0;
}

static int msm_eeprom_platform_probe(struct platform_device *pdev)
{
	int rc = 0;
	uint32_t temp;

	struct msm_camera_cci_client *cci_client = NULL;
	struct msm_eeprom_ctrl_t *e_ctrl = NULL;
	struct msm_eeprom_board_info *eb_info = NULL;
	struct device_node *of_node = pdev->dev.of_node;
	struct msm_camera_power_ctrl_t *power_info = NULL;

	CDBG("%s E\n", __func__);

	e_ctrl = kzalloc(sizeof(*e_ctrl), GFP_KERNEL);
	if (!e_ctrl) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	e_ctrl->eeprom_v4l2_subdev_ops = &msm_eeprom_subdev_ops;
	e_ctrl->eeprom_mutex = &msm_eeprom_mutex;

	e_ctrl->is_supported = 0;
	if (!of_node) {
		pr_err("%s dev.of_node NULL\n", __func__);
		kfree(e_ctrl);
		return -EINVAL;
	}

	rc = of_property_read_u32(of_node, "cell-index",
		&pdev->id);
	CDBG("cell-index %d, rc %d\n", pdev->id, rc);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		kfree(e_ctrl);
		return rc;
	}
	e_ctrl->subdev_id = pdev->id;

#if !defined(CONFIG_USE_ACTUATOR_FPGA)
	rc = of_property_read_u32(of_node, "qcom,cci-master",
		&e_ctrl->cci_master);
	CDBG("qcom,cci-master %d, rc %d\n", e_ctrl->cci_master, rc);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		kfree(e_ctrl);
		return rc;
	}
#endif
	rc = of_property_read_u32(of_node, "qcom,slave-addr",
		&temp);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		kfree(e_ctrl);
		return rc;
	}

#if !defined(CONFIG_USE_ACTUATOR_FPGA)
	rc = of_property_read_u32(of_node, "qcom,i2c-freq-mode",
		&e_ctrl->i2c_freq_mode);
	CDBG("qcom,i2c_freq_mode %d, rc %d\n", e_ctrl->i2c_freq_mode, rc);
	if (rc < 0) {
		pr_err("%s qcom,i2c-freq-mode read fail. Setting to 0 %d\n",
			__func__, rc);
		e_ctrl->i2c_freq_mode = 0;
	}

	if (e_ctrl->i2c_freq_mode >= I2C_MAX_MODES) {
		pr_err("%s:%d invalid i2c_freq_mode = %d\n", __func__, __LINE__,
			e_ctrl->i2c_freq_mode);
		kfree(e_ctrl);
		return -EINVAL;
	}
#else
	e_ctrl->i2c_freq_mode = 0;
#endif

	/* Set platform device handle */
	e_ctrl->pdev = pdev;
	/* Set device type as platform device */
	e_ctrl->eeprom_device_type = MSM_CAMERA_PLATFORM_DEVICE;

#if defined(CONFIG_USE_ACTUATOR_FPGA)
	e_ctrl->i2c_client.dev_id = 0x11; //front eeprom
	e_ctrl->i2c_client.i2c_func_tbl = &msm_eeprom_fpga_iic_func_tbl;
	e_ctrl->i2c_client.addr_type = MSM_CAMERA_I2C_WORD_ADDR;
#else
	e_ctrl->i2c_client.i2c_func_tbl = &msm_eeprom_cci_func_tbl;
#endif

	e_ctrl->i2c_client.cci_client = kzalloc(sizeof(
		struct msm_camera_cci_client), GFP_KERNEL);
	if (!e_ctrl->i2c_client.cci_client) {
		pr_err("%s failed no memory\n", __func__);
		kfree(e_ctrl);
		return -ENOMEM;
	}

	e_ctrl->eboard_info = kzalloc(sizeof(
		struct msm_eeprom_board_info), GFP_KERNEL);
	if (!e_ctrl->eboard_info) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto cciclient_free;
	}
	eb_info = e_ctrl->eboard_info;
	power_info = &eb_info->power_info;
	eb_info->i2c_slaveaddr = temp;

	power_info->clk_info = cam_8974_clk_info;
	power_info->clk_info_size = ARRAY_SIZE(cam_8974_clk_info);
	power_info->dev = &pdev->dev;


#if !defined(CONFIG_USE_ACTUATOR_FPGA)
	rc = of_property_read_u32(of_node, "qcom,i2c-freq-mode",
		&eb_info->i2c_freq_mode);
	if (rc < 0 || (eb_info->i2c_freq_mode >= I2C_MAX_MODES)) {
		eb_info->i2c_freq_mode = I2C_STANDARD_MODE;
		CDBG("%s Default I2C standard speed mode.\n", __func__);
	}
#endif

	CDBG("qcom,slave-addr = 0x%X\n", eb_info->i2c_slaveaddr);
	cci_client = e_ctrl->i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = e_ctrl->cci_master;
	cci_client->i2c_freq_mode = e_ctrl->i2c_freq_mode;
	cci_client->sid = eb_info->i2c_slaveaddr >> 1;
	cci_client->retries = 3;
	cci_client->id_map = 0;
	cci_client->i2c_freq_mode = eb_info->i2c_freq_mode;

	rc = of_property_read_string(of_node, "qcom,eeprom-name",
		&eb_info->eeprom_name);
	CDBG("%s qcom,eeprom-name %s, rc %d\n", __func__,
		eb_info->eeprom_name, rc);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto board_free;
	}

#if 0	// remove eebin. This is not used.
	rc = msm_eeprom_cmm_dts(e_ctrl->eboard_info, of_node);
	if (rc < 0)
		CDBG("%s MM data miss:%d\n", __func__, __LINE__);
#endif

	rc = msm_eeprom_get_dt_data(e_ctrl);
	if (rc)
		goto board_free;

	rc = msm_eeprom_parse_memory_map(of_node, &e_ctrl->cal_data);
	if (rc < 0)
		goto board_free;

	rc = msm_camera_power_up(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
	if (rc) {
		pr_err("failed rc %d\n", rc);
		goto memdata_free;
	}

#if 1 // To reduce booting time.
	pr_info("%s: Skip read eeprom-%d in probe func\n", __func__, e_ctrl->subdev_id);
#else
	rc = read_eeprom_memory(e_ctrl, &e_ctrl->cal_data);
	if (rc < 0) {
		pr_err("%s read_eeprom_memory failed\n", __func__);
		goto power_down;
	}
	for (j = 0; j < e_ctrl->cal_data.num_data; j++)
		CDBG("memory_data[%d] = 0x%X\n", j,
			e_ctrl->cal_data.mapdata[j]);

	e_ctrl->is_supported |= msm_eeprom_match_crc(&e_ctrl->cal_data, e_ctrl->subdev_id);
#endif

	rc = msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
	if (rc) {
		pr_err("failed rc %d\n", rc);
		goto memdata_free;
	}

	if (0 > of_property_read_u32(of_node, "qcom,sensor-position",
				&temp)) {
		pr_err("%s:%d Fail position, Default sensor position\n", __func__, __LINE__);
		temp = 0;
	}
	pr_err("%s qcom,sensor-position %d\n", __func__,temp);

	v4l2_subdev_init(&e_ctrl->msm_sd.sd,
		e_ctrl->eeprom_v4l2_subdev_ops);
	v4l2_set_subdevdata(&e_ctrl->msm_sd.sd, e_ctrl);
	platform_set_drvdata(pdev, &e_ctrl->msm_sd.sd);
	e_ctrl->msm_sd.sd.internal_ops = &msm_eeprom_internal_ops;
	e_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	e_ctrl->msm_sd.sd.entity.flags = temp;
	snprintf(e_ctrl->msm_sd.sd.name,
		ARRAY_SIZE(e_ctrl->msm_sd.sd.name), "msm_eeprom");
	media_entity_init(&e_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	e_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	e_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_EEPROM;
	msm_sd_register(&e_ctrl->msm_sd);

#ifdef CONFIG_COMPAT
	msm_eeprom_v4l2_subdev_fops = v4l2_subdev_fops;
	msm_eeprom_v4l2_subdev_fops.compat_ioctl32 =
		msm_eeprom_subdev_fops_ioctl32;
	e_ctrl->msm_sd.sd.devnode->fops = &msm_eeprom_v4l2_subdev_fops;
#endif

	e_ctrl->is_supported = (e_ctrl->is_supported << 1) | 1;
	CDBG("%s X\n", __func__);
	return rc;

#if 0 // This is not used.
power_down:
	msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
#endif

memdata_free:
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
board_free:
	kfree(e_ctrl->eboard_info);
cciclient_free:
	kfree(e_ctrl->i2c_client.cci_client);
	kfree(e_ctrl);
	return rc;
}

static int msm_eeprom_platform_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct msm_eeprom_ctrl_t  *e_ctrl;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	e_ctrl = (struct msm_eeprom_ctrl_t *)v4l2_get_subdevdata(sd);
	if (!e_ctrl) {
		pr_err("%s: eeprom device is NULL\n", __func__);
		return 0;
	}

	kfree(e_ctrl->i2c_client.cci_client);
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
	if (e_ctrl->eboard_info) {
		kfree(e_ctrl->eboard_info->power_info.gpio_conf);
		kfree(e_ctrl->eboard_info);
	}
	kfree(e_ctrl);
	return 0;
}

static int msm_eeprom_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int rc = 0;
	uint32_t temp = 0;
	struct msm_eeprom_ctrl_t *e_ctrl = NULL;
	struct msm_camera_power_ctrl_t *power_info = NULL;
	struct device_node *of_node = client->dev.of_node;

	CDBG("%s E\n", __func__);

	if (!of_node) {
		pr_err("%s of_node NULL\n", __func__);
		return -EINVAL;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s i2c_check_functionality failed\n", __func__);
		goto probe_failure;
	}

	e_ctrl = kzalloc(sizeof(*e_ctrl), GFP_KERNEL);
	if (!e_ctrl) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	e_ctrl->eeprom_v4l2_subdev_ops = &msm_eeprom_subdev_ops;
	e_ctrl->eeprom_mutex = &msm_eeprom_mutex;
	CDBG("%s client = 0x%pK\n", __func__, client);

	//e_ctrl->eboard_info = (struct msm_eeprom_board_info *)(id->driver_data);
	e_ctrl->eboard_info = kzalloc(sizeof(
		struct msm_eeprom_board_info), GFP_KERNEL);
	if (!e_ctrl->eboard_info) {
		pr_err("%s:%d board info NULL\n", __func__, __LINE__);
		rc = -EINVAL;
		goto memdata_free;
	}

	rc = of_property_read_u32(of_node, "qcom,slave-addr", &temp);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		goto board_free;
	}

	rc = of_property_read_u32(of_node, "cell-index",
			&e_ctrl->subdev_id);
	CDBG("cell-index/subdev_id %d, rc %d\n", e_ctrl->subdev_id, rc);
	if (rc < 0) {
		pr_err("failed read, rc %d\n", rc);
		goto board_free;
	}

	power_info = &e_ctrl->eboard_info->power_info;
	e_ctrl->eboard_info->i2c_slaveaddr = temp;
	e_ctrl->i2c_client.client = client;
	e_ctrl->is_supported = 0;

	CDBG("%s:%d e_ctrl->eboard_info->i2c_slaveaddr = 0x%x\n", __func__, __LINE__ , e_ctrl->eboard_info->i2c_slaveaddr);

	/* Set device type as I2C */
	e_ctrl->eeprom_device_type = MSM_CAMERA_I2C_DEVICE;
	e_ctrl->i2c_client.i2c_func_tbl = &msm_eeprom_qup_func_tbl;
	e_ctrl->i2c_client.addr_type = MSM_CAMERA_I2C_WORD_ADDR;

	if (e_ctrl->eboard_info->i2c_slaveaddr != 0)
		e_ctrl->i2c_client.client->addr =
					e_ctrl->eboard_info->i2c_slaveaddr;
	power_info->clk_info = cam_8960_clk_info;
	power_info->clk_info_size = ARRAY_SIZE(cam_8960_clk_info);
	power_info->dev = &client->dev;

	rc = of_property_read_string(of_node, "qcom,eeprom-name",
				&e_ctrl->eboard_info->eeprom_name);
	CDBG("%s qcom,eeprom-name %s, rc %d\n", __func__,
				e_ctrl->eboard_info->eeprom_name, rc);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto board_free;
	}
	rc = msm_eeprom_get_dt_data(e_ctrl);
	if (rc)
		goto board_free;
	rc = msm_eeprom_parse_memory_map(of_node, &e_ctrl->cal_data);
	if (rc < 0)
		pr_err("%s: no cal memory map\n", __func__);
	rc = msm_camera_power_up(power_info, e_ctrl->eeprom_device_type,
			&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
	if (rc) {
		pr_err("%s failed power up %d\n", __func__, __LINE__);
		goto board_free;
	}
	rc = msm_eeprom_match_id(e_ctrl, TRUE);
	if (rc < 0)
		pr_err("%s: eeprom not matching %d\n", __func__, rc);
#if 1
	pr_err("%s: Skip front read eeprom in probe func\n", __func__);
#else
	if (e_ctrl->cal_data.map) {
		rc = read_eeprom_memory(e_ctrl, &e_ctrl->cal_data);
		if (rc < 0) {
			pr_err("%s: read cal data failed\n", __func__);
			goto power_down;
		}
		e_ctrl->is_supported |= msm_eeprom_match_crc(
			&e_ctrl->cal_data, e_ctrl->subdev_id);
	}
#endif
	rc = msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
			&e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
	if (rc) {
		pr_err("failed rc %d\n", rc);
		goto power_down;
	}

	if (0 > of_property_read_u32(of_node, "qcom,sensor-position",
				&temp)) {
		pr_err("%s:%d Fail position, Default sensor position\n", __func__, __LINE__);
		temp = 0;
	}
	pr_err("%s qcom,sensor-position %d\n", __func__,temp);

	/* Initialize sub device */
	v4l2_i2c_subdev_init(&e_ctrl->msm_sd.sd,
		e_ctrl->i2c_client.client,
		e_ctrl->eeprom_v4l2_subdev_ops);
	v4l2_set_subdevdata(&e_ctrl->msm_sd.sd, e_ctrl);
	e_ctrl->msm_sd.sd.internal_ops = &msm_eeprom_internal_ops;
	e_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&e_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	e_ctrl->msm_sd.sd.entity.flags = temp;
	e_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	e_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_EEPROM;
	msm_sd_register(&e_ctrl->msm_sd);
	e_ctrl->is_supported = (e_ctrl->is_supported << 1) | 1;
	CDBG("%s EEPROM-%d e_ctrl->is_supported rc %x\n", __func__, e_ctrl->subdev_id, e_ctrl->is_supported);
	pr_err("%s success result=%d is_supported = 0x%04X\n", __func__, rc, e_ctrl->is_supported);

#ifdef CONFIG_COMPAT
	msm_eeprom_v4l2_subdev_fops = v4l2_subdev_fops;
	msm_eeprom_v4l2_subdev_fops.compat_ioctl32 =
		msm_eeprom_subdev_fops_ioctl32;
	e_ctrl->msm_sd.sd.devnode->fops = &msm_eeprom_v4l2_subdev_fops;
#endif
	return rc;

power_down:
        msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
                &e_ctrl->i2c_client, false, SUB_DEVICE_TYPE_EEPROM);
board_free:
        kfree(e_ctrl->eboard_info);
memdata_free:
	kfree(e_ctrl);
probe_failure:
	pr_err("%s failed! rc = %d\n", __func__, rc);
	return rc;
}

static int msm_eeprom_i2c_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct msm_eeprom_ctrl_t  *e_ctrl;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	e_ctrl = (struct msm_eeprom_ctrl_t *)v4l2_get_subdevdata(sd);
	if (!e_ctrl) {
		pr_err("%s: eeprom device is NULL\n", __func__);
		return 0;
	}

	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
	if (e_ctrl->eboard_info) {
		kfree(e_ctrl->eboard_info->power_info.gpio_conf);
		kfree(e_ctrl->eboard_info);
	}
	kfree(e_ctrl);
	return 0;
}

static const struct of_device_id msm_eeprom_dt_match[] = {
	{ .compatible = "qcom,eeprom" },
	{ }
};

MODULE_DEVICE_TABLE(of, msm_eeprom_dt_match);

static const struct of_device_id msm_eeprom_i2c_dt_match[] = {
	{ .compatible = "qcom,eeprom" },
	{ }
};

MODULE_DEVICE_TABLE(of, msm_eeprom_i2c_dt_match);

static struct platform_driver msm_eeprom_platform_driver = {
	.driver = {
		.name = "qcom,eeprom",
		.owner = THIS_MODULE,
		.of_match_table = msm_eeprom_dt_match,
	},
	.probe = msm_eeprom_platform_probe,
	.remove = msm_eeprom_platform_remove,
};

static const struct i2c_device_id msm_eeprom_i2c_id[] = {
	{ "msm_eeprom", (kernel_ulong_t)NULL},
	{ }
};

static struct i2c_driver msm_eeprom_i2c_driver = {
	.id_table = msm_eeprom_i2c_id,
	.probe  = msm_eeprom_i2c_probe,
	.remove = msm_eeprom_i2c_remove,
	.driver = {
		.name = "msm_eeprom",
		.owner = THIS_MODULE,
		.of_match_table = msm_eeprom_i2c_dt_match,
	},
};

static struct spi_driver msm_eeprom_spi_driver = {
	.driver = {
		.name = "qcom_eeprom",
		.owner = THIS_MODULE,
		.of_match_table = msm_eeprom_dt_match,
	},
	.probe = msm_eeprom_spi_probe,
	.remove = msm_eeprom_spi_remove,
};

static int __init msm_eeprom_init_module(void)
{
	int rc = 0, spi_rc = 0;
	CDBG("%s E\n", __func__);
	rc = platform_driver_register(&msm_eeprom_platform_driver);
	CDBG("%s:%d platform rc %d\n", __func__, __LINE__, rc);

	spi_rc = spi_register_driver(&msm_eeprom_spi_driver);
	CDBG("%s:%d spi rc %d\n", __func__, __LINE__, spi_rc);
	rc = i2c_add_driver(&msm_eeprom_i2c_driver);

	if (rc < 0 && spi_rc < 0)
		pr_err("%s:%d probe failed\n", __func__, __LINE__);
	else
		pr_info("%s:%d probe succeed\n", __func__, __LINE__);

	return rc;
}

static void __exit msm_eeprom_exit_module(void)
{
	platform_driver_unregister(&msm_eeprom_platform_driver);
	spi_unregister_driver(&msm_eeprom_spi_driver);
	i2c_del_driver(&msm_eeprom_i2c_driver);
}

module_init(msm_eeprom_init_module);
module_exit(msm_eeprom_exit_module);
MODULE_DESCRIPTION("MSM EEPROM driver");
MODULE_LICENSE("GPL v2");
