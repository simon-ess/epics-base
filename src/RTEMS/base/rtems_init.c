/*************************************************************************\
* Copyright (c) 2002 The University of Saskatchewan
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/*
 * RTEMS startup task for EPICS
 *  $Id$
 *      Author: W. Eric Norum
 *              eric.norum@usask.ca
 *              (306) 966-5394
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rtems.h>
#include <rtems/libcsupport.h>
#include <rtems/error.h>
#include <rtems/stackchk.h>
#include <rtems/rtems_bsdnet.h>
#include <rtems/imfs.h>
#include <bsp.h>

#include <epicsThread.h>
#include <epicsTime.h>
#include <envDefs.h>
#include <errlog.h>
#include <logClient.h>
#include <osiUnistd.h>
#include <iocsh.h>

/*
 * Architecture-dependent routines
 */
#if defined(__mcpu32__) || defined(__mcf528x__)
static void
logReset (void)
{
    int bit, rsr;
    int i;
    const char *cp;
    char cbuf[80];

#if defined(__mcpu32__)
    rsr = m360.rsr;
    m360.rsr = ~0;
#else
    rsr = MCF5282_RESET_RSR;
#endif
    for (i = 0, bit = 0x80 ; bit != 0 ; bit >>= 1) {
        if (rsr & bit) {
            switch (bit) {
#if defined(__mcpu32__)
            case 0x80:  cp = "RESETH*";         break;
            case 0x40:  cp = "POWER-UP";        break;
            case 0x20:  cp = "WATCHDOG";        break;
            case 0x10:  cp = "DOUBLE FAULT";    break;
            case 0x04:  cp = "LOST CLOCK";      break;
            case 0x02:  cp = "RESET";           break;
            case 0x01:  cp = "RESETS*";         break;
#else
            case MCF5282_RESET_RSR_LVD:  cp = "Low-voltage detect"; break;
            case MCF5282_RESET_RSR_SOFT: cp = "Software reset";     break;
            case MCF5282_RESET_RSR_WDR:  cp = "Watchdog reset";     break;
            case MCF5282_RESET_RSR_POR:  cp = "Power-on reset";     break;
            case MCF5282_RESET_RSR_EXT:  cp = "External reset";     break;
            case MCF5282_RESET_RSR_LOC:  cp = "Loss of clock";      break;
            case MCF5282_RESET_RSR_LOL:  cp = "Loss of lock";       break;
#endif
            default:    cp = "??";              break;
            }
            i += sprintf (cbuf+i, cp); 
            rsr &= ~bit;
            if (rsr)
                i += sprintf (cbuf+i, ", "); 
            else
                break;
        }
    }
    errlogPrintf ("Startup after %s.\n", cbuf);
}

#else

static void
logReset (void)
{
    errlogPrintf ("Started.\n");
}
#endif

#ifdef __i386__
/*
 * Remote debugger support
 *
 * i386-rtems-gdb -b 38400 example(binary from EPICS build -- not netbootable image!)
 * (gdb) target remote /dev/ttyS0
 */
int enableRemoteDebugging = 0;  /* Global so gdb can set before download */
static void
initRemoteGdb(int ticksPerSecond)
{
    if (enableRemoteDebugging) {
        init_remote_gdb();
        rtems_task_wake_after(ticksPerSecond);
        breakpoint();
    }
}
#endif

/*
 ***********************************************************************
 *                         FATAL ERROR REPORTING                       *
 ***********************************************************************
 */
/*
 * Delay for a while, then terminate
 */
static void
delayedPanic (const char *msg)
{
    rtems_interval ticksPerSecond;

    rtems_clock_get (RTEMS_CLOCK_GET_TICKS_PER_SECOND, &ticksPerSecond);
    rtems_task_wake_after (ticksPerSecond);
    rtems_panic (msg);
}

/*
 * Log error and terminate
 */
void
LogFatal (const char *msg, ...)
{
    va_list ap;

    va_start (ap, msg);
    errlogVprintf (msg, ap);
    va_end (ap);
    delayedPanic (msg);
}

/*
 * Log RTEMS error and terminate
 */
void
LogRtemsFatal (const char *msg, rtems_status_code sc)
{
    errlogPrintf ("%s: %s\n", msg, rtems_status_text (sc));
    delayedPanic (msg);
}

/*
 * Log network error and terminate
 */
void
LogNetFatal (const char *msg, int err)
{
    errlogPrintf ("%s: %d\n", msg, err);
    delayedPanic (msg);
}

void *
mustMalloc(int size, const char *msg)
{
    void *p;

    if ((p = malloc (size)) == NULL)
        LogFatal ("Can't allocate space for %s.\n", msg);
    return p;
}

/*
 ***********************************************************************
 *                         REMOTE FILE ACCESS                          *
 ***********************************************************************
 */
#ifdef OMIT_NFS_SUPPORT
# include <rtems/tftp.h>
#endif

static int
initialize_local_filesystem(const char **argv)
{
    argv[0] = rtems_bsdnet_bootp_boot_file_name;

#if defined(__mcf528x__)
    extern char _DownloadLocation[], _edata[], _FlashBase[], _FlashSize[];
    unsigned long flashIndex = _edata - _DownloadLocation;
    char *header;

    header = _FlashBase + flashIndex;
    if (memcmp(header + 257, "ustar  ", 8) == 0) {
        int fd;
        printf ("***** Unpack in-memory file system (IMFS) *****\n");
        if (rtems_tarfs_load("/", header, (unsigned long)_FlashSize - flashIndex) != 0) {
            printf("Can't unpack tar filesystem\n");
            return 0;
        }
        if ((fd = open(rtems_bsdnet_bootp_cmdline, 0)) >= 0) {
            close(fd);
            printf ("***** Found startup script (%s) in IMFS *****\n", rtems_bsdnet_bootp_cmdline);
            argv[1] = rtems_bsdnet_bootp_cmdline;
            return 1;
        }
        printf ("***** Startup script (%s) not in IMFS *****\n", rtems_bsdnet_bootp_cmdline);
    }
#endif
    return 0;
}

static void
initialize_remote_filesystem(const char **argv, int hasLocalFilesystem)
{
#ifdef OMIT_NFS_SUPPORT
    printf ("***** Initializing TFTP *****\n");
    rtems_bsdnet_initialize_tftp_filesystem ();
    if (!hasLocalFilesystem) {
        char *path;
        int pathsize = 200;
        int l;

        path = mustMalloc(pathsize, "Command path name ");
        strcpy (path, "/TFTP/BOOTP_HOST/epics/");
        l = strlen (path);
        if (gethostname (&path[l], pathsize - l - 10) || (path[l] == '\0'))
            LogFatal ("Can't get host name");
        strcat (path, "/st.cmd");
        argv[1] = path;
    }
#else
    extern char *env_nfsServer;
    extern char *env_nfsPath;
    extern char *env_nfsMountPoint;
    char *server_name;
    char *server_path;
    char *mount_point;
    char *cp;
    int l = 0;

    printf ("***** Initializing NFS *****\n");
    rpcUdpInit();
    nfsInit(0,0);
    if (env_nfsServer && env_nfsPath && env_nfsMountPoint) {
        server_name = env_nfsServer;
        server_path = env_nfsPath;
        mount_point = env_nfsMountPoint;
        cp = mount_point;
        while ((cp = strchr(cp+1, '/')) != NULL) {
            *cp = '\0';
            if ((mkdir (mount_point, 0755) != 0)
             && (errno != EEXIST))
                LogFatal("Can't create directory \"%s\": %s.\n",
                                                mount_point, strerror(errno));
            *cp = '/';
        }
        argv[1] = rtems_bsdnet_bootp_cmdline;
    }
    else if (hasLocalFilesystem) {
        return;
    }
    else {
        /*
         * Use first component of nvram/bootp command line pathname
         * to set up initial NFS mount.  A "/tftpboot/" is prepended
         * if the pathname does not begin with a '/'.  This allows
         * NFS and TFTP to have a similar view of the remote system.
         */
        if (rtems_bsdnet_bootp_cmdline[0] == '/')
            cp = rtems_bsdnet_bootp_cmdline + 1;
        else
            cp = rtems_bsdnet_bootp_cmdline;
        cp = strchr(cp, '/');
        if ((cp == NULL)
         || ((l = cp - rtems_bsdnet_bootp_cmdline) == 0))
            LogFatal("\"%s\" is not a valid command pathname.\n", rtems_bsdnet_bootp_cmdline);
        cp = mustMalloc(l + 20, "NFS mount paths");
        server_path = cp;
        if (rtems_bsdnet_bootp_cmdline[0] == '/') {
            mount_point = server_path;
            strncpy(mount_point, rtems_bsdnet_bootp_cmdline, l);
            mount_point[l] = '\0';
            argv[1] = rtems_bsdnet_bootp_cmdline;
        }
        else {
            char *abspath = mustMalloc(strlen(rtems_bsdnet_bootp_cmdline)+2,"Absolute command path");
            strcpy(server_path, "/tftpboot/");
            mount_point = server_path + strlen(server_path);
            strncpy(mount_point, rtems_bsdnet_bootp_cmdline, l);
            mount_point[l] = '\0';
            mount_point--;
            strcpy(abspath, "/");
            strcat(abspath, rtems_bsdnet_bootp_cmdline);
            argv[1] = abspath;
        }
        server_name = rtems_bsdnet_bootp_server_name;
    }
    nfsMount(server_name, server_path, mount_point);
#endif
}

/*
 * Get to the startup script directory
 * The TFTP filesystem requires a trailing '/' on chdir arguments.
 */
static void
set_directory (const char *commandline)
{
    const char *cp;
    char *directoryPath;
    int l;

    cp = strrchr(commandline, '/');
    if (cp == NULL) {
        l = 0;
        cp = "/";
    }
    else {
        l = cp - commandline;
        cp = commandline;
    }
    directoryPath = mustMalloc(l + 2, "Command path directory ");
    strncpy(directoryPath, cp, l);
    directoryPath[l] = '/';
    directoryPath[l+1] = '\0';
    if (chdir (directoryPath) < 0)
        LogFatal ("Can't set initial directory(%s): %s\n", directoryPath, strerror(errno));
    free(directoryPath);
}

/*
 ***********************************************************************
 *                         RTEMS/EPICS COMMANDS                        *
 ***********************************************************************
 */
/*
 * RTEMS status
 */
static void
rtems_netstat (unsigned int level)
{
    rtems_bsdnet_show_if_stats ();
    rtems_bsdnet_show_mbuf_stats ();
    if (level >= 1) {
        rtems_bsdnet_show_inet_routes ();
    }
    if (level >= 2) {
        rtems_bsdnet_show_ip_stats ();
        rtems_bsdnet_show_icmp_stats ();
        rtems_bsdnet_show_udp_stats ();
        rtems_bsdnet_show_tcp_stats ();
    }
}

static const iocshArg netStatArg0 = { "level",iocshArgInt};
static const iocshArg * const netStatArgs[1] = {&netStatArg0};
static const iocshFuncDef netStatFuncDef = {"netstat",1,netStatArgs};
static void netStatCallFunc(const iocshArgBuf *args)
{
    rtems_netstat(args[0].ival);
}

static const iocshFuncDef stackCheckFuncDef = {"stackCheck",0,NULL};
static void stackCheckCallFunc(const iocshArgBuf *args)
{
    Stack_check_Dump_usage ();
}

static const iocshFuncDef heapSpaceFuncDef = {"heapSpace",0,NULL};
static void heapSpaceCallFunc(const iocshArgBuf *args)
{
    unsigned long n = malloc_free_space();

    if (n >= 1024*1000) {
        double x = (double)n / (1024 * 1024);
        printf("Heap space: %.1f MB\n", x);
    }
    else {
        double x = (double)n / 1024;
        printf("Heap space: %.1f kB\n", x);
    }
}

#ifndef OMIT_NFS_SUPPORT
static const iocshArg nfsMountArg0 = { "[uid.gid@]host",iocshArgString};
static const iocshArg nfsMountArg1 = { "server path",iocshArgString};
static const iocshArg nfsMountArg2 = { "mount point",iocshArgString};
static const iocshArg * const nfsMountArgs[3] = {&nfsMountArg0,&nfsMountArg1,
                                                 &nfsMountArg2};
static const iocshFuncDef nfsMountFuncDef = {"nfsMount",3,nfsMountArgs};
static void nfsMountCallFunc(const iocshArgBuf *args)
{
    char *cp = args[2].sval;
    while ((cp = strchr(cp+1, '/')) != NULL) {
        *cp = '\0';
        if ((mkdir (args[2].sval, 0755) != 0) && (errno != EEXIST)) {
            printf("Can't create directory \"%s\": %s.\n",
                                            args[2].sval, strerror(errno));
            return;
        }
        *cp = '/';
    }
    nfsMount(args[0].sval, args[1].sval, args[2].sval);
}
#endif

/*
 * Register RTEMS-specific commands
 */
static void iocshRegisterRTEMS (void)
{
    iocshRegister(&netStatFuncDef, netStatCallFunc);
    iocshRegister(&stackCheckFuncDef, stackCheckCallFunc);
    iocshRegister(&heapSpaceFuncDef, heapSpaceCallFunc);
#ifndef OMIT_NFS_SUPPORT
    iocshRegister(&nfsMountFuncDef, nfsMountCallFunc);
#endif
}

/*
 * Set up the console serial line (no handshaking)
 */
static void
initConsole (void)
{
    struct termios t;

    if (tcgetattr (fileno (stdin), &t) < 0) {
        printf ("tcgetattr failed: %s\n", strerror (errno));
        return;
    }
    t.c_iflag &= ~(IXOFF | IXON | IXANY);
    if (tcsetattr (fileno (stdin), TCSANOW, &t) < 0) {
        printf ("tcsetattr failed: %s\n", strerror (errno));
        return;
    }
}

/*
 * Ensure that the configuration object files
 * get pulled in from the library
 */
extern rtems_configuration_table Configuration;
extern struct rtems_bsdnet_config rtems_bsdnet_config;
const void *rtemsConfigArray[] = {
    &Configuration,
    &rtems_bsdnet_config
};

/*
 * Hook to ensure that BSP cleanup code gets run on exit
 */
static void
exitHandler(void)
{
    rtems_shutdown_executive(0);
}

/*
 * RTEMS Startup task
 */
rtems_task
Init (rtems_task_argument ignored)
{
    int i;
    const char *argv[3] = { NULL, NULL, NULL };
    rtems_interval ticksPerSecond;
    rtems_task_priority newpri;
    rtems_status_code sc;
    rtems_time_of_day now;

    /*
     * Get configuration
     */
    rtems_clock_get (RTEMS_CLOCK_GET_TICKS_PER_SECOND, &ticksPerSecond);

    /*
     * Explain why we're here
     */
    logReset();

    /*
     * Architecture-specific hooks
     */
#if defined(__i386__)
    initRemoteGdb(ticksPerSecond);
#endif
    if (rtems_bsdnet_config.bootp == NULL) {
        extern void setBootConfigFromNVRAM(void);
        setBootConfigFromNVRAM();
    }

    /*
     * Override RTEMS configuration
     */
    rtems_task_set_priority (
                     RTEMS_SELF,
                     epicsThreadGetOssPriorityValue(epicsThreadPriorityIocsh),
                     &newpri);

    /*
     * Create a reasonable environment
     */
    initConsole ();
    putenv ("TERM=xterm");
    putenv ("IOCSH_HISTSIZE=20");
    if (rtems_bsdnet_config.hostname) {
        char *cp = mustMalloc(strlen(rtems_bsdnet_config.hostname)+3, "iocsh prompt");
        sprintf(cp, "%s> ", rtems_bsdnet_config.hostname);
        epicsEnvSet ("IOCSH_PS1", cp);
        epicsEnvSet("IOC_NAME", rtems_bsdnet_config.hostname);
    }
    else {
        epicsEnvSet ("IOCSH_PS1", "epics> ");
        epicsEnvSet ("IOC_NAME", ":UnnamedIoc:");
    }

    /*
     * Start network
     */
    if (rtems_bsdnet_config.network_task_priority == 0) {
        unsigned int p;
        if (epicsThreadHighestPriorityLevelBelow(epicsThreadPriorityScanLow, &p)
                                            == epicsThreadBooleanStatusSuccess)
            rtems_bsdnet_config.network_task_priority = epicsThreadGetOssPriorityValue(p);
    }
    printf("\n***** Initializing network *****\n");
    rtems_bsdnet_initialize_network();
    initialize_remote_filesystem(argv, initialize_local_filesystem(argv));

    /*
     * Use BSP-supplied time of day if available
     */
    if (rtems_clock_get(RTEMS_CLOCK_GET_TOD,&now) != RTEMS_SUCCESSFUL) {
        for (i = 0 ; ; i++) {
            printf ("***** Initializing NTP *****\n");
            if (rtems_bsdnet_synchronize_ntp (0, 0) >= 0)
                break;
            rtems_task_wake_after (5*ticksPerSecond);
            if (i >= 12) {
                printf ("    *************** WARNING ***************\n");
                printf ("    ***** NO RESPONSE FROM NTP SERVER *****\n");
                printf ("    *****  TIME SET TO DEFAULT VALUE  *****\n");
                printf ("    ***************************************\n");
                now.year = 2001;
                now.month = 1;
                now.day = 1;
                now.hour = 0;
                now.minute = 0;
                now.second = 0;
                now.ticks = 0;
                if ((sc = rtems_clock_set (&now)) != RTEMS_SUCCESSFUL)
                    printf ("***** Can't set time: %s\n", rtems_status_text (sc));
                break;
            }
        }
    }
    if (getenv("TZ") == NULL) {
        const char *tzp = envGetConfigParamPtr(&EPICS_TIMEZONE);
        if (tzp == NULL) {
            printf("Warning -- no timezone information available -- times will be displayed as GMT.\n");
        }
        else {
            char tz[10];
            int minWest, toDst = 0, fromDst = 0;
            if(sscanf(tzp, "%9[^:]::%d:%d:%d", tz, &minWest, &toDst, &fromDst) < 2) {
                printf("Warning: EPICS_TIMEZONE (%s) unrecognizable -- times will be displayed as GMT.\n", tzp);
            }
            else {
                char posixTzBuf[40];
                char *p = posixTzBuf;
                p += sprintf(p, "%cST %d:%d", tz[0], minWest/60, minWest%60);
                if (toDst != fromDst)
                    p += sprintf(p, " %cDT", tz[0]);
                epicsEnvSet("TZ", posixTzBuf);
            }
        }
    }
    tzset();

    /*
     * Run the EPICS startup script
     */
    printf ("***** Starting EPICS application *****\n");
    iocshRegisterRTEMS ();
    set_directory (argv[1]);
    epicsEnvSet ("IOC_STARTUP_SCRIPT", argv[1]);
    atexit(exitHandler);
    i = main ((sizeof argv / sizeof argv[0]) - 1, argv);
    printf ("***** IOC application terminating *****\n");
    epicsThreadSleep(1.0);
    epicsExit(0);
}