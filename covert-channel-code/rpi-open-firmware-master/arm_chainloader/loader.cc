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
Second stage bootloader.

=============================================================================*/

#include <string.h>
#include <drivers/fatfs/ff.h>
#include <chainloader.h>
#include <drivers/mailbox.hpp>
#include <drivers/block_device.hpp>
#include <libfdt.h>
#include <memory_map.h>

#define logf(fmt, ...) printf("[LDR:%s]: " fmt, __FUNCTION__, ##__VA_ARGS__);

FATFS g_BootVolumeFs;

#define ROOT_VOLUME_PREFIX "0:"
#define DTB_LOAD_ADDRESS    0xF000000 // 240 mb from start
#define KERNEL_LOAD_ADDRESS 0x2000000 // 32 mb from start

typedef void (*linux_t)(uint32_t, uint32_t, void *);

static_assert((MEM_USABLE_START + 0x800000) < KERNEL_LOAD_ADDRESS,
              "memory layout would not allow for kernel to be loaded at KERNEL_LOAD_ADDRESS, please check memory_map.h");

struct LoaderImpl {

    linux_t kernel;

    inline bool file_exists(const char *path) {
        FRESULT r = f_stat(path, NULL);
        if (r != FR_OK) {
            logf("Stat %s: %d\n", path, r);
            return false;
        }
        return true;
    }

    size_t read_file(const char *path, uint8_t *&dest, bool should_alloc = true) {
        /* ensure file exists first */
        if (!file_exists(path))
            panic("attempted to read %s, but it does not exist", path);

        /* read entire file into buffer */
        FIL fp;
        f_open(&fp, path, FA_READ);

        unsigned int len = f_size(&fp);

        if (should_alloc) {
            /*
             * since this can be used for strings, there's no harm in reserving an
             * extra byte for the null terminator and appending it.
             */
            uint8_t *buffer = new uint8_t[len + 1];
            dest = buffer;
            buffer[len] = 0;
        }

        logf("%s: reading %d bytes to 0x%X (allocated=%d) ...\n", path, len, (unsigned int) dest, should_alloc);

        f_read(&fp, dest, len, &len);
        f_close(&fp);

        return len;
    }

    size_t write_file(const char *path, uint8_t *&src, unsigned int len) {
        /* read entire file into buffer */
        FIL fp;
        FRESULT res = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);

        logf("Opened file %s with code %d\n", path, res);
        logf("%s: writing %d bytes from 0x%X ...\n", path, len, (unsigned int) src);

        f_write(&fp, src, len, &len);
        f_close(&fp);

        return len;
    }

    uint8_t *load_fdt(const char *filename, uint8_t *cmdline) {
        /* read device tree blob */
        uint8_t *fdt = reinterpret_cast<uint8_t *>(DTB_LOAD_ADDRESS);
        size_t sz = read_file(filename, fdt, false);
        logf("FDT loaded at %X, size is %d\n", (unsigned int) fdt, sz);

        void *v_fdt = reinterpret_cast<void *>(fdt);

        int res;

        if ((res = fdt_check_header(v_fdt)) != 0) {
            panic("FDT blob invalid, fdt_check_header returned %d", res);
        }

        /* pass in command line args */
        res = fdt_open_into(v_fdt, v_fdt, sz + 4096);

        int node = fdt_path_offset(v_fdt, "/chosen");
        if (node < 0)
            panic("no chosen node in fdt");

        res = fdt_setprop(v_fdt, node, "bootargs", cmdline, strlen((char *) cmdline) + 1);

        /* pass in a memory map, skipping first meg for bootcode */
        int memory = fdt_path_offset(v_fdt, "/memory");
        if (memory < 0)
            panic("no memory node in fdt");

        /* start the memory map at 1M/16 and grow continuous for 256M
         * TODO: does this disrupt I/O? */

        char dtype[] = "memory";
        uint8_t memmap[] = {0x00, 0x00, 0x01, 0x00, 0x30, 0x00, 0x00, 0x00};
        res = fdt_setprop(v_fdt, memory, "reg", (void *) memmap, sizeof(memmap));

        logf("FDT loaded at 0x%X\n", (unsigned int) fdt);

        return fdt;
    }

    void teardown_hardware() {
        BlockDevice *bd = get_sdhost_device();
        if (bd)
            bd->stop();
    }

    LoaderImpl() {
        logf("Mounting boot partition ...\n");
        FRESULT r = f_mount(&g_BootVolumeFs, ROOT_VOLUME_PREFIX, 1);
        if (r != FR_OK) {
            panic("failed to mount boot partition, error: %d", (int) r);
        }
        logf("Boot partition mounted!\n");

        /* read the kernel as a function pointer at fixed address */
        uint8_t *zImage = reinterpret_cast<uint8_t *>(KERNEL_LOAD_ADDRESS);
        kernel = reinterpret_cast<linux_t>(zImage);

        const char *kernelPath[] = {"kernel.img", "kernel1.img", "kernel2.img", "kernell.img", "ker.img", "zImage"};
        const char *loadPath = "kernel.img";
        for (auto &path : kernelPath) {
            if (file_exists(path)) {
                loadPath = path;
                break;
            }
        }
        size_t ksize = read_file(loadPath, zImage, false);
        logf("Kernel Image loaded at 0x%X\n", (unsigned int) kernel);

        /* TEST FILE WRITE */
        //size_t wrsz = write_file("testfile.txt", zImage, 128);
        //logf("size written: %lu\n", wrsz);

        /* read the command-line null-terminated */
        uint8_t *cmdline;
        size_t cmdlen = read_file("cmdline.txt", cmdline);

        logf("kernel cmdline: %s\n", cmdline);

        /* load flat device tree */
        uint8_t *fdt = load_fdt("rpi.dtb", cmdline);

        /* once the fdt contains the cmdline, it is not needed */
        delete[] cmdline;

        /* flush the cache */
        logf("Flushing....\n")
        for (uint8_t *i = zImage; i < zImage + ksize; i += 32) {
            __asm__ __volatile__ ("mcr p15,0,%0,c7,c10,1" : : "r" (i) : "memory");
        }

        /* the eMMC card in particular needs to be reset */
        teardown_hardware();

        /*uint8_t *outname;
        size_t outname_len = read_file("outname.txt", outname);
        logf("%s\n", outname);
        FIL outname_file;
        f_open(&outname_file, "outname.txt", FA_WRITE);
        f_truncate(&outname_file);
        f_write();
        f_close();*/

        /* fire away -- this should never return */
        logf("Jumping to the Linux kernel...\n");
        kernel(0, ~0, fdt);
    }
};

static LoaderImpl STATIC_APP
g_Loader {
};
