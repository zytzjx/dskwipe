/* A utility program for copying files. Specialised for "files" that
 * represent devices that understand the SCSI command set.
 *
 * Copyright (C) 1999 - 2016 D. Gilbert and P. Allworth
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.

   This program is a specialisation of the Unix "dd" command in which
   either the input or the output file is a scsi generic device, raw
   device, a block device or a normal file. The block size ('bs') is
   assumed to be 512 if not given. This program complains if 'ibs' or
   'obs' are given with a value that differs from 'bs' (or the default 512).
   If 'if' is not given or 'if=-' then stdin is assumed. If 'of' is
   not given or 'of=-' then stdout assumed.

   A non-standard argument "bpt" (blocks per transfer) is added to control
   the maximum number of blocks in each transfer. The default value is 128.
   For example if "bs=512" and "bpt=32" then a maximum of 32 blocks (16 KiB
   in this case) is transferred to or from the sg device in a single SCSI
   command. The actual size of the SCSI READ or WRITE command block can be
   selected with the "cdbsz" argument.

   This version is designed for the linux kernel 2.4, 2.6 and 3 series.
*/

#define _XOPEN_SOURCE 600
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/file.h>
#include <linux/major.h>
#include <linux/fs.h>   /* <sys/mount.h> */
#include <time.h>
#include <stdbool.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_cmds_extra.h"
#include "sg_io_linux.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"
#include "getopt.h"

#include "common.h"

static const char * version_str = "5.87 20201124";

#define ME "dskwipe: "

#define APPNAME			"dskWipe"
#define APPVERSION		"1.0.0.1"
#define APPCOPYRIGHT	"CopyRight(c) 2017-2027."

static char *progname = APPNAME;

/* #define SG_DEBUG */

#define STR_SZ 1024
#define INOUTF_SZ 512
#define EBUFF_SZ 512

#define DEF_BLOCK_SIZE 512
#define DEF_BLOCKS_PER_TRANSFER 128
#define DEF_BLOCKS_PER_2048TRANSFER 32
#define DEF_SCSI_CDBSZ 10
#define MAX_SCSI_CDBSZ 16

#define DEF_MODE_CDB_SZ 10
#define DEF_MODE_RESP_LEN 252
#define RW_ERR_RECOVERY_MP 1
#define CACHING_MP 8
#define CONTROL_MP 0xa

#define SENSE_BUFF_LEN 64       /* Arbitrary, could be larger */
#define READ_CAP_REPLY_LEN 8
#define RCAP16_REPLY_LEN 32
#define READ_LONG_OPCODE 0x3E
#define READ_LONG_CMD_LEN 10
#define READ_LONG_DEF_BLK_INC 8

#define DEF_TIMEOUT 60000       /* 60,000 millisecs == 60 seconds */

#ifndef RAW_MAJOR
#define RAW_MAJOR 255   /*unlikey value */
#endif

#define SG_LIB_FLOCK_ERR 90

#define FT_OTHER 1              /* filetype is probably normal */
#define FT_SG 2                 /* filetype is sg char device or supports
                                   SG_IO ioctl */
#define FT_RAW 4                /* filetype is raw char device */
#define FT_DEV_NULL 8           /* either "/dev/null" or "." as filename */
#define FT_ST 16                /* filetype is st char device (tape) */
#define FT_BLOCK 32             /* filetype is block device */
#define FT_FIFO 64              /* filetype is a fifo (name pipe) */
#define FT_ERROR 128            /* couldn't "stat" file */

#define DEV_NULL_MINOR_NUM 3

/* If platform does not support O_DIRECT then define it harmlessly */
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#define MIN_RESERVED_SIZE 8192

#define MAX_UNIT_ATTENTIONS 10
#define MAX_ABORTED_CMDS 256

static int sum_of_resids = 0;

static int64_t dd_count = -1;
static int64_t req_count = 0;
static int64_t in_full = 0;
static int in_partial = 0;
static int64_t out_full = 0;
static int out_partial = 0;
static int64_t out_sparse = 0;
static int recovered_errs = 0;
static int unrecovered_errs = 0;
static int read_longs = 0;
static int num_retries = 0;

static int do_time = 1;
static int verbose = 0;
static int start_tm_valid = 0;
static struct timeval start_tm;
static struct timeval start_record;
static int blk_sz = 0;
static int max_uas = MAX_UNIT_ATTENTIONS;
static int max_aborted = MAX_ABORTED_CMDS;
static int coe_limit = 0;
static int coe_count = 0;

static unsigned char * zeros_buff = NULL;
static int read_long_blk_inc = READ_LONG_DEF_BLK_INC;

static const char * proc_allow_dio = "/proc/scsi/sg/allow_dio";


//          1         2         3         4         5         6         7         8         9
// 123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890
#define HEADER  "                     This      All      All     This                               Single\n" \
"Pass No. of          Pass   Passes   Passes     Pass              Est.     %s/      %s/\n" \
" No. Passes Byte Complete Complete  Elapsed  Consume    Start   Finish   Second   Second\n" \
"---- ------ ---- -------- -------- -------- -------- -------- -------- -------- --------\n"
// 1234 123456 0xff 100.000% 100.000% 00:00:00 00:00:00 00:00:00 00:00:00 12345.67  //consume

#ifdef DEBUG
#define FORMAT_STRING "%4d %6d %4s %7.3f%% %7.3f%%%9s%9s %8s %8s%9.2f%9.2f\r"
#else
#define FORMAT_STRING "%4d %6d %4s %7.3f%% %7.3f%%%9s%9s %8s %8s%9.2f%9.2f\r"
#endif


#define BYTES_PER_ELEMENT (3)

#define SECTORS_PER_READ (128)

#define RANDOMDATAFLAG		-1
#define CHECKDATAFLAG		-2

typedef enum {
	RANDOM_NONE,
	RANDOM_XORSHIFT,
} RandomMode;
// \todo convert separate wipe arrays to one array

int dod_bytes[] = { 0x00, 0xff, RANDOMDATAFLAG };

int dod_elements = sizeof(dod_bytes) / sizeof(dod_bytes[0]);

// source: BCWipe-1.6-5/bcwipe/wipe.h
int dod7_bytes[] = {0x35, 0xca, 0x97, 0x68, 0xac, 0x53, -1};

int dod7_elements = sizeof(dod7_bytes) / sizeof(dod7_bytes[0]);

int bci_bytes[] = {0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xaa};

int bci_elements = sizeof(bci_bytes) / sizeof(bci_bytes[0]);

int doe_bytes[] = {-1, -1, 0x00};

int doe_elements = sizeof(doe_bytes) / sizeof(doe_bytes[0]);

int schneier_bytes[] = {0xff, 0x00, -1, -1, -1, -1, -1};

int schneier_elements = sizeof(schneier_bytes) / sizeof(schneier_bytes[0]);

int wipe_bytes[1024] = { 0 };

int wipe_elements = 0;

// source: http://www.cs.auckland.ac.nz/~pgut001/pubs/secure_del.html
int gutmann_bytes[][BYTES_PER_ELEMENT] = {
	{-1,   -1,   -1}, // 1
	{-1,   -1,   -1}, // 2
	{-1,   -1,   -1}, // 3
	{-1,   -1,   -1}, // 4
	{0x55, 0x55, 0x55}, // 5
	{0xAA, 0xAA, 0xAA}, // 6
	{0x92, 0x49, 0x24}, // 7
	{0x49, 0x24, 0x92}, // 8
	{0x24, 0x92, 0x49}, // 9
	{0x00, 0x00, 0x00}, // 10
	{0x11, 0x11, 0x11}, // 11
	{0x22, 0x22, 0x22}, // 12
	{0x33, 0x33, 0x33}, // 13
	{0x44, 0x44, 0x44}, // 14
	{0x55, 0x55, 0x55}, // 15
	{0x66, 0x66, 0x66}, // 16
	{0x77, 0x77, 0x77}, // 17
	{0x88, 0x88, 0x88}, // 18
	{0x99, 0x99, 0x99}, // 19
	{0xAA, 0xAA, 0xAA}, // 20
	{0xBB, 0xBB, 0xBB}, // 21
	{0xCC, 0xCC, 0xCC}, // 22
	{0xDD, 0xDD, 0xDD}, // 23
	{0xEE, 0xEE, 0xEE}, // 24
	{0xFF, 0xFF, 0xFF}, // 25
	{0x92, 0x49, 0x24}, // 26
	{0x49, 0x24, 0x92}, // 27
	{0x24, 0x92, 0x49}, // 28
	{0x6D, 0xB6, 0xDB}, // 29
	{0xB6, 0xDB, 0x6D}, // 30
	{0xDB, 0x6D, 0xB6}, // 31
	{-1,   -1,   -1}, // 32
	{-1,   -1,   -1}, // 33
	{-1,   -1,   -1}, // 34
	{-1,   -1,   -1}  // 35
};

int gutmann_elements = sizeof(gutmann_bytes) / sizeof(gutmann_bytes[0]);

static char *short_options = "de:Egkn:p:rs:Sw:vV:yz:D?";


static struct option long_options[] = {
  {"bci",		no_argument,		0, 'b'},
  {"bruce",		no_argument,		0, 'S'},
  {"dod",		no_argument,		0, 'd'},
  {"dod3",		no_argument,		0, 'd'},
  {"dod7",		no_argument,		0, 'D'},
  {"doe",		no_argument,		0, 'E'},
  {"end",		required_argument,	0, 'e'},
  //{"exit",		required_argument,	0, 'x'},
  {"verbose",		required_argument,	0, 'V'},
  {"gutmann",	no_argument,		0, 'g'},
  {"help",		no_argument,		0, '?'},
//  {"ignore",	no_argument,		0, 'i'},
//  {"ignore-errors",	no_argument,		0, 'i'},
  {"kilo",		no_argument,		0, 'k'},
  {"kilobyte",	no_argument,		0, 'k'},
  {"read",		no_argument,		0, 'r'},
  {"refresh",	required_argument,	0, 'z'},
  {"schneier",	no_argument,		0, 'S'},
  {"sectors",	required_argument,	0, 'n'},
  {"start",		required_argument,	0, 's'},
  {"version",	no_argument,		0, 'v'},
//  {"vsitr",		no_argument,		0, 'b'},
  {"yes",		no_argument,		0, 'y'},
  {"llformat",	required_argument,  0, 'w'},

  {"customwipe",required_argument,	0, 'p'}, // gdisk compatible
  {"dodwipe",	no_argument,		0, 'd'}, // gdisk compatible
  {"sure",		no_argument,		0, 'y'}, // gdisk compatible
  {NULL,		0,					0, 0}
};

void version() {
	printf(APPNAME " " APPVERSION " - " __DATE__ "\n");
	printf(APPCOPYRIGHT "\n");
}

void _usage() {
	fprintf(stderr, "Usage: %s [options] device(s) [byte(s)]\n"
		" bytes can be one or more numbers between 0 to 255, use 0xNN for hexidecimal,\n"
		"  0NNN for octal, r for random bytes, default is 0\n"
		"\nOptions:\n"
		" -p | --passes n  Wipe device n times (default is 1)\n"
		" -d | --dod       Wipe device using US DoD 5220.22-M method (3 passes)\n"
		" -E | --doe       Wipe device using US DoE method (3 passes)\n"
		" -D | --dod7      Wipe device using US DoD 5200.28-STD method (7 passes)\n"
		" -S | --schneier  Wipe device using Bruce Schneier's method (7 passes)\n"
		" -b | --bci       Wipe device using German BCI/VSITR method (7 passes)\n"
		" -g | --gutmann   Wipe device using Peter Gutmann's method (35 passes)\n"
		" -k | --kilobyte  Use 1024 for kilobyte (default is 1000)\n"
		" -y | --yes       Start processing without waiting for confirmation\n"
      //          1         2         3         4         5         6         7
      // 12345678901234567890123456789012345678901234567890123456789012345678901234567890

		//" -q | --quiet     Display less information (-qq = quieter, etc.)\n"
		" -z | --refresh n Refresh display every n seconds (default is 1)\n"
		" -n | --sectors n Write n sectors at once (1-65535, default is %d)\n"
		" -s | --start   n Start at relative sector n (default is 0)\n"
		" -e | --end     n End at relative sector n (default is last sector)\n"
		" -r | --read      Only read the data on the device (DOES NOT WIPE!)\n"
		//" -i | --ignore    Ignore certain read/write errors\n"
		" -v | --version   Show version and copyright information and quit\n"
		" -? | --help      Show this help message and quit (-?? = more help, etc.)\n",
		progname, SECTORS_PER_READ);
}

void examples() {
	fprintf(stderr,
		"\nExamples:\n"
		" %s /dev/sg1                  & erase disk once using the byte 0\n"
		" %s /dev/sg1 1                & erase disk once using the byte 1\n"
		" %s /dev/sg1 0 255            & erase disk twice using bytes 0 then 255\n"
		" %s --dod /dev/sg1            & erase disk using DoD 5220.22-M method\n"
		" %s /dev/sg1 0 0xff r         & same as --dod (bytes 0, 255, weak random)\n"
		" %s -p 2 /dev/sg1 0 1         & erase disk 4 times using bytes 0/1/0/1\n"
		" %s -p 2 --dod /dev/sg1       & erase disk twice using DoD method\n"
		" %s /dev/sg1 r r              & erase disk twice using weak RNG\n",
		progname,
		progname,
		progname,
		progname,
		progname,
		progname,
		progname,
		progname);
}

void usage(int exit_code) {
	_usage();
	exit(exit_code);
}

struct flags_t {
    int append;
    int cdbsz;
    int coe;
    int dio;
    int direct;
    int dpo;
    int dsync;
    int excl;
    int fua;
    int flock;
    int nocache;
    int sgio;
    int pdt;
    int sparse;
    int retries;
};

static struct flags_t iflag;
static struct flags_t oflag;

static void calc_duration_throughput(int contin);


static void
install_handler(int sig_num, void (*sig_handler) (int sig))
{
    struct sigaction sigact;
    sigaction (sig_num, NULL, &sigact);
    if (sigact.sa_handler != SIG_IGN)
    {
        sigact.sa_handler = sig_handler;
        sigemptyset (&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction (sig_num, &sigact, NULL);
    }
}


static void
print_stats_sg(const char * str)
{
    if (0 != dd_count)
        pr2serr("  remaining block count=%" PRId64 "\n", dd_count);
    pr2serr("%s%" PRId64 "+%d records in\n", str, in_full - in_partial,
            in_partial);
    pr2serr("%s%" PRId64 "+%d records out\n", str, out_full - out_partial,
            out_partial);
    if (oflag.sparse)
        pr2serr("%s%" PRId64 " bypassed records out\n", str, out_sparse);
    if (recovered_errs > 0)
        pr2serr("%s%d recovered errors\n", str, recovered_errs);
    if (num_retries > 0)
        pr2serr("%s%d retries attempted\n", str, num_retries);
    if (iflag.coe || oflag.coe) {
        pr2serr("%s%d unrecovered errors\n", str, unrecovered_errs);
        pr2serr("%s%d read_longs fetched part of unrecovered read errors\n",
                str, read_longs);
    } else if (unrecovered_errs)
        pr2serr("%s%d unrecovered error(s)\n", str, unrecovered_errs);
}


static void
interrupt_handler(int sig)
{
    struct sigaction sigact;

    sigact.sa_handler = SIG_DFL;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(sig, &sigact, NULL);
    pr2serr("Interrupted by signal,");
    if (do_time)
        calc_duration_throughput(0);
    print_stats_sg("");
    kill(getpid (), sig);
}


static void
siginfo_handler(int sig)
{
    if (sig) { ; }      /* unused, dummy to suppress warning */
    pr2serr("Progress report, continuing ...\n");
    if (do_time)
        calc_duration_throughput(1);
    print_stats_sg("  ");
}

static int bsg_major_checked = 0;
static int bsg_major = 0;

static void
find_bsg_major(void)
{
    const char * proc_devices = "/proc/devices";
    FILE *fp;
    char a[128];
    char b[128];
    char * cp;
    int n;

    if (NULL == (fp = fopen(proc_devices, "r"))) {
        if (verbose)
            pr2serr("fopen %s failed: %s\n", proc_devices, strerror(errno));
        return;
    }
    while ((cp = fgets(b, sizeof(b), fp))) {
        if ((1 == sscanf(b, "%126s", a)) &&
            (0 == memcmp(a, "Character", 9)))
            break;
    }
    while (cp && (cp = fgets(b, sizeof(b), fp))) {
        if (2 == sscanf(b, "%d %126s", &n, a)) {
            if (0 == strcmp("bsg", a)) {
                bsg_major = n;
                break;
            }
        } else
            break;
    }
    if (verbose > 5) {
        if (cp)
            pr2serr("found bsg_major=%d\n", bsg_major);
        else
            pr2serr("found no bsg char device in %s\n", proc_devices);
    }
    fclose(fp);
}


static int
dd_filetype(const char * filename)
{
    struct stat st;
    size_t len = strlen(filename);

    if ((1 == len) && ('.' == filename[0]))
        return FT_DEV_NULL;
    if (stat(filename, &st) < 0)
        return FT_ERROR;
    if (S_ISCHR(st.st_mode)) {
        /* major() and minor() defined in sys/sysmacros.h */
        if ((MEM_MAJOR == major(st.st_rdev)) &&
            (DEV_NULL_MINOR_NUM == minor(st.st_rdev)))
            return FT_DEV_NULL;
        if (RAW_MAJOR == major(st.st_rdev))
            return FT_RAW;
        if (SCSI_GENERIC_MAJOR == major(st.st_rdev))
            return FT_SG;
        if (SCSI_TAPE_MAJOR == major(st.st_rdev))
            return FT_ST;
        if (! bsg_major_checked) {
            bsg_major_checked = 1;
            find_bsg_major();
        }
        if (bsg_major == (int)major(st.st_rdev))
            return FT_SG;
    } else if (S_ISBLK(st.st_mode))
        return FT_BLOCK;
    else if (S_ISFIFO(st.st_mode))
        return FT_FIFO;
    return FT_OTHER;
}


static char *
dd_filetype_str(int ft, char * buff)
{
    int off = 0;

    if (FT_DEV_NULL & ft)
        off += snprintf(buff + off, 32, "null device ");
    if (FT_SG & ft)
        off += snprintf(buff + off, 32, "SCSI generic (sg) device ");
    if (FT_BLOCK & ft)
        off += snprintf(buff + off, 32, "block device ");
    if (FT_FIFO & ft)
        off += snprintf(buff + off, 32, "fifo (named pipe) ");
    if (FT_ST & ft)
        off += snprintf(buff + off, 32, "SCSI tape device ");
    if (FT_RAW & ft)
        off += snprintf(buff + off, 32, "raw device ");
    if (FT_OTHER & ft)
        off += snprintf(buff + off, 32, "other (perhaps ordinary file) ");
    if (FT_ERROR & ft)
        off += snprintf(buff + off, 32, "unable to 'stat' file ");
    return buff;
}


static void
usage__()
{
    pr2serr("Usage: sg_dd  [bs=BS] [count=COUNT] [ibs=BS] [if=IFILE] "
            "[iflag=FLAGS]\n"
            "              [obs=BS] [of=OFILE] [oflag=FLAGS] "
            "[seek=SEEK] [skip=SKIP]\n"
            "              [--help] [--version]\n\n"
            "              [blk_sgio=0|1] [bpt=BPT] [cdbsz=6|10|12|16] "
            "[coe=0|1|2|3]\n"
            "              [coe_limit=CL] [dio=0|1] [odir=0|1] "
            "[of2=OFILE2] [retries=RETR]\n"
            "              [sync=0|1] [time=0|1] [verbose=VERB]\n"
            "  where:\n"
            "    blk_sgio    0->block device use normal I/O(def), 1->use "
            "SG_IO\n"
            "    bpt         is blocks_per_transfer (default is 128 or 32 "
            "when BS>=2048)\n"
            "    bs          block size (default is 512)\n");
    pr2serr("    cdbsz       size of SCSI READ or WRITE cdb (default is "
            "10)\n"
            "    coe         0->exit on error (def), 1->continue on sg "
            "error (zero\n"
            "                fill), 2->also try read_long on unrecovered "
            "reads,\n"
            "                3->and set the CORRCT bit on the read long\n"
            "    coe_limit   limit consecutive 'bad' blocks on reads to CL "
            "times\n"
            "                when COE>1 (default: 0 which is no limit)\n"
            "    count       number of blocks to copy (def: device size)\n"
            "    dio         for direct IO, 1->attempt, 0->indirect IO (def)\n"
            "    ibs         input block size (if given must be same as "
            "'bs=')\n"
            "    if          file or device to read from (def: stdin)\n"
            "    iflag       comma separated list from: [coe,dio,direct,"
            "dpo,dsync,excl,\n"
            "                flock,fua,nocache,null,sgio]\n"
            "    obs         output block size (if given must be same as "
            "'bs=')\n"
            "    odir        1->use O_DIRECT when opening block dev, "
            "0->don't(def)\n"
            "    of          file or device to write to (def: stdout), "
            "OFILE of '.'\n");
    pr2serr("                treated as /dev/null\n"
            "    of2         additional output file (def: /dev/null), "
            "OFILE2 should be\n"
            "                normal file or pipe\n"
            "    oflag       comma separated list from: [append,coe,dio,"
            "direct,dpo,\n"
            "                dsync,excl,flock,fua,nocache,null,sgio,"
            "sparse]\n"
            "    retries     retry sgio errors RETR times (def: 0)\n"
            "    seek        block position to start writing to OFILE\n"
            "    skip        block position to start reading from IFILE\n"
            "    sync        0->no sync(def), 1->SYNCHRONIZE CACHE on "
            "OFILE after copy\n"
            "    time        0->no timing(def), 1->time plus calculate "
            "throughput\n"
            "    verbose     0->quiet(def), 1->some noise, 2->more noise, "
            "etc\n"
            "    --help      print out this usage message then exit\n"
            "    --version   print version information then exit\n\n"
            "copy from IFILE to OFILE, similar to dd command; "
            "specialized for SCSI devices\n");
}


/* Return of 0 -> success, see sg_ll_read_capacity*() otherwise */
static int
scsi_read_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
{
    int res;
    unsigned int ui;
    unsigned char rcBuff[RCAP16_REPLY_LEN];
    int verb;

    verb = (verbose ? verbose - 1: 0);
    res = sg_ll_readcap_10(sg_fd, 0, 0, rcBuff, READ_CAP_REPLY_LEN, 1, verb);
    if (0 != res)
        return res;

    if ((0xff == rcBuff[0]) && (0xff == rcBuff[1]) && (0xff == rcBuff[2]) &&
        (0xff == rcBuff[3])) {
        int64_t ls;

        res = sg_ll_readcap_16(sg_fd, 0, 0, rcBuff, RCAP16_REPLY_LEN, 1,
                               verb);
        if (0 != res)
            return res;
        ls = (int64_t)sg_get_unaligned_be64(rcBuff);
        *num_sect = ls + 1;
        *sect_sz = (int)sg_get_unaligned_be32(rcBuff + 8);
    } else {
        ui = sg_get_unaligned_be32(rcBuff);
        /* take care not to sign extend values > 0x7fffffff */
        *num_sect = (int64_t)ui + 1;
        *sect_sz = (int)sg_get_unaligned_be32(rcBuff + 4);
    }
    if (verbose)
        pr2serr("      number of blocks=%" PRId64 " [0x%" PRIx64 "], "
                "block size=%d\n", *num_sect, *num_sect, *sect_sz);
    return 0;
}


/* Return of 0 -> success, -1 -> failure. BLKGETSIZE64, BLKGETSIZE and */
/* BLKSSZGET macros problematic (from <linux/fs.h> or <sys/mount.h>). */
//static int
//read_blkdev_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
//{
//#ifdef BLKSSZGET
//    if ((ioctl(sg_fd, BLKSSZGET, sect_sz) < 0) && (*sect_sz > 0)) {
//        perror("BLKSSZGET ioctl error");
//        return -1;
//    } else {
// #ifdef BLKGETSIZE64
//        uint64_t ull;
//
//        if (ioctl(sg_fd, BLKGETSIZE64, &ull) < 0) {
//
//            perror("BLKGETSIZE64 ioctl error");
//            return -1;
//        }
//        *num_sect = ((int64_t)ull / (int64_t)*sect_sz);
//        if (verbose)
//            pr2serr("      [bgs64] number of blocks=%" PRId64 " [0x%" PRIx64
//                    "], block size=%d\n", *num_sect, *num_sect, *sect_sz);
// #else
//        unsigned long ul;
//
//        if (ioctl(sg_fd, BLKGETSIZE, &ul) < 0) {
//            perror("BLKGETSIZE ioctl error");
//            return -1;
//        }
//        *num_sect = (int64_t)ul;
//        if (verbose)
//            pr2serr("      [bgs] number of blocks=%" PRId64 " [0x%" PRIx64
//                    "],  block size=%d\n", *num_sect, *num_sect, *sect_sz);
// #endif
//    }
//    return 0;
//#else
//    if (verbose)
//        pr2serr("      BLKSSZGET+BLKGETSIZE ioctl not available\n");
//    *num_sect = 0;
//    *sect_sz = 0;
//    return -1;
//#endif
//}


static int
sg_build_scsi_cdb(unsigned char * cdbp, int cdb_sz, unsigned int blocks,
                  int64_t start_block, int write_true, int fua, int dpo)
{
    int rd_opcode[] = {0x8, 0x28, 0xa8, 0x88};
    int wr_opcode[] = {0xa, 0x2a, 0xaa, 0x8a};
    int sz_ind;

    memset(cdbp, 0, cdb_sz);
    if (dpo)
        cdbp[1] |= 0x10;
    if (fua)
        cdbp[1] |= 0x8;
    switch (cdb_sz) {
    case 6:
        sz_ind = 0;
        cdbp[0] = (unsigned char)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be24(0x1fffff & start_block, cdbp + 1);
        cdbp[4] = (256 == blocks) ? 0 : (unsigned char)blocks;
        if (blocks > 256) {
            pr2serr(ME "for 6 byte commands, maximum number of blocks is "
                    "256\n");
            return 1;
        }
        if ((start_block + blocks - 1) & (~0x1fffff)) {
            pr2serr(ME "for 6 byte commands, can't address blocks beyond "
                    "%d\n", 0x1fffff);
            return 1;
        }
        if (dpo || fua) {
            pr2serr(ME "for 6 byte commands, neither dpo nor fua bits "
                    "supported\n");
            return 1;
        }
        break;
    case 10:
        sz_ind = 1;
        cdbp[0] = (unsigned char)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be32(start_block, cdbp + 2);
        sg_put_unaligned_be16(blocks, cdbp + 7);
        if (blocks & (~0xffff)) {
            pr2serr(ME "for 10 byte commands, maximum number of blocks is "
                    "%d\n", 0xffff);
            return 1;
        }
        break;
    case 12:
        sz_ind = 2;
        cdbp[0] = (unsigned char)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be32(start_block, cdbp + 2);
        sg_put_unaligned_be32(blocks, cdbp + 6);
        break;
    case 16:
        sz_ind = 3;
        cdbp[0] = (unsigned char)(write_true ? wr_opcode[sz_ind] :
                                               rd_opcode[sz_ind]);
        sg_put_unaligned_be64(start_block, cdbp + 2);
        sg_put_unaligned_be32(blocks, cdbp + 10);
        break;
    default:
        pr2serr(ME "expected cdb size of 6, 10, 12, or 16 but got %d\n",
                cdb_sz);
        return 1;
    }
    return 0;
}


/* 0 -> successful, SG_LIB_SYNTAX_ERROR -> unable to build cdb,
   SG_LIB_CAT_UNIT_ATTENTION -> try again,
   SG_LIB_CAT_MEDIUM_HARD_WITH_INFO -> 'io_addrp' written to,
   SG_LIB_CAT_MEDIUM_HARD -> no info field,
   SG_LIB_CAT_NOT_READY, SG_LIB_CAT_ABORTED_COMMAND,
   -2 -> ENOMEM
   -1 other errors */
static int
sg_read_low(int sg_fd, unsigned char * buff, int blocks, int64_t from_block,
            int bs, const struct flags_t * ifp, int * diop,
            uint64_t * io_addrp)
{
    unsigned char rdCmd[MAX_SCSI_CDBSZ];
    unsigned char senseBuff[SENSE_BUFF_LEN];
    const unsigned char * sbp;
    struct sg_io_hdr io_hdr;
    int res, k, info_valid, slen;

    if (sg_build_scsi_cdb(rdCmd, ifp->cdbsz, blocks, from_block, 0,
                          ifp->fua, ifp->dpo)) {
        pr2serr(ME "bad rd cdb build, from_block=%" PRId64 ", blocks=%d\n",
                from_block, blocks);
        return SG_LIB_SYNTAX_ERROR;
    }

    memset(&io_hdr, 0, sizeof(struct sg_io_hdr));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = ifp->cdbsz;
    io_hdr.cmdp = rdCmd;
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = bs * blocks;
    io_hdr.dxferp = buff;
    io_hdr.mx_sb_len = SENSE_BUFF_LEN;
    io_hdr.sbp = senseBuff;
    io_hdr.timeout = DEF_TIMEOUT;
    io_hdr.pack_id = (int)from_block;
    if (diop && *diop)
        io_hdr.flags |= SG_FLAG_DIRECT_IO;

    if (verbose > 2) {
        pr2serr("    read cdb: ");
        for (k = 0; k < ifp->cdbsz; ++k)
            pr2serr("%02x ", rdCmd[k]);
        pr2serr("\n");
    }
    while (((res = ioctl(sg_fd, SG_IO, &io_hdr)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        ;
    if (res < 0) {
        if (ENOMEM == errno)
            return -2;
        perror("reading (SG_IO) on sg device, error");
        return -1;
    }
    if (verbose > 2)
        pr2serr("      duration=%u ms\n", io_hdr.duration);
    res = sg_err_category3(&io_hdr);
    sbp = io_hdr.sbp;
    slen = io_hdr.sb_len_wr;
    switch (res) {
    case SG_LIB_CAT_CLEAN:
        break;
    case SG_LIB_CAT_RECOVERED:
        ++recovered_errs;
        info_valid = sg_get_sense_info_fld(sbp, slen, io_addrp);
        if (info_valid) {
            pr2serr("    lba of last recovered error in this READ=0x%" PRIx64
                    "\n", *io_addrp);
            if (verbose > 1)
                sg_chk_n_print3("reading", &io_hdr, 1);
        } else {
            pr2serr("Recovered error: [no info] reading from block=0x%" PRIx64
                    ", num=%d\n", from_block, blocks);
            sg_chk_n_print3("reading", &io_hdr, verbose > 1);
        }
        break;
    case SG_LIB_CAT_ABORTED_COMMAND:
    case SG_LIB_CAT_UNIT_ATTENTION:
        sg_chk_n_print3("reading", &io_hdr, verbose > 1);
        return res;
    case SG_LIB_CAT_MEDIUM_HARD:
        if (verbose > 1)
            sg_chk_n_print3("reading", &io_hdr, verbose > 1);
        ++unrecovered_errs;
        info_valid = sg_get_sense_info_fld(sbp, slen, io_addrp);
        /* MMC devices don't necessarily set VALID bit */
        if ((info_valid) || ((5 == ifp->pdt) && (*io_addrp > 0)))
            return SG_LIB_CAT_MEDIUM_HARD_WITH_INFO;
        else {
            pr2serr("Medium, hardware or blank check error but no lba of "
                    "failure in sense\n");
            return res;
        }
        break;
    case SG_LIB_CAT_NOT_READY:
        ++unrecovered_errs;
        if (verbose > 0)
            sg_chk_n_print3("reading", &io_hdr, verbose > 1);
        return res;
    case SG_LIB_CAT_ILLEGAL_REQ:
        if (5 == ifp->pdt) {    /* MMC READs can go down this path */
            struct sg_scsi_sense_hdr ssh;
            bool ili;

            if (verbose > 1)
                sg_chk_n_print3("reading", &io_hdr, verbose > 1);
            if (sg_scsi_normalize_sense(sbp, slen, &ssh) &&
                (0x64 == ssh.asc) && (0x0 == ssh.ascq)) {
                if (sg_get_sense_filemark_eom_ili(sbp, slen, NULL, NULL,
                                                  &ili) && ili) {
                    info_valid = sg_get_sense_info_fld(sbp, slen, io_addrp);
                    if (*io_addrp > 0) {
                        ++unrecovered_errs;
                        return SG_LIB_CAT_MEDIUM_HARD_WITH_INFO;
                    } else
                        pr2serr("MMC READ gave 'illegal mode for this track' "
                                "and ILI but no LBA of failure\n");
                }
                ++unrecovered_errs;
                return SG_LIB_CAT_MEDIUM_HARD;
            }
        }
        /* drop through */
        //no break
    default:
        ++unrecovered_errs;
        if (verbose > 0)
            sg_chk_n_print3("reading", &io_hdr, verbose > 1);
        return res;
    }
    if (diop && *diop &&
        ((io_hdr.info & SG_INFO_DIRECT_IO_MASK) != SG_INFO_DIRECT_IO))
        *diop = 0;      /* flag that dio not done (completely) */
    sum_of_resids += io_hdr.resid;
    return 0;
}


/* 0 -> successful, SG_LIB_SYNTAX_ERROR -> unable to build cdb,
   SG_LIB_CAT_UNIT_ATTENTION -> try again, SG_LIB_CAT_NOT_READY,
   SG_LIB_CAT_MEDIUM_HARD, SG_LIB_CAT_ABORTED_COMMAND,
   -2 -> ENOMEM, -1 other errors */
static int
sg_read(int sg_fd, unsigned char * buff, int blocks, int64_t from_block,
        int bs, struct flags_t * ifp, int * diop, int * blks_readp)
{
    uint64_t io_addr;
    int64_t lba;
    int res, blks, repeat, xferred;
    unsigned char * bp;
    int retries_tmp;
    int ret = 0;
    int may_coe = 0;

    retries_tmp = ifp->retries;
    for (xferred = 0, blks = blocks, lba = from_block, bp = buff;
         blks > 0; blks = blocks - xferred) {
        io_addr = 0;
        repeat = 0;
        may_coe = 0;
        res = sg_read_low(sg_fd, bp, blks, lba, bs, ifp, diop, &io_addr);
        switch (res) {
        case 0:
            if (blks_readp)
                *blks_readp = xferred + blks;
            if (coe_limit > 0)
                coe_count = 0;  /* good read clears coe_count */
            return 0;
        case -2:        /* ENOMEM */
            return res;
        case SG_LIB_CAT_NOT_READY:
            pr2serr("Device (r) not ready\n");
            return res;
        case SG_LIB_CAT_ABORTED_COMMAND:
            if (--max_aborted > 0) {
                pr2serr("Aborted command, continuing (r)\n");
                repeat = 1;
            } else {
                pr2serr("Aborted command, too many (r)\n");
                return res;
            }
            break;
        case SG_LIB_CAT_UNIT_ATTENTION:
            if (--max_uas > 0) {
                pr2serr("Unit attention, continuing (r)\n");
                repeat = 1;
            } else {
                pr2serr("Unit attention, too many (r)\n");
                return res;
            }
            break;
        case SG_LIB_CAT_MEDIUM_HARD_WITH_INFO:
            if (retries_tmp > 0) {
                pr2serr(">>> retrying a sgio read, lba=0x%" PRIx64 "\n",
                        (uint64_t)lba);
                --retries_tmp;
                ++num_retries;
                if (unrecovered_errs > 0)
                    --unrecovered_errs;
                repeat = 1;
            }
            ret = SG_LIB_CAT_MEDIUM_HARD;
            break; /* unrecovered read error at lba=io_addr */
        case SG_LIB_SYNTAX_ERROR:
            ifp->coe = 0;
            ret = res;
            goto err_out;
        case -1:
            ret = res;
            goto err_out;
        case SG_LIB_CAT_MEDIUM_HARD:
            may_coe = 1;
            //no break
        default:
            if (retries_tmp > 0) {
                pr2serr(">>> retrying a sgio read, lba=0x%" PRIx64 "\n",
                        (uint64_t)lba);
                --retries_tmp;
                ++num_retries;
                if (unrecovered_errs > 0)
                    --unrecovered_errs;
                repeat = 1;
                break;
            }
            ret = res;
            goto err_out;
        }
        if (repeat)
            continue;
        if ((io_addr < (uint64_t)lba) ||
            (io_addr >= (uint64_t)(lba + blks))) {
                pr2serr("  Unrecovered error lba 0x%" PRIx64 " not in "
                        "correct range:\n\t[0x%" PRIx64 ",0x%" PRIx64 "]\n",
                        io_addr, (uint64_t)lba,
                        (uint64_t)(lba + blks - 1));
            may_coe = 1;
            goto err_out;
        }
        blks = (int)(io_addr - (uint64_t)lba);
        if (blks > 0) {
            if (verbose)
                pr2serr("  partial read of %d blocks prior to medium error\n",
                        blks);
            res = sg_read_low(sg_fd, bp, blks, lba, bs, ifp, diop, &io_addr);
            switch (res) {
            case 0:
                break;
            case -1:
                ifp->coe = 0;
                ret = res;
                goto err_out;
            case -2:
                pr2serr("ENOMEM again, unexpected (r)\n");
                return -1;
            case SG_LIB_CAT_NOT_READY:
                pr2serr("device (r) not ready\n");
                return res;
            case SG_LIB_CAT_UNIT_ATTENTION:
                pr2serr("Unit attention, unexpected (r)\n");
                return res;
            case SG_LIB_CAT_ABORTED_COMMAND:
                pr2serr("Aborted command, unexpected (r)\n");
                return res;
            case SG_LIB_CAT_MEDIUM_HARD_WITH_INFO:
            case SG_LIB_CAT_MEDIUM_HARD:
                ret = SG_LIB_CAT_MEDIUM_HARD;
                goto err_out;
            case SG_LIB_SYNTAX_ERROR:
            default:
                pr2serr(">> unexpected result=%d from sg_read_low() 2\n",
                        res);
                ret = res;
                goto err_out;
            }
        }
        xferred += blks;
        if (0 == ifp->coe) {
            /* give up at block before problem unless 'coe' */
            if (blks_readp)
                *blks_readp = xferred;
            return ret;
        }
        if (bs < 32) {
            pr2serr(">> bs=%d too small for read_long\n", bs);
            return -1;  /* nah, block size can't be that small */
        }
        bp += (blks * bs);
        lba += blks;
        if ((0 != ifp->pdt) || (ifp->coe < 2)) {
            pr2serr(">> unrecovered read error at blk=%" PRId64 ", pdt=%d, "
                    "use zeros\n", lba, ifp->pdt);
            memset(bp, 0, bs);
        } else if (io_addr < UINT_MAX) {
            unsigned char * buffp;
            int offset, nl, r, ok, corrct;

            buffp = (unsigned char*)malloc(bs * 2);
            if (NULL == buffp) {
                pr2serr(">> heap problems\n");
                return -1;
            }
            corrct = (ifp->coe > 2) ? 1 : 0;
            res = sg_ll_read_long10(sg_fd, /* pblock */0, corrct, lba, buffp,
                                    bs + read_long_blk_inc, &offset, 1,
                                    verbose);
            ok = 0;
            switch (res) {
            case 0:
                ok = 1;
                ++read_longs;
                break;
            case SG_LIB_CAT_ILLEGAL_REQ_WITH_INFO:
                nl = bs + read_long_blk_inc - offset;
                if ((nl < 32) || (nl > (bs * 2))) {
                    pr2serr(">> read_long(10) len=%d unexpected\n", nl);
                    break;
                }
                /* remember for next read_long attempt, if required */
                read_long_blk_inc = nl - bs;

                if (verbose)
                    pr2serr("read_long(10): adjusted len=%d\n", nl);
                r = sg_ll_read_long10(sg_fd, 0, corrct, lba, buffp, nl,
                                      &offset, 1, verbose);
                if (0 == r) {
                    ok = 1;
                    ++read_longs;
                    break;
                } else
                    pr2serr(">> unexpected result=%d on second "
                            "read_long(10)\n", r);
                break;
            case SG_LIB_CAT_INVALID_OP:
                pr2serr(">> read_long(10); not supported\n");
                break;
            case SG_LIB_CAT_ILLEGAL_REQ:
                pr2serr(">> read_long(10): bad cdb field\n");
                break;
            case SG_LIB_CAT_NOT_READY:
                pr2serr(">> read_long(10): device not ready\n");
                break;
            case SG_LIB_CAT_UNIT_ATTENTION:
                pr2serr(">> read_long(10): unit attention\n");
                break;
            case SG_LIB_CAT_ABORTED_COMMAND:
                pr2serr(">> read_long(10): aborted command\n");
                break;
            default:
                pr2serr(">> read_long(10): problem (%d)\n", res);
                break;
            }
            if (ok)
                memcpy(bp, buffp, bs);
            else
                memset(bp, 0, bs);
            free(buffp);
        } else {
            pr2serr(">> read_long(10) cannot handle blk=%" PRId64 ", use "
                    "zeros\n", lba);
            memset(bp, 0, bs);
        }
        ++xferred;
        bp += bs;
        ++lba;
        if ((coe_limit > 0) && (++coe_count > coe_limit)) {
            if (blks_readp)
                *blks_readp = xferred + blks;
            pr2serr(">> coe_limit on consecutive reads exceeded\n");
            return SG_LIB_CAT_MEDIUM_HARD;
        }
    }
    if (blks_readp)
        *blks_readp = xferred;
    return 0;

err_out:
    if (ifp->coe) {
        memset(bp, 0, bs * blks);
        pr2serr(">> unable to read at blk=%" PRId64 " for %d bytes, use "
                "zeros\n", lba, bs * blks);
        if (blks > 1)
            pr2serr(">>   try reducing bpt to limit number of zeros written "
                    "near bad block(s)\n");
        /* fudge success */
        if (blks_readp)
            *blks_readp = xferred + blks;
        if ((coe_limit > 0) && (++coe_count > coe_limit)) {
            pr2serr(">> coe_limit on consecutive reads exceeded\n");
            return ret;
        }
        return may_coe ? 0 : ret;
    } else
        return ret ? ret : -1;
}


/* 0 -> successful, SG_LIB_SYNTAX_ERROR -> unable to build cdb,
   SG_LIB_CAT_NOT_READY, SG_LIB_CAT_UNIT_ATTENTION, SG_LIB_CAT_MEDIUM_HARD,
   SG_LIB_CAT_ABORTED_COMMAND, -2 -> recoverable (ENOMEM),
   -1 -> unrecoverable error + others */
static int
sg_write(int sg_fd, unsigned char * buff, int blocks, int64_t to_block,
         int bs, const struct flags_t * ofp, int * diop)
{
    unsigned char wrCmd[MAX_SCSI_CDBSZ];
    unsigned char senseBuff[SENSE_BUFF_LEN];
    struct sg_io_hdr io_hdr;
    int res, k, info_valid;
    uint64_t io_addr = 0;

    if (sg_build_scsi_cdb(wrCmd, ofp->cdbsz, blocks, to_block, 1, ofp->fua,
                          ofp->dpo)) {
        pr2serr(ME "bad wr cdb build, to_block=%" PRId64 ", blocks=%d\n",
                to_block, blocks);
        return SG_LIB_SYNTAX_ERROR;
    }

    memset(&io_hdr, 0, sizeof(struct sg_io_hdr));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = ofp->cdbsz;
    io_hdr.cmdp = wrCmd;
    io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    io_hdr.dxfer_len = bs * blocks;
    io_hdr.dxferp = buff;
    io_hdr.mx_sb_len = SENSE_BUFF_LEN;
    io_hdr.sbp = senseBuff;
    io_hdr.timeout = DEF_TIMEOUT;
    io_hdr.pack_id = (int)to_block;
    if (diop && *diop)
        io_hdr.flags |= SG_FLAG_DIRECT_IO;

    if (verbose > 2) {
        pr2serr("    write cdb: ");
        for (k = 0; k < ofp->cdbsz; ++k)
            pr2serr("%02x ", wrCmd[k]);
        pr2serr("\n");
    }
    while (((res = ioctl(sg_fd, SG_IO, &io_hdr)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        ;
    if (res < 0) {
        if (ENOMEM == errno)
            return -2;
        perror("writing (SG_IO) on sg device, error");
        return -1;
    }

    if (verbose > 2)
        pr2serr("      duration=%u ms\n", io_hdr.duration);
    res = sg_err_category3(&io_hdr);
    switch (res) {
    case SG_LIB_CAT_CLEAN:
        break;
    case SG_LIB_CAT_RECOVERED:
        ++recovered_errs;
        info_valid = sg_get_sense_info_fld(io_hdr.sbp, io_hdr.sb_len_wr,
                                           &io_addr);
        if (info_valid) {
            pr2serr("    lba of last recovered error in this WRITE=0x%" PRIx64
                    "\n", io_addr);
            if (verbose > 1)
                sg_chk_n_print3("writing", &io_hdr, 1);
        } else {
            pr2serr("Recovered error: [no info] writing to block=0x%" PRIx64
                    ", num=%d\n", to_block, blocks);
            sg_chk_n_print3("writing", &io_hdr, verbose > 1);
        }
        break;
    case SG_LIB_CAT_ABORTED_COMMAND:
    case SG_LIB_CAT_UNIT_ATTENTION:
        sg_chk_n_print3("writing", &io_hdr, verbose > 1);
        return res;
    case SG_LIB_CAT_NOT_READY:
        ++unrecovered_errs;
        pr2serr("device not ready (w)\n");
        return res;
    case SG_LIB_CAT_MEDIUM_HARD:
    default:
        sg_chk_n_print3("writing", &io_hdr, verbose > 1);
        ++unrecovered_errs;
        if (ofp->coe) {
            pr2serr(">> ignored errors for out blk=%" PRId64 " for %d "
                    "bytes\n", to_block, bs * blocks);
            return 0; /* fudge success */
        } else
            return res;
    }
    if (diop && *diop &&
        ((io_hdr.info & SG_INFO_DIRECT_IO_MASK) != SG_INFO_DIRECT_IO))
        *diop = 0;      /* flag that dio not done (completely) */
    return 0;
}


static void
calc_duration_throughput(int contin)
{
    struct timeval end_tm, res_tm;
    double a, b;
    int64_t blks;

    if (start_tm_valid && (start_tm.tv_sec || start_tm.tv_usec)) {
        blks = (in_full > out_full) ? in_full : out_full;
        gettimeofday(&end_tm, NULL);
        res_tm.tv_sec = end_tm.tv_sec - start_tm.tv_sec;
        res_tm.tv_usec = end_tm.tv_usec - start_tm.tv_usec;
        if (res_tm.tv_usec < 0) {
            --res_tm.tv_sec;
            res_tm.tv_usec += 1000000;
        }
        a = res_tm.tv_sec;
        a += (0.000001 * res_tm.tv_usec);
        b = (double)blk_sz * blks;
        pr2serr("time to transfer data%s: %d.%06d secs",
                (contin ? " so far" : ""), (int)res_tm.tv_sec,
                (int)res_tm.tv_usec);
        if ((a > 0.00001) && (b > 511))
            pr2serr(" at %.2f MB/sec\n", b / (a * 1000000.0));
        else
            pr2serr("\n");
    }
}

static void
calc_duration_progressbar(int delay)
{
    struct timeval end_tm, res_tm;
    double a, b;
    int64_t blks;

    if (start_tm_valid && (start_tm.tv_sec || start_tm.tv_usec)) {
        blks = (in_full > out_full) ? in_full : out_full;
        gettimeofday(&end_tm, NULL);
        if (end_tm.tv_sec - start_record.tv_sec >= delay)
        {
        	start_record.tv_sec = end_tm.tv_sec;
        	res_tm.tv_sec = end_tm.tv_sec - start_tm.tv_sec;
        	res_tm.tv_usec = end_tm.tv_usec - start_tm.tv_usec;
        	if (res_tm.tv_usec < 0)
        	{
        	   --res_tm.tv_sec;
        	   res_tm.tv_usec += 1000000;
        	}
            a = res_tm.tv_sec;
            a += (0.000001 * res_tm.tv_usec);
            b = (double)blk_sz * blks;
            pr2serr("time to transfer data%s: %d.%06d secs",
                    "", (int)res_tm.tv_sec,
                    (int)res_tm.tv_usec);
            if ((a > 0.00001) && (b > 511))
                pr2serr(" at %.2f MB/sec\r", b / (a * 1000000.0));
        }
    }
}



/* Process arguments given to 'iflag=" or 'oflag=" options. Returns 0
 * on success, 1 on error. */
static int
process_flags(const char * arg, struct flags_t * fp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        pr2serr("no flag found\n");
        return 1;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "append"))
            fp->append = 1;
        else if (0 == strcmp(cp, "coe"))
            ++fp->coe;
        else if (0 == strcmp(cp, "dio"))
            fp->dio = 1;
        else if (0 == strcmp(cp, "direct"))
            fp->direct = 1;
        else if (0 == strcmp(cp, "dpo"))
            fp->dpo = 1;
        else if (0 == strcmp(cp, "dsync"))
            ++fp->dsync;
        else if (0 == strcmp(cp, "excl"))
            fp->excl = 1;
        else if (0 == strcmp(cp, "fua"))
            ++fp->fua;
        else if (0 == strcmp(cp, "nocache"))
            ++fp->nocache;
        else if (0 == strcmp(cp, "null"))
        {}
        else if (0 == strcmp(cp, "sgio"))
            fp->sgio = 1;
        else if (0 == strcmp(cp, "sparse"))
            ++fp->sparse;
        else if (0 == strcmp(cp, "flock"))
            ++fp->flock;
        else {
            pr2serr("unrecognised flag: %s\n", cp);
            return 1;
        }
        cp = np;
    } while (cp);
    return 0;
}

/* Process arguments given to 'conv=" option. Returns 0 on success,
 * 1 on error. */
static int
process_conv(const char * arg, struct flags_t * ifp, struct flags_t * ofp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        pr2serr("no conversions found\n");
        return 1;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
#if 0
        if (0 == strcmp(cp, "fdatasync"))
            ++ofp->fdatasync;
        else if (0 == strcmp(cp, "fsync"))
            ++ofp->fsync;
#endif
        if (0 == strcmp(cp, "noerror"))
            ++ifp->coe;         /* will still fail on write error */
        else if (0 == strcmp(cp, "notrunc"))
        {}        /* this is the default action of ddpt so ignore */
        else if (0 == strcmp(cp, "null"))
        {}
#if 0
        else if (0 == strcmp(cp, "sparing"))
            ++ofp->sparing;
#endif
        else if (0 == strcmp(cp, "sparse"))
            ++ofp->sparse;
        else if (0 == strcmp(cp, "sync"))
        {}  /* dd(susv4): pad errored block(s) with zeros but ddpt does
                 * that by default. Typical dd use: 'conv=noerror,sync' */
#if 0
        else if (0 == strcmp(cp, "trunc"))
            ++ofp->trunc;
#endif
        else {
            pr2serr("unrecognised flag: %s\n", cp);
            return 1;
        }
        cp = np;
    } while (cp);
    return 0;
}

/* Returns open input file descriptor (>= 0) or a negative value
 * (-SG_LIB_FILE_ERROR or -SG_LIB_CAT_OTHER) if error.
 */
static int
open_if(const char * inf, int64_t skip, int bpt, struct flags_t * ifp,
        int * in_typep, int verbose)
{
    int infd, flags, fl, t, verb, res;
    char ebuff[EBUFF_SZ];
    struct sg_simple_inquiry_resp sir;

    verb = (verbose ? verbose - 1: 0);
    *in_typep = dd_filetype(inf);
    if (verbose)
        pr2serr(" >> Input file type: %s\n",
                dd_filetype_str(*in_typep, ebuff));
    if (FT_ERROR & *in_typep) {
        pr2serr(ME "unable access %s\n", inf);
        goto file_err;
    } else if ((FT_BLOCK & *in_typep) && ifp->sgio)
        *in_typep |= FT_SG;

    if (FT_ST & *in_typep) {
        pr2serr(ME "unable to use scsi tape device %s\n", inf);
        goto file_err;
    } else if (FT_SG & *in_typep) {
        flags = O_NONBLOCK;
        if (ifp->direct)
            flags |= O_DIRECT;
        if (ifp->excl)
            flags |= O_EXCL;
        if (ifp->dsync)
            flags |= O_SYNC;
        fl = O_RDWR;
        if ((infd = open(inf, fl | flags)) < 0) {
            fl = O_RDONLY;
            if ((infd = open(inf, fl | flags)) < 0) {
                snprintf(ebuff, EBUFF_SZ,
                         ME "could not open %s for sg reading", inf);
                perror(ebuff);
                goto file_err;
            }
        }
        if (verbose)
            pr2serr("        open input(sg_io), flags=0x%x\n", fl | flags);
        if (sg_simple_inquiry(infd, &sir, 0, verb)) {
            pr2serr("INQUIRY failed on %s\n", inf);
            goto other_err;
        }
        ifp->pdt = sir.peripheral_type;
        if (verbose)
            pr2serr("    %s: %.8s  %.16s  %.4s  [pdt=%d]\n", inf, sir.vendor,
                    sir.product, sir.revision, ifp->pdt);
        if (! (FT_BLOCK & *in_typep)) {
            t = blk_sz * bpt;
            res = ioctl(infd, SG_SET_RESERVED_SIZE, &t);
            if (res < 0)
                perror(ME "SG_SET_RESERVED_SIZE error");
            res = ioctl(infd, SG_GET_VERSION_NUM, &t);
            if ((res < 0) || (t < 30000)) {
                if (FT_BLOCK & *in_typep)
                    pr2serr(ME "SG_IO unsupported on this block device\n");
                else
                    pr2serr(ME "sg driver prior to 3.x.y\n");
                goto file_err;
            }
        }
    } else {
        flags = O_RDONLY;
        if (ifp->direct)
            flags |= O_DIRECT;
        if (ifp->excl)
            flags |= O_EXCL;
        if (ifp->dsync)
            flags |= O_SYNC;
        infd = open(inf, flags);
        if (infd < 0) {
            snprintf(ebuff, EBUFF_SZ,
                     ME "could not open %s for reading", inf);
            perror(ebuff);
            goto file_err;
        } else {
            if (verbose)
                pr2serr("        open input, flags=0x%x\n", flags);
            if (skip > 0) {
                __off64_t offset = skip;

                offset *= blk_sz;       /* could exceed 32 bits here! */
                if (lseek64(infd, offset, SEEK_SET) < 0) {
                    snprintf(ebuff, EBUFF_SZ, ME "couldn't skip to "
                             "required position on %s", inf);
                    perror(ebuff);
                    goto file_err;
                }
                if (verbose)
                    pr2serr("  >> skip: lseek64 SEEK_SET, byte offset=0x%"
                            PRIx64 "\n", (uint64_t)offset);
            }
#ifdef HAVE_POSIX_FADVISE
            if (ifp->nocache) {
                int rt;

                rt = posix_fadvise(infd, 0, 0, POSIX_FADV_SEQUENTIAL);
                if (rt)
                    pr2serr("open_if: posix_fadvise(SEQUENTIAL), err=%d\n",
                            rt);
            }
#endif
        }
    }
    if (ifp->flock) {
        res = flock(infd, LOCK_EX | LOCK_NB);
        if (res < 0) {
            close(infd);
            snprintf(ebuff, EBUFF_SZ, ME "flock(LOCK_EX | LOCK_NB) on %s "
                     "failed", inf);
            perror(ebuff);
            return -SG_LIB_FLOCK_ERR;
        }
    }
    return infd;

file_err:
    return -SG_LIB_FILE_ERROR;
other_err:
    return -SG_LIB_CAT_OTHER;
}

/* Returns open output file descriptor (>= 0), -1 for don't
 * bother opening (e.g. /dev/null), or a more negative value
 * (-SG_LIB_FILE_ERROR or -SG_LIB_CAT_OTHER) if error.
 */
static int
open_of(const char * outf, int64_t seek, int bpt, struct flags_t * ofp,
        int * out_typep, int verbose)
{
    int outfd, flags, t, verb, res;
    char ebuff[EBUFF_SZ];
    struct sg_simple_inquiry_resp sir;

    verb = (verbose ? verbose - 1: 0);
    *out_typep = dd_filetype(outf);
    if (verbose)
        pr2serr(" >> Output file type: %s\n",
                dd_filetype_str(*out_typep, ebuff));

    if ((FT_BLOCK & *out_typep) && ofp->sgio)
        *out_typep |= FT_SG;

    if (FT_ST & *out_typep) {
        pr2serr(ME "unable to use scsi tape device %s\n", outf);
        goto file_err;
    } else if (FT_SG & *out_typep) {
        flags = O_RDWR | O_NONBLOCK;
        if (ofp->direct)
            flags |= O_DIRECT;
        if (ofp->excl)
            flags |= O_EXCL;
        if (ofp->dsync)
            flags |= O_SYNC;
        if ((outfd = open(outf, flags)) < 0) {
            snprintf(ebuff, EBUFF_SZ,
                     ME "could not open %s for sg writing", outf);
            perror(ebuff);
            goto file_err;
        }
        if (verbose)
            pr2serr("        open output(sg_io), flags=0x%x\n", flags);
        if (sg_simple_inquiry(outfd, &sir, 0, verb)) {
            pr2serr("INQUIRY failed on %s\n", outf);
            goto other_err;
        }
        ofp->pdt = sir.peripheral_type;
        if (verbose)
            pr2serr("    %s: %.8s  %.16s  %.4s  [pdt=%d]\n", outf, sir.vendor,
                    sir.product, sir.revision, ofp->pdt);
        if (! (FT_BLOCK & *out_typep)) {
            t = blk_sz * bpt;
            res = ioctl(outfd, SG_SET_RESERVED_SIZE, &t);
            if (res < 0)
                perror(ME "SG_SET_RESERVED_SIZE error");
            res = ioctl(outfd, SG_GET_VERSION_NUM, &t);
            if ((res < 0) || (t < 30000)) {
                pr2serr(ME "sg driver prior to 3.x.y\n");
                goto file_err;
            }
        }
    } else if (FT_DEV_NULL & *out_typep)
        outfd = -1; /* don't bother opening */
    else {
        if (! (FT_RAW & *out_typep)) {
            flags = O_WRONLY | O_CREAT;
            if (ofp->direct)
                flags |= O_DIRECT;
            if (ofp->excl)
                flags |= O_EXCL;
            if (ofp->dsync)
                flags |= O_SYNC;
            if (ofp->append)
                flags |= O_APPEND;
            if ((outfd = open(outf, flags, 0666)) < 0) {
                snprintf(ebuff, EBUFF_SZ,
                        ME "could not open %s for writing", outf);
                perror(ebuff);
                goto file_err;
            }
        } else {
            flags = O_WRONLY;
            if (ofp->direct)
                flags |= O_DIRECT;
            if (ofp->excl)
                flags |= O_EXCL;
            if (ofp->dsync)
                flags |= O_SYNC;
            if ((outfd = open(outf, flags)) < 0) {
                snprintf(ebuff, EBUFF_SZ,
                        ME "could not open %s for raw writing", outf);
                perror(ebuff);
                goto file_err;
            }
        }
        if (verbose)
            pr2serr("        %s output, flags=0x%x\n",
                    ((O_CREAT & flags) ? "create" : "open"), flags);
        if (seek > 0) {
            __off64_t offset = seek;

            offset *= blk_sz;       /* could exceed 32 bits here! */
            if (lseek64(outfd, offset, SEEK_SET) < 0) {
                snprintf(ebuff, EBUFF_SZ,
                    ME "couldn't seek to required position on %s", outf);
                perror(ebuff);
                goto file_err;
            }
            if (verbose)
                pr2serr("   >> seek: lseek64 SEEK_SET, byte offset=0x%" PRIx64
                        "\n", (uint64_t)offset);
        }
    }
    if (ofp->flock) {
        res = flock(outfd, LOCK_EX | LOCK_NB);
        if (res < 0) {
            close(outfd);
            snprintf(ebuff, EBUFF_SZ, ME "flock(LOCK_EX | LOCK_NB) on %s "
                     "failed", outf);
            perror(ebuff);
            return -SG_LIB_FLOCK_ERR;
        }
    }
    return outfd;

file_err:
    return -SG_LIB_FILE_ERROR;
other_err:
    return -SG_LIB_CAT_OTHER;
}


int
main__(int argc, char * argv[])
{
    int64_t skip = 0;
    int64_t seek = 0;
    int64_t out2_off = 0;
    int ibs = 0;
    int obs = 0;
    int bpt = DEF_BLOCKS_PER_TRANSFER;
    int bpt_given = 0;
    char str[STR_SZ];
    char * key;
    char * buf;
    char inf[INOUTF_SZ];
    int in_type = FT_OTHER;
    char outf[INOUTF_SZ];
    char out2f[INOUTF_SZ];
    int out_type = FT_OTHER;
    //int out2_type = FT_OTHER;
    int dio_incomplete = 0;
    int cdbsz_given = 0;
    int do_sync = 0;
    int blocks = 0;
    int res, k, t, buf_sz, dio_tmp, first, blocks_per;
    int infd, outfd, out2fd, retries_tmp, blks_read;
    //int bytes_read, bytes_of2, bytes_of;
    unsigned char * wrkBuff;
    unsigned char * wrkPos;
    int64_t in_num_sect = -1;
    int64_t out_num_sect = -1;
    int out_sect_sz; //in_sect_sz,
    char ebuff[EBUFF_SZ];
    int sparse_skip = 0;
    int penult_sparse_skip = 0;
    int penult_blocks = 0;
    int ret = 0;

    inf[0] = '\0';
    outf[0] = '\0';
    out2f[0] = '\0';
    iflag.cdbsz = DEF_SCSI_CDBSZ;
    oflag.cdbsz = DEF_SCSI_CDBSZ;
    if (argc < 2) {
        pr2serr("Won't default both IFILE to stdin _and_ OFILE to stdout\n");
        pr2serr("For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }

    printf("sg_lib_version: %s\n", sg_lib_version());

    time_t rawtime;
    struct tm* timeinfo;

    time ( &rawtime );
    timeinfo = localtime ( &rawtime );

    printf("Start Task local time and date: %s", asctime (timeinfo) );

    for (k = 1; k < argc; k++) {
        if (argv[k]) {
            strncpy(str, argv[k], STR_SZ);
            str[STR_SZ - 1] = '\0';
        } else
            continue;
        for (key = str, buf = key; *buf && *buf != '=';)
            buf++;
        if (*buf)
            *buf++ = '\0';
        if (0 == strncmp(key, "app", 3)) {
            iflag.append = sg_get_num(buf);
            oflag.append = iflag.append;
        } else if (0 == strcmp(key, "blk_sgio")) {
            iflag.sgio = sg_get_num(buf);
            oflag.sgio = iflag.sgio;
        } else if (0 == strcmp(key, "bpt")) {
            bpt = sg_get_num(buf);
            if (-1 == bpt) {
                pr2serr(ME "bad argument to 'bpt='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            bpt_given = 1;
        } else if (0 == strcmp(key, "bs")) {
            blk_sz = sg_get_num(buf);
            if (-1 == blk_sz) {
                pr2serr(ME "bad argument to 'bs='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "cdbsz")) {
            iflag.cdbsz = sg_get_num(buf);
            oflag.cdbsz = iflag.cdbsz;
            cdbsz_given = 1;
        } else if (0 == strcmp(key, "coe")) {
            iflag.coe = sg_get_num(buf);
            oflag.coe = iflag.coe;
        } else if (0 == strcmp(key, "coe_limit")) {
            coe_limit = sg_get_num(buf);
            if (-1 == coe_limit) {
                pr2serr(ME "bad argument to 'coe_limit='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "conv")) {
            if (process_conv(buf, &iflag, &oflag)) {
                pr2serr(ME "bad argument to 'conv='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "count")) {
            if (0 != strcmp("-1", buf)) {
                dd_count = sg_get_llnum(buf);
                if (-1LL == dd_count) {
                    pr2serr(ME "bad argument to 'count='\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
            }   /* treat 'count=-1' as calculate count (same as not given) */
        } else if (0 == strcmp(key, "dio")) {
            oflag.dio = sg_get_num(buf);
            iflag.dio = oflag.dio;
        } else if (0 == strcmp(key, "fua")) {
            t = sg_get_num(buf);
            oflag.fua = (t & 1) ? 1 : 0;
            iflag.fua = (t & 2) ? 1 : 0;
        } else if (0 == strcmp(key, "ibs"))
            ibs = sg_get_num(buf);
        else if (strcmp(key, "if") == 0) {
            if ('\0' != inf[0]) {
                pr2serr("Second IFILE argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(inf, buf, INOUTF_SZ);
        } else if (0 == strcmp(key, "iflag")) {
            if (process_flags(buf, &iflag)) {
                pr2serr(ME "bad argument to 'iflag='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "obs"))
            obs = sg_get_num(buf);
        else if (0 == strcmp(key, "odir")) {
            iflag.direct = sg_get_num(buf);
            oflag.direct = iflag.direct;
        } else if (strcmp(key, "of") == 0) {
            if ('\0' != outf[0]) {
                pr2serr("Second OFILE argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(outf, buf, INOUTF_SZ);
        } else if (strcmp(key, "of2") == 0) {
            if ('\0' != out2f[0]) {
                pr2serr("Second OFILE2 argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(out2f, buf, INOUTF_SZ);
        } else if (0 == strcmp(key, "oflag")) {
            if (process_flags(buf, &oflag)) {
                pr2serr(ME "bad argument to 'oflag='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "retries")) {
            iflag.retries = sg_get_num(buf);
            oflag.retries = iflag.retries;
            if (-1 == iflag.retries) {
                pr2serr(ME "bad argument to 'retries='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "seek")) {
            seek = sg_get_llnum(buf);
            if (-1LL == seek) {
                pr2serr(ME "bad argument to 'seek='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "skip")) {
            skip = sg_get_llnum(buf);
            if (-1LL == skip) {
                pr2serr(ME "bad argument to 'skip='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "sync"))
            do_sync = sg_get_num(buf);
        else if (0 == strcmp(key, "time"))
            do_time = sg_get_num(buf);
        else if (0 == strncmp(key, "verb", 4))
            verbose = sg_get_num(buf);
        else if ((0 == strncmp(key, "--help", 7)) ||
                 (0 == strncmp(key, "-h", 2)) ||
                 (0 == strcmp(key, "-?"))) {
            usage__();
            return 0;
        } else if ((0 == strncmp(key, "--vers", 6)) ||
                   (0 == strcmp(key, "-V"))) {
            pr2serr(ME "%s\n", version_str);
            return 0;
        } else {
            pr2serr("Unrecognized option '%s'\n", key);
            pr2serr("For more information use '--help'\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (blk_sz <= 0) {
        blk_sz = DEF_BLOCK_SIZE;
        pr2serr("Assume default 'bs' (block size) of %d bytes\n", blk_sz);
    }
    if ((ibs && (ibs != blk_sz)) || (obs && (obs != blk_sz))) {
        pr2serr("If 'ibs' or 'obs' given must be same as 'bs'\n");
        pr2serr("For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((skip < 0) || (seek < 0)) {
        pr2serr("skip and seek cannot be negative\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((oflag.append > 0) && (seek > 0)) {
        pr2serr("Can't use both append and seek switches\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (bpt < 1) {
        pr2serr("bpt must be greater than 0\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (iflag.sparse)
        pr2serr("sparse flag ignored for iflag\n");

    /* defaulting transfer size to 128*2048 for CD/DVDs is too large
       for the block layer in lk 2.6 and results in an EIO on the
       SG_IO ioctl. So reduce it in that case. */
    if ((blk_sz >= 2048) && (0 == bpt_given))
        bpt = DEF_BLOCKS_PER_2048TRANSFER;
#ifdef SG_DEBUG
    pr2serr(ME "if=%s skip=%" PRId64 " of=%s seek=%" PRId64 " count=%" PRId64
            "\n", inf, skip, outf, seek, dd_count);
#endif
    install_handler(SIGINT, interrupt_handler);
    install_handler(SIGQUIT, interrupt_handler);
    install_handler(SIGPIPE, interrupt_handler);
    install_handler(SIGUSR1, siginfo_handler);

    infd = STDIN_FILENO;
    outfd = STDOUT_FILENO;
    iflag.pdt = -1;
    oflag.pdt = -1;
    if (inf[0] && ('-' != inf[0])) {
        infd = open_if(inf, skip, bpt, &iflag, &in_type, verbose);
        if (infd < 0)
            return -infd;
    }

    if (outf[0] && ('-' != outf[0])) {
        outfd = open_of(outf, seek, bpt, &oflag, &out_type, verbose);
        if (outfd < -1)
            return -outfd;
    }

    if (out2f[0]) {
        //out2_type = dd_filetype(out2f);
        if ((out2fd = open(out2f, O_WRONLY | O_CREAT, 0666)) < 0) {
            res = errno;
            snprintf(ebuff, EBUFF_SZ,
                     ME "could not open %s for writing", out2f);
            perror(ebuff);
            return res;
        }
    } else
        out2fd = -1;

    if ((STDIN_FILENO == infd) && (STDOUT_FILENO == outfd)) {
        pr2serr("Can't have both 'if' as stdin _and_ 'of' as stdout\n");
        pr2serr("For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (oflag.sparse) {
        if (STDOUT_FILENO == outfd) {
            pr2serr("oflag=sparse needs seekable output file\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }

    if ((dd_count < 0) || ((verbose > 0) && (0 == dd_count))) {
//        in_num_sect = -1;
//        in_sect_sz = -1;
//        if (FT_SG & in_type) {
//            res = scsi_read_capacity(infd, &in_num_sect, &in_sect_sz);
//            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
//                pr2serr("Unit attention (readcap in), continuing\n");
//                res = scsi_read_capacity(infd, &in_num_sect, &in_sect_sz);
//            } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
//                pr2serr("Aborted command (readcap in), continuing\n");
//                res = scsi_read_capacity(infd, &in_num_sect, &in_sect_sz);
//            }
//            if (0 != res) {
//                if (res == SG_LIB_CAT_INVALID_OP)
//                    pr2serr("read capacity not supported on %s\n", inf);
//                else if (res == SG_LIB_CAT_NOT_READY)
//                    pr2serr("read capacity failed on %s - not ready\n", inf);
//                else
//                    pr2serr("Unable to read capacity on %s\n", inf);
//                in_num_sect = -1;
//            } else if (in_sect_sz != blk_sz)
//                pr2serr(">> warning: block size on %s confusion: bs=%d, "
//                        "device claims=%d\n", inf, blk_sz, in_sect_sz);
//        } else if (FT_BLOCK & in_type) {
//            if (0 != read_blkdev_capacity(infd, &in_num_sect, &in_sect_sz)) {
//                pr2serr("Unable to read block capacity on %s\n", inf);
//                in_num_sect = -1;
//            }
//            if (blk_sz != in_sect_sz) {
//                pr2serr("block size on %s confusion: bs=%d, device "
//                        "claims=%d\n", inf, blk_sz, in_sect_sz);
//                in_num_sect = -1;
//            }
//        }
//        if (in_num_sect > skip)
//            in_num_sect -= skip;

        out_num_sect = -1;
        out_sect_sz = -1;
        if (FT_SG & out_type) {
            res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
                pr2serr("Unit attention (readcap out), continuing\n");
                res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
            } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
                pr2serr("Aborted command (readcap out), continuing\n");
                res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
            }
            if (0 != res) {
                if (res == SG_LIB_CAT_INVALID_OP)
                    pr2serr("read capacity not supported on %s\n", outf);
                else
                    pr2serr("Unable to read capacity on %s\n", outf);
                out_num_sect = -1;
            } else if (blk_sz != out_sect_sz){
                pr2serr(">> warning: block size on %s confusion: bs=%d, "
                        "device claims=%d\n", outf, blk_sz, out_sect_sz);
                blk_sz = out_sect_sz;
            }
        }
//        else if (FT_BLOCK & out_type) {
//            if (0 != read_blkdev_capacity(outfd, &out_num_sect,
//                                          &out_sect_sz)) {
//                pr2serr("Unable to read block capacity on %s\n", outf);
//                out_num_sect = -1;
//            } else if (blk_sz != out_sect_sz) {
//                pr2serr("block size on %s confusion: bs=%d, device "
//                        "claims=%d\n", outf, blk_sz, out_sect_sz);
//                out_num_sect = -1;
//            }
//        }
        if (out_num_sect > seek)
            out_num_sect -= seek;
#ifdef SG_DEBUG
        pr2serr("Start of loop, count=%" PRId64 ", in_num_sect=%" PRId64
                ", out_num_sect=%" PRId64 "\n", dd_count, in_num_sect,
                out_num_sect);
#endif
        if (dd_count < 0) {
            if (in_num_sect > 0) {
                if (out_num_sect > 0)
                    dd_count = (in_num_sect > out_num_sect) ? out_num_sect :
                                                           in_num_sect;
                else
                    dd_count = in_num_sect;
            } else
                dd_count = out_num_sect;
        }
    }

    if (dd_count < 0) {
        pr2serr("Couldn't calculate count, please give one\n");
        return SG_LIB_CAT_OTHER;
    }
    if (! cdbsz_given) {
        if ((FT_SG & in_type) && (MAX_SCSI_CDBSZ != iflag.cdbsz) &&
            (((dd_count + skip) > UINT_MAX) || (bpt > USHRT_MAX))) {
            pr2serr("Note: SCSI command size increased to 16 bytes (for "
                    "'if')\n");
            iflag.cdbsz = MAX_SCSI_CDBSZ;
        }
        if ((FT_SG & out_type) && (MAX_SCSI_CDBSZ != oflag.cdbsz) &&
            (((dd_count + seek) > UINT_MAX) || (bpt > USHRT_MAX))) {
            pr2serr("Note: SCSI command size increased to 16 bytes (for "
                    "'of')\n");
            oflag.cdbsz = MAX_SCSI_CDBSZ;
        }
    }

    if (iflag.dio || iflag.direct || oflag.direct || (FT_RAW & in_type) ||
        (FT_RAW & out_type)) {
        size_t psz;

#if defined(HAVE_SYSCONF) && defined(_SC_PAGESIZE)
        psz = sysconf(_SC_PAGESIZE); /* POSIX.1 (was getpagesize()) */
#else
        psz = 4096;     /* give up, pick likely figure */
#endif

#ifdef HAVE_POSIX_MEMALIGN
        {
            int err;

            err = posix_memalign((void **)&wrkBuff, psz, blk_sz * bpt);
            if (err) {
                pr2serr("posix_memalign: error [%d] out of memory?\n", err);
                return SG_LIB_CAT_OTHER;
            }
            wrkPos = wrkBuff;
        }
#else
        wrkBuff = (unsigned char*)malloc(blk_sz * bpt + psz);
        if (0 == wrkBuff) {
            pr2serr("Not enough user memory for work buffer\n");
            return SG_LIB_CAT_OTHER;
        }
        wrkPos = (unsigned char *)(((uintptr_t)wrkBuff + psz - 1) &
                                   (~(psz - 1)));
#endif
    } else
    {
        wrkBuff = (unsigned char*)malloc(blk_sz * bpt);
        if (0 == wrkBuff) {
            pr2serr("Not enough user memory\n");
            return SG_LIB_CAT_OTHER;
        }
        wrkPos = wrkBuff;
    }

    blocks_per = bpt;
#ifdef SG_DEBUG
    pr2serr("Start of loop, count=%" PRId64 ", blocks_per=%d\n", dd_count,
            blocks_per);
#endif
    if (do_time) {
        start_tm.tv_sec = 0;
        start_tm.tv_usec = 0;
        gettimeofday(&start_tm, NULL);
        start_tm_valid = 1;
        start_record.tv_sec = start_tm.tv_sec;
        start_record.tv_usec = start_tm.tv_usec;
        printf(HEADER, "MB","MB");
    }
    req_count = dd_count;

    /* <<< main loop that does the copy >>> */
    while (dd_count > 0) {
        //bytes_read = 0;
        //bytes_of = 0;
        //bytes_of2 = 0;
        penult_sparse_skip = sparse_skip;
        penult_blocks = penult_sparse_skip ? blocks : 0;
        sparse_skip = 0;
        blocks = (dd_count > blocks_per) ? blocks_per : dd_count;
        if (FT_SG & in_type) {
            dio_tmp = iflag.dio;
            res = sg_read(infd, wrkPos, blocks, skip, blk_sz, &iflag,
                          &dio_tmp, &blks_read);
            if (-2 == res) {     /* ENOMEM, find what's available+try that */
                if (ioctl(infd, SG_GET_RESERVED_SIZE, &buf_sz) < 0) {
                    perror("RESERVED_SIZE ioctls failed");
                    ret = res;
                    break;
                }
                if (buf_sz < MIN_RESERVED_SIZE)
                    buf_sz = MIN_RESERVED_SIZE;
                blocks_per = (buf_sz + blk_sz - 1) / blk_sz;
                if (blocks_per < blocks) {
                    blocks = blocks_per;
                    pr2serr("Reducing read to %d blocks per loop\n",
                            blocks_per);
                    res = sg_read(infd, wrkPos, blocks, skip, blk_sz,
                                  &iflag, &dio_tmp, &blks_read);
                }
            }
            if (res) {
                pr2serr("sg_read failed,%s at or after lba=%" PRId64 " [0x%"
                        PRIx64 "]\n", ((-2 == res) ?
                                 " try reducing bpt," : ""), skip, skip);
                ret = res;
                break;
            } else {
                if (blks_read < blocks) {
                    dd_count = 0;   /* force exit after write */
                    blocks = blks_read;
                }
                in_full += blocks;
                if (iflag.dio && (0 == dio_tmp))
                    dio_incomplete++;
            }
        } else {
            while (((res = read(infd, wrkPos, blocks * blk_sz)) < 0) &&
                   ((EINTR == errno) || (EAGAIN == errno)))
                ;
            if (verbose > 2)
                pr2serr("read(unix): count=%d, res=%d\n", blocks * blk_sz,
                        res);
            if (res < 0) {
                snprintf(ebuff, EBUFF_SZ, ME "reading, skip=%" PRId64 " ",
                         skip);
                perror(ebuff);
                ret = -1;
                break;
            } else if (res < blocks * blk_sz) {
                dd_count = 0;
                blocks = res / blk_sz;
                if ((res % blk_sz) > 0) {
                    blocks++;
                    in_partial++;
                }
            }
            //bytes_read = res;
            in_full += blocks;
        }

        if (0 == blocks)
            break;      /* nothing read so leave loop */

        if (out2f[0]) {
            while (((res = write(out2fd, wrkPos, blocks * blk_sz)) < 0) &&
                   ((EINTR == errno) || (EAGAIN == errno)))
                ;
            if (verbose > 2)
                pr2serr("write to of2: count=%d, res=%d\n", blocks * blk_sz,
                        res);
            if (res < 0) {
                snprintf(ebuff, EBUFF_SZ, ME "writing to of2, seek=%" PRId64
                         " ", seek);
                perror(ebuff);
                ret = -1;
                break;
            }
            //bytes_of2 = res;
            out2_off += res;
        }

        if ((oflag.sparse) && (dd_count > blocks) &&
            (! (FT_DEV_NULL & out_type))) {
            if (NULL == zeros_buff) {
                zeros_buff = (unsigned char *)malloc(blocks * blk_sz);
                if (NULL == zeros_buff) {
                    pr2serr("zeros_buff malloc failed\n");
                    ret = -1;
                    break;
                }
                memset(zeros_buff, 0, blocks * blk_sz);
            }
            if (0 == memcmp(wrkPos, zeros_buff, blocks * blk_sz))
                sparse_skip = 1;
        }
        if (sparse_skip) {
            if (FT_SG & out_type) {
                out_sparse += blocks;
                if (verbose > 2)
                    pr2serr("sparse bypassing sg_write: seek blk=%" PRId64
                            ", offset blks=%d\n", seek, blocks);
            } else if (FT_DEV_NULL & out_type)
            {}
            else {
                __off64_t offset = blocks * blk_sz;
                __off64_t off_res;

                if (verbose > 2)
                    pr2serr("sparse bypassing write: seek=%" PRId64 ", rel "
                            "offset=%" PRId64 "\n", (seek * blk_sz),
                            (int64_t)offset);
                off_res = lseek64(outfd, offset, SEEK_CUR);
                if (off_res < 0) {
                    pr2serr("sparse tried to bypass write: seek=%" PRId64
                            ", rel offset=%" PRId64 " but ...\n",
                            (seek * blk_sz), (int64_t)offset);
                    perror("lseek64 on output");
                    ret = SG_LIB_FILE_ERROR;
                    break;
                } else if (verbose > 4)
                    pr2serr("oflag=sparse lseek64 result=%" PRId64 "\n",
                            (int64_t)off_res);
                out_sparse += blocks;
            }
        } else if (FT_SG & out_type) {
            dio_tmp = oflag.dio;
            retries_tmp = oflag.retries;
            first = 1;
            while (1) {
                ret = sg_write(outfd, wrkPos, blocks, seek, blk_sz,
                               &oflag, &dio_tmp);
                if (0 == ret)
                    break;
                if ((SG_LIB_CAT_NOT_READY == ret) ||
                    (SG_LIB_SYNTAX_ERROR == ret))
                    break;
                else if ((-2 == ret) && first) {
                    /* ENOMEM: find what's available and try that */
                    if (ioctl(outfd, SG_GET_RESERVED_SIZE, &buf_sz) < 0) {
                        perror("RESERVED_SIZE ioctls failed");
                        break;
                    }
                    if (buf_sz < MIN_RESERVED_SIZE)
                        buf_sz = MIN_RESERVED_SIZE;
                    blocks_per = (buf_sz + blk_sz - 1) / blk_sz;
                    if (blocks_per < blocks) {
                        blocks = blocks_per;
                        pr2serr("Reducing write to %d blocks per loop\n",
                                blocks);
                    } else
                        break;
                } else if ((SG_LIB_CAT_UNIT_ATTENTION == ret) && first) {
                    if (--max_uas > 0)
                        pr2serr("Unit attention, continuing (w)\n");
                    else {
                        pr2serr("Unit attention, too many (w)\n");
                        break;
                    }
                } else if ((SG_LIB_CAT_ABORTED_COMMAND == ret) && first) {
                    if (--max_aborted > 0)
                        pr2serr("Aborted command, continuing (w)\n");
                    else {
                        pr2serr("Aborted command, too many (w)\n");
                        break;
                    }
                } else if (ret < 0)
                    break;
                else if (retries_tmp > 0) {
                    pr2serr(">>> retrying a sgio write, lba=0x%" PRIx64 "\n",
                            (uint64_t)seek);
                    --retries_tmp;
                    ++num_retries;
                    if (unrecovered_errs > 0)
                        --unrecovered_errs;
                } else
                    break;
                first = 0;
            }
            if (0 != ret) {
                pr2serr("sg_write failed,%s seek=%" PRId64 "\n",
                        ((-2 == ret) ? " try reducing bpt," : ""), seek);
                break;
            } else {
                out_full += blocks;
                if (oflag.dio && (0 == dio_tmp))
                    dio_incomplete++;
            }
        } else if (FT_DEV_NULL & out_type)
            out_full += blocks; /* act as if written out without error */
        else {
            while (((res = write(outfd, wrkPos, blocks * blk_sz)) < 0) &&
                   ((EINTR == errno) || (EAGAIN == errno)))
                ;
            if (verbose > 2)
                pr2serr("write(unix): count=%d, res=%d\n", blocks * blk_sz,
                        res);
            if (res < 0) {
                snprintf(ebuff, EBUFF_SZ, ME "writing, seek=%" PRId64 " ",
                         seek);
                perror(ebuff);
                ret = -1;
                break;
            } else if (res < blocks * blk_sz) {
                pr2serr("output file probably full, seek=%" PRId64 " ", seek);
                blocks = res / blk_sz;
                out_full += blocks;
                if ((res % blk_sz) > 0)
                    out_partial++;
                ret = -1;
                break;
            } else {
                out_full += blocks;
                //bytes_of = res;
            }
        }
#ifdef HAVE_POSIX_FADVISE
        {
            int rt, in_valid, out2_valid, out_valid;

            in_valid = ((FT_OTHER == in_type) || (FT_BLOCK == in_type));
            out2_valid = ((FT_OTHER == out2_type) || (FT_BLOCK == out2_type));
            out_valid = ((FT_OTHER == out_type) || (FT_BLOCK == out_type));
            if (iflag.nocache && (bytes_read > 0) && in_valid) {
                rt = posix_fadvise(infd, 0, (skip * blk_sz) + bytes_read,
                                   POSIX_FADV_DONTNEED);
                // rt = posix_fadvise(infd, (skip * blk_sz), bytes_read,
                                   // POSIX_FADV_DONTNEED);
                // rt = posix_fadvise(infd, 0, 0, POSIX_FADV_DONTNEED);
                if (rt)         /* returns error as result */
                    pr2serr("posix_fadvise on read, skip=%" PRId64
                            " ,err=%d\n", skip, rt);
            }
            if ((oflag.nocache & 2) && (bytes_of2 > 0) && out2_valid) {
                rt = posix_fadvise(out2fd, 0, 0, POSIX_FADV_DONTNEED);
                if (rt)
                    pr2serr("posix_fadvise on of2, seek=%" PRId64
                            " ,err=%d\n", seek, rt);
            }
            if ((oflag.nocache & 1) && (bytes_of > 0) && out_valid) {
                rt = posix_fadvise(outfd, 0, 0, POSIX_FADV_DONTNEED);
                if (rt)
                    pr2serr("posix_fadvise on output, seek=%" PRId64
                            " ,err=%d\n", seek, rt);
            }
        }
#endif
        if (dd_count > 0)
            dd_count -= blocks;
        skip += blocks;
        seek += blocks;

        if (do_time)
        	calc_duration_progressbar(5);
    } /* end of main loop that does the copy ... */
    if (ret && penult_sparse_skip && (penult_blocks > 0)) {
        /* if error and skipped last output due to sparse ... */
        if ((FT_SG & out_type) || (FT_DEV_NULL & out_type))
        {}
        else {
            /* ... try writing to extend ofile to length prior to error */
            while (((res = write(outfd, zeros_buff, penult_blocks * blk_sz))
                    < 0) && ((EINTR == errno) || (EAGAIN == errno)))
                ;
            if (verbose > 2)
                pr2serr("write(unix, sparse after error): count=%d, res=%d\n",
                        penult_blocks * blk_sz, res);
            if (res < 0) {
                snprintf(ebuff, EBUFF_SZ, ME "writing(sparse after error), "
                        "seek=%" PRId64 " ", seek);
                perror(ebuff);
            }
        }
    }

    if (do_time)
        calc_duration_throughput(0);

    if (do_sync) {
        if (FT_SG & out_type) {
            pr2serr(">> Synchronizing cache on %s\n", outf);
            res = sg_ll_sync_cache_10(outfd, 0, 0, 0, 0, 0, 1, 0);
            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
                pr2serr("Unit attention (out, sync cache), continuing\n");
                res = sg_ll_sync_cache_10(outfd, 0, 0, 0, 0, 0, 0, 0);
            }
            if (0 != res)
                pr2serr("Unable to synchronize cache\n");
        }
    }
    free(wrkBuff);
    if (zeros_buff)
        free(zeros_buff);
    if (STDIN_FILENO != infd)
        close(infd);
    if (! ((STDOUT_FILENO == outfd) || (FT_DEV_NULL & out_type)))
        close(outfd);
    if (0 != dd_count) {
        pr2serr("Some error occurred,");
        if (0 == ret)
            ret = SG_LIB_CAT_OTHER;
    }
    print_stats_sg("");
    if (dio_incomplete) {
        int fd;
        char c;

        pr2serr(">> Direct IO requested but incomplete %d times\n",
                dio_incomplete);
        if ((fd = open(proc_allow_dio, O_RDONLY)) >= 0) {
            if (1 == read(fd, &c, 1)) {
                if ('0' == c)
                    pr2serr(">>> %s set to '0' but should be set to '1' for "
                            "direct IO\n", proc_allow_dio);
            }
            close(fd);
        }
    }
    if (sum_of_resids)
        pr2serr(">> Non-zero sum of residual counts=%d\n", sum_of_resids);

    time ( &rawtime );
    timeinfo = localtime ( &rawtime );

    printf("Start Task local time and date: %s", asctime (timeinfo) );
    return (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
}

typedef enum {
	WIPEMODE_NORMAL,
	WIPEMODE_DOD,
	WIPEMODE_DOD7,
	WIPEMODE_GUTMANN,
	WIPEMODE_DOE,
	WIPEMODE_SCHNEIER,
	WIPEMODE_BCI,
	WIPEMODE_JEFFERY
} WipeMode;


struct _opt {
	unsigned int	passes;
	WipeMode		mode;
	bool			yes;
	RandomMode		random;
	unsigned int	sectors;
	int64_t			start;
	int64_t			end;
	bool			read;
	unsigned int	help;
	bool			kilobyte;
	unsigned int	refresh;
	bool			ignore;
	bool			checksum;
};

typedef struct _opt t_opt;

static t_opt opt = {
	0,					/* passes */
	WIPEMODE_NORMAL,	/* normal, dod, dod7, gutmann */
	false,				/* yes */
	RANDOM_NONE,		/* pseudo, windows, cryptographic */
	DEF_BLOCKS_PER_TRANSFER,	/* sectors */
	0,					/* start */
	0,					/* end */
	false,				/* read */
	0,					/* help */
	false,				/* kilobyte */
	5,					/* refresh */
	false,				/* ignore */
	false,				/* check sum calc */
};

struct _stats {
	char			*device_name;
	unsigned int	bytes_per_sector;

	int64_t			start_ticks;
	struct tm		lpStartTime;
	char			start_time[20];
	int64_t			wiping_ticks;

	int64_t			all_start_ticks;
	int64_t			all_wiping_ticks;

	int64_t			passwiping_ticks;
};

typedef struct _stats t_stats;

static int64_t
get_ticks(t_stats *stats)
{
	struct timeval tm;
	tm.tv_sec = 0;
	tm.tv_usec = 0;
	gettimeofday(&tm, NULL);
	return tm.tv_sec;
}

static char *seconds_to_hhmmss(uint seconds, char *rv, int bufsiz) {
	uint hours = seconds / 3600;
	seconds -= hours * 3600;

	uint minutes = seconds / 60;
	seconds -= minutes * 60;

	if (hours > 99) {
		uint days = hours / 24;
		hours -= days * 24;
		if (days > 99) {
			snprintf(rv, bufsiz, "%03dd %02dh", days, hours);
			return rv;
		}
		snprintf(rv, bufsiz, "%02dd%02d%02d", days, hours, minutes);
		return rv;
	}

	snprintf(rv, bufsiz, "%02d:%02d:%02d", hours, minutes, seconds);

	return rv;
}


static void print_stats(unsigned int pass, char *s_byte, int64_t sector, t_stats *stats, int passescnt)
{
	int64_t starting_sector = opt.start;
	int64_t ending_sector = opt.end;

	int64_t done_sectors	= (ending_sector * ((int64_t) pass - 1)) + sector;
	int64_t total_sectors = ending_sector * (passescnt);

	int64_t single_sectors = sector;
	//int64_t single_total_sectors = ending_sector * 1;


	double all_pct = (double) (int64_t) done_sectors / (double) (int64_t) total_sectors * 100.0;
	//double single_pct = (double)(int64_t)single_sectors / (double)(int64_t)single_total_sectors * 100.0;

	int64_t remaining_ticks = 0;

	int64_t elapsed_ticks = get_ticks(stats) - stats->start_ticks;

	if (done_sectors) {
		remaining_ticks = (int64_t)(((double)(int64_t)((total_sectors - done_sectors) / (double)(int64_t)done_sectors) * elapsed_ticks));
	}

	double kilo = opt.kilobyte ? 1024.0 : 1000.0;

	double mb_sec = 0;
	double mb_sec_single = 0;
	double secondspass = 0;


	if (stats->wiping_ticks) {
		int64_t bytes = done_sectors * stats->bytes_per_sector;
		double megabytes = (double) (int64_t) bytes / (kilo * kilo);
		double seconds = (double) (int64_t) stats->wiping_ticks;

		int64_t bytessingle = single_sectors * stats->bytes_per_sector;
		double megabytessingle = (double)(int64_t)bytessingle / (kilo * kilo);
		secondspass = (double)(int64_t)stats->passwiping_ticks;

		//printf("\nsector=%20I64d done_sectors=%20I64d bytes=%20I64d megabytes=%20.10f seconds=%20.10f\n", sector, done_sectors, bytes, megabytes, seconds);
		if (seconds > 0) {
			mb_sec = megabytes / seconds;
		}
		if (secondspass > 0){
			mb_sec_single = megabytessingle / secondspass;
		}
	}

	sector -= starting_sector;
	ending_sector -= starting_sector;

	double this_pct = (double) (int64_t) sector / (double) (int64_t) ending_sector * 100.0;

	if (sector >= ending_sector) {
		this_pct = 100.0;
		all_pct = (double)(pass)* 100.0 / (double)passescnt;
		if (pass >= opt.passes) {
			this_pct = 100.0;
			all_pct = 100.0;
			remaining_ticks = 0;
		}
	}

	char consume_time[255];
	seconds_to_hhmmss((uint)secondspass, consume_time, sizeof(consume_time));


	uint remaining_seconds = (uint) (remaining_ticks);

	char remaining_time[255];
	seconds_to_hhmmss(remaining_seconds, remaining_time, sizeof(remaining_time));

	uint elapsed_seconds = (uint) (elapsed_ticks );

	char elapsed_time[255];
	seconds_to_hhmmss(elapsed_seconds, elapsed_time, sizeof(elapsed_time));

	double dbsec = mb_sec;
	if (dbsec < 1) dbsec = 1;
	int64_t tems = (int64_t)((double)(int64_t)(total_sectors * stats->bytes_per_sector) / (kilo * kilo) / (dbsec));

	char finish_time[255] = { 0 };
	snprintf(finish_time, sizeof(finish_time), "%08" PRId64, tems);

	char buf[1024];
	snprintf(buf, sizeof(buf), "%.3f%% - %s - %s - %s", all_pct, remaining_time, stats->device_name, progname);

	printf(FORMAT_STRING,
		pass,
		passescnt,
		s_byte,
		this_pct,
		all_pct,
		elapsed_time,
		consume_time,//remaining_time,
		stats->start_time,
		finish_time,
		mb_sec,
		mb_sec_single);
	fflush(stdout);
}

static int
CheckSumCount(int bytes, int *byte)
{
	int nCount = 0;
	int n = 0;
	for (unsigned int pass = 1; pass <= opt.passes; ++pass) {
		int byte_to_write = 0;
		switch (opt.mode) {
		case WIPEMODE_NORMAL:
			if (bytes == 0) {
				byte_to_write = 0;
			}
			else {
				n = (pass - 1) % bytes;
				byte_to_write = byte[n];
			}

			break;

		case WIPEMODE_DOD:
			n = (pass - 1) % dod_elements;
			byte_to_write = dod_bytes[n];

			break;

		case WIPEMODE_DOD7:
			n = (pass - 1) % dod7_elements;
			byte_to_write = dod7_bytes[n];

			break;

		case WIPEMODE_GUTMANN:
			n = (pass - 1) % gutmann_elements;
			byte_to_write = gutmann_bytes[n][0];

			break;

		case WIPEMODE_DOE:
			n = (pass - 1) % doe_elements;
			byte_to_write = doe_bytes[n];

			break;

		case WIPEMODE_SCHNEIER:
			n = (pass - 1) % schneier_elements;
			byte_to_write = schneier_bytes[n];

			break;

		case WIPEMODE_BCI:
			n = (pass - 1) % bci_elements;
			byte_to_write = bci_bytes[n];

			break;

		case WIPEMODE_JEFFERY:
			break;

		}
		if (byte_to_write == CHECKDATAFLAG)
		{
			nCount++;
		}
	}
	return nCount;
}

static int
wipe_device(char *device_name, int bytes, int *byte, t_stats *stats)
{
	stats->start_ticks = get_ticks(stats);
	stats->wiping_ticks = 0;

	stats->device_name = device_name;

	int bpt = DEF_BLOCKS_PER_TRANSFER;
	int out_type = FT_OTHER;
	int outfd, retries_tmp;
    int64_t out_num_sect = -1;
    int out_sect_sz;
    int res;

	outfd = open_of(device_name, opt.start, bpt, &oflag, &out_type, verbose);
	if (outfd < -1)
	   return -outfd;

    out_num_sect = -1;
    out_sect_sz = -1;
    if (FT_SG & out_type) {
        res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
        if (SG_LIB_CAT_UNIT_ATTENTION == res) {
            pr2serr("Unit attention (readcap out), continuing\n");
            res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
        } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
            pr2serr("Aborted command (readcap out), continuing\n");
            res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
        }
        if (0 != res) {
            if (res == SG_LIB_CAT_INVALID_OP)
                pr2serr("read capacity not supported on %s\n", device_name);
            else
                pr2serr("Unable to read capacity on %s\n", device_name);
            out_num_sect = -1;
        } else if (blk_sz != out_sect_sz){
            pr2serr(">> warning: block size on %s confusion: bs=%d, "
                    "device claims=%d\n", device_name, blk_sz, out_sect_sz);
            blk_sz = out_sect_sz;
            stats->bytes_per_sector = blk_sz;
        }
    }//MUST is FT_SG support. other not support


	if (opt.end > out_num_sect) {
		pr2serr("Ending sector must be less than or equal to %" PRId64 " for %s", out_num_sect, device_name);
		exit(-2);
	}
	if (opt.end == 0) {
		opt.end = out_num_sect;
	}

    if (out_num_sect > opt.start)
        out_num_sect -= opt.start;


	if (opt.start > opt.end) {
		pr2serr("Ending sector must be greater than starting sector");
		exit(-2);
	}

	time_t t = time(NULL);
	stats->lpStartTime= *localtime(&t);
	snprintf(stats->start_time, sizeof(stats->start_time), "%02d:%02d:%02d", stats->lpStartTime.tm_hour, stats->lpStartTime.tm_min, stats->lpStartTime.tm_sec);
	uint64_t last_ticks = get_ticks(stats);

	printf(HEADER, opt.kilobyte ? " MiB" : "MB", opt.kilobyte ? " MiB" : "MB");

	unsigned int bytes_to_process = opt.sectors * stats->bytes_per_sector;
	unsigned char *sector_data = (unsigned char *) malloc(bytes_to_process + BYTES_PER_ELEMENT);

	int nCheckCount = 0;
	int CheckSumPasses = CheckSumCount(bytes, byte);


	for (unsigned int pass = 1; pass <= opt.passes; ++pass)
	{
		int byte_to_write = 0;
		int j;
		unsigned char chars[3]={0};
		unsigned int n;

		stats->passwiping_ticks = 0;

		RandomMode random = (bytes == 0) ? opt.random : RANDOM_NONE;

		switch (opt.mode) {
			case WIPEMODE_NORMAL:
				if (bytes == 0) {
					byte_to_write = 0;
				} else {
					int n = (pass - 1) % bytes;
					byte_to_write = byte[n];
					if (byte_to_write < 0) {
						if (random == RANDOM_NONE) {
							random = opt.random ? opt.random : RANDOM_XORSHIFT;
						}
					}
				}

				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) byte_to_write;
				}
				break;

			case WIPEMODE_DOD:
				n = (pass - 1) % dod_elements;
				byte_to_write = dod_bytes[n];
				if (byte_to_write < 0) {
					if (random == RANDOM_NONE) {
						random = opt.random ? opt.random : RANDOM_XORSHIFT;
					}
				} else {
					random = RANDOM_NONE;
				}
				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) byte_to_write;
				}
				break;

			case WIPEMODE_DOD7:
				n = (pass - 1) % dod7_elements;
				byte_to_write = dod7_bytes[n];
				if (byte_to_write < 0) {
					if (random == RANDOM_NONE) {
						random = opt.random ? opt.random : RANDOM_XORSHIFT;
					}
				} else {
					random = RANDOM_NONE;
				}
				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) byte_to_write;
				}
				break;

			case WIPEMODE_GUTMANN:
				n = (pass - 1) % gutmann_elements;
				byte_to_write = gutmann_bytes[n][0];
				if (byte_to_write < 0) {
					if (random == RANDOM_NONE) {
						random = opt.random ? opt.random : RANDOM_XORSHIFT;
					}
				} else {
					random = RANDOM_NONE;
				}
				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) gutmann_bytes[n][j];
				}
				break;

			case WIPEMODE_DOE:
				n = (pass - 1) % doe_elements;
				byte_to_write = doe_bytes[n];
				if (byte_to_write < 0) {
					if (random == RANDOM_NONE) {
						random = opt.random ? opt.random : RANDOM_XORSHIFT;
					}
				} else {
					random = RANDOM_NONE;
				}
				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) byte_to_write;
				}
				break;

			case WIPEMODE_SCHNEIER:
				n = (pass - 1) % schneier_elements;
				byte_to_write = schneier_bytes[n];
				if (byte_to_write < 0) {
					if (random == RANDOM_NONE) {
						random = opt.random ? opt.random : RANDOM_XORSHIFT;
					}
				} else {
					random = RANDOM_NONE;
				}
				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) byte_to_write;
				}
				break;

			case WIPEMODE_BCI:
				n = (pass - 1) % bci_elements;
				byte_to_write = bci_bytes[n];
				if (byte_to_write < 0) {
					if (random == RANDOM_NONE) {
						random = opt.random ? opt.random : RANDOM_XORSHIFT;
					}
				} else {
					random = RANDOM_NONE;
				}
				for (j = 0; j < BYTES_PER_ELEMENT; ++j) {
					chars[j] = (unsigned char) byte_to_write;
				}
				break;

			case WIPEMODE_JEFFERY:
				for (unsigned int i = 0; i <= stats->bytes_per_sector * opt.sectors - wipe_elements; i += wipe_elements) {
					for (int j = 0; j < wipe_elements; ++j) {
						sector_data[i + j] = wipe_bytes[j];
					}
				}
				//YEC Low Level Format Write Buffer //20,44,65,63,61,59,47,20,
				break;

		}

		if (byte_to_write == CHECKDATAFLAG)
		{
			nCheckCount++;
			//check_device(hnd, last_sector, stats);
			continue;
		}

		char s_byte[5];

		switch (random) {
			case RANDOM_XORSHIFT:
				sprintf(s_byte, "xors");
				break;
			default:
				if (opt.mode != WIPEMODE_JEFFERY)
				{
					sprintf(s_byte, "0x%02x", byte_to_write);
					for (unsigned int i = 0; i <= stats->bytes_per_sector * opt.sectors - BYTES_PER_ELEMENT; i += BYTES_PER_ELEMENT) {
						for (int j = 0; j < BYTES_PER_ELEMENT; ++j) {
							sector_data[i + j] = chars[j];
						}
					}
				}
				else
				{//YEC Low Level Format Write Buffer //20,44,65,63,61,59,47,20,
					sprintf(s_byte, "llfm");
				}

		}

		unsigned long sectors_to_process = opt.sectors;

		int buf_sz, dio_tmp, first, blocks_per;
		int ret;
		int64_t seek = opt.start;
		int dio_incomplete = 0;
		for (int64_t sector = opt.start; sector <= opt.end; sector += opt.sectors)
		{
			if (sector + sectors_to_process > opt.end)
			{
				sectors_to_process = (unsigned long) (opt.end - sector);
				if (sectors_to_process == 0)
				{
					fprintf(stderr, "sector				=%" PRIx64 "\n", sector);
					break;
				}
			}

			if(random == RANDOM_XORSHIFT)
			{
				unsigned int i;
				for (i = 0; i < bytes_to_process; i+=sizeof(unsigned int))
				{
					*(unsigned int *)(&sector_data[i]) = xor128();
				}
			}



			uint64_t before_ticks = get_ticks(stats);
			if (FT_SG & out_type)
			{
				dio_tmp = oflag.dio;
				retries_tmp = oflag.retries;
				first = 1;
				while (1) {
					ret = sg_write(outfd, sector_data, sectors_to_process, seek, blk_sz,
								   &oflag, &dio_tmp);
					if (0 == ret)
						break;
					if ((SG_LIB_CAT_NOT_READY == ret) ||
						(SG_LIB_SYNTAX_ERROR == ret))
						break;
					else if ((-2 == ret) && first) {
						/* ENOMEM: find what's available and try that */
						if (ioctl(outfd, SG_GET_RESERVED_SIZE, &buf_sz) < 0) {
							perror("RESERVED_SIZE ioctls failed");
							break;
						}
						if (buf_sz < MIN_RESERVED_SIZE)
							buf_sz = MIN_RESERVED_SIZE;
						blocks_per = (buf_sz + blk_sz - 1) / blk_sz;
						if (blocks_per < sectors_to_process) {
							sectors_to_process = blocks_per;
							pr2serr("Reducing write to %d blocks per loop\n",
									(int)sectors_to_process);
						} else
							break;
					} else if ((SG_LIB_CAT_UNIT_ATTENTION == ret) && first) {
						if (--max_uas > 0)
							pr2serr("Unit attention, continuing (w)\n");
						else {
							pr2serr("Unit attention, too many (w)\n");
							break;
						}
					} else if ((SG_LIB_CAT_ABORTED_COMMAND == ret) && first) {
						if (--max_aborted > 0)
							pr2serr("Aborted command, continuing (w)\n");
						else {
							pr2serr("Aborted command, too many (w)\n");
							break;
						}
					} else if (ret < 0)
						break;
					else if (retries_tmp > 0) {
						pr2serr(">>> retrying a sgio write, lba=0x%" PRIx64 "\n",
								(uint64_t)opt.start);
						--retries_tmp;
						++num_retries;
						if (unrecovered_errs > 0)
							--unrecovered_errs;
					} else
						break;
					first = 0;
				}
				if (0 != ret) {
					pr2serr("sg_write failed,%s seek=%" PRId64 "\n",
							((-2 == ret) ? " try reducing bpt," : ""), seek);
					break;
				} else {
					out_full += sectors_to_process;
					if (oflag.dio && (0 == dio_tmp))
						dio_incomplete++;
				}
			}
			else if (FT_DEV_NULL & out_type)
			            out_full += sectors_to_process; /* act as if written out without error */

	        seek += sectors_to_process;

	        uint64_t after_ticks = get_ticks(stats);
	        stats->wiping_ticks += after_ticks - before_ticks;
	        stats->passwiping_ticks += after_ticks - before_ticks;

	        uint64_t seconds = (after_ticks - last_ticks);
	        if (seconds >= opt.refresh)
	        {
	        	last_ticks = after_ticks;
	        	print_stats(pass - nCheckCount, s_byte, sector, stats, opt.passes - CheckSumPasses);
	        	//print_stats(pass, s_byte, sector, stats, opt.passes);
	        }

		}
		print_stats(pass - nCheckCount, s_byte, opt.end, stats, opt.passes - CheckSumPasses);
		//print_stats(pass, s_byte, opt.end, stats, opt.passes);
	}
	free(sector_data);
	close(outfd);

	return res;
}

int
main(int argc, char * argv[])
{
	progname = basename(argv[0]);

	opterr = 0;
	int option_index = 0;
	optind = 1;

	while (true) {
		int c = getopt_long(argc, argv, short_options, long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
			case 'w':
				opt.mode = WIPEMODE_JEFFERY;
				char * pch = strtok(optarg, ",;");
				while (pch != NULL)
				{
					wipe_bytes[wipe_elements] = atoi(pch);
					wipe_elements++;
					pch = strtok(NULL, ",;");
				}
				if (wipe_elements == 0)
				{//20,44,65,63,61,59,47,20,YEC //4A 65 66 66 65 72 79 20
					BYTE llfdata[] = { 0x4A, 0x65, 0x66, 0x66, 0x65, 0x72, 0x79, 0x20 };
					for(; wipe_elements < sizeof(llfdata);)
					{
						wipe_bytes[wipe_elements] = llfdata[wipe_elements];
						wipe_elements++;
					}
				}
				break;
			case 'p': /* -p | --passes n  Wipe device n times (default is 1) */
				opt.passes = atoi(optarg);
				if (opt.passes == 0 || opt.passes >= 10000)
					usage(1);
				break;
			case 'd': /* -d | --dod       Wipe device using DoD 5220.22-M method (3 passes) */
				opt.mode = WIPEMODE_DOD;
				break;
			case 'D': /* -D | --dod7      Wipe device using DoD 5200.28-STD method (7 passes) */
				opt.mode = WIPEMODE_DOD7;
				break;
			case 'g': /* -g | --gutmann   Wipe device using Gutmann method (35 passes) */
				opt.mode = WIPEMODE_GUTMANN;
				break;
			case 'E': /* -E | --doe       Wipe device using US DoE method (3 passes) */
				opt.mode = WIPEMODE_DOE;
				break;
			case 'S': /* -S | --schneier  Wipe device using Bruce Schneier's method (7 passes) */
				opt.mode = WIPEMODE_SCHNEIER;
				break;
			case 'b': /* -b | --bci       Wipe device using German BCI/VSITR method (7 passes) */
				opt.mode = WIPEMODE_BCI;
				break;
			case 'y':
				opt.yes = true;
				break;
			case 'i':
				opt.ignore = true;
				break;
			case 'k':
				opt.kilobyte = true;
				break;
			case 'z':
				opt.refresh = atoi(optarg);
				break;
			case 'V':
				verbose = atoi(optarg);
				break;
			case 'n': /* -n | --sectors n Write n sectors at once (default is %d) */
				opt.sectors = atoi(optarg);
				if (opt.sectors == 0)
					usage(1);
				if (opt.sectors >= 0x100000)
					usage(1);
				break;
			case 's': // -s | --start   n Start at sector n (default is first sector)
				opt.start = strtoull(optarg, (char **)NULL, 10);
				if (opt.start == (int64_t) -1)
					usage(1);
				break;
			case 'e': // -e | --end     n End at sector n (default is last sector)
				opt.end = strtoull(optarg, (char **)NULL, 10);
				if (opt.end == 0)
					usage(1);
				break;
			case 'r': /* -r | --read      Only read the data on the device (DOES NOT WIPE!) */
				opt.read = true;
				break;
			case 'v': /* -v | --version   Show version and copyright information and quit */
				version();
				exit(0);
				break;
			case '?': /* -? | --help      Show this help message and quit */
				++opt.help;
				break;
			case ':':
				fprintf(stderr, "Option -%c requires an operand\n", optopt);
				break;
			default:
				usage(1);
		}
	}

	if (opt.help) {
		_usage();
		if (opt.help > 1) {
			examples();
		}
		exit(0);
	}

	int devices = 0;
	int bytes = 0;
	int i=0;
	for (i = optind; i < argc; ++i) {
		if (argv[i][0] == '/'&&argv[i][1] == 'd'&&argv[i][2] == 'e'&&argv[i][3] == 'v'&&argv[i][4] == '/') {
			++devices;
			continue;
		}
		++bytes;
	}

	if (devices == 0) {
		pr2serr("%s: No devices specified\n", progname);
		usage(1);
	}

	char **device	= (char **) malloc(sizeof(char *) * devices);
	int *byte		= NULL;

	if (bytes) {
		byte = (int *) malloc(sizeof(int) * bytes);
	}

	devices = 0;
	bytes = 0;
	for (i = optind; i < argc; ++i) {
		device[devices] = (char *) malloc((strlen(argv[i]) + 6) * sizeof(char));

		if (argv[i][0] == '/' &&
				argv[i][1] == 'd' &&
				argv[i][2] == 'e' &&
				argv[i][3] == 'v' &&
				argv[i][4] == '/') {
			strcpy(device[devices++], argv[i]);
			continue;
		}

		int byte_to_write = 0;
		while (1) {
			if (strncasecmp(argv[i], "0x", 2) == 0) {
				if (sscanf(argv[i], "%x", &byte_to_write) == 0) {
					usage(1);
				}
				break;
			}

			if (argv[i][0] == '0') {
				if (sscanf(argv[i], "%o", &byte_to_write) == 0) {
					usage(1);
				}
				break;
			}

			if (toupper(argv[i][0]) == 'R') {
				byte_to_write = -1;
				break;
			}

			if (toupper(argv[i][0]) == 'C') {
				byte_to_write = -2;
				break;
			}

			byte_to_write = atoi(argv[i]);

			if (byte_to_write > 0) {
				byte_to_write &= 0xff;
			}
			break;
		}
		byte[bytes++] = byte_to_write;
	}

	if (bytes == 0) {
		bytes = 1;
		byte = (int *) malloc(sizeof(int) * bytes);
		byte[0] = 0;
	}

	if (opt.passes == 0) {
		opt.passes = 1;
	}

	switch (opt.mode) {
		case WIPEMODE_NORMAL:
			opt.passes *= bytes;
			break;
		case WIPEMODE_DOD:
			opt.passes *= dod_elements;
			break;
		case WIPEMODE_DOD7:
			opt.passes *= dod7_elements;
			break;
		case WIPEMODE_GUTMANN:
			opt.passes *= gutmann_elements;
			break;
		case WIPEMODE_DOE:
			opt.passes *= doe_elements;
			break;
		case WIPEMODE_SCHNEIER:
			opt.passes *= schneier_elements;
			break;
		case WIPEMODE_BCI:
			opt.passes *= bci_elements;
			break;
		case WIPEMODE_JEFFERY:
			opt.passes *= 1;
			break;
		default:
			perror("Unimplemented wipe mode");
	}

	if (opt.passes >= 10000) {
		usage(1);
	}

    install_handler(SIGINT, interrupt_handler);
    install_handler(SIGQUIT, interrupt_handler);
    install_handler(SIGPIPE, interrupt_handler);
    install_handler(SIGUSR1, siginfo_handler);

	t_stats stats;
	stats.device_name = NULL;
	stats.bytes_per_sector = 0;
	printf("sg_lib_version: %s\n", sg_lib_version());

	time_t rawtime;
	struct tm* timeinfo;

	time ( &rawtime );
	timeinfo = localtime ( &rawtime );

	printf("Start Task local time and date: %s\n", asctime (timeinfo) );
    oflag.cdbsz = DEF_SCSI_CDBSZ;


	for (i = 0; i < devices; ++i) {
		wipe_device(device[i], bytes, byte, &stats);
	}

	time ( &rawtime );
	timeinfo = localtime ( &rawtime );

	printf("\end Task local time and date: %s", asctime (timeinfo) );

	return 0;
}
