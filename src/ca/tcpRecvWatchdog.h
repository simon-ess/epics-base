/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/

/*  
 *
 *                              
 *                    L O S  A L A M O S
 *              Los Alamos National Laboratory
 *               Los Alamos, New Mexico 87545
 *                                  
 *  Copyright, 1986, The Regents of the University of California.
 *                                  
 *           
 *	Author Jeffrey O. Hill
 *	johill@lanl.gov
 *	505 665 1831
 */

#ifndef tcpRecvWatchdogh  
#define tcpRecvWatchdogh

#include "epicsTimer.h"

class tcpiiu;

class tcpRecvWatchdog : private epicsTimerNotify {
public:
    tcpRecvWatchdog ( epicsMutex & cbMutex, 
        cacContextNotify & ctxNotify,
        epicsMutex & mutex, tcpiiu &, 
        double periodIn, epicsTimerQueue & );
    virtual ~tcpRecvWatchdog ();
    void sendBacklogProgressNotify (
        epicsGuard < epicsMutex > &,
        const epicsTime & currentTime );
    void messageArrivalNotify (
        epicsGuard < epicsMutex > & guard,
        const epicsTime & currentTime );
    void probeResponseNotify ( 
        epicsGuard < epicsMutex > &,
        const epicsTime & currentTime );
    void beaconArrivalNotify ( 
        epicsGuard < epicsMutex > &,
        const epicsTime & currentTime );
    void beaconAnomalyNotify ( epicsGuard < epicsMutex > & );
    void connectNotify (
        epicsGuard < epicsMutex > & );
    void sendTimeoutNotify (
        epicsGuard < epicsMutex > & cbGuard,
        epicsGuard < epicsMutex > & guard,
        const epicsTime & currentTime );
    void cancel ();
    void shutdown ();
    void show ( unsigned level ) const;
    double delay () const;
private:
    const double period;
    epicsTimer & timer;
    epicsMutex & cbMutex;
    cacContextNotify & ctxNotify;
    epicsMutex & mutex;
    tcpiiu & iiu;
    bool probeResponsePending;
    bool beaconAnomaly;
    bool probeTimeoutDetected;
    bool shuttingDown;
    expireStatus expire ( const epicsTime & currentTime );
	tcpRecvWatchdog ( const tcpRecvWatchdog & );
	tcpRecvWatchdog & operator = ( const tcpRecvWatchdog & );
};

#endif // #ifndef tcpRecvWatchdogh
