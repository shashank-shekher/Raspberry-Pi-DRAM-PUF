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
SDHOST driver. This used to be known as ALTMMC.

=============================================================================*/

#include <chainloader.h>
#include <hardware.h>
#include <drivers/BCM2708Gpio.hpp>

#include "sd_proto.hpp"
#include "block_device.hpp"

#include <stdio.h>

#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_READ_THRESHOLD_SHIFT 14
#define SDEDM_THRESHOLD_MASK     0x1f

#define SAFE_READ_THRESHOLD     4
#define SAFE_WRITE_THRESHOLD    4

#define VOLTAGE_SUPPLY_RANGE 0x100
#define CHECK_PATTERN 0x55

#define SDHSTS_BUSY_IRPT                0x400
#define SDHSTS_BLOCK_IRPT               0x200
#define SDHSTS_SDIO_IRPT                0x100
#define SDHSTS_REW_TIME_OUT             0x80
#define SDHSTS_CMD_TIME_OUT             0x40
#define SDHSTS_CRC16_ERROR              0x20
#define SDHSTS_CRC7_ERROR               0x10
#define SDHSTS_FIFO_ERROR               0x08

#define SDEDM_FSM_MASK           0xf
#define SDEDM_FSM_IDENTMODE      0x0
#define SDEDM_FSM_DATAMODE       0x1
#define SDEDM_FSM_READDATA       0x2
#define SDEDM_FSM_WRITEDATA      0x3
#define SDEDM_FSM_READWAIT       0x4
#define SDEDM_FSM_READCRC        0x5
#define SDEDM_FSM_WRITECRC       0x6
#define SDEDM_FSM_WRITEWAIT1     0x7
#define SDEDM_FSM_POWERDOWN      0x8
#define SDEDM_FSM_POWERUP        0x9
#define SDEDM_FSM_WRITESTART1    0xa
#define SDEDM_FSM_WRITESTART2    0xb
#define SDEDM_FSM_GENPULSES      0xc
#define SDEDM_FSM_WRITEWAIT2     0xd
#define SDEDM_FSM_STARTPOWDOWN   0xf

#define SDHSTS_TRANSFER_ERROR_MASK      (SDHSTS_CRC7_ERROR|SDHSTS_CRC16_ERROR|SDHSTS_REW_TIME_OUT|SDHSTS_FIFO_ERROR)
#define SDHSTS_ERROR_MASK               (SDHSTS_CMD_TIME_OUT|SDHSTS_TRANSFER_ERROR_MASK)

#define logf(fmt, ...) { print_timestamp(); printf("[EMMC:%s]: " fmt, __FUNCTION__, ##__VA_ARGS__); }

#define kIdentSafeClockRate 0x148

struct sample {
  uint32_t state;
  uint32_t time;
  uint32_t tag;
};

struct BCM2708SDHost : BlockDevice {
  bool is_sdhc;
  bool is_high_capacity;
  bool card_ready;

  uint32_t ocr;
  uint32_t rca;

  uint32_t cid[4];
  uint32_t csd[4];

  uint64_t capacity_bytes;

  uint32_t r[4];

  uint32_t current_cmd;

  struct sample samples[10];
  int last_sample;

  void maybe_record_sample(uint32_t tag, bool force = false) {
    if (last_sample >= 10) return;

    uint32_t edm = SH_EDM;
    if (last_sample == 0) {
      force = true;
    } else {
      if ( (edm&0xf) != samples[last_sample-1].state) force = true;
      if (tag != samples[last_sample-1].tag) force = true;
    }
    if (!force) return;

    samples[last_sample].state = edm&0xf;
    samples[last_sample].time = ST_CLO;
    samples[last_sample].tag = tag;
    last_sample++;
  }

	void set_power(bool on) {
		SH_VDD = on ? SH_VDD_POWER_ON_SET : 0x0;
	}

  bool wait(uint32_t timeout = 100000) {
    uint32_t t = timeout;

    while(SH_CMD & SH_CMD_NEW_FLAG_SET) {
      if (t == 0) {
        logf("timed out after %ldus!\n", timeout)
        return false;
      }
      t--;
      udelay(10);
    }

    return true;
  }

  bool send_raw(uint32_t command, uint32_t arg = 0) {
    uint32_t sts;

    wait();

    sts = SH_HSTS;
    if (sts & SDHSTS_ERROR_MASK) SH_HSTS = sts;

    current_cmd = command & SH_CMD_COMMAND_SET;

    SH_ARG = arg;
    SH_CMD = command | SH_CMD_NEW_FLAG_SET;

    mfence();

    return true;
  }

  bool __attribute__((noinline)) send(uint32_t command, uint32_t arg = 0) {
    return send_raw(command & SH_CMD_COMMAND_SET, arg);
  }

  bool send_136_resp(uint32_t command, uint32_t arg = 0) {
    return send_raw((command & SH_CMD_COMMAND_SET) | SH_CMD_LONG_RESPONSE_SET, arg);
  }

  bool __attribute__((noinline)) send_no_resp(uint32_t command, uint32_t arg = 0) {
    return send_raw((command & SH_CMD_COMMAND_SET) | SH_CMD_NO_RESPONSE_SET, arg);
  }

  void configure_pinmux() {
    BCM2708Gpio *gpio = &gGPIO;
    for (int i=48; i<54; i++) {
      gpio->setFunction(i, kBCM2708Pinmux_ALT0);
    }

    logf("waiting for pinmux pull update ...\n");

    GP_PUD = 2;
    mfence();
    udelay(500);
    GP_PUD = 0;

    logf("waiting for pinmux clock update ...\n");

    /* are these in bank 1 or 2? ah who gives a fuck ... */
    // TODO, this changes EVERY SINGLE PIN to pullup, not just the ones we care about
    GP_PUDCLK1 = GP_PUDCLK1_PUDCLKn32_SET;
    GP_PUDCLK2 = GP_PUDCLK2_PUDCLKn64_SET;
    udelay(500);

    logf("ok ...\n");
    GP_PUDCLK1 = 0;
    GP_PUDCLK2 = 0;

    logf("pinmux configured for aux0\n");
  }

  void reset() {
    logf("resetting controller ...\n");
    set_power(false);

    SH_CMD = 0;
    SH_ARG = 0;
    SH_TOUT = 0xF00000;
    SH_CDIV = 0;
    SH_HSTS = 0x7f8;
    SH_HCFG = 0;
    SH_HBCT = 0;
    SH_HBLC = 0;

    uint32_t temp = SH_EDM;

    temp &= ~((SDEDM_THRESHOLD_MASK<<SDEDM_READ_THRESHOLD_SHIFT) |
              (SDEDM_THRESHOLD_MASK<<SDEDM_WRITE_THRESHOLD_SHIFT));
    temp |= (SAFE_READ_THRESHOLD << SDEDM_READ_THRESHOLD_SHIFT) |
            (SAFE_WRITE_THRESHOLD << SDEDM_WRITE_THRESHOLD_SHIFT);

    SH_EDM = temp;
    udelay(300);

    set_power(true);

    udelay(300);
    mfence();
  }

  inline void get_response() {
    r[0] = SH_RSP0;
    r[1] = SH_RSP1;
    r[2] = SH_RSP2;
    r[3] = SH_RSP3;
  }

  bool __attribute__((noinline)) wait_and_get_response() {
    if (!wait())
      return false;

    get_response();

    //printf("Cmd: 0x%x Resp: %08x %08x %08x %08x\n", current_cmd, r[0], r[1], r[2], r[3]);

    if (SH_CMD & SH_CMD_FAIL_FLAG_SET) {
      if (SH_HSTS & SDHSTS_ERROR_MASK) {
        logf("ERROR: sdhost status: 0x%lx\n", SH_HSTS);
        return false;
      }
      logf("ERROR: unknown error, SH_CMD=0x%lx\n", SH_CMD);
      return false;
    }


    return true;
  }

  bool query_voltage_and_type() {
    uint32_t t;

    /* identify */
    send(SD_SEND_IF_COND, 0x1AA);
    wait_and_get_response();

    /* set voltage */
    t = MMC_OCR_3_3V_3_4V;
    if (r[0] == 0x1AA) {
      t |= MMC_OCR_HCS;
      is_sdhc = true;
    }

    /* query voltage and type */
    for (;;) {
      send(MMC_APP_CMD); /* 55 */
      wait();
      send(SD_APP_OP_COND, t);

      if (!wait_and_get_response())
              return false;

      if (r[0] & MMC_OCR_MEM_READY)
              break;

      logf("waiting for SD (0x%lx) ...\n", r[0]);
      udelay(100);
    }

    logf("SD card has arrived!\n");

    is_high_capacity = (r[0] & MMC_OCR_HCS) == MMC_OCR_HCS;

    if (is_high_capacity)
      logf("This is an SDHC card!\n");

    return true;

  }

  inline void copy_136_to(uint32_t* dest) {
    dest[0] = r[0];
    dest[1] = r[1];
    dest[2] = r[2];
    dest[3] = r[3];
  }

  bool identify_card() {
    logf("identifying card ...\n");

    send_136_resp(MMC_ALL_SEND_CID);
    if (!wait_and_get_response())
            return false;

    /* for SD this gets RCA */
    send(MMC_SET_RELATIVE_ADDR);
    if (!wait_and_get_response())
            return false;
    rca = SD_R6_RCA(r);

    logf("RCA = 0x%lx\n", rca);

    send_136_resp(MMC_SEND_CID, MMC_ARG_RCA(rca));
    if (!wait_and_get_response())
            return false;

    copy_136_to(cid);

    /* get card specific data */
    send_136_resp(MMC_SEND_CSD, MMC_ARG_RCA(rca));
    if (!wait_and_get_response())
            return false;

    copy_136_to(csd);

    return true;
  }

//#define DUMP_READ

  bool wait_for_fifo_data(uint32_t timeout = 100000) {
    uint32_t t = timeout;

    while ((SH_HSTS & SH_HSTS_DATA_FLAG_SET) == 0) {
      maybe_record_sample(3);
      if (t == 0) {
        putchar('\n');
        logf("ERROR: no FIFO data, timed out after %ldus!\n", timeout)
        return false;
      }
      t--;
      udelay(5);
    }

    return true;
  }

//#define DUMP_WRITE

	bool wait_for_fifo_write_access(uint32_t timeout = 100000) {
		// TODO: How to do this?
		/*uint32_t t = timeout;

		while ((SH_HSTS & SH_HSTS_DATA_FLAG_SET) == 0) {
			if (t == 0) {
				putchar('\n');
				logf("ERROR: no FIFO data, timed out after %ldus!\n", timeout)
				return false;
			}
			t--;
			udelay(10);
		}*/

		return true;
	}

  void drain_fifo() {
    /* fuck me with a rake ... gently */

    wait();

    while (SH_HSTS & SH_HSTS_DATA_FLAG_SET) {
      SH_DATA;
      mfence();
    }
  }

  void drain_fifo_nowait() {
    while (true) {
      SH_DATA;

      uint32_t hsts = SH_HSTS;
      if (hsts != SH_HSTS_DATA_FLAG_SET)
        break;
    }
  }

  virtual bool read_block(uint32_t sector, uint32_t* buf, uint32_t count) override {
    int chunks = 128 * count;
    last_sample = 0;
    SH_HBCT = block_size;
    SH_HBLC = count;
    maybe_record_sample(0);
    if (!card_ready)
      panic("card not ready");

    if (!is_high_capacity)
      sector <<= 9;

#ifdef DUMP_READ
    if (buf) {
      logf("Reading %d bytes from sector %d using FIFO ...\n", block_size, sector);
    } else {
      logf("Reading %d bytes from sector %d using FIFO > /dev/null ...\n", block_size, sector);
    }
#endif

    maybe_record_sample(1);
    /* drain junk from FIFO */
    drain_fifo();

    /* enter READ mode */
    if (count == 1) {
      send_raw(MMC_READ_BLOCK_SINGLE | SH_CMD_READ_CMD_SET | SH_CMD_BUSY_CMD_SET, sector);
    } else {
      send_raw(MMC_READ_BLOCK_MULTIPLE | SH_CMD_READ_CMD_SET | SH_CMD_BUSY_CMD_SET, sector);
    }
    wait();
    maybe_record_sample(2, true);

    int i;
    uint32_t hsts_err = 0;


#ifdef DUMP_READ
    if (buf)
      printf("----------------------------------------------------\n");
#endif

    /* drain useful data from FIFO */
    for (i = 0; i < chunks; i++) {
      maybe_record_sample(3);
      /* wait for FIFO */
      if (!wait_for_fifo_data()) {
              break;
      }
      maybe_record_sample(3);

      hsts_err = SH_HSTS & SDHSTS_ERROR_MASK;
      if (hsts_err) {
        logf("ERROR: transfer error on FIFO word %d(sector %ld): 0x%lx edm: 0x%lx\n", i, sector, SH_HSTS, SH_EDM);
        break;
      }

      volatile uint32_t data = SH_DATA;

#ifdef DUMP_READ
      printf("%08x ", data);
#endif
      if (buf) *(buf++) = data;
    }

    maybe_record_sample(4);
    send_raw(MMC_STOP_TRANSMISSION | SH_CMD_BUSY_CMD_SET);
    maybe_record_sample(5);

#ifdef DUMP_READ
          printf("\n");
          if (buf)
                  printf("----------------------------------------------------\n");
#endif

          if (hsts_err) {
                  logf("ERROR: Transfer error, status: 0x%lx\n", SH_HSTS);
                  return false;
          }

#ifdef DUMP_READ
          if (buf)
                  logf("Completed read for %ld\n", sector);
#endif

#if 0
if (sector == 0) {
printf("sector: %ld\n", sector);
for (int i=0; i<last_sample; i++) {
  uint32_t last = samples[i].time;
  if (i > 0) last = samples[i-i].time;
  printf("%d: %ld %ld %ld %ld\n", i, samples[i].state, samples[i].time, samples[i].tag, samples[i].time - last);
}
printf("\n");
}
#endif
          return true;
  }

	/*virtual bool write_block(uint32_t sector, const uint32_t* buf) override {
    uint32_t hsts = 0;
    int copy_words = block_size / sizeof(uint32_t);
    printf("writing to sector %ld\n", sector);

    int burst_words, words;
    uint32_t edm;
    send_raw(MMC_WRITE_BLOCK_SINGLE | SH_CMD_WRITE_CMD_SET, sector);
    wait();
    while (copy_words) {

      burst_words = min(SDDATA_FIFO_PIO_BURST, copy_words);
      edm = SH_EDM;
      words = SDDATA_FIFO_WORDS - edm_fifo_fill(edm);

      if (words < burst_words) {
        int fsm_state = (edm & SDEDM_FSM_MASK);

        if (fsm_state != SDEDM_FSM_WRITEDATA &&
            fsm_state != SDEDM_FSM_WRITEWAIT1 &&
            fsm_state != SDEDM_FSM_WRITEWAIT2 &&
            fsm_state != SDEDM_FSM_WRITECRC &&
            fsm_state != SDEDM_FSM_WRITESTART1 &&
            fsm_state != SDEDM_FSM_WRITESTART2) {
          hsts = SH_HSTS;
          printf("fsm %x, hsts %08x\n", fsm_state, hsts);
          if (hsts & SDHSTS_ERROR_MASK)
            break;
        }

        continue;
      } else if (words > copy_words) {
        words = copy_words;
      }

      copy_words -= words;

      while (words) {
        SH_DATA = *(buf++);
        words--;
      }
    }
    send_raw(MMC_STOP_TRANSMISSION | SH_CMD_BUSY_CMD_SET);

    return true;
  }*/

  bool select_card() {
    send(MMC_SELECT_CARD, MMC_ARG_RCA(rca));

    if (!wait())
      return false;

    return true;
  }

  bool __attribute__ ((always_inline)) init_card() {
    char pnm[8];
    uint32_t block_length;
    uint32_t clock_div = 0;
    uint8_t mid;
    uint16_t oid;
    uint8_t revision;
    uint32_t serial;
    uint16_t date;

    send_no_resp(MMC_GO_IDLE_STATE);

    if (!query_voltage_and_type()) {
            logf("ERROR: Failed to query card voltage!\n");
            return false;
    }

    if (!identify_card()) {
            logf("ERROR: Failed to identify card!\n");
            return false;
    }

    SD_CID_PNM_CPY(cid, pnm);
    mid = SD_CID_MID(cid);
    oid = SD_CID_OID(cid);
    revision = SD_CID_REV(cid);
    serial = SD_CID_PSN(cid);
    date = SD_CID_MDT(cid);

    logf("Detected SD card:\n");
    printf("    Date: 0x%x\n", date);
    printf("    Serial: 0x%lx\n", serial);
    printf("    Revision: 0x%x\n", revision);
    printf("    Product : %s\n", pnm);
    printf("    OID: 0x%x\n", oid);
    printf("    MID: 0x%x\n", mid);

    if (SD_CSD_CSDVER(csd) == SD_CSD_CSDVER_2_0) {
            printf("    CSD     : Ver 2.0\n");
            printf("    Capacity: %d\n", SD_CSD_V2_CAPACITY(csd));
            printf("    Size    : %d\n", SD_CSD_V2_C_SIZE(csd));

            block_length = 1 << SD_CSD_V2_BL_LEN;

            /* work out the capacity of the card in bytes */
            capacity_bytes = ((uint64_t)SD_CSD_V2_CAPACITY(csd) * block_length);

            clock_div = 5;
    } else if (SD_CSD_CSDVER(csd) == SD_CSD_CSDVER_1_0) {
            printf("    CSD     : Ver 1.0\n");
            printf("    Capacity: %d\n", SD_CSD_CAPACITY(csd));
            printf("    Size    : %d\n", SD_CSD_C_SIZE(csd));

            block_length = 1 << SD_CSD_READ_BL_LEN(csd);

            /* work out the capacity of the card in bytes */
            capacity_bytes = ((uint64_t)SD_CSD_CAPACITY(csd) * block_length);

            clock_div = 5;
    } else {
            printf("ERROR: Unknown CSD version 0x%x!\n", SD_CSD_CSDVER(csd));
            return false;
    }

    printf("    BlockLen: 0x%lx\n", block_length);

    if (!select_card()) {
            logf("ERROR: Failed to select card!\n");
            return false;
    }

    if (SD_CSD_CSDVER(csd) == SD_CSD_CSDVER_1_0) {
            /*
             * only needed for 1.0 ones, the 2.0 ones have this
             * fixed at 512.
             */
            logf("Setting block length to 512 ...\n");
            send(MMC_SET_BLOCKLEN, 512);
            if (!wait()) {
                    logf("ERROR: Failed to set block length!\n");
                    return false;
            }
    }

    block_size = 512;

    logf("Card initialization complete: %s %ldMB SD%s Card\n", pnm, (uint32_t)(capacity_bytes >> 20), is_high_capacity ? "HC" : "");

    /*
     * this makes some dangerous assumptions that the all csd2 cards are sdio cards
     * and all csd1 cards are sd cards and that mmc cards won't be used. this also assumes
     * PLLC.CORE0 is at 250MHz which is probably a safe assumption since we set it.
     */
    if (clock_div) {
            logf("Identification complete, changing clock to %ldMHz for data mode ...\n", 250 / clock_div);
            SH_CDIV = clock_div - 2;
    }

#if 0
    send(MMC_APP_CMD); /* 55 */
    wait();
    send(SD_APP_SET_BUS_WIDTH, 2);

    if (!wait_and_get_response()) {
      puts("failed to set bus width");
    } else {
      //SH_HCFG |= SH_HCFG_WIDE_EXT_BUS_SET;
    }
#endif

    return true;
  }

  void restart_controller() {
    is_sdhc = false;

    logf("hcfg 0x%lX, cdiv 0x%lX, edm 0x%lX, hsts 0x%lX\n",
         SH_HCFG,
         SH_CDIV,
         SH_EDM,
         SH_HSTS);

    logf("Restarting the eMMC controller ...\n");

    configure_pinmux();
    reset();

    SH_HCFG &= ~SH_HCFG_WIDE_EXT_BUS_SET;
    SH_HCFG = SH_HCFG_SLOW_CARD_SET | SH_HCFG_WIDE_INT_BUS_SET;
    SH_CDIV = kIdentSafeClockRate;

    udelay(300);
    mfence();

    if (init_card()) {
            card_ready = true;

            /*
             * looks like a silicon bug to me or a quirk of csd2, who knows
             */
            for (int i = 0; i < 3; i++) {
                    if (!read_block(0, nullptr, 1)) {
                            panic("fifo flush cycle %d failed", i);
                    }
            }
    } else {
            panic("failed to reinitialize the eMMC controller");
    }
  }

  virtual void stop() override {
    if (card_ready) {
            logf("flushing fifo ...\n");
            drain_fifo_nowait();

            logf("asking card to enter idle state ...\n");
            SH_CDIV = kIdentSafeClockRate;
            udelay(150);

            send_no_resp(MMC_GO_IDLE_STATE);
            udelay(500);
    }

    logf("stopping sdhost controller driver ...\n");

    SH_CMD = 0;
    SH_ARG = 0;
    SH_TOUT = 0xA00000;
    SH_CDIV = 0x1FB;

    logf("powering down controller ...\n");
    SH_VDD = 0;
    SH_HCFG = 0;
    SH_HBCT = 0x400;
    SH_HBLC = 0;
    SH_HSTS = 0x7F8;

    logf("resetting state machine ...\n");

    SH_CMD = 0;
    SH_ARG = 0;
  }

  BCM2708SDHost() {
    restart_controller();
    logf("eMMC driver sucessfully started!\n");
  }

  /* WRITE STUFF */


// Adapted from https://github.com/ARM-software/u-boot/blob/master/drivers/mmc/bcm2835_sdhost.c
// and https://github.com/zeoneo/rpi-3b-wifi/blob/master/src/sdhost.c

// Quick hack to make min work here
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define SDDATA_FIFO_PIO_BURST	8
#define SDDATA_FIFO_WORDS	    16

#define SDEDM_FIFO_FILL_SHIFT	4
#define SDEDM_FIFO_FILL_MASK	0x1f
static uint32_t edm_fifo_fill(uint32_t edm) {
  return (edm >> SDEDM_FIFO_FILL_SHIFT) & SDEDM_FIFO_FILL_MASK;
}
#define LOG_DEBUG           //printf
#define MicroDelay          udelay
#define BIT(nr)		(1 << (nr))
#define MMC_DATA_READ		1
#define MMC_DATA_WRITE		2
#define MMC_RSP_PRESENT (1 << 0)
#define MMC_RSP_136	(1 << 1)		/* 136 bit response */
#define MMC_RSP_CRC	(1 << 2)		/* expect valid crc */
#define MMC_RSP_BUSY	(1 << 3)		/* card may send busy */
#define MMC_RSP_OPCODE	(1 << 4)		/* response contains opcode */
#define SDHST_TIMEOUT_MAX_USEC 1000
#define MMC_CMD_GO_IDLE_STATE		0
#define MMC_CMD_SEND_OP_COND		1
#define MMC_CMD_ALL_SEND_CID		2
#define MMC_CMD_SET_RELATIVE_ADDR	3
#define MMC_CMD_SET_DSR			4
#define MMC_CMD_SWITCH			6
#define MMC_CMD_SELECT_CARD		7
#define MMC_CMD_SEND_EXT_CSD		8
#define MMC_CMD_SEND_CSD		9
#define MMC_CMD_SEND_CID		10
#define MMC_CMD_STOP_TRANSMISSION	12
#define MMC_CMD_SEND_STATUS		13
#define MMC_CMD_SET_BLOCKLEN		16
#define MMC_CMD_READ_SINGLE_BLOCK	17
#define MMC_CMD_READ_MULTIPLE_BLOCK	18
#define MMC_CMD_SEND_TUNING_BLOCK		19
#define MMC_CMD_SEND_TUNING_BLOCK_HS200	21
#define MMC_CMD_SET_BLOCK_COUNT         23
#define MMC_CMD_WRITE_SINGLE_BLOCK	24
#define MMC_CMD_WRITE_MULTIPLE_BLOCK	25
#define MMC_CMD_ERASE_GROUP_START	35
#define MMC_CMD_ERASE_GROUP_END		36
#define MMC_CMD_ERASE			38
#define MMC_CMD_APP_CMD			55
#define MMC_CMD_SPI_READ_OCR		58
#define MMC_CMD_SPI_CRC_ON_OFF		59
#define MMC_CMD_RES_MAN			62
#define MMC_STATUS_RDY_FOR_DATA (1 << 8)
#define MMC_STATUS_CURR_STATE	(0xf << 9)
#define MMC_STATUS_MASK		(~0x0206BF7F)
#define MMC_STATE_PRG		(7 << 9)
#define MMC_RSP_R1	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R1b	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE| \
			MMC_RSP_BUSY)
#define SDEDM_FORCE_DATA_MODE BIT(19)
#define NULL                0
#define SDCMD_CMD_MASK      0x3f

struct mmc_cmd {
	unsigned short cmdidx;
	unsigned int resp_type;
	unsigned int cmdarg;
	unsigned int response[4];
};

struct mmc_data {
	union {
		char *dest;
		const char *src; /* src buffers don't get written to */
	};
	unsigned int flags;
	unsigned int blocks;
	unsigned int blocksize;
};

struct __attribute__((__packed__, aligned(4))) sdhost_state {
    uint32_t blocks;
    uint8_t use_busy;
    struct mmc_cmd* cmd;
    struct mmc_data* data;
};

  static inline int is_power_of_2(uint64_t x) {
      return !(x & (x - 1));
  }

static int bcm2835_read_wait_sdcmd() {
    int timeout = 1000;
    while ((SH_CMD & SH_CMD_NEW_FLAG_SET) && --timeout > 0) {
        MicroDelay(SDHST_TIMEOUT_MAX_USEC);
    }

    // Timeout counter is either zero or -1
    if (timeout <= 0) {
        LOG_DEBUG("%s: timeout (%d us)\n", __func__, SDHST_TIMEOUT_MAX_USEC);
    }
    return SH_CMD;
}

static void bcm2835_prepare_data(struct sdhost_state* host, struct mmc_data* data) {
    // LOG_DEBUG("preparing data \n ");

    host->data = data;
    if (!data)
        return;

    /* Use PIO */
    host->blocks = data->blocks;

    SH_HBCT = data->blocksize;
    SH_HBLC = data->blocks;
}

  static int bcm2835_send_command(struct sdhost_state* host, struct mmc_cmd* cmd, struct mmc_data* data) {
      uint32_t sdcmd, sdhsts;

      // LOG_DEBUG("Command Index %d \n", cmd->cmdidx);

      if ((cmd->resp_type & MMC_RSP_136) && (cmd->resp_type & MMC_RSP_BUSY)) {
          LOG_DEBUG("unsupported response type!\n");
          return -1;
      }

      sdcmd = bcm2835_read_wait_sdcmd();
      if (sdcmd & SH_CMD_NEW_FLAG_SET) {
          LOG_DEBUG("previous command never completed.\n");
          //bcm2835_dumpregs();
          return -2;
      }

      host->cmd = cmd;
      // LOG_DEBUG("Host cmd: %x host->cmd->resp_type:%d %d\n", host->cmd, host->cmd->resp_type, cmd->resp_type);

      /* Clear any error flags */
      sdhsts = SH_HSTS;
      // bcm2835_dumpregs();
      if (sdhsts & SDHSTS_ERROR_MASK) {
        SH_HSTS = sdhsts;
      }

      SH_ARG = 0;
      MicroDelay(1000);
      bcm2835_prepare_data(host, data);
      // LOG_DEBUG("Starting command execution arg: %x \n", cmd->cmdarg);
      SH_ARG = cmd->cmdarg;

      sdcmd = cmd->cmdidx & SDCMD_CMD_MASK;
      // LOG_DEBUG("Starting command execution sdcmd: %x \n", sdcmd);

      host->use_busy = 0;
      if (!(cmd->resp_type & MMC_RSP_PRESENT)) {
          sdcmd |= SH_CMD_NO_RESPONSE_SET;
      } else {
          if (cmd->resp_type & MMC_RSP_136)
              sdcmd |= SH_CMD_LONG_RESPONSE_SET;
          if (cmd->resp_type & MMC_RSP_BUSY) {
              sdcmd |= SH_CMD_BUSY_CMD_SET;
              host->use_busy = 1;
          }
      }

      if (data) {
          if (data->flags & MMC_DATA_WRITE)
              sdcmd |= SH_CMD_WRITE_CMD_SET;
          if (data->flags & MMC_DATA_READ)
              sdcmd |= SH_CMD_READ_CMD_SET;
      }

      SH_CMD = sdcmd | SH_CMD_NEW_FLAG_SET;
      // LOG_DEBUG("Ending command execution sdcmd: %x \n", sdcmd);
      return 0;
  }

static int bcm2835_finish_command(struct sdhost_state* host) {
    struct mmc_cmd* cmd = host->cmd;
    uint32_t sdcmd;
    int ret = 0;

    sdcmd = bcm2835_read_wait_sdcmd();

    /* Check for errors */
    if (sdcmd & SH_CMD_NEW_FLAG_SET) {
        LOG_DEBUG("command never completed. sdcmd: %x SH_CMD_NEW_FLAG_SET: %x\n", sdcmd, SH_CMD_NEW_FLAG_SET);
        //bcm2835_dumpregs();
        return -1;
    } else if (sdcmd & SH_CMD_FAIL_FLAG_SET) {

        uint32_t sdhsts = SH_HSTS;
        if (sdhsts & SDHSTS_ERROR_MASK) {
            LOG_DEBUG("command Failed Check Error. sdcmd: %x status: %x\n", sdcmd, sdhsts);
            return -1;
        }
        LOG_DEBUG("command Failed Unknown Error. sdcmd: %x status: %x\n", sdcmd, sdhsts);
        return -1;

        /* Clear the errors */
        SH_HSTS = SDHSTS_ERROR_MASK;

        if (!(sdhsts & SDHSTS_CRC7_ERROR) || (host->cmd->cmdidx != MMC_CMD_SEND_OP_COND)) {
            if (sdhsts & SDHSTS_CMD_TIME_OUT) {
                LOG_DEBUG("unexpected error cond1:%d cond2:%d \n", (!(sdhsts & SDHSTS_CRC7_ERROR)),
                          (host->cmd->cmdidx != MMC_CMD_SEND_OP_COND));
                //bcm2835_dumpregs(host);
                ret = -1;
            } else {
                LOG_DEBUG("unexpected command %d error\n", host->cmd->cmdidx);
                cmd->response[0] = SH_RSP0;
                cmd->response[1] = SH_RSP1;
                cmd->response[2] = SH_RSP2;
                cmd->response[3] = SH_RSP3;
                //bcm2835_dumpregs(host);
                ret = -2;
            }
            return ret;
        }
    }
    // LOG_DEBUG("Is command %d AA\n", cmd->resp_type);
    if (cmd->resp_type & MMC_RSP_PRESENT) {
        // LOG_DEBUG("Is command %d  BB\n", cmd->resp_type);
        if (cmd->resp_type & MMC_RSP_136) {
            cmd->response[0] = SH_RSP0;
            cmd->response[1] = SH_RSP1;
            cmd->response[2] = SH_RSP2;
            cmd->response[3] = SH_RSP3;
            // bcm2835_dumpregs();
        } else {
            cmd->response[0] = SH_RSP0;
            // LOG_DEBUG("Is else command %d \n", cmd->resp_type);
        }
    }
    // bcm2835_dumpregs(host);
    /* Processed actual command. */
    host->cmd = NULL;
    // printf("Returning bcm2835_finish_command \n");
    return ret;
}

static int bcm2835_wait_transfer_complete() {
    int timediff = 0;

    while (1) {
        uint32_t edm, fsm;

        edm = SH_EDM;
        fsm = edm & SDEDM_FSM_MASK;

        if ((fsm == SDEDM_FSM_IDENTMODE) || (fsm == SDEDM_FSM_DATAMODE))
            break;

        if ((fsm == SDEDM_FSM_READWAIT) || (fsm == SDEDM_FSM_WRITESTART1) || (fsm == SDEDM_FSM_READDATA)) {
            SH_EDM = edm | SDEDM_FORCE_DATA_MODE;
            break;
        }

        /* Error out after 100000 register reads (~1s) */
        if (timediff++ == 200000) {
            LOG_DEBUG("wait_transfer_complete - still waiting after %d retries\n", timediff);
            // bcm2835_dumpregs();
            return -1;
        }
    }

    return 0;
}

static int bcm2835_transfer_block_pio(struct sdhost_state* host, bool is_read) {
    struct mmc_data* data = host->data;
    size_t blksize        = data->blocksize;
    int copy_words;
    uint32_t hsts = 0;
    uint32_t* buf;

    if (blksize % sizeof(uint32_t)) {
        LOG_DEBUG("Size error \n");
        return -1;
    }

    buf = is_read ? (uint32_t*) data->dest : (uint32_t*) data->src;

    if (is_read) {
        data->dest += blksize;
    } else {
        data->src += blksize;
    }

    copy_words = blksize / sizeof(uint32_t);

    /*
     * Copy all contents from/to the FIFO as far as it reaches,
     * then wait for it to fill/empty again and rewind.
     */
    while (copy_words) {
        int burst_words, words;
        uint32_t edm;

        burst_words = min(SDDATA_FIFO_PIO_BURST, copy_words);
        edm         = SH_EDM;
        if (is_read) {
            words = edm_fifo_fill(edm);
        } else {
            words = SDDATA_FIFO_WORDS - edm_fifo_fill(edm);
        }

        if (words < burst_words) {
            int fsm_state = (edm & SDEDM_FSM_MASK);

            if ((is_read && (fsm_state != SDEDM_FSM_READDATA && fsm_state != SDEDM_FSM_READWAIT &&
                             fsm_state != SDEDM_FSM_READCRC)) ||
                (!is_read && (fsm_state != SDEDM_FSM_WRITEDATA && fsm_state != SDEDM_FSM_WRITEWAIT1 &&
                              fsm_state != SDEDM_FSM_WRITEWAIT2 && fsm_state != SDEDM_FSM_WRITECRC &&
                              fsm_state != SDEDM_FSM_WRITESTART1 && fsm_state != SDEDM_FSM_WRITESTART2))) {

                hsts = SH_HSTS;
                LOG_DEBUG("fsm %x, hsts %08x\n", fsm_state, hsts);
                if (hsts & SDHSTS_ERROR_MASK) {
                    break;
                }
            }
            continue;
        } else if (words > copy_words) {
            words = copy_words;
        }
        copy_words -= words;

        /* Copy current chunk to/from the FIFO */
        while (words) {
            if (is_read)
                *(buf++) = SH_DATA;
            else
                SH_DATA = *(buf++);
            words--;
        }
    }

    return 0;
}

static int bcm2835_transfer_pio(struct sdhost_state* host) {
    uint32_t sdhsts;
    bool is_read;
    int ret = 0;

    is_read = (host->data->flags & MMC_DATA_READ) != 0;
    ret     = bcm2835_transfer_block_pio(host, is_read);
    if (ret)
        return ret;

    sdhsts = SH_HSTS;
    if (sdhsts & (SDHSTS_CRC16_ERROR | SDHSTS_CRC7_ERROR | SDHSTS_FIFO_ERROR)) {
        LOG_DEBUG("%s transfer error - HSTS %08x\n", is_read ? "read" : "write", sdhsts);
        ret = -1;
    } else if ((sdhsts & (SDHSTS_CMD_TIME_OUT | SDHSTS_REW_TIME_OUT))) {
        LOG_DEBUG("%s timeout error - HSTS %08x\n", is_read ? "read" : "write", sdhsts);
        ret = -2;
    }

    return ret;
}

static int bcm2835_check_cmd_error(struct sdhost_state* host, uint32_t intmask) {
    int ret = -1;

    if (!(intmask & SDHSTS_ERROR_MASK))
        return 0;

    if (!host->cmd)
        return -1;

    LOG_DEBUG("sdhost_busy_irq: intmask %08x\n", intmask);
    if (intmask & SDHSTS_CRC7_ERROR) {
        ret = -2;
    } else if (intmask & (SDHSTS_CRC16_ERROR | SDHSTS_FIFO_ERROR)) {
        ret = -2;
    } else if (intmask & (SDHSTS_REW_TIME_OUT | SDHSTS_CMD_TIME_OUT)) {
        ret = -3;
    }
    //bcm2835_dumpregs();
    return ret;
}

static int bcm2835_check_data_error(struct sdhost_state* host, uint32_t intmask) {
    int ret = 0;

    if (!host->data)
        return 0;
    if (intmask & (SDHSTS_CRC16_ERROR | SDHSTS_FIFO_ERROR))
        ret = -1;
    if (intmask & SDHSTS_REW_TIME_OUT)
        ret = -2;

    if (ret)
        LOG_DEBUG("%s:%d Mask: %lx Error: %d\n", __func__, __LINE__, intmask, ret);

    return ret;
}

static int bcm2835_transmit(struct sdhost_state* host) {
    uint32_t intmask = SH_HSTS;
    int ret;
    LOG_DEBUG("Transmit, SH_HSTS: %x\n", intmask);

    /* Check for errors */
    ret = bcm2835_check_data_error(host, intmask);
    if (ret) {
        LOG_DEBUG("Data error\n");
        return ret;
    }

    ret = bcm2835_check_cmd_error(host, intmask);
    if (ret) {
        LOG_DEBUG("Cmd error\n");
        return ret;
    }

    /* Handle wait for busy end */
    if (host->use_busy && (intmask & SDHSTS_BUSY_IRPT)) {
        SH_HSTS = SDHSTS_BUSY_IRPT;
        host->use_busy = false;
        bcm2835_finish_command(host);
    }

    /* Handle PIO data transfer */
    if (host->data) {
        ret = bcm2835_transfer_pio(host);
        if (ret)
            return ret;
        host->blocks--;
        if (host->blocks == 0) {
            /* Wait for command to complete for real */
            ret = bcm2835_wait_transfer_complete();
            if (ret)
                return ret;
            /* Transfer complete */
            host->data = NULL;
        }
    }
    LOG_DEBUG("Return %s \n", __func__);
    return 0;
}

int bcm2835_send_cmd(struct sdhost_state* host, struct mmc_cmd* cmd, struct mmc_data* data) {
    uint32_t edm, fsm;
    int ret = 0;
    // LOG_DEBUG("received command : %d \n ", cmd->cmdidx);

    edm = SH_EDM;
    fsm = edm & SDEDM_FSM_MASK;

    if ((fsm != SDEDM_FSM_IDENTMODE) && (fsm != SDEDM_FSM_DATAMODE) &&
        (cmd->cmdidx != MMC_STOP_TRANSMISSION)) {
        LOG_DEBUG("previous command (%d) not complete (EDM %08x)\n", SH_CMD & SDCMD_CMD_MASK, edm);
        //bcm2835_dumpregs(host);

        if (cmd) {
            LOG_DEBUG("Error command\n");
            return -1;
        }
        return 0;
    }

    // LOG_DEBUG("Previous command  %x \n", readl(SDCMD));
    if (cmd) {
        ret = bcm2835_send_command(host, cmd, data);
        if (!ret && !host->use_busy) {
            ret = bcm2835_finish_command(host);
        }
    }

    /* Wait for completion of busy signal or data transfer */
    while (host->use_busy || host->data) {
      if (host->data)
        LOG_DEBUG("host->use_busy: %d host->data: %x|%d|%d|%x|%x\n", host->use_busy, host->data,
            host->data->blocks, host->data->blocksize, host->data->src, host->data->flags);
      else
        LOG_DEBUG("host->use_busy: %d host->data: %x\n", host->use_busy, host->data);
        ret = bcm2835_transmit(host);
        if (ret) {
            break;
        }
    }
    // LOG_DEBUG("Return from command: \n");
    return ret;
}

int mmc_send_status(struct sdhost_state *host, int timeout)
{
	struct mmc_cmd cmd;
	int err, retries = 5;

	cmd.cmdidx = MMC_CMD_SEND_STATUS;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = rca << 16;

	while (1) {
		err = bcm2835_send_cmd(host, &cmd, NULL);
		if (!err) {
			if ((cmd.response[0] & MMC_STATUS_RDY_FOR_DATA) &&
			    (cmd.response[0] & MMC_STATUS_CURR_STATE) !=
			     MMC_STATE_PRG)
				break;

			if (cmd.response[0] & MMC_STATUS_MASK) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
				LOG_DEBUG("Status Error: 0x%08X\n",
				       cmd.response[0]);
#endif
				return -1;
			}
		} else if (--retries < 0)
			return err;

		if (timeout-- <= 0)
			break;

		udelay(1000);
	}

	if (timeout <= 0) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
		LOG_DEBUG("Timeout waiting card ready\n");
#endif
		return -2;
	}

	return 0;
}

void mmc_write_blocks(struct sdhost_state *host, uint32_t sector, uint32_t count, const uint32_t* buf) {
    struct mmc_cmd cmd;
    struct mmc_data data;
    printf("mmc_write_blocks: sector:%d count:%d\n", sector, count);
    cmd.cmdidx = count == 1 ? MMC_CMD_WRITE_SINGLE_BLOCK : MMC_CMD_WRITE_MULTIPLE_BLOCK;
    cmd.resp_type = MMC_RSP_R1;
    cmd.cmdarg = sector * block_size;
    data.src = reinterpret_cast<const char*>(buf);
    data.flags = MMC_DATA_WRITE;
    data.blocks = count;
    data.blocksize = block_size;
    bcm2835_send_cmd(host, &cmd, &data);
    if (count > 1) {
      cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
      cmd.resp_type = MMC_RSP_R1b;
      cmd.cmdarg = 0;
      if (bcm2835_send_cmd(host, &cmd, NULL)) {
        printf("mmc fail to send stop cmd\n");
        return;
      }
    }

	  mmc_send_status(host, 1000);
}

	virtual bool write_block(uint32_t sector, const uint32_t* buf, uint32_t count) override {
    struct sdhost_state host;
    host.blocks = 0;
    host.cmd = 0;
    host.data = 0;
    host.use_busy = 0;
    unsigned long cur, blocks_todo = count;
    do {
      cur = blocks_todo;
      mmc_write_blocks(&host, sector, cur, buf);
      blocks_todo -= cur;
      sector += cur;
      buf += cur * block_size;
    } while (blocks_todo > 0);
    return true;
	}
};

BCM2708SDHost *g_SDHostDriver;

void sdhost_init() {
  g_SDHostDriver = new BCM2708SDHost();
}

BlockDevice* get_sdhost_device() {
  if (!g_SDHostDriver) panic("sdhost not initialized yet\n");
  return g_SDHostDriver;
}
