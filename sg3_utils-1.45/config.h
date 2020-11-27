/* config.h.  Generated from config.h.in by configure.  */
/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Define to 1 if you have the <byteswap.h> header file. */
#define HAVE_BYTESWAP_H 1

/* Define to 1 if you have the `clock_gettime' function. */
#define HAVE_CLOCK_GETTIME 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the `getopt_long' function. */
#define HAVE_GETOPT_LONG 1

/* Define to 1 if you have the `gettimeofday' function. */
#define HAVE_GETTIMEOFDAY 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <linux/bsg.h> header file. */
#define HAVE_LINUX_BSG_H 1

/* Define to 1 if you have the <linux/kdev_t.h> header file. */
#define HAVE_LINUX_KDEV_T_H 1

/* Define to 1 if you have the <linux/nvme_ioctl.h> header file. */
#define HAVE_LINUX_NVME_IOCTL_H 1

/* Have Linux sg v4 header */
/* #undef HAVE_LINUX_SG_V4_HDR */

/* Define to 1 if you have the <linux/types.h> header file. */
#define HAVE_LINUX_TYPES_H 1

/* Define to 1 if you have the `lseek64' function. */
#define HAVE_LSEEK64 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Found NVMe */
#define HAVE_NVME 1

/* Define to 1 if you have the `posix_fadvise' function. */
#define HAVE_POSIX_FADVISE 1

/* Define to 1 if you have the `posix_memalign' function. */
#define HAVE_POSIX_MEMALIGN 1

/* Define to 1 if you have the `pthread_cancel' function. */
#define HAVE_PTHREAD_CANCEL 1

/* Define to 1 if you have the `pthread_kill' function. */
#define HAVE_PTHREAD_KILL 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the `sysconf' function. */
#define HAVE_SYSCONF 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* use generic little-endian/big-endian instead */
/* #undef IGNORE_FAST_LEBE */

/* option ignored */
/* #undef IGNORE_LINUX_BSG */

/* even if Linux sg v4 available, use v3 instead */
/* #undef IGNORE_LINUX_SGV4 */

/* compile out NVMe support */
/* #undef IGNORE_NVME */

/* Define to the sub-directory where libtool stores uninstalled libraries. */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "sg3_utils"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "dgilbert@interlog.com"

/* Define to the full name of this package. */
#define PACKAGE_NAME "sg3_utils"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "sg3_utils 1.45"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "sg3_utils"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.45"

/* sg3_utils on android */
/* #undef SG_LIB_ANDROID */

/* sg3_utils Build Host */
#define SG_LIB_BUILD_HOST "x86_64-pc-linux-gnu"

/* sg3_utils on FreeBSD */
/* #undef SG_LIB_FREEBSD */

/* sg3_utils on linux */
#define SG_LIB_LINUX 1

/* also MinGW environment */
/* #undef SG_LIB_MINGW */

/* sg3_utils on Tru64 UNIX */
/* #undef SG_LIB_OSF1 */

/* sg3_utils on Solaris */
/* #undef SG_LIB_SOLARIS */

/* sg3_utils on Win32 */
/* #undef SG_LIB_WIN32 */

/* full SCSI sense strings and NVMe status strings */
#define SG_SCSI_STRINGS 1

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "1.45"

/* enable Win32 SPT Direct */
/* #undef WIN32_SPT_DIRECT */
