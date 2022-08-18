/*=============================================================================
Copyright (C) 2016-2017 Authors of rpi-open-firmware
All rights reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

FILE DESCRIPTION
Block device.

=============================================================================*/

#ifdef __cplusplus
extern "C" {
#endif
void sdhost_init();

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
struct BlockDevice {
  unsigned int block_size;

  template <typename T>
  inline bool read_block(uint32_t sector, T* dest_buffer, uint32_t count) {
    return read_block(sector, reinterpret_cast<uint32_t*>(dest_buffer), count);
  }

	template <typename T>
	inline bool write_block(uint32_t sector, T* src_buffer, uint32_t count) {
		return write_block(sector, reinterpret_cast<const uint32_t*>(src_buffer), count);
	}

  inline unsigned int get_block_size() {
    return block_size;
  }

  virtual bool read_block(uint32_t sector, uint32_t* buf, uint32_t count) = 0;

	virtual bool write_block(uint32_t sector, const uint32_t* buf, uint32_t count) = 0;

  /* called to stop the block device */
  virtual void stop() {}
};

extern BlockDevice* get_sdhost_device();
#endif
