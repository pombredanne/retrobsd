/*
 * SecureDigital flash drive on SPI port.
 *
 * These cards are known to work:
 * 1) NCP SD 256Mb       - type 1, 249856 kbytes,  244 Mbytes
 * 2) Patriot SD 2Gb     - type 2, 1902592 kbytes, 1858 Mbytes
 * 3) Wintec microSD 2Gb - type 2, 1969152 kbytes, 1923 Mbytes
 * 4) Transcend SDHC 4Gb - type 3, 3905536 kbytes, 3814 Mbytes
 * 5) Verbatim SD 2Gb    - type 2, 1927168 kbytes, 1882 Mbytes
 * 6) SanDisk SDHC 4Gb   - type 3, 3931136 kbytes, 3833 Mbytes
 *
 * Copyright (C) 2010 Serge Vakulenko, <serge@vak.ru>
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/dk.h>
#include <sys/disk.h>
#include <sys/spi.h>
#include <sys/debug.h>
#include <sys/kconfig.h>
#include <machine/sd.h>

/*
 * Two SD/MMC disks on SPI.
 * Signals for SPI1:
 *      D0  - SDO1
 *      D10 - SCK1
 *      C4  - SDI1
 */
#define NSD             2
#define SECTSIZE        512
#define SPI_ENHANCED            /* use SPI fifo */
#ifndef SD0_MHZ
#define SD0_MHZ         13      /* speed 13.33 MHz */
#endif
#ifndef SD1_MHZ
#define SD1_MHZ         13      /* speed 13.33 MHz */
#endif

#define TIMO_WAIT_WDONE 400000
#define TIMO_WAIT_WIDLE 200000
#define TIMO_WAIT_CMD   100000
#define TIMO_WAIT_WDATA 30000
#define TIMO_READ       90000
#define TIMO_SEND_OP    8000
#define TIMO_CMD        7000
#define TIMO_SEND_CSD   6000
#define TIMO_WAIT_WSTOP 5000

#define sdunit(dev)     ((minor(dev) & 8) >> 3)
#define sdpart(dev)     ((minor(dev) & 7))
#define RAWPART         0               /* 'x' partition */

#define NPARTITIONS     4
#define MBR_MAGIC       0xaa55

/*
 * Driver's data per disk drive.
 */
struct disk {
    /*
     * Partition table.
     */
    struct diskpart part[NPARTITIONS+1];

    /*
     * Card type.
     */
    int     card_type;
#define TYPE_UNKNOWN    0
#define TYPE_SD_LEGACY  1
#define TYPE_SD_II      2
#define TYPE_SDHC       3

    struct spiio spiio;         /* interface to SPI port */
    int     label_writable;     /* is sector 0 writable? */
    int     dkindex;            /* disk index for statistics */
    u_int   openpart;           /* all partitions open on this drive */
};

struct disk sddrives[NSD];      /* Table of units */

int sd_timo_cmd;                /* Max timeouts, for sysctl */
int sd_timo_send_op;
int sd_timo_send_csd;
int sd_timo_read;
int sd_timo_wait_cmd;
int sd_timo_wait_wdata;
int sd_timo_wait_wdone;
int sd_timo_wait_wstop;
int sd_timo_wait_widle;

/*
 * Definitions for MMC/SDC commands.
 */
#define CMD_GO_IDLE             0       /* CMD0 */
#define CMD_SEND_OP_MMC         1       /* CMD1 (MMC) */
#define CMD_SEND_IF_COND        8
#define CMD_SEND_CSD            9
#define CMD_SEND_CID            10
#define CMD_STOP                12
#define CMD_SEND_STATUS         13      /* CMD13 */
#define CMD_SET_BLEN            16
#define CMD_READ_SINGLE         17
#define CMD_READ_MULTIPLE       18
#define CMD_SET_BCOUNT          23      /* (MMC) */
#define CMD_SET_WBECNT          23      /* ACMD23 (SDC) */
#define CMD_WRITE_SINGLE        24
#define CMD_WRITE_MULTIPLE      25
#define CMD_SEND_OP_SDC         41      /* ACMD41 (SDC) */
#define CMD_APP                 55      /* CMD55 */
#define CMD_READ_OCR            58

#define DATA_START_BLOCK        0xFE    /* start data for single block */
#define STOP_TRAN_TOKEN         0xFD    /* stop token for write multiple */
#define WRITE_MULTIPLE_TOKEN    0xFC    /* start data for write multiple */

const struct devspec sddevs[] = {
    { 0, "sd0" },
    { 1, "sd0a" }, { 2, "sd0b" }, { 3, "sd0c" }, { 4, "sd0d" },
    { 0, 0 }
};

/*
 * Release the card's /CS signal.
 * Add extra clocks after a deselect.
 */
static void card_release(struct spiio *io)
{
    spi_deselect(io);
    spi_transfer(io, 0xFF);
}

/*
 * Wait while busy, up to 300 msec.
 */
static void card_wait_ready(int unit, int limit, int *maxcount)
{
    int i;
    struct spiio *io = &sddrives[unit].spiio;

    spi_transfer(io, 0xFF);
    for (i=0; i<limit; i++)
    {
        if (spi_transfer(io, 0xFF) == 0xFF)
        {
            if (*maxcount < i)
                *maxcount = i;
            return;
        }
    }
    printf("sd%d: wait_ready(%d) failed\n", unit, limit);
}

/*
 * Send a command and address to SD media.
 * Return response:
 *   FF - timeout
 *   00 - command accepted
 *   01 - command received, card in idle state
 *
 * Other codes:
 *   bit 0 = Idle state
 *   bit 1 = Erase Reset
 *   bit 2 = Illegal command
 *   bit 3 = Communication CRC error
 *   bit 4 = Erase sequence error
 *   bit 5 = Address error
 *   bit 6 = Parameter error
 *   bit 7 = Always 0
 */
static int card_cmd(unsigned int unit, unsigned int cmd, unsigned int addr)
{
    int i, reply;
    struct spiio *io = &sddrives[unit].spiio;

    /* Wait for not busy, up to 300 msec. */
    if (cmd != CMD_GO_IDLE)
        card_wait_ready(unit, TIMO_WAIT_CMD, &sd_timo_wait_cmd);

    /* Send a comand packet (6 bytes). */
    spi_transfer(io, cmd | 0x40);
    spi_transfer(io, addr >> 24);
    spi_transfer(io, addr >> 16);
    spi_transfer(io, addr >> 8);
    spi_transfer(io, addr);

    /* Send cmd checksum for CMD_GO_IDLE.
     * For all other commands, CRC is ignored. */
    if (cmd == CMD_GO_IDLE)
        spi_transfer(io, 0x95);
    else if (cmd == CMD_SEND_IF_COND)
        spi_transfer(io, 0x87);
    else
        spi_transfer(io, 0xFF);

    /* Wait for a response. */
    for (i=0; i<TIMO_CMD; i++)
    {
        reply = spi_transfer(io, 0xFF);
        if (! (reply & 0x80))
        {
            if (sd_timo_cmd < i)
                sd_timo_cmd = i;
            return reply;
        }
    }
    if (cmd != CMD_GO_IDLE)
    {
        printf("sd%d: card_cmd timeout, cmd=%02x, addr=%08x, reply=%02x\n",
            unit, cmd, addr, reply);
    }
    return reply;
}

/*
 * Initialize a card.
 * Return nonzero if successful.
 */
static int card_init(int unit)
{
    int i, reply;
    unsigned char response[4];
    int timeout = 4;
    struct spiio *io = &sddrives[unit].spiio;
    struct disk *du = &sddrives[unit];

    /* Slow speed: 250 kHz */
    spi_brg(io, 250);

    du->card_type = TYPE_UNKNOWN;
    do {
        /* Unselect the card. */
        card_release(io);

        /* Send 80 clock cycles for start up. */
        for (i=0; i<10; i++)
            spi_transfer(io, 0xFF);

        /* Select the card and send a single GO_IDLE command. */
        spi_select(io);
        timeout--;
        reply = card_cmd(unit, CMD_GO_IDLE, 0);

    } while (reply != 1 && timeout != 0);

    card_release(io);
    if (reply != 1)
    {
        /* It must return Idle. */
        return 0;
    }

    /* Check SD version. */
    spi_select(io);
    reply = card_cmd(unit, CMD_SEND_IF_COND, 0x1AA);
    if (reply & 4)
    {
        /* Illegal command: card type 1. */
        card_release(io);
        du->card_type = TYPE_SD_LEGACY;
    } else {
        response[0] = spi_transfer(io, 0xFF);
        response[1] = spi_transfer(io, 0xFF);
        response[2] = spi_transfer(io, 0xFF);
        response[3] = spi_transfer(io, 0xFF);
        card_release(io);
        if (response[3] != 0xAA)
        {
            printf("sd%d: cannot detect card type, response=%02x-%02x-%02x-%02x\n",
                unit, response[0], response[1], response[2], response[3]);
            return 0;
        }
        du->card_type = TYPE_SD_II;
    }

    /* Send repeatedly SEND_OP until Idle terminates. */
    for (i=0; ; i++)
    {
        spi_select(io);
        card_cmd(unit, CMD_APP, 0);
        reply = card_cmd(unit, CMD_SEND_OP_SDC,
                         (du->card_type == TYPE_SD_II) ? 0x40000000 : 0);
        spi_select(io);
        if (reply == 0)
            break;
        if (i >= TIMO_SEND_OP)
        {
            /* Init timed out. */
            printf("card_init: SEND_OP timed out, reply = %d\n", reply);
            return 0;
        }
    }
    if (sd_timo_send_op < i)
        sd_timo_send_op = i;

    /* If SD2 read OCR register to check for SDHC card. */
    if (du->card_type == TYPE_SD_II)
    {
        spi_select(io);
        reply = card_cmd(unit, CMD_READ_OCR, 0);
        if (reply != 0)
        {
            card_release(io);
            printf("sd%d: READ_OCR failed, reply=%02x\n", unit, reply);
            return 0;
        }
        response[0] = spi_transfer(io, 0xFF);
        response[1] = spi_transfer(io, 0xFF);
        response[2] = spi_transfer(io, 0xFF);
        response[3] = spi_transfer(io, 0xFF);
        card_release(io);
        if ((response[0] & 0xC0) == 0xC0)
        {
            du->card_type = TYPE_SDHC;
        }
    }

    /* Fast speed. */
    spi_brg(io, SD0_MHZ * 1000);
    return 1;
}

/*
 * Get disk size in kbytes.
 * Return nonzero if successful.
 */
static int card_size(int unit)
{
    unsigned char csd [16];
    unsigned csize, n;
    int reply, i;
    int nsectors;
    struct spiio *io = &sddrives[unit].spiio;

    spi_select(io);
    reply = card_cmd(unit, CMD_SEND_CSD, 0);
    if (reply != 0) {
        /* Command rejected. */
        card_release(io);
        return 0;
    }

    /* Wait for a response. */
    for (i=0; ; i++) {
        reply = spi_transfer(io, 0xFF);
        if (reply == DATA_START_BLOCK)
            break;
        if (i >= TIMO_SEND_CSD) {
            /* Command timed out. */
            card_release(io);
            printf("sd%d: card_size: SEND_CSD timed out, reply = %d\n",
                unit, reply);
            return 0;
        }
    }
    if (sd_timo_send_csd < i)
        sd_timo_send_csd = i;

    /* Read data. */
    for (i=0; i<sizeof(csd); i++) {
        csd [i] = spi_transfer(io, 0xFF);
    }
    /* Ignore CRC. */
    spi_transfer(io, 0xFF);
    spi_transfer(io, 0xFF);

    /* Disable the card. */
    card_release(io);

    /* CSD register has different structure
     * depending upon protocol version. */
    switch (csd[0] >> 6) {
    case 1:                     /* SDC ver 2.00 */
        csize = csd[9] + (csd[8] << 8) + 1;
        nsectors = csize << 10;
        break;
    case 0:                     /* SDC ver 1.XX or MMC. */
        n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
        csize = (csd[8] >> 6) + (csd[7] << 2) + ((csd[6] & 3) << 10) + 1;
        nsectors = csize << (n - 9);
        break;
    default:                    /* Unknown version. */
        return 0;
    }
    return nsectors >> 1;
}

/*
 * Read a block of data.
 * Return nonzero if successful.
 */
static int card_read(int unit, unsigned int offset, char *data, unsigned int bcount)
{
    int reply, i;
    struct spiio *io = &sddrives[unit].spiio;
    struct disk *du = &sddrives[unit];

    /* Send read-multiple command. */
    spi_select(io);
    if (du->card_type != TYPE_SDHC)
        offset <<= 9;
    reply = card_cmd(unit, CMD_READ_MULTIPLE, offset<<1);
    if (reply != 0)
    {
        /* Command rejected. */
        printf("sd%d: card_read: bad READ_MULTIPLE reply = %d, offset = %08x\n",
            unit, reply, offset<<1);
        card_release(io);
        return 0;
    }

again:
    /* Wait for a response. */
    for (i=0; ; i++)
    {
        int x = spl0();
        reply = spi_transfer(io, 0xFF);
        splx(x);
        if (reply == DATA_START_BLOCK)
            break;
        if (i >= TIMO_READ)
        {
            /* Command timed out. */
            printf("sd%d: card_read: READ_MULTIPLE timed out, reply = %d\n",
                unit, reply);
            card_release(io);
            return 0;
        }
    }
    if (sd_timo_read < i)
        sd_timo_read = i;

    /* Read data. */
    if (bcount >= SECTSIZE)
    {
        spi_bulk_read_32_be(io, SECTSIZE, data);
        data += SECTSIZE;
    } else {
        spi_bulk_read(io, bcount, (unsigned char *)data);
        data += bcount;
        for (i=bcount; i<SECTSIZE; i++)
            spi_transfer(io, 0xFF);
    }
    /* Ignore CRC. */
    spi_transfer(io, 0xFF);
    spi_transfer(io, 0xFF);

    if (bcount > SECTSIZE)
    {
        /* Next sector. */
        bcount -= SECTSIZE;
        goto again;
    }

    /* Stop a read-multiple sequence. */
    card_cmd(unit, CMD_STOP, 0);
    card_release(io);
    return 1;
}

/*
 * Write a block of data.
 * Return nonzero if successful.
 */
static int card_write(int unit, unsigned offset, char *data, unsigned bcount)
{
    unsigned reply, i;
    struct spiio *io = &sddrives[unit].spiio;
    struct disk *du = &sddrives[unit];

    /* Send pre-erase count. */
    spi_select(io);
    card_cmd(unit, CMD_APP, 0);
    reply = card_cmd(unit, CMD_SET_WBECNT, (bcount + SECTSIZE - 1) / SECTSIZE);
    if (reply != 0)
    {
        /* Command rejected. */
        card_release(io);
        printf("sd%d: card_write: bad SET_WBECNT reply = %02x, count = %u\n",
            unit, reply, (bcount + SECTSIZE - 1) / SECTSIZE);
        return 0;
    }

    /* Send write-multiple command. */
    if (du->card_type != TYPE_SDHC)
        offset <<= 9;
    reply = card_cmd(unit, CMD_WRITE_MULTIPLE, offset<<1);
    if (reply != 0)
    {
        /* Command rejected. */
        card_release(io);
        printf("sd%d: card_write: bad WRITE_MULTIPLE reply = %02x\n", unit, reply);
        return 0;
    }
    card_release(io);
again:
    /* Select, wait while busy. */
    spi_select(io);
    card_wait_ready(unit, TIMO_WAIT_WDATA, &sd_timo_wait_wdata);

    /* Send data. */
    spi_transfer(io, WRITE_MULTIPLE_TOKEN);
    if (bcount >= SECTSIZE)
    {
        spi_bulk_write_32_be(io, SECTSIZE, data);
        data += SECTSIZE;
    } else {
        spi_bulk_write(io, bcount, (unsigned char *)data);
        data += bcount;
        for (i=bcount; i<SECTSIZE; i++)
            spi_transfer(io, 0xFF);
    }
    /* Send dummy CRC. */
    spi_transfer(io, 0xFF);
    spi_transfer(io, 0xFF);

    /* Check if data accepted. */
    reply = spi_transfer(io, 0xFF);
    if ((reply & 0x1f) != 0x05)
    {
        /* Data rejected. */
        card_release(io);
        printf("sd%d: card_write: data rejected, reply = %02x\n", unit, reply);
        return 0;
    }

    /* Wait for write completion. */
    int x = spl0();
    card_wait_ready(unit, TIMO_WAIT_WDONE, &sd_timo_wait_wdone);
    splx(x);
    card_release(io);

    if (bcount > SECTSIZE)
    {
        /* Next sector. */
        bcount -= SECTSIZE;
        goto again;
    }

    /* Stop a write-multiple sequence. */
    spi_select(io);
    card_wait_ready(unit, TIMO_WAIT_WSTOP, &sd_timo_wait_wstop);
    spi_transfer(io, STOP_TRAN_TOKEN);
    card_wait_ready(unit, TIMO_WAIT_WIDLE, &sd_timo_wait_widle);
    card_release(io);
    return 1;
}

/*
 * Detect a card.
 */
static int sd_setup(int unit)
{
    struct spiio *io = &sddrives[unit].spiio;
    struct disk *du = &sddrives[unit];
    u_short buf[256];

#ifdef SD0_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 0) {
        LAT_CLR(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        udelay(1000);
    }
#endif

#ifdef SD1_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 1) {
        LAT_CLR(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        udelay(1000);
    }
#endif

    if (! card_init(unit)) {
        printf("sd%d: no SD/MMC card detected\n", unit);
        return 0;
    }
    /* Get the size of raw partition. */
    bzero(du->part, sizeof(du->part));
    du->part[RAWPART].dp_offset = 0;
    du->part[RAWPART].dp_size = card_size(unit);
    if (du->part[RAWPART].dp_size == 0) {
        printf("sd%d: cannot get card size\n", unit);
        return 0;
    }
    printf("sd%d: type %s, size %u kbytes, speed %u Mbit/sec\n", unit,
        (du->card_type == TYPE_SDHC) ? "SDHC" :
        (du->card_type == TYPE_SD_II) ? "II" : "I",
        du->part[RAWPART].dp_size / 2,
        spi_get_brg(io) / 1000);

    /* Read partition table. */
    int s = splbio();
    if (! card_read(unit, 0, (char*)buf, sizeof(buf))) {
        splx(s);
        printf("sd%d: cannot read partition table\n", unit);
        return 0;
    }
    splx(s);
    if (buf[255] == MBR_MAGIC) {
        bcopy(&buf[223], &du->part[1], 64);
#if 1
        int i;
        for (i=1; i<=NPARTITIONS; i++) {
            if (du->part[i].dp_type != 0)
                printf("sd%d%c: partition type %02x, sector %u, size %u kbytes\n",
                    unit, i+'a'-1, du->part[i].dp_type,
                    du->part[i].dp_offset,
                    du->part[i].dp_size / 2);
        }
#endif
    }
    return 1;
}

/*
 * Disable power to the SD card.
 */
static void sd_release(int unit)
{
    struct disk *du = &sddrives[unit];

    /* Forget the partition table. */
    du->part[RAWPART].dp_size = 0;

#ifdef SD0_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 0) {
        /* Enable SD0 phy - pin is assumed to be active low */
        TRIS_CLR(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        LAT_SET(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        udelay(1000);
    }
#endif

#ifdef SD1_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 1) {
        /* Enable SD1 phy - pin is assumed to be active low */
        TRIS_CLR(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        LAT_SET(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        udelay(1000);
    }
#endif
}

int sdopen(dev_t dev, int flags, int mode)
{
    int unit = sdunit(dev);
    int part = sdpart(dev);
    struct disk *du = &sddrives[unit];
    unsigned mask, i;

    if (unit >= NSD || part > NPARTITIONS)
        return ENXIO;

    /*
     * Setup the SD card interface.
     */
    if (du->part[RAWPART].dp_size == 0) {
        if (! sd_setup(unit)) {
            return ENODEV;
        }
    }
    mask = 1 << part;

    /*
     * Warn if a partion is opened
     * that overlaps another partition which is open
     * unless one is the "raw" partition (whole disk).
     */
    if (part != RAWPART && (du->openpart & mask) == 0) {
        unsigned start = du->part[part].dp_offset;
        unsigned end = start + du->part[part].dp_size;

        /* Check for overlapped partitions. */
        for (i=0; i<=NPARTITIONS; i++) {
            struct diskpart *pp = &du->part[i];

            if (i == part || i == RAWPART)
                continue;

            if (pp->dp_offset + pp->dp_size <= start ||
                pp->dp_offset >= end)
                continue;

            if (du->openpart & (1 << i))
                printf("sd%d%c: overlaps open partition (sd%d%c)\n",
                    unit, part + 'a' - 1,
                    unit, pp - du->part + 'a' - 1);
        }
    }
    du->openpart |= mask;
//printf("--- %s: OK\n", __func__);
    return 0;
}

int sdclose(dev_t dev, int mode, int flag)
{
    int unit = sdunit(dev);
    int part = sdpart(dev);
    struct disk *du = &sddrives[unit];

    if (unit >= NSD || part > NPARTITIONS)
        return ENODEV;

    du->openpart &= ~(1 << part);
    if (du->openpart == 0) {
        /* All partitions closed.
         * Release the SD card. */
        sd_release(unit);
    }
    return 0;
}

/*
 * Get disk size in kbytes.
 * Return nonzero if successful.
 */
daddr_t sdsize(dev_t dev)
{
    int unit = sdunit(dev);
    int part = sdpart(dev);
    struct disk *du = &sddrives[unit];

    if (unit >= NSD || part > NPARTITIONS || du->openpart == 0)
        return 0;

    return du->part[part].dp_size;
}

void sdstrategy(struct buf *bp)
{
    int unit = sdunit(bp->b_dev);
    struct disk *du = &sddrives[unit];
    int offset = bp->b_blkno;
    int s;

    /*
     * Determine the size of the transfer, and make sure it is
     * within the boundaries of the partition.
     */
    struct diskpart *p = &du->part[sdpart(bp->b_dev)];
    long nblk = btod(bp->b_bcount);

    offset += p->dp_offset >> 1;
//printf("--- %s: sdpart=%u, offset=%u, psize=%u, nblk=%u\n", __func__, sdpart(bp->b_dev), offset, p->dp_size, nblk);
    if (offset == 0 &&
        ! (bp->b_flags & B_READ) && ! du->label_writable)
    {
        /* Write to partition table not allowed. */
        bp->b_error = EROFS;
bad:        bp->b_flags |= B_ERROR;
        biodone(bp);
//printf("--- %s: error\n", __func__);
        return;
    }
    if (bp->b_blkno + nblk > p->dp_size) {
        /* if exactly at end of disk, return an EOF */
        if (bp->b_blkno == p->dp_size) {
            bp->b_resid = bp->b_bcount;
            biodone(bp);
//printf("--- %s: done EOF\n", __func__);
            return;
        }
        /* or truncate if part of it fits */
        nblk = p->dp_size - bp->b_blkno;
        if (nblk <= 0) {
            bp->b_error = EINVAL;
            goto bad;
        }
        bp->b_bcount = nblk << DEV_BSHIFT;
    }

    if (bp->b_dev == swapdev) {
        led_control(LED_SWAP, 1);
    } else {
        led_control(LED_DISK, 1);
    }

    s = splbio();
#ifdef UCB_METER
    if (du->dkindex >= 0) {
        dk_busy |= 1 << du->dkindex;
        dk_xfer[du->dkindex]++;
        dk_bytes[du->dkindex] += bp->b_bcount;
    }
#endif

    if (bp->b_flags & B_READ) {
        card_read(unit, offset, bp->b_addr, bp->b_bcount);
    } else {
        card_write(unit, offset, bp->b_addr, bp->b_bcount);
    }

    biodone(bp);
    if (bp->b_dev == swapdev) {
        led_control(LED_SWAP, 0);
    } else {
        led_control(LED_DISK, 0);
    }
#ifdef UCB_METER
    if (du->dkindex >= 0)
        dk_busy &= ~(1 << du->dkindex);
#endif
    splx(s);
//printf("--- %s: OK\n", __func__);
}

int sdioctl(dev_t dev, u_int cmd, caddr_t addr, int flag)
{
    int unit = sdunit(dev);
    int part = sdpart(dev);
    struct diskpart *dp;
    int i, error = 0;

    switch (cmd) {

    case DIOCGETMEDIASIZE:
        /* Get disk size in kbytes. */
        dp = &sddrives[unit].part[part];
        *(int*) addr = dp->dp_size;
        break;

    case DIOCREINIT:
        for (i=0; i<=NPARTITIONS; i++)
            bflush(makedev(major(dev), i));
        sd_setup(unit);
        break;

    case DIOCGETPART:
        /* Get partition table entry. */
        dp = &sddrives[unit].part[part];
//printf("--- %s: DIOCGETPART unit = %d, part = %d, type = %u, size = %u\n", __func__, unit, part, dp->dp_type, dp->dp_size);
        *(struct diskpart*) addr = *dp;
        break;

    default:
        error = EINVAL;
        break;
    }
    return error;
}

/*
 * Test to see if device is present.
 * Return true if found and initialized ok.
 */
static int
sd_probe(config)
    struct conf_device *config;
{
    int unit = config->dev_unit;
    int cs = config->dev_pins[0];
    struct spiio *io = &sddrives[unit].spiio;

    if (unit < 0 || unit >= NSD)
        return 0;
    printf("sd%u: port SPI%d, pin cs=R%c%d\n", unit,
        config->dev_ctlr, gpio_portname(cs), gpio_pinno(cs));

    int port = (cs >> 4) - 1;
    int pin = cs & 15;
    struct gpioreg *base = port + (struct gpioreg*) &TRISA;

    if (spi_setup(io, config->dev_ctlr, (unsigned int*) base, pin) != 0) {
        printf("sd%u: cannot open SPI%u port\n", unit, config->dev_ctlr);
        return 0;
    }

    /* Disable power to the SD card. */
    sd_release(unit);

    spi_brg(io, SD0_MHZ * 1000);
    spi_set(io, PIC32_SPICON_CKE);

#ifdef UCB_METER
    dk_alloc(&sddrives[unit].dkindex, 1, sddevs[unit].devname);
#endif
    return 1;
}

struct driver sddriver = {
    "sd", sd_probe,
};

#if 0
//TODO:

sdclose,

#endif