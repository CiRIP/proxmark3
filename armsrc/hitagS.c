//-----------------------------------------------------------------------------
// Borrowed initially from https://github.com/Proxmark/proxmark3/pull/167/files
// Copyright (C) 2016 Oguzhan Cicek, Hendrik Schwartke, Ralf Spenneberg
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Hitag S emulation (preliminary test version)
//-----------------------------------------------------------------------------

#include "hitagS.h"

#include "proxmark3_arm.h"
#include "cmd.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "util.h"
#include "string.h"
#include "commonutil.h"
#include "hitag2/hitag2_crypto.h"
#include "lfadc.h"
#include "crc.h"
#include <protocols.h>

#define CRC_PRESET 0xFF
#define CRC_POLYNOM 0x1D

static struct hitagS_tag tag;
static uint8_t page_to_be_written = 0;
static int block_data_left = 0;

typedef enum modulation {
    AC2K = 0,
    AC4K,
    MC4K,
    MC8K
} MOD;

static MOD m = AC2K;               // used modulation
static uint32_t temp_uid;
static int temp2 = 0;
static int sof_bits;               // number of start-of-frame bits
static uint8_t pwdh0, pwdl0, pwdl1; // password bytes
static uint32_t rnd = 0x74124485;   // random number
//#define SENDBIT_TEST

/* array index 3 2 1 0 // bytes in sim.bin file are 0 1 2 3
// UID is 0 1 2 3 // tag.uid is 3210
// datasheet HitagS_V11.pdf bytes in tables printed 3 2 1 0

#db# UID: 5F C2 11 84
#db# conf0: C9 conf1: 00 conf2: 00
                3  2  1  0
#db# Page[ 0]: 84 11 C2 5F uid
#db# Page[ 1]: AA 00 00 C9 conf, HITAG S 256
#db# Page[ 2]: 4E 4F 54 48
#db# Page[ 3]: 52 4B 49 4D
#db# Page[ 4]: 00 00 00 00
#db# Page[ 5]: 00 00 00 00
#db# Page[ 6]: 00 00 00 00
#db# Page[ 7]: 4B 4F 5F 57 */

#define ht2bs_4a(a,b,c,d)   (~(((a|b)&c)^(a|d)^b))
#define ht2bs_4b(a,b,c,d)   (~(((d|c)&(a^b))^(d|a|b)))
#define ht2bs_5c(a,b,c,d,e) (~((((((c^e)|d)&a)^b)&(c^b))^(((d^e)|a)&((d^b)|c))))

// Sam7s has several timers, we will use the source TIMER_CLOCK3 (aka AT91C_TC_CLKS_TIMER_DIV3_CLOCK)
// TIMER_CLOCK3 = MCK/32, MCK is running at 48 MHz, Timer is running at 48MHz/32 = 1500 KHz
// Hitag units (T0) have duration of 8 microseconds (us), which is 1/125000 per second (carrier)
// T0 = TIMER_CLOCK3 / 125000 = 12

#define T0                             12

#define HITAG_FRAME_LEN                20

// TC0 and TC1 are 16-bit counters and will overflow after 5461 * T0
// Ensure not to set these timings above 5461 (~43ms) when comparing without considering overflow, as they will never reach that value.

#define HITAG_T_STOP                   36  /* T_EOF should be > 36 */
#define HITAG_T_LOW                    8   /* T_LOW should be 4..10 */
#define HITAG_T_0_MIN                  15  /* T[0] should be 18..22 */
#define HITAG_T_1_MIN                  25  /* T[1] should be 26..30 */
#define HITAG_T_0                      20  /* T[0] should be 18..22 */
#define HITAG_T_1                      28  /* T[1] should be 26..30 */
// #define HITAG_T_EOF   40 /* T_EOF should be > 36 */
#define HITAG_T_EOF                    80   /* T_EOF should be > 36 */
#define HITAG_T_WAIT_RESP              200  /* T_wresp should be 204..212 */
#define HITAG_T_WAIT_SC                200   /* T_wsc should be 90..5000 */
#define HITAG_T_WAIT_FIRST             300  /* T_wfc should be 280..565 (T_ttf) */
#define HITAG_T_PROG_MAX               750  /* T_prog should be 716..726 */

#define HITAG_T_TAG_ONE_HALF_PERIOD    10
#define HITAG_T_TAG_TWO_HALF_PERIOD    25
#define HITAG_T_TAG_THREE_HALF_PERIOD  41
#define HITAG_T_TAG_FOUR_HALF_PERIOD   57

#define HITAG_T_TAG_HALF_PERIOD        16
#define HITAG_T_TAG_FULL_PERIOD        32

#define HITAG_T_TAG_CAPTURE_ONE_HALF   13
#define HITAG_T_TAG_CAPTURE_TWO_HALF   25
#define HITAG_T_TAG_CAPTURE_THREE_HALF 41
#define HITAG_T_TAG_CAPTURE_FOUR_HALF  57

/*
 * Implementation of the crc8 calculation from Hitag S
 * from http://www.proxmark.org/files/Documents/125%20kHz%20-%20Hitag/HitagS.V11.pdf
 */
static void calc_crc(unsigned char *crc, unsigned char data, unsigned char Bitcount) {
    *crc ^= data; // crc = crc (exor) data
    do {
        if (*crc & 0x80) { // if (MSB-CRC == 1)
            *crc <<= 1; // CRC = CRC Bit-shift left
            *crc ^= CRC_POLYNOM; // CRC = CRC (exor) CRC_POLYNOM
        } else {
            *crc <<= 1; // CRC = CRC Bit-shift left
        }
    } while (--Bitcount);
}

static void hitag_send_bit(int bit, bool ledcontrol) {

    if (ledcontrol) LED_A_ON();
    // Reset clock for the next bit
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;

    switch (m) {
        case AC2K: {
            if (bit == 0) {
                // AC Coding --__
                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 32) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 64)  {};

            } else {
                // AC coding -_-_
                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 16) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 32) {};

                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 48) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 64) {};

            }
            if (ledcontrol) LED_A_OFF();
            break;
        }
        case AC4K: {
            if (bit == 0) {
                // AC Coding --__
                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_TAG_HALF_PERIOD) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_TAG_FULL_PERIOD) {};

            } else {
                // AC coding -_-_
                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 8) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 16) {};

                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 24) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 32) {};
            }
            if (ledcontrol) LED_A_OFF();
            break;
        }
        case MC4K: {
            if (bit == 0) {
                // Manchester: Unloaded, then loaded |__--|
                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 16) {};

                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 32) {};

            } else {
                // Manchester: Loaded, then unloaded |--__|
                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 16) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 32) {};

            }
            if (ledcontrol) LED_A_OFF();
            break;
        }
        case MC8K: {
            if (bit == 0) {
                // Manchester: Unloaded, then loaded |__--|
                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 8) {};

                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 16) {};

            } else {
                // Manchester: Loaded, then unloaded |--__|
                HIGH(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 8) {};

                LOW(GPIO_SSC_DOUT);
                while (AT91C_BASE_TC0->TC_CV < T0 * 16) {};

            }
            if (ledcontrol) LED_A_OFF();
            break;
        }
        default: {
            break;
        }
    }
}

static void hitag_send_frame(const uint8_t *frame, size_t frame_len, bool ledcontrol) {

    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("hitag_send_frame: (%i) %02X %02X %02X %02X", frame_len, frame[0], frame[1], frame[2], frame[3]);
    }

    // The beginning of the frame is hidden in some high level; pause until our bits will have an effect
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;
    HIGH(GPIO_SSC_DOUT);
    switch (m) {
        case AC4K:
        case MC8K: {
            while (AT91C_BASE_TC0->TC_CV < T0 * 40) {}; //FADV
            break;
        }
        case AC2K:
        case MC4K: {
            while (AT91C_BASE_TC0->TC_CV < T0 * 20) {}; //STD + ADV
            break;
        }
    }

    // SOF - send start of frame
    for (size_t i = 0; i < sof_bits; i++) {
        hitag_send_bit(1, ledcontrol);
    }

    // Send the content of the frame
    for (size_t i = 0; i < frame_len; i++) {
        hitag_send_bit((frame[i / 8] >> (7 - (i % 8))) & 1, ledcontrol);
    }

    LOW(GPIO_SSC_DOUT);
}

static void hitag_reader_send_bit(int bit, bool ledcontrol) {

    if (ledcontrol) LED_A_ON();
    // Reset clock for the next bit
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;
    while (AT91C_BASE_TC0->TC_CV > 0);

    // Binary puls length modulation (BPLM) is used to encode the data stream
    // This means that a transmission of a one takes longer than that of a zero

    HIGH(GPIO_SSC_DOUT);

#ifdef SENDBIT_TEST
    // Wait for 4-10 times the carrier period
    while (AT91C_BASE_TC0->TC_CV < T0 * 6) {};

    LOW(GPIO_SSC_DOUT);

    if (bit == 0) {
        // Zero bit: |_-|
        while (AT91C_BASE_TC0->TC_CV < T0 * 11) {};
    } else {
        // One bit: |_--|
        while (AT91C_BASE_TC0->TC_CV < T0 * 14) {};
    }
#else
    // Wait for 4-10 times the carrier period
    while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_LOW) {};

    LOW(GPIO_SSC_DOUT);

    if (bit == 0) {
        // Zero bit: |_-|
        while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_0) {};
    } else {
        // One bit: |_--|
        while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_1) {};
    }
#endif

    if (ledcontrol) LED_A_OFF();
}

static void hitag_reader_send_frame(const uint8_t *frame, size_t frame_len, bool ledcontrol) {
    // Send the content of the frame
    for (size_t i = 0; i < frame_len; i++) {
//        if (frame[0] == 0xf8) {
        //Dbprintf("BIT: %d",(frame[i / 8] >> (7 - (i % 8))) & 1);
//        }
        hitag_reader_send_bit((frame[i / 8] >> (7 - (i % 8))) & 1, ledcontrol);
    }
    // send EOF
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;
    while (AT91C_BASE_TC0->TC_CV > 0);
    HIGH(GPIO_SSC_DOUT);

    // Wait for 4-10 times the carrier period
    while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_LOW) {};

    LOW(GPIO_SSC_DOUT);
}

static void hitagS_init_clock(void) {

    // Enable Peripheral Clock for
    //   Timer Counter 0, used to measure exact timing before answering
    //   Timer Counter 1, used to capture edges of the tag frames
    AT91C_BASE_PMC->PMC_PCER |= (1 << AT91C_ID_TC0) | (1 << AT91C_ID_TC1);

    AT91C_BASE_PIOA->PIO_BSR = GPIO_SSC_FRAME;

    // Disable timer during configuration
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

    // TC0: Capture mode, clock source = MCK/32 (TIMER_CLOCK3), no triggers
    AT91C_BASE_TC0->TC_CMR = AT91C_TC_CLKS_TIMER_DIV3_CLOCK;

    // TC1: Capture mode, clock source = MCK/32 (TIMER_CLOCK3), TIOA is external trigger,
    // external trigger falling edge, set RA on falling edge of TIOA.
    AT91C_BASE_TC1->TC_CMR =
        AT91C_TC_CLKS_TIMER_DIV3_CLOCK |
        AT91C_TC_ETRGEDG_FALLING | // external trigger on falling edge
        AT91C_TC_ABETRG |          // TIOA is used as an external trigger.
        AT91C_TC_LDRA_FALLING |    // load RA on on falling edge
        AT91C_TC_ACPA_CLEAR |      // RA comperator clears TIOA (carry bit)
        AT91C_TC_ASWTRG_SET;       // SWTriger sets TIOA (carry bit)

    AT91C_BASE_TC1->TC_RA  = 1; // clear carry bit on next clock cycle

    // Enable and reset counters
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

    // for (size_t i = 0; i < 10; i++) __asm("");
    // uint16_t cv0 = AT91C_BASE_TC0->TC_CV;

    // synchronized startup procedure
    // In theory, with MCK/32, we shouldn't be waiting longer than 32 instruction statements, right?
    while (AT91C_BASE_TC0->TC_CV > 0) {}; // wait until TC0 returned to zero
//    while (AT91C_BASE_TC0->TC_CV < 2) {}; // and has started (TC_CV > TC_RA, now TC1 is cleared)

    // Dbprintf("TC0_CV0:%i TC0_CV:%i TC1_CV:%i", cv0, AT91C_BASE_TC0->TC_CV, AT91C_BASE_TC1->TC_CV);
}

static void hitagS_stop_clock(void) {
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;
}

/*
 * to check if the right uid was selected
 */
static int check_select(const uint8_t *rx, uint32_t uid) {

    unsigned char resp[48];
    uint32_t ans = 0x0;

    for (int i = 0; i < 48; i++) {
        resp[i] = (rx[i / 8] >> (7 - (i % 8))) & 0x1;
    }

    for (int i = 0; i < 32; i++) {
        ans += resp[5 + i] << (31 - i);
    }

    // global var?
    temp_uid = ans;

    if (ans == tag.uid) {
        return 1;
    }

    return 0;
}

static void hitagS_set_frame_modulation(void) {
    switch (tag.mode) {
        case HT_STANDARD: {
            sof_bits = 1;
            m = MC4K;
            break;
        }
        case HT_ADVANCED: {
            sof_bits = 6;
            m = MC4K;
            break;
        }
        case HT_FAST_ADVANCED: {
            sof_bits = 6;
            m = MC8K;
            break;
        }
        default: {
            break;
        }
    }
}

/*
 * handles all commands from a reader
 */
static void hitagS_handle_reader_command(uint8_t *rx, const size_t rxlen,
                                         uint8_t *tx, size_t *txlen) {
    uint8_t rx_air[HITAG_FRAME_LEN];
    uint64_t state;
    unsigned char crc;

    // Copy the (original) received frame how it is send over the air
    memcpy(rx_air, rx, nbytes(rxlen));

    // Reset the transmission frame length
    *txlen = 0;

    // Try to find out which command was send by selecting on length (in bits)
    switch (rxlen) {
        case 5: {
            //UID request with a selected response protocol mode
            if (g_dbglevel >= DBG_EXTENDED)
                Dbprintf("UID request: length: %i first byte: %02x", rxlen, rx[0]);

            tag.pstate = HT_READY;
            tag.tstate = HT_NO_OP;
            if ((rx[0] & 0xf0) == HITAGS_UID_REQ_STD) {
                if (g_dbglevel >= DBG_EXTENDED)
                    Dbprintf("HT_STANDARD");

                tag.mode = HT_STANDARD;
                sof_bits = 1;
                m = AC2K;
            }
            if ((rx[0] & 0xf0) == HITAGS_UID_REQ_ADV) {
                tag.mode = HT_ADVANCED;
                if (g_dbglevel >= DBG_EXTENDED)
                    Dbprintf("HT_ADVANCED");

                sof_bits = 3;
                m = AC2K;
            }

            if ((rx[0] & 0xf0) == HITAGS_UID_REQ_FADV) {
                if (g_dbglevel >= DBG_EXTENDED)
                    Dbprintf("HT_FAST_ADVANCED");

                tag.mode = HT_FAST_ADVANCED;
                sof_bits = 3;
                m = AC4K;
            }
            //send uid as a response
            *txlen = 32;
            for (int i = 0; i < 4; i++) {
                tx[i] = (tag.uid >> (24 - (i * 8))) & 0xFF;
            }
            break;
        }
        case 45: {
            //select command from reader received
            if (g_dbglevel >= DBG_EXTENDED) {
                DbpString("SELECT");
            }

            if ((rx[0] & 0xf8) == HITAGS_SELECT && check_select(rx, tag.uid) == 1) {
                if (g_dbglevel >= DBG_EXTENDED) {
                    DbpString("SELECT match");
                }

                //if the right tag was selected
                *txlen = 32;
                hitagS_set_frame_modulation();

                //send configuration
                for (int i = 0; i < 4; i++) {
                    tx[i] = tag.pages[1][i];
                }

                tx[3] = 0xff;

                if (tag.mode != HT_STANDARD) {

                    *txlen = 40;
                    crc = CRC_PRESET;

                    for (int i = 0; i < 4; i++) {
                        calc_crc(&crc, tx[i], 8);
                    }

                    tx[4] = crc;
                }
            }
            break;
        }
        case 64: {
            //challenge message received
            Dbprintf("Challenge for UID: %X", temp_uid);
            temp2++;
            *txlen = 32;
            state = ht2_hitag2_init(REV64(tag.key),
                                    REV32((tag.pages[0][3] << 24) + (tag.pages[0][2] << 16) + (tag.pages[0][1] << 8) + tag.pages[0][0]),
                                    REV32((rx[3] << 24) + (rx[2] << 16) + (rx[1] << 8) + rx[0])
                                   );
            Dbprintf(",{0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
                     rx[0], rx[1], rx[2], rx[3],
                     rx[4], rx[5], rx[6], rx[7]
                    );

            hitagS_set_frame_modulation();

            for (int i = 0; i < 4; i++) {
                ht2_hitag2_byte(&state);
            }

            //send con2, pwdh0, pwdl0, pwdl1 encrypted as a response
            tx[0] = ht2_hitag2_byte(&state) ^ tag.pages[1][2];
            tx[1] = ht2_hitag2_byte(&state) ^ tag.pwdh0;
            tx[2] = ht2_hitag2_byte(&state) ^ tag.pwdl0;
            tx[3] = ht2_hitag2_byte(&state) ^ tag.pwdl1;

            if (tag.mode != HT_STANDARD) {
                //add crc8
                *txlen = 40;
                crc = CRC_PRESET;
                calc_crc(&crc, tag.pages[1][2], 8);
                calc_crc(&crc, tag.pwdh0, 8);
                calc_crc(&crc, tag.pwdl0, 8);
                calc_crc(&crc, tag.pwdl1, 8);
                tx[4] = (crc ^ ht2_hitag2_byte(&state));
            }
            /*
             * some readers do not allow to authenticate multiple times in a row with the same tag.
             * use this to change the uid between authentications.

             if (temp2 % 2 == 0) {
             tag.uid = 0x11223344;
             tag.pages[0][0] = 0x11;
             tag.pages[0][1] = 0x22;
             tag.pages[0][2] = 0x33;
             tag.pages[0][3] = 0x44;
             } else {
             tag.uid = 0x55667788;
             tag.pages[0][0] = 0x55;
             tag.pages[0][1] = 0x66;
             tag.pages[0][2] = 0x77;
             tag.pages[0][3] = 0x88;
             }
             */
            break;
        }
        case 40: {
            if (g_dbglevel >= DBG_EXTENDED)
                Dbprintf("WRITE DATA");
            //data received to be written
            if (tag.tstate == HT_WRITING_PAGE_DATA) {
                tag.tstate = HT_NO_OP;
                tag.pages[page_to_be_written][0] = rx[0];
                tag.pages[page_to_be_written][1] = rx[1];
                tag.pages[page_to_be_written][2] = rx[2];
                tag.pages[page_to_be_written][3] = rx[3];
                //send ack
                *txlen = 2;
                tx[0] = 0x40;
                page_to_be_written = 0;
                hitagS_set_frame_modulation();

            } else if (tag.tstate == HT_WRITING_BLOCK_DATA) {
                tag.pages[page_to_be_written][0] = rx[0];
                tag.pages[page_to_be_written][1] = rx[1];
                tag.pages[page_to_be_written][2] = rx[2];
                tag.pages[page_to_be_written][3] = rx[3];
                //send ack
                *txlen = 2;
                tx[0] = 0x40;
                hitagS_set_frame_modulation();
                page_to_be_written++;
                block_data_left--;

                if (block_data_left == 0) {
                    tag.tstate = HT_NO_OP;
                    page_to_be_written = 0;
                }
            }
            break;
        }
        case 20: {
            //write page, write block, read page or read block command received
            if ((rx[0] & 0xf0) == HITAGS_READ_PAGE) { //read page
                //send page data
                uint8_t page = ((rx[0] & 0x0f) * 16) + ((rx[1] & 0xf0) / 16);
                *txlen = 32;
                tx[0] = tag.pages[page][0];
                tx[1] = tag.pages[page][1];
                tx[2] = tag.pages[page][2];
                tx[3] = tag.pages[page][3];

                if (tag.LKP && page == 1) {
                    tx[3] = 0xFF;
                }

                hitagS_set_frame_modulation();

                if (tag.mode != HT_STANDARD) {
                    //add crc8
                    *txlen = 40;
                    crc = CRC_PRESET;
                    for (int i = 0; i < 4; i++) {
                        calc_crc(&crc, tx[i], 8);
                    }
                    tx[4] = crc;
                }

                if (tag.LKP && (page == 2 || page == 3)) {
                    //if reader asks for key or password and the LKP-mark is set do not respond
                    sof_bits = 0;
                    *txlen = 0;
                }

            } else if ((rx[0] & 0xf0) == HITAGS_READ_BLOCK) { //read block

                uint8_t page = ((rx[0] & 0x0f) * 16) + ((rx[1] & 0xf0) / 16);
                *txlen = 32 * 4;

                //send page,...,page+3 data
                for (int i = 0; i < 4; i++) {
                    tx[0 + i * 4] = tag.pages[page + 0 + i * 4][0];
                    tx[1 + i * 4] = tag.pages[page + 1 + i * 4][1];
                    tx[2 + i * 4] = tag.pages[page + 2 + i * 4][2];
                    tx[3 + i * 4] = tag.pages[page + 3 + i * 4][3];
                }

                hitagS_set_frame_modulation();

                if (tag.mode != HT_STANDARD) {
                    //add crc8
                    *txlen = 32 * 4 + 8;
                    crc = CRC_PRESET;
                    for (int i = 0; i < 16; i++) {
                        calc_crc(&crc, tx[i], 8);
                    }
                    tx[16] = crc;
                }

                if ((page) % 4 != 0 || (tag.LKP && (page) == 0)) {
                    sof_bits = 0;
                    *txlen = 0;
                }

            } else if ((rx[0] & 0xf0) == HITAGS_WRITE_PAGE) { //write page

                uint8_t page = ((rx[0] & 0x0f) * 16) + ((rx[1] & 0xf0) / 16);

                if ((tag.LCON && page == 1)
                        || (tag.LKP && (page == 2 || page == 3))) {
                    //deny
                    *txlen = 0;
                } else {
                    //allow
                    *txlen = 2;
                    tx[0] = 0x40;
                    page_to_be_written = page;
                    tag.tstate = HT_WRITING_PAGE_DATA;
                }

            } else if ((rx[0] & 0xf0) == HITAGS_WRITE_BLOCK) { //write block

                uint8_t page = ((rx[0] & 0x0f) * 6) + ((rx[1] & 0xf0) / 16);
                hitagS_set_frame_modulation();

                if (page % 4 != 0 || page == 0) {
                    //deny
                    *txlen = 0;
                } else {
                    //allow
                    *txlen = 2;
                    tx[0] = 0x40;
                    page_to_be_written = page;
                    block_data_left = 4;
                    tag.tstate = HT_WRITING_BLOCK_DATA;
                }
            }
            break;
        }
        default: {
            if (g_dbglevel >= DBG_EXTENDED) {
                Dbprintf("unknown rxlen: (%i) %02X %02X %02X %02X ...", rxlen, rx[0], rx[1], rx[2], rx[3]);
            }
            break;
        }
    }
}

/*
 * Emulates a Hitag S Tag with the given data from the .hts file
 */
void SimulateHitagSTag(bool tag_mem_supplied, const uint8_t *data, bool ledcontrol) {

    StopTicks();

    int response = 0, overflow = 0;
    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;
    uint8_t txbuf[HITAG_FRAME_LEN];
    uint8_t *tx = txbuf;
    size_t txlen = 0;

    // Reset the received frame, frame count and timing info
    memset(rx, 0x00, sizeof(rx));

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);

    // Clean up trace and prepare it for storing frames
    set_tracing(true);
    clear_trace();

    DbpString("Starting Hitag S simulation");
    if (ledcontrol) LED_D_ON();

    tag.pstate = HT_READY;
    tag.tstate = HT_NO_OP;

    // read tag data into memory
    if (tag_mem_supplied) {

        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 4; j++) {
                tag.pages[i][j] = 0x0;
            }
        }

        DbpString("Loading hitag S memory...");
        memcpy((uint8_t *)tag.pages, data, 4 * 64);
    } else {
        // use the last read tag
    }

    tag.uid = ((tag.pages[0][3]) << 24) | ((tag.pages[0][2]) << 16) | ((tag.pages[0][1]) << 8) | tag.pages[0][0];
    tag.key = (((uint64_t)tag.pages[3][3]) << 40) |
              (((uint64_t)tag.pages[3][2]) << 32) |
              (((uint64_t)tag.pages[3][1]) << 24) |
              (((uint64_t)tag.pages[3][0]) << 16) |
              (((uint64_t)tag.pages[2][3]) << 8) |
              (((uint64_t)tag.pages[2][2]));

    tag.pwdl0 = tag.pages[2][0];
    tag.pwdl1 = tag.pages[2][1];
    tag.pwdh0 = tag.pages[1][3];
    //con0
    tag.max_page = 64;

    if ((tag.pages[1][0] & 0x2) == 0 && (tag.pages[1][0] & 0x1) == 1) {
        tag.max_page = 8;
    }

    if ((tag.pages[1][0] & 0x2) == 0 && (tag.pages[1][0] & 0x1) == 0) {
        tag.max_page = 0;
    }

    if (g_dbglevel >= DBG_EXTENDED) {

        for (int i = 0; i < tag.max_page; i++) {
            Dbprintf("Page[%2d]: %02X %02X %02X %02X", i,
                     (tag.pages[i][3]) & 0xFF,
                     (tag.pages[i][2]) & 0xFF,
                     (tag.pages[i][1]) & 0xFF,
                     tag.pages[i][0] & 0xFF
                    );
        }
    }

    //con1
    tag.auth = 0;
    if ((tag.pages[1][1] & 0x80) == 0x80) {
        tag.auth = 1;
    }

    tag.LCON = 0;
    if ((tag.pages[1][1] & 0x2) == 0x02) {
        tag.LCON = 1;
    }

    tag.LKP = 0;
    if ((tag.pages[1][1] & 0x1) == 0x01) {
        tag.LKP = 1;
    }

    //con2
    //0=read write 1=read only
    tag.LCK7 = 0;
    if ((tag.pages[1][2] & 0x80) == 0x80) {
        tag.LCK7 = 1;
    }

    tag.LCK6 = 0;
    if ((tag.pages[1][2] & 0x40) == 0x040) {
        tag.LCK6 = 1;
    }

    tag.LCK5 = 0;
    if ((tag.pages[1][2] & 0x20) == 0x20) {
        tag.LCK5 = 1;
    }

    tag.LCK4 = 0;
    if ((tag.pages[1][2] & 0x10) == 0x10) {
        tag.LCK4 = 1;
    }

    tag.LCK3 = 0;
    if ((tag.pages[1][2] & 0x8) == 0x08) {
        tag.LCK3 = 1;
    }

    tag.LCK2 = 0;
    if ((tag.pages[1][2] & 0x4) == 0x04) {
        tag.LCK2 = 1;
    }

    tag.LCK1 = 0;
    if ((tag.pages[1][2] & 0x2) == 0x02) {
        tag.LCK1 = 1;
    }

    tag.LCK0 = 0;
    if ((tag.pages[1][2] & 0x1) == 0x01) {
        tag.LCK0 = 1;
    }


    // Set up simulator mode, frequency divisor which will drive the FPGA
    // and analog mux selection.
    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT);
    FpgaSendCommand(FPGA_CMD_SET_DIVISOR, LF_DIVISOR_125); //125kHz
    SetAdcMuxFor(GPIO_MUXSEL_LOPKD);

    // Configure output pin that is connected to the FPGA (for modulating)
    AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;
    AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT;

    // Disable modulation at default, which means release resistance
    LOW(GPIO_SSC_DOUT);

    // Enable Peripheral Clock for
    //   Timer Counter 0, used to measure exact timing before answering
    //   Timer Counter 1, used to capture edges of the tag frames
    AT91C_BASE_PMC->PMC_PCER |= (1 << AT91C_ID_TC0) | (1 << AT91C_ID_TC1);

    AT91C_BASE_PIOA->PIO_BSR = GPIO_SSC_FRAME;

    // Disable timer during configuration
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

    // TC0: Capture mode, default timer source = MCK/32 (TIMER_CLOCK3), no triggers
    AT91C_BASE_TC0->TC_CMR = AT91C_TC_CLKS_TIMER_DIV3_CLOCK;

    // TC1: Capture mode, default timer source = MCK/32 (TIMER_CLOCK3), TIOA is external trigger,
    // external trigger rising edge, load RA on rising edge of TIOA.
    AT91C_BASE_TC1->TC_CMR = AT91C_TC_CLKS_TIMER_DIV3_CLOCK
                             | AT91C_TC_ETRGEDG_RISING | AT91C_TC_ABETRG | AT91C_TC_LDRA_RISING;

    // Enable and reset counter
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

    // synchronized startup procedure
    while (AT91C_BASE_TC0->TC_CV > 0); // wait until TC0 returned to zero

    while ((BUTTON_PRESS() == false) && (data_available() == false)) {

        WDT_HIT();

        // Receive frame, watch for at most T0*EOF periods
        while (AT91C_BASE_TC1->TC_CV < T0 * HITAG_T_EOF) {

            // Check if rising edge in modulation is detected
            if (AT91C_BASE_TC1->TC_SR & AT91C_TC_LDRAS) {

                // Retrieve the new timing values
                int ra = (AT91C_BASE_TC1->TC_RA / T0) + overflow;
                overflow = 0;

                // Reset timer every frame, we have to capture the last edge for timing
                AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

                if (ledcontrol) LED_B_ON();

                // Capture reader frame
                if (ra >= HITAG_T_STOP) {
                    if (rxlen != 0) {
                        //DbpString("weird0?");
                    }
                    // Capture the T0 periods that have passed since last communication or field drop (reset)
                    response = (ra - HITAG_T_LOW);
                } else if (ra >= HITAG_T_1_MIN) {
                    // '1' bit
                    rx[rxlen / 8] |= 1 << (7 - (rxlen % 8));
                    rxlen++;
                } else if (ra >= HITAG_T_0_MIN) {
                    // '0' bit
                    rx[rxlen / 8] |= 0 << (7 - (rxlen % 8));
                    rxlen++;
                } else {
                    // Ignore weird value, is to small to mean anything
                }
            }
        }

        // Check if frame was captured
        if (rxlen > 0) {
            LogTraceBits(rx, rxlen, response, response, true);

            // Disable timer 1 with external trigger to avoid triggers during our own modulation
            AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

            // Process the incoming frame (rx) and prepare the outgoing frame (tx)
            hitagS_handle_reader_command(rx, rxlen, tx, &txlen);

            // Wait for HITAG_T_WAIT_RESP carrier periods after the last reader bit,
            // not that since the clock counts since the rising edge, but T_Wait1 is
            // with respect to the falling edge, we need to wait actually (T_Wait1 - T_Low)
            // periods. The gap time T_Low varies (4..10). All timer values are in
            // terms of T0 units
            while (AT91C_BASE_TC0->TC_CV < T0 * (HITAG_T_WAIT_RESP - HITAG_T_LOW)) {};

            // Send and store the tag answer (if there is any)
            if (txlen > 0) {
                // Transmit the tag frame
                hitag_send_frame(tx, txlen, ledcontrol);
                LogTraceBits(tx, txlen, 0, 0, false);
            }

            // Enable and reset external trigger in timer for capturing future frames
            AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

            // Reset the received frame and response timing info
            memset(rx, 0x00, sizeof(rx));
            response = 0;

            if (ledcontrol) LED_B_OFF();
        }
        // Reset the frame length
        rxlen = 0;
        // Save the timer overflow, will be 0 when frame was received
        overflow += (AT91C_BASE_TC1->TC_CV / T0);
        // Reset the timer to restart while-loop that receives frames
        AT91C_BASE_TC1->TC_CCR = AT91C_TC_SWTRG;

    }

    set_tracing(false);
    lf_finalize(ledcontrol);
    // release allocated memory from BigBuff.
    BigBuf_free();

    DbpString("Sim Stopped");
}

static void hitagS_receive_frame(uint8_t *rx, size_t sizeofrx, size_t *rxlen, uint32_t *resptime, bool ledcontrol) {

    // Reset values for receiving frames
    memset(rx, 0x00, sizeofrx);
    *rxlen = 0;

    int lastbit = 1;
    bool bSkip = true;
    *resptime = 0;
    uint32_t errorCount = 0;
    bool bStarted = false;

    uint32_t ra_i=0, h2 = 0, h3 = 0, h4 = 0;
    uint8_t edges[160]={0};

    // Dbprintf("TC0_CV:%i TC1_CV:%i TC1_RA:%i", AT91C_BASE_TC0->TC_CV, AT91C_BASE_TC1->TC_CV ,AT91C_BASE_TC1->TC_RA);

    // Receive frame, watch for at most T0*HITAG_T_PROG_MAX periods
    while (AT91C_BASE_TC0->TC_CV < (T0 * HITAG_T_PROG_MAX)) {

        // Check if falling edge in tag modulation is detected
        if (AT91C_BASE_TC1->TC_SR & AT91C_TC_LDRAS) {

            // Retrieve the new timing values
            uint32_t ra = AT91C_BASE_TC1->TC_RA / T0;
            edges[ra_i++] = ra;
            // Reset timer every frame, we have to capture the last edge for timing
            AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;

            if (ledcontrol) LED_B_ON();

            // Capture tag frame (manchester decoding using only falling edges)

            if (bStarted == false) {

                // Capture the T0 periods that have passed since last communication or field drop (reset)
                *resptime = ra - HITAG_T_TAG_HALF_PERIOD;

                if (ra >= HITAG_T_WAIT_RESP) {
                    bStarted = true;

                    // We always receive a 'one' first, which has the falling edge after a half period |-_|                    
                    rx[0] = 0x80;
                    (*rxlen)++;
                } else {
                    errorCount++;
                }

            } else if (ra >= HITAG_T_TAG_CAPTURE_FOUR_HALF) {

                // Manchester coding example |-_|_-|-_| (101)
                rx[(*rxlen) / 8] |= 0 << (7 - ((*rxlen) % 8));
                (*rxlen)++;

                rx[(*rxlen) / 8] |= 1 << (7 - ((*rxlen) % 8));
                (*rxlen)++;
                h4++;
            } else if (ra >= HITAG_T_TAG_CAPTURE_THREE_HALF) {

                // Manchester coding example |_-|...|_-|-_| (0...01)
                rx[(*rxlen) / 8] |= 0 << (7 - ((*rxlen) % 8));
                (*rxlen)++;

                // We have to skip this half period at start and add the 'one' the second time
                if (bSkip == false) {
                    rx[(*rxlen) / 8] |= 1 << (7 - ((*rxlen) % 8));
                    (*rxlen)++;
                }

                lastbit = !lastbit;
                bSkip = !bSkip;
                h3++;
            } else if (ra >= HITAG_T_TAG_CAPTURE_TWO_HALF) {
                // Manchester coding example |_-|_-| (00) or |-_|-_| (11)
                // bit is same as last bit
                rx[(*rxlen) / 8] |= lastbit << (7 - ((*rxlen) % 8));
                (*rxlen)++;
                h2++;
            } else {
                // Ignore weird value, is to small to mean anything
                errorCount++;
            }
        }

        // if we saw over 100 weird values break it probably isn't hitag...
        if (errorCount > 100 || (*rxlen) / 8 >= sizeofrx) {
            break;
        }

        // We can break this loop if we received the last bit from a frame
        // max periods between 2 falling edge
        // RTF AC64 |--__|--__| (00) 64 * T0
        // RTF MC32 |_-|-_|_-| (010) 48 * T0
        if (AT91C_BASE_TC1->TC_CV > (T0 * 80)) {
            if ((*rxlen)) {
                break;
            }
        }
    }
    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("RX0 %i:%02X.. err:%i resptime:%i h2:%i h3:%i h4:%i edges:", *rxlen, rx[0], errorCount, *resptime, h2, h3, h4);
        Dbhexdump(ra_i, edges, false);
    }
}

static void sendReceiveHitagS(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t sizeofrx, size_t *prxbits, int t_wait, bool ledcontrol, bool ac_seq) {

    LogTraceBits(tx, txlen, HITAG_T_WAIT_SC, HITAG_T_WAIT_SC, true);

    // Send and store the reader command
    // Disable timer 1 with external trigger to avoid triggers during our own modulation
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

    // Wait for HITAG_T_WAIT_SC carrier periods after the last tag bit before transmitting,
    // Since the clock counts since the last falling edge, a 'one' means that the
    // falling edge occurred halfway the period. with respect to this falling edge,
    // we need to wait (T_Wait2 + half_tag_period) when the last was a 'one'.
    // All timer values are in terms of T0 units
    while (AT91C_BASE_TC0->TC_CV < T0 * t_wait) {};

    // Transmit the reader frame
    hitag_reader_send_frame(tx, txlen, ledcontrol);

    // Enable and reset external trigger in timer for capturing future frames
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

    uint32_t resptime = 0;
    size_t rxlen = 0;
    hitagS_receive_frame(rx, sizeofrx, &rxlen, &resptime, ledcontrol);
    int k = 0;

    // Check if frame was captured and store it
    if (rxlen > 0) {

        uint8_t response_bit[sizeofrx * 8];

        for (size_t i = 0; i < rxlen; i++) {
            response_bit[i] = (rx[i / 8] >> (7 - (i % 8))) & 1;
        }

        if (g_dbglevel >= DBG_EXTENDED) {
            Dbprintf("htS: rxlen...... %zu", rxlen);
            Dbprintf("htS: sizeofrx... %zu", sizeofrx);
            DbpString("htS: response_bit:");
            Dbhexdump(rxlen, response_bit, false);
        }

        memset(rx, 0x00, sizeofrx);

        if (ac_seq) {

            // Tag Response is AC encoded
            // We used UID Request Advanced,  meaning AC SEQ header is  111.
            for (int i = 7; i < rxlen; i += 2) {

                rx[k / 8] |= response_bit[i] << (7 - (k % 8));

                k++;

                if (k > 8 * sizeofrx) {
                    break;
                }
            }

            // TODO: It's very confusing to reinterpreter the MC to AC; we should implement a more straightforward approach.
            // add the lost bit zero, when AC64 last bit is zero
            if (k % 8 == 7) {
                k++;
            }

            if (g_dbglevel >= DBG_EXTENDED) {
                DbpString("htS: ac sequence compress");
                Dbhexdump(k / 8, rx, false);
            }

        } else {

            if (g_dbglevel >= DBG_EXTENDED) {
                DbpString("htS: skipping 6 bit header");
            }

            // ignore first 6 bits: SOF (actually 1 or 6 depending on response protocol)
            // or rather a header.
            for (size_t i = 6; i < rxlen; i++) {

                rx[k / 8] |= response_bit[i] << (7 - (k % 8));
                k++;

                if (k > 8 * sizeofrx) {
                    break;
                }
            }
        }
        LogTraceBits(rx, k, resptime, resptime, false);
    }
    *prxbits = k;
}

static size_t concatbits(uint8_t *dst, size_t dstskip, const uint8_t *src, size_t srcstart, size_t srclen) {
    // erase dstbuf bits that will be overriden
    dst[dstskip / 8] &= 0xFF - ((1 << (7 - (dstskip % 8) + 1)) - 1);
    for (size_t i = (dstskip / 8) + 1; i <= (dstskip + srclen) / 8; i++) {
        dst[i] = 0;
    }

    for (size_t i = 0; i < srclen; i++) {
        // equiv of dstbufbits[dstbufskip + i] = srcbufbits[srcbufstart + i]
        dst[(dstskip + i) / 8] |= ((src[(srcstart + i) / 8] >> (7 - ((srcstart + i) % 8))) & 1) << (7 - ((dstskip + i) % 8));
    }

    return dstskip + srclen;
}

static int selectHitagS(const lf_hitag_data_t *packet, uint8_t *tx, size_t sizeoftx, uint8_t *rx, size_t sizeofrx, int t_wait, bool ledcontrol) {

    StopTicks();

    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);

    // Clean up trace and prepare it for storing frames
    set_tracing(true);
    clear_trace();

    if (ledcontrol) LED_D_ON();

    hitagS_init_clock();

    // Set fpga in edge detect with reader field, we can modulate as reader now
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT | FPGA_LF_EDGE_DETECT_READER_FIELD);
    FpgaSendCommand(FPGA_CMD_SET_DIVISOR, LF_DIVISOR_125); //125kHz
    SetAdcMuxFor(GPIO_MUXSEL_LOPKD);

    // Configure output and enable pin that is connected to the FPGA (for modulating)
    AT91C_BASE_PIOA->PIO_OER |= GPIO_SSC_DOUT;
    AT91C_BASE_PIOA->PIO_PER |= GPIO_SSC_DOUT;

    // Disable modulation at default, which means enable the field
    LOW(GPIO_SSC_DOUT);

    // Dbprintf("TC0_CV:%i TC1_CV:%i TC1_RA:%i", AT91C_BASE_TC0->TC_CV, AT91C_BASE_TC1->TC_CV, AT91C_BASE_TC1->TC_RA);

    // UID request standard   00110
    // UID request Advanced   1100x
    // UID request FAdvanced  11010
    size_t txlen = 0;
    size_t rxlen = 0;
    uint8_t cmd = HITAGS_UID_REQ_ADV;
    txlen = concatbits(tx, txlen, &cmd, 0, 5);
    sendReceiveHitagS(tx, txlen, rx, sizeofrx, &rxlen, t_wait, ledcontrol, true);

    if (rxlen != 32) {
        DbpString("UID Request failed!");
        return -1;
    }

    tag.uid = (rx[3] << 24 | rx[2] << 16 | rx[1] << 8 | rx[0]);

    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("UID: %02X %02X %02X %02X", rx[0], rx[1], rx[2], rx[3]);
    }

    //select uid
    txlen = 0;
    cmd = HITAGS_SELECT;
    txlen = concatbits(tx, txlen, &cmd, 0, 5);
    txlen = concatbits(tx, txlen, rx, 0, 32);
    uint8_t crc = CRC8Hitag1Bits(tx, txlen);
    txlen = concatbits(tx, txlen, &crc, 0, 8);

    sendReceiveHitagS(tx, txlen, rx, sizeofrx, &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

    if (rxlen != 40) {
        Dbprintf("Select UID failed! %i", rxlen);
        return -1;
    }

    uint8_t conf_pages[3];
    conf_pages[0] = rx[0];

    //check which memorysize this tag has
    if ((conf_pages[0] & 0x3) == 0x00) {
        tag.max_page = 32 / 32;
    } else if ((conf_pages[0] & 0x3) == 0x1) {
        tag.max_page = 256 / 32;
    } else if ((conf_pages[0] & 0x3) == 0x2) {
        tag.max_page = 2048 / 32;
    }

    conf_pages[1] = rx[1];
    tag.auth = (conf_pages[1] >> 7) & 0x1;
    tag.TTFC = (conf_pages[1] >> 6) & 0x1;
    tag.TTFDR = (conf_pages[1] >> 5) & 0x3;
    tag.TTFM = (conf_pages[1] >> 3) & 0x3;
    tag.LCON = (conf_pages[1] >> 1) & 0x1;
    tag.LKP = (conf_pages[1] >> 0) & 0x1;

    conf_pages[2] = rx[2];

    tag.LCK7 = (conf_pages[2] >> 7) & 0x1;
    tag.LCK6 = (conf_pages[2] >> 6) & 0x1;
    tag.LCK5 = (conf_pages[2] >> 5) & 0x1;
    tag.LCK4 = (conf_pages[2] >> 4) & 0x1;
    tag.LCK3 = (conf_pages[2] >> 3) & 0x1;
    tag.LCK2 = (conf_pages[2] >> 2) & 0x1;
    tag.LCK1 = (conf_pages[2] >> 1) & 0x1;
    tag.LCK0 = (conf_pages[2] >> 0) & 0x1;

    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("conf 0: %02X conf 1: %02X conf 2: %02X", conf_pages[0], conf_pages[1], conf_pages[2]);
    }

    if (tag.auth == 1) {
        uint64_t key = 0;
        //if the tag is in authentication mode try the key or challenge
        if (packet->cmd == RHTSF_KEY || packet->cmd == WHTSF_KEY) {

            if (g_dbglevel >= DBG_EXTENDED) {
                DbpString("Authenticating using key:");
                Dbhexdump(6, packet->key, false);
            }
            key = ((uint64_t)packet->key[0]) <<  0 |
                  ((uint64_t)packet->key[1]) <<  8 |
                  ((uint64_t)packet->key[2]) << 16 |
                  ((uint64_t)packet->key[3]) << 24 |
                  ((uint64_t)packet->key[4]) << 32 |
                  ((uint64_t)packet->key[5]) << 40
                  ;

            uint64_t state = ht2_hitag2_init(REV64(key), REV32(tag.uid), REV32(rnd));

            uint8_t auth_ks[4];
            for (int i = 0; i < 4; i++) {
                auth_ks[i] = ht2_hitag2_byte(&state) ^ 0xff;
            }

            txlen = 0;
            uint8_t revrnd[4] = {rnd, rnd >> 8, rnd >> 16, rnd >> 24};
            txlen = concatbits(tx, txlen, revrnd, 0, 32);
            txlen = concatbits(tx, txlen, auth_ks, 0, 32);

            if (g_dbglevel >= DBG_EXTENDED) {
                Dbprintf("%02X %02X %02X %02X %02X %02X %02X %02X"
                         , tx[0], tx[1], tx[2], tx[3]
                         , tx[4], tx[5], tx[6], tx[7]
                        );
            }

        } else if (packet->cmd == RHTSF_CHALLENGE || packet->cmd == WHTSF_CHALLENGE) {

            if (g_dbglevel >= DBG_EXTENDED) {
                DbpString("Authenticating using nr,ar pair:");
                Dbhexdump(8, packet->NrAr, false);
            }

            uint64_t NrAr = 0;
            NrAr = ((uint64_t)packet->NrAr[7]) <<  0 |
                   ((uint64_t)packet->NrAr[6]) <<  8 |
                   ((uint64_t)packet->NrAr[5]) << 16 |
                   ((uint64_t)packet->NrAr[4]) << 24 |
                   ((uint64_t)packet->NrAr[3]) << 32 |
                   ((uint64_t)packet->NrAr[2]) << 40 |
                   ((uint64_t)packet->NrAr[1]) << 48 |
                   ((uint64_t)packet->NrAr[0]) << 56;

            txlen = 64;
            for (int i = 0; i < 8; i++) {
                tx[i] = ((NrAr >> (56 - (i * 8))) & 0xFF);
            }

        } else if (packet->cmd == RHTSF_PLAIN || packet->cmd == WHTSF_PLAIN) {
            Dbprintf("Error, " _YELLOW_("AUT=1") " This tag is configured in Authentication Mode");
            return -1;
        } else {
            Dbprintf("Error, unknown function: " _RED_("%d"), packet->cmd);
            return -1;
        }

        sendReceiveHitagS(tx, txlen, rx, sizeofrx, &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

        if (rxlen != 40) {
            Dbprintf("Authenticate failed! " _RED_("%i"), rxlen);
            return -1;
        }

        //encrypted con2,password received.
        if (g_dbglevel >= DBG_EXTENDED) {
            Dbprintf("UID:::%X", tag.uid);
            Dbprintf("RND:::%X", rnd);
        }

        //decrypt password
        pwdh0 = 0;
        pwdl0 = 0;
        pwdl1 = 0;
        if (packet->cmd == RHTSF_KEY || packet->cmd == WHTSF_KEY) {

            uint64_t state = ht2_hitag2_init(REV64(key), REV32(tag.uid), REV32(rnd));
            for (int i = 0; i < 4; i++) {
                ht2_hitag2_byte(&state);
            }

            uint8_t con2 = rx[0] ^ ht2_hitag2_byte(&state);
            pwdh0 = rx[1] ^ ht2_hitag2_byte(&state);
            pwdl0 = rx[2] ^ ht2_hitag2_byte(&state);
            pwdl1 = rx[3] ^ ht2_hitag2_byte(&state);

            if (g_dbglevel >= DBG_EXTENDED) {
                Dbprintf("con2 %02X pwdh0 %02X pwdl0 %02X pwdl1 %02X", con2, pwdh0, pwdl0, pwdl1);
            }
            //Dbprintf("%X %02X", rnd, ((rx[4] & 0x0f) * 16) + ((rx[5] & 0xf0) / 16));
            //rnd += 1;
        }
    }
    return 0;
}

/*
 * Authenticates to the Tag with the given key or challenge.
 * If the key was given the password will be decrypted.
 * Reads every page of a hitag S transpoder.
 */
void ReadHitagS(const lf_hitag_data_t *payload, bool ledcontrol) {

    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;

    uint8_t tx[HITAG_FRAME_LEN];

    if (selectHitagS(payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol) == -1) {

        hitagS_stop_clock();
        set_tracing(false);
        lf_finalize(ledcontrol);
        reply_ng(CMD_LF_HITAGS_READ, PM3_ERFTRANS, NULL, 0);
        return;
    }

    int pageNum = 0;

    while ((BUTTON_PRESS() == false) && (data_available() == false)) {

        WDT_HIT();

        //send read request
        size_t txlen = 0;
        uint8_t cmd = HITAGS_READ_PAGE;
        txlen = concatbits(tx, txlen, &cmd, 0, 4);
        uint8_t addr = pageNum;
        txlen = concatbits(tx, txlen, &addr, 0, 8);
        uint8_t crc = CRC8Hitag1Bits(tx, txlen);
        txlen = concatbits(tx, txlen, &crc, 0, 8);

        sendReceiveHitagS(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

        if (rxlen == 0) {
            Dbprintf("Read page failed!");
            break;
        }

        //save received data - 40 bits
        for (int i = 0; i < 4 && i < rxlen; i++) {   // set page bytes from received bits
            tag.pages[pageNum][i] = rx[i];
        }

        if (g_dbglevel >= DBG_EXTENDED) {
            if (tag.auth && tag.LKP && pageNum == 1) {
                Dbprintf("Page[%2d]: %02X %02X %02X %02X", pageNum, pwdh0,
                        (tag.pages[pageNum][2]) & 0xff,
                        (tag.pages[pageNum][1]) & 0xff,
                        tag.pages[pageNum][0] & 0xff);
            } else {
                Dbprintf("Page[%2d]: %02X %02X %02X %02X", pageNum,
                        (tag.pages[pageNum][3]) & 0xff,
                        (tag.pages[pageNum][2]) & 0xff,
                        (tag.pages[pageNum][1]) & 0xff,
                        tag.pages[pageNum][0] & 0xff);
            }
        }

        pageNum++;
        //display key and password if possible
        if (pageNum == 2 && tag.auth == 1 && tag.LKP) {
            if (payload->cmd == RHTSF_KEY) {
                Dbprintf("Page[ 2]: %02X %02X %02X %02X",
                         payload->key[1],
                         payload->key[0],
                         pwdl1,
                         pwdl0
                        );
                Dbprintf("Page[ 3]: %02X %02X %02X %02X",
                         payload->key[5],
                         payload->key[4],
                         payload->key[3],
                         payload->key[2]
                        );
            } else {
                //if the authentication is done with a challenge the key and password are unknown
                Dbprintf("Page[ 2]: __ __ __ __");
                Dbprintf("Page[ 3]: __ __ __ __");
            }
            // since page 2+3 are not accessible when LKP == 1 and AUT == 1 fastforward to next readable page
            pageNum = 4;
        }
        if (pageNum >= tag.max_page) {
            break;
        }
    }

    hitagS_stop_clock();
    set_tracing(false);
    lf_finalize(ledcontrol);
    reply_ng(CMD_LF_HITAGS_READ, PM3_SUCCESS, (uint8_t *)tag.pages, sizeof(tag.pages));
}

/*
 * Authenticates to the Tag with the given Key or Challenge.
 * Writes the given 32Bit data into page_
 */
void WritePageHitagS(const lf_hitag_data_t *payload, bool ledcontrol) {

    //check for valid input
    if (payload->page == 0) {
        Dbprintf("Error, invalid page");
        reply_ng(CMD_LF_HITAGS_WRITE, PM3_EINVARG, NULL, 0);
        return;
    }

    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;

    uint8_t tx[HITAG_FRAME_LEN];
    size_t txlen = 0;

    int res = PM3_ESOFT;

    if (selectHitagS(payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol) == -1) {
        res = PM3_ERFTRANS;
        goto write_end;
    }

    //check if the given page exists
    if (payload->page > tag.max_page) {
        Dbprintf("Error, page number too large");
        res = PM3_EINVARG;
        goto write_end;
    }

    //send write page request
    txlen = 0;

    uint8_t cmd = HITAGS_WRITE_PAGE;
    txlen = concatbits(tx, txlen, &cmd, 0, 4);

    uint8_t addr = payload->page;
    txlen = concatbits(tx, txlen, &addr, 0, 8);

    uint8_t crc = CRC8Hitag1Bits(tx, txlen);
    txlen = concatbits(tx, txlen, &crc, 0, 8);

    sendReceiveHitagS(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

    if ((rxlen != 2) || (rx[0] >> (8 - 2) != 0x01)) {
        Dbprintf("no write access on page " _YELLOW_("%d"), payload->page);
        res = PM3_ESOFT;
        goto write_end;
    }

    // //ACK received to write the page. send data
    // uint8_t data[4] = {0, 0, 0, 0};
    // switch (payload->cmd) {
    //     case WHTSF_PLAIN:
    //     case WHTSF_CHALLENGE:
    //     case WHTSF_KEY:
    //         data[0] = payload->data[3];
    //         data[1] = payload->data[2];
    //         data[2] = payload->data[1];
    //         data[3] = payload->data[0];
    //         break;
    //     default: {
    //         res = PM3_EINVARG;
    //         goto write_end;
    //     }
    // }

    txlen = 0;
    txlen = concatbits(tx, txlen, payload->data, 0, 32);
    crc = CRC8Hitag1Bits(tx, txlen);
    txlen = concatbits(tx, txlen, &crc, 0, 8);

    sendReceiveHitagS(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

    if ((rxlen != 2) || (rx[0] >> (8 - 2) != 0x01)) {
        res = PM3_ESOFT; //  write failed
    } else {
        res = PM3_SUCCESS;
    }

write_end:
    hitagS_stop_clock();
    set_tracing(false);
    lf_finalize(ledcontrol);
    reply_ng(CMD_LF_HITAGS_WRITE, res, NULL, 0);
}

/*
 * Tries to authenticate to a Hitag S Transponder with the given challenges from a .cc file.
 * Displays all Challenges that failed.
 * When collecting Challenges to break the key it is possible that some data
 * is not received correctly due to Antenna problems. This function
 * detects these challenges.
 */
void Hitag_check_challenges(const uint8_t *data, uint32_t datalen, bool ledcontrol) {

    //check for valid input
    if (datalen < 8) {
        Dbprintf("Error, missing challenges");
        reply_ng(CMD_LF_HITAGS_TEST_TRACES, PM3_EINVARG, NULL, 0);
        return;
    }
    uint32_t dataoffset = 0;

    uint8_t rx[HITAG_FRAME_LEN];
    uint8_t tx[HITAG_FRAME_LEN];

    while ((BUTTON_PRESS() == false) && (data_available() == false)) {
        // Watchdog hit
        WDT_HIT();

        lf_hitag_data_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.cmd = RHTSF_CHALLENGE;

        memcpy(payload.NrAr, data + dataoffset, 8);

        int res = selectHitagS(&payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol);
        Dbprintf("Challenge %s: %02X %02X %02X %02X %02X %02X %02X %02X",
                 res == -1 ? "failed " : "success",
                 payload.NrAr[0], payload.NrAr[1],
                 payload.NrAr[2], payload.NrAr[3],
                 payload.NrAr[4], payload.NrAr[5],
                 payload.NrAr[6], payload.NrAr[7]
                );

        if (res == -1) {
            // Need to do a dummy UID select that will fail
            FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
            SpinDelay(2);
            selectHitagS(&payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol);
        }

        dataoffset += 8;
        if (dataoffset >= datalen - 8) {
            break;
        }
        // reset field
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

        // min t_reset = 2ms
        SpinDelay(2);
    }

    hitagS_stop_clock();
    set_tracing(false);
    lf_finalize(ledcontrol);
    reply_ng(CMD_ACK, PM3_SUCCESS, NULL, 0);
    return;
}
