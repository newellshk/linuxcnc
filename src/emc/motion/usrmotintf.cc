/********************************************************************
* Description: usrmotintf.cc
*   Defs for interface functions (init, exit, read, write) for user
*   processes which communicate with the real-time motion controller
*   in emcmot.c
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
* $Revision$
* $Author$
* $Date$
********************************************************************/

#include "config.h"     	/* LINELEN definition */
#include <stdlib.h>		/* exit() */
#include <sys/stat.h>
#include <string.h>		/* memcpy() */
#include <float.h>		/* DBL_MIN */
#include "motion.h"		/* emcmot_status_t,CMD */
#include "emcmotcfg.h"		/* EMCMOT_ERROR_NUM,LEN */
#include "emcmotglb.h"		/* SHMEM_KEY */
#include "usrmotintf.h"		/* these decls */
#include "_timer.h"

#include "inifile.hh"

#define READ_TIMEOUT_SEC 0	/* seconds for timeout */
#define READ_TIMEOUT_USEC 100000	/* microseconds for timeout */

#include "rtapi.h"

static int inited = 0;		/* flag if inited */

static emcmot_command_t *emcmotCommand = 0;
static emcmot_status_t *emcmotStatus = 0;
static emcmot_config_t *emcmotConfig = 0;
static emcmot_debug_t *emcmotDebug = 0;
static emcmot_error_t *emcmotError = 0;
static emcmot_struct_t *emcmotStruct = 0;
emcmot_struct_t *emcmotshmem = NULL;	// Shared memory base address.

/* usrmotIniLoad() loads params (SHMEM_KEY, COMM_TIMEOUT, COMM_WAIT)
   from named ini file */
int usrmotIniLoad(const char *filename)
{
    const char *inistring;
    int saveInt;
    double saveDouble;
    Inifile *inifile;

    inifile = new Inifile(filename);
    /* open it */
    if (inifile->valid() == false) {
	rtapi_print("can't find emcmot ini file %s\n", filename);
	return -1;
    }

    saveInt = SHMEM_KEY;
    if (NULL != (inistring = inifile->find("SHMEM_KEY", "EMCMOT"))) {
	if (1 == sscanf(inistring, "%i", &SHMEM_KEY)) {
	    /* found it */
	} else {
	    /* found, but invalid */
	    SHMEM_KEY = saveInt;
	    rtapi_print
		("invalid [EMCMOT] SHMEM_KEY in %s (%s); using default %d\n",
		filename, inistring, SHMEM_KEY);
	}
    } else {
	/* not found, using default */
	rtapi_print("[EMCMOT] SHMEM_KEY not found in %s; using default %d\n",
	    filename, SHMEM_KEY);
    }
    saveDouble = EMCMOT_COMM_TIMEOUT;
    if (NULL != (inistring = inifile->find("COMM_TIMEOUT", "EMCMOT"))) {
	if (1 == sscanf(inistring, "%lf", &EMCMOT_COMM_TIMEOUT)) {
	    /* found it */
	} else {
	    /* found, but invalid */
	    EMCMOT_COMM_TIMEOUT = saveDouble;
	    rtapi_print
		("invalid [EMCMOT] COMM_TIMEOUT in %s (%s); using default %f\n",
		filename, inistring, EMCMOT_COMM_TIMEOUT);
	}
    } else {
	/* not found, using default */
	rtapi_print
	    ("[EMCMOT] COMM_TIMEOUT not found in %s; using default %f\n",
	    filename, EMCMOT_COMM_TIMEOUT);
    }

    saveDouble = EMCMOT_COMM_WAIT;
    if (NULL != (inistring = inifile->find("COMM_WAIT", "EMCMOT"))) {
	if (1 == sscanf(inistring, "%lf", &EMCMOT_COMM_WAIT)) {
	    /* found it */
	} else {
	    /* found, but invalid */
	    EMCMOT_COMM_WAIT = saveDouble;
	    rtapi_print
		("invalid [EMCMOT] COMM_WAIT in %s (%s); using default %f\n",
		filename, inistring, EMCMOT_COMM_WAIT);
	}
    } else {
	/* not found, using default */
	rtapi_print("[EMCMOT] COMM_WAIT not found in %s; using default %f\n",
	    filename, EMCMOT_COMM_WAIT);
    }

    delete inifile;

    return 0;
}

/* writes command from c */
int usrmotWriteEmcmotCommand(emcmot_command_t * c)
{
    emcmot_status_t s;
    static int commandNum = 0;
    static unsigned char headCount = 0;
    double end;

    c->head = ++headCount;
    c->tail = c->head;
    c->commandNum = ++commandNum;

    /* check for mapped mem still around */
    if (0 == emcmotCommand) {
	return EMCMOT_COMM_ERROR_CONNECT;
    }
    /* copy entire command structure to shared memory */
    *emcmotCommand = *c;
    /* poll for receipt of command */
    /* set timeout for comm failure, now + timeout */
    end = etime() + EMCMOT_COMM_TIMEOUT;
    /* now check to see if it got it */
    while (etime() < end) {
	/* update status */
	if (( usrmotReadEmcmotStatus(&s) == 0 ) && ( s.commandNumEcho == commandNum )) {
	    /* now check emcmot status flag */
	    if (s.commandStatus == EMCMOT_COMMAND_OK) {
		return EMCMOT_COMM_OK;
	    } else {
		return EMCMOT_COMM_ERROR_COMMAND;
	    }
	}
    }
    return EMCMOT_COMM_ERROR_TIMEOUT;
}

/* copies status to s */
int usrmotReadEmcmotStatus(emcmot_status_t * s)
{
    int split_read_count;
    
    /* check for shmem still around */
    if (0 == emcmotStatus) {
	return EMCMOT_COMM_ERROR_CONNECT;
    }
    split_read_count = 0;
    do {
	/* copy status struct from shmem to local memory */
	memcpy(s, emcmotStatus, sizeof(emcmot_status_t));
	/* got it, now check head-tail matche */
	if (s->head == s->tail) {
	    /* head and tail match, done */
	    return EMCMOT_COMM_OK;
	}
	/* inc counter and try again, max three times */
    } while ( ++split_read_count < 3 );
fprintf(stderr, "%%" ); fflush(stderr);
    return EMCMOT_COMM_SPLIT_READ_TIMEOUT;
}

/* copies config to s */
int usrmotReadEmcmotConfig(emcmot_config_t * s)
{
    int split_read_count;
    
    /* check for shmem still around */
    if (0 == emcmotConfig) {
	return EMCMOT_COMM_ERROR_CONNECT;
    }
    split_read_count = 0;
    do {
	/* copy config struct from shmem to local memory */
	memcpy(s, emcmotConfig, sizeof(emcmot_config_t));
	/* got it, now check head-tail matches */
	if (s->head == s->tail) {
	    /* head and tail match, done */
	    return EMCMOT_COMM_OK;
	}
	/* inc counter and try again, max three times */
    } while ( ++split_read_count < 3 );
printf("ReadEmcmotConfig COMM_SPLIT_READ_TIMEOUT\n" );
    return EMCMOT_COMM_SPLIT_READ_TIMEOUT;
}

/* copies debug to s */
int usrmotReadEmcmotDebug(emcmot_debug_t * s)
{
    int split_read_count;
    
    /* check for shmem still around */
    if (0 == emcmotDebug) {
	return EMCMOT_COMM_ERROR_CONNECT;
    }
    split_read_count = 0;
    do {
	/* copy debug struct from shmem to local memory */
	memcpy(s, emcmotDebug, sizeof(emcmot_debug_t));
	/* got it, now check head-tail matches */
	if (s->head == s->tail) {
	    /* head and tail match, done */
	    return EMCMOT_COMM_OK;
	}
	/* inc counter and try again, max three times */
    } while ( ++split_read_count < 3 );
printf("ReadEmcmotDebug COMM_SPLIT_READ_TIMEOUT\n" );
    return EMCMOT_COMM_SPLIT_READ_TIMEOUT;
}

/* copies error to s */
int usrmotReadEmcmotError(char *e)
{
    /* check to see if ptr still around */
    if (emcmotError == 0) {
	return -1;
    }

    /* returns 0 if something, -1 if not */
    return emcmotErrorGet(emcmotError, e);
}

/*
 htostr()

 converts short int to 0-1 style string, in s. Assumes a short is 2 bytes.
*/
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - don't know if this is still needed 
				 */

static int htostr(char *s, unsigned short h)
{
    int t;

    for (t = 15; t >= 0; t--) {
	s[t] = h % 2 ? '1' : '0';
	h >>= 1;
    }
    s[16] = 0;			/* null terminate */

    return 0;
}
#endif

void printEmcPose(EmcPose * pose)
{
    printf("x=%f\ty=%f\tz=%f\ta=%f\tb=%f\tc=%f",
	pose->tran.x, pose->tran.y, pose->tran.z, pose->a, pose->b, pose->c);
}

void printTPstruct(TP_STRUCT * tp)
{
    printf("queueSize=%d\n", tp->queueSize);
    printf("cycleTime=%f\n", tp->cycleTime);
    printf("vMax=%f\n", tp->vMax);
    printf("vScale=%f\n", tp->vScale);
    printf("aMax=%f\n", tp->aMax);
    printf("vLimit=%f\n", tp->vLimit);
    printf("wMax=%f\n", tp->wMax);
    printf("wDotMax=%f\n", tp->wDotMax);
    printf("nextId=%d\n", tp->nextId);
    printf("execId=%d\n", tp->execId);
    printf("termCond=%d\n", tp->termCond);
    printf("currentPos :");
    printEmcPose(&tp->currentPos);
    printf("\n");
    printf("goalPos :");
    printEmcPose(&tp->goalPos);
    printf("\n");
    printf("done=%d\n", tp->done);
    printf("depth=%d\n", tp->depth);
    printf("activeDepth=%d\n", tp->activeDepth);
    printf("aborting=%d\n", tp->aborting);
    printf("pausing=%d\n", tp->pausing);
}

void usrmotPrintEmcmotDebug(emcmot_debug_t d, int which)
{
//    int t;

    printf("running time: \t%f\n", d.running_time);
    switch (which) {
    case 0:
	printf("split:        \t%d\n", d.split);
	printf("teleop desiredVel: \t%f\t%f\t%f\t%f\t%f\t%f\n",
	    d.teleop_data.desiredVel.tran.x,
	    d.teleop_data.desiredVel.tran.y,
	    d.teleop_data.desiredVel.tran.z,
	    d.teleop_data.desiredVel.a,
	    d.teleop_data.desiredVel.b, d.teleop_data.desiredVel.c);
	printf("teleop currentVel: \t%f\t%f\t%f\t%f\t%f\t%f\n",
	    d.teleop_data.currentVel.tran.x,
	    d.teleop_data.currentVel.tran.y,
	    d.teleop_data.currentVel.tran.z,
	    d.teleop_data.currentVel.a,
	    d.teleop_data.currentVel.b, d.teleop_data.currentVel.c);
	printf("teleop desiredAccell: \t%f\t%f\t%f\t%f\t%f\t%f\n",
	    d.teleop_data.desiredAccell.tran.x,
	    d.teleop_data.desiredAccell.tran.y,
	    d.teleop_data.desiredAccell.tran.z,
	    d.teleop_data.desiredAccell.a,
	    d.teleop_data.desiredAccell.b, d.teleop_data.desiredAccell.c);
	printf("teleop currentAccell: \t%f\t%f\t%f\t%f\t%f\t%f\n",
	    d.teleop_data.currentAccell.tran.x,
	    d.teleop_data.currentAccell.tran.y,
	    d.teleop_data.currentAccell.tran.z,
	    d.teleop_data.currentAccell.a,
	    d.teleop_data.currentAccell.b, d.teleop_data.currentAccell.c);
	break;
/*! \todo Another #if 0 */
#if 0
	printf("\nferror:        ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", d.ferrorCurrent[t]);
	}
	printf("\n");

	printf("\nferror High:        ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", d.ferrorHighMark[t]);
	}
	printf("\n");
	break;
#endif
    case 5:
	printf("traj  m/m/a:\t%f\t%f\t%f\n", d.tMin, d.tMax, d.tAvg);
	printf("tMmxavg : sum=%f, in=%d, size=%d, index=%d\n",
	    d.tMmxavg.sum, d.tMmxavg.in, d.tMmxavg.size, d.tMmxavg.index);
/*! \todo Another #if 0 */
#if 0
	if (d.tMmxavg.in > 0 && d.tMmxavg.size > 0) {
	    printf("tMmxavg : nums \n\t ");
	    for (t = 0; t < d.tMmxavg.in && t < d.tMmxavg.size; t++) {
		printf("%3.3e \t", d.tMmxavgSpace[t]);
		if (t % 8 == 0 && t > 0) {
		    printf("\n\t ");
		}
	    }
	}
#endif
	printf("\n");
	printf("servo m/m/a:\t%f\t%f\t%f\n", d.sMin, d.sMax, d.sAvg);
	printf("sMmxavg : sum=%f, in=%d, size=%d, index=%d\n",
	    d.sMmxavg.sum, d.sMmxavg.in, d.sMmxavg.size, d.sMmxavg.index);

/*! \todo Another #if 0 */
#if 0
	if (d.sMmxavg.in > 0 && d.sMmxavg.size > 0) {
	    printf("sMmxavg : nums \n\t ");
	    for (t = 0; t < d.sMmxavg.in && t < d.sMmxavg.size; t++) {
		printf("%3.3e \t", d.sMmxavgSpace[t]);
		if (t % 8 == 0 && t > 0) {
		    printf("\n\t ");
		}
	    }
	}
#endif
	printf("\n");
	printf("(off) m/m/a:\t%f\t%f\t%f\n", d.nMin, d.nMax, d.nAvg);
	printf("nMmxavg : sum=%f, in=%d, size=%d, index=%d\n",
	    d.nMmxavg.sum, d.nMmxavg.in, d.nMmxavg.size, d.nMmxavg.index);
/*! \todo Another #if 0 */
#if 0
	if (d.nMmxavg.in > 0 && d.nMmxavg.size > 0) {
	    printf("nMmxavg : nums \n\t ");
	    for (t = 0; t < d.nMmxavg.in && t < d.nMmxavg.size; t++) {
		printf("%3.3e \t", d.nMmxavgSpace[t]);
		if (t % 8 == 0 && t > 0) {
		    printf("\n\t ");
		}
	    }
	}
#endif
	printf("\n");

	printf("(cycle to cycle  time) m/m/a:\t%f\t%f\t%f\n", d.yMin, d.yMax,
	    d.yAvg);
	printf("yMmxavg : sum=%f, in=%d, size=%d, index=%d\n", d.yMmxavg.sum,
	    d.yMmxavg.in, d.yMmxavg.size, d.yMmxavg.index);
/*! \todo Another #if 0 */
#if 0
	if (d.yMmxavg.in > 0 && d.yMmxavg.size > 0) {
	    printf("nMmxavg : nums \n\t ");
	    for (t = 0; t < d.yMmxavg.in && t < d.yMmxavg.size; t++) {
		printf("%3.3e \t", d.yMmxavgSpace[t]);
		if (t % 8 == 0 && t > 0) {
		    printf("\n\t ");
		}
	    }
	}
#endif

	printf("\n");

	printf("(frequency compute  time) m/m/a:\t%f\t%f\t%f\n", d.fMin,
	    d.fMax, d.fAvg);
	printf("fMmxavg : sum=%f, in=%d, size=%d, index=%d\n", d.fMmxavg.sum,
	    d.fMmxavg.in, d.fMmxavg.size, d.fMmxavg.index);
/*! \todo Another #if 0 */
#if 0
	if (d.fMmxavg.in > 0 && d.fMmxavg.size > 0) {
	    printf("nMmxavg : nums \n\t ");
	    for (t = 0; t < d.fMmxavg.in && t < d.fMmxavg.size; t++) {
		printf("%3.3e \t", d.fMmxavgSpace[t]);
		if (t % 8 == 0 && t > 0) {
		    printf("\n\t ");
		}
	    }
	}
#endif

	printf("\n");

	printf("(frequecy cycle to cycle  time) m/m/a:\t%f\t%f\t%f\n",
	    d.fyMin, d.fyMax, d.fyAvg);
	printf("fyMmxavg : sum=%f, in=%d, size=%d, index=%d\n",
	    d.fyMmxavg.sum, d.fyMmxavg.in, d.fyMmxavg.size, d.fyMmxavg.index);
/*! \todo Another #if 0 */
#if 0
	if (d.fyMmxavg.in > 0 && d.fyMmxavg.size > 0) {
	    printf("nMmxavg : nums \n\t ");
	    for (t = 0; t < d.fyMmxavg.in && t < d.fyMmxavg.size; t++) {
		printf("%3.3e \t", d.fyMmxavgSpace[t]);
		if (t % 8 == 0 && t > 0) {
		    printf("\n\t ");
		}
	    }
	}
#endif

	printf("\n");
	break;

    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
//      printf("jointPos[%d]: %f\n", which - 6, d.jointPos[(which - 6)]);
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	printf("coarseJointPos[%d]: %f\n",
	    which - 6, d.coarseJointPos[(which - 6)]);
	printf("jointVel[%d]: %f\n", which - 6, d.jointVel[(which - 6)]);
	printf("rawInput[%d]: %f\n", which - 6, d.rawInput[(which - 6)]);
	printf("rawOutput[%d]: %f\n", which - 6, d.rawOutput[(which - 6)]);
#endif
//      printf("bcompincr[%d]: %f\n", which - 6, d.bcompincr[(which - 6)]);
	break;

    case 12:
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	printf("\noldInput:  ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", d.oldInput[t]);
	}
	printf("\nrawInput:  ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", d.rawInput[t]);
	}
	printf("\ninverseInputScale:  ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", d.inverseInputScale[t]);
	}
#endif
	printf("\n");

    default:
	break;
    }

}

void usrmotPrintEmcmotConfig(emcmot_config_t c, int which)
{
//    int t;
//    char m[32];

    switch (which) {
    case 0:
	printf("debug level   \t%d\n", c.debug);
	printf("traj time:    \t%f\n", c.trajCycleTime);
	printf("servo time:   \t%f\n", c.servoCycleTime);
	printf("interp rate:  \t%d\n", c.interpolationRate);
	printf("v limit:      \t%f\n", c.limitVel);
	printf("axis vlimit:  \t");
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - waiting for new structs */
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%f ", c.axisLimitVel[t]);
	}
	printf("\n");
	printf("axis acc: \t");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%f ", c.axisLimitAcc[t]);
	}
	printf("\n");
#endif
	printf("\n");
	break;

    case 1:
	printf("pid stuff is obsolete\n");
/*! \todo Another #if 0 */
#if 0
	printf
	    ("pid:\tP\tI\tD\tFF0\tFF1\tFF2\tBCKLSH\tBIAS\tMAXI\tDEADBAND\tCYCLE TIME\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf
		("\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%f\t%f\n",
		c.pid[t].p, c.pid[t].i, c.pid[t].d, c.pid[t].ff0,
		c.pid[t].ff1, c.pid[t].ff2, c.pid[t].backlash, c.pid[t].bias,
		c.pid[t].maxError, c.pid[t].deadband, c.pid[t].cycleTime);
	}
	printf("\n");
#endif
	break;

    case 3:
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - waiting for new structs */
	printf("pos limits:   ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", c.maxLimit[t]);
	}

	printf("\nneg limits:   ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", c.minLimit[t]);
	}

	printf("\nmax ferror:   ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", c.maxFerror[t]);
	}
	printf("\n");

	printf("\nmin ferror:   ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", c.minFerror[t]);
	}
	printf("\n");

	printf("\nhome offsets:  ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", c.homeOffset[t]);
	}
	printf("\n");
#endif
	break;

    default:
	break;
    }

}

/* status printing function */
void usrmotPrintEmcmotStatus(emcmot_status_t s, int which)
{
//    int t;
//    char m[32];

    switch (which) {
    case 0:
	printf("mode:         \t%s\n",
	    s.motionFlag & EMCMOT_MOTION_TELEOP_BIT ? "teleop" :
	    (s.motionFlag & EMCMOT_MOTION_COORD_BIT ? "coord" : "free")
	    );
	printf("cmd:          \t%d\n", s.commandEcho);
	printf("cmd num:      \t%d\n", s.commandNumEcho);
	printf("heartbeat:    \t%u\n", s.heartbeat);
	printf("compute time: \t%f\n", s.computeTime);
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	printf("axes enabled: \t");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d", s.axisFlag[t] & EMCMOT_AXIS_ENABLE_BIT ? 1 : 0);
	}
	printf("\n");
#endif
	printf("cmd pos:      \t%f\t%f\t%f\t%f\t%f\t%f\n",
	    s.carte_pos_cmd.tran.x, s.carte_pos_cmd.tran.y,
	    s.carte_pos_cmd.tran.z, s.carte_pos_cmd.a, s.carte_pos_cmd.b,
	    s.carte_pos_cmd.c);
	printf("act pos:      \t%f\t%f\t%f\t%f\t%f\t%f\n",
	    s.carte_pos_fb.tran.x, s.carte_pos_fb.tran.y,
	    s.carte_pos_fb.tran.z, s.carte_pos_fb.a, s.carte_pos_fb.b,
	    s.carte_pos_fb.c);
	printf("joint data:\n");
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	printf(" cmd: ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.joint_pos_cmd[t]);
	}
	printf("\n");
	printf(" fb:  ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.joint_pos_fb[t]);
	}
	printf("\n");
	printf(" vel: ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.joint_vel_cmd[t]);
	}
	printf("\n");
	printf(" ferr:");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.ferrorCurrent[t]);
	}
	printf("\n");
	printf(" lim:");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.ferrorLimit[t]);
	}
	printf("\n");
	printf(" max:");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.ferrorHighMark[t]);
	}
	printf("\n");
#endif
	printf("velocity:     \t%f\n", s.vel);
	printf("accel:        \t%f\n", s.acc);
	printf("id:           \t%d\n", s.id);
	printf("depth:        \t%d\n", s.depth);
	printf("active depth: \t%d\n", s.activeDepth);
	printf("inpos:        \t%d\n",
	    s.motionFlag & EMCMOT_MOTION_INPOS_BIT ? 1 : 0);
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	printf("vscales:      \tQ: %.2f", s.qVscale);
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%d: %.2f", t, s.axVscale[t]);
	}
	printf("\n");
#endif
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	printf("homing:       \t");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d", s.axisFlag[0] & EMCMOT_AXIS_HOMING_BIT ? 1 : 0);
	}
	printf("\n");
#endif
	printf("enabled:     \t%s\n",
	    s.motionFlag & EMCMOT_MOTION_ENABLE_BIT ? "ENABLED" : "DISABLED");
	printf("probe value: %d\n", s.probeVal);
	printf("probe Tripped: %d\n", s.probeTripped);
	printf("probing: %d\n", s.probing);
	printf("probed pos:      \t%f\t%f\t%f\n",
	    s.probedPos.tran.x, s.probedPos.tran.y, s.probedPos.tran.z);
	break;

    case 2:
	/* print motion and axis flags */
/*! \todo Another #if 0 */
#if 0				/*! \todo FIXME - change to work with joint
				   structures */
	htostr(m, s.motionFlag);
	printf("motion:   %s\n", m);
	printf("axes:     ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    htostr(m, s.axisFlag[t]);
	    printf("%s ", m);
	}
	printf("\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_ENABLE_BIT) != 0));
	}
	printf("enable\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_ACTIVE_BIT) != 0));
	}
	printf("active\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_INPOS_BIT) != 0));
	}
	printf("inpos\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_ERROR_BIT) != 0));
	}
	printf("error\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t",
		((s.axisFlag[t] & EMCMOT_AXIS_MAX_SOFT_LIMIT_BIT) != 0));
	}
	printf("max_soft_limit\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t",
		((s.axisFlag[t] & EMCMOT_AXIS_MIN_SOFT_LIMIT_BIT) != 0));
	}
	printf("min_soft_limit\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t",
		((s.axisFlag[t] & EMCMOT_AXIS_MAX_HARD_LIMIT_BIT) != 0));
	}
	printf("max_hard_limit\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t",
		((s.axisFlag[t] & EMCMOT_AXIS_MIN_HARD_LIMIT_BIT) != 0));
	}
	printf("min_hard_limit\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t",
		((s.axisFlag[t] & EMCMOT_AXIS_HOME_SWITCH_BIT) != 0));
	}
	printf("home_switch\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_HOMING_BIT) != 0));
	}
	printf("homing\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_HOMED_BIT) != 0));
	}
	printf("homed\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_FERROR_BIT) != 0));
	}
	printf("ferror\n");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("%d\t", ((s.axisFlag[t] & EMCMOT_AXIS_FAULT_BIT) != 0));
	}
#endif
	printf("fault\n");
	printf("\npolarity: ");
	printf("limit override: %d\n", s.overrideLimits);
	break;

    case 4:
	printf("scales handled in HAL now!\n");
/*! \todo Another #if 0 */
#if 0
	printf("output scales: ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.outputScale[t]);
	}

	printf("\noutput offsets:");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.outputOffset[t]);
	}

	printf("\ninput scales:  ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.inputScale[t]);
	}

	printf("\ninput offsets: ");
	for (t = 0; t < EMCMOT_MAX_AXIS; t++) {
	    printf("\t%f", s.inputOffset[t]);
	}

	printf("\n");
#endif
	break;

    default:
	break;
    }
}

static int module_id;
static int shmem_id;

int usrmotInit(char *modname)
{
    int retval;

    module_id = rtapi_init(modname);
    if (module_id < 0) {
	fprintf(stderr,
	    "usrmotintf: ERROR: rtapi init failed\n");
	return -1;
    }
    /* get shared memory block from RTAPI */
    shmem_id = rtapi_shmem_new(SHMEM_KEY, module_id, sizeof(emcmot_struct_t));
    if (shmem_id < 0) {
	fprintf(stderr,
	    "usrmotintf: ERROR: could not open shared memory\n");
	rtapi_exit(module_id);
	return -1;
    }
    /* get address of shared memory area */
    retval = rtapi_shmem_getptr(shmem_id, (void **) &emcmotStruct);
    if (retval != RTAPI_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "usrmotintf: ERROR: could not access shared memory\n");
	rtapi_exit(module_id);
	return -1;
    }
    /* got it */
    emcmotCommand = &(emcmotStruct->command);
    emcmotStatus = &(emcmotStruct->status);
    emcmotDebug = &(emcmotStruct->debug);
    emcmotConfig = &(emcmotStruct->config);
    emcmotError = &(emcmotStruct->error);

    emcmotshmem = emcmotStruct;

    inited = 1;

    return 0;
}

int usrmotExit(void)
{
    if (NULL != emcmotStruct) {
	rtapi_shmem_delete(shmem_id, module_id);
	rtapi_exit(module_id);
    }

    emcmotStruct = 0;
    emcmotCommand = 0;
    emcmotStatus = 0;
    emcmotError = 0;
/*! \todo Another #if 0 */
#if 0
/*! \todo FIXME - comp structs no longer in shmem */
    for (axis = 0; axis < EMCMOT_MAX_AXIS; axis++) {
	emcmotComp[axis] = 0;
    }
#endif
    emcmotshmem = 0;

    inited = 0;
    return 0;
}

int usrmotLoadComp(int axis, const char *file)
{
    FILE *fp;
    char buffer[LINELEN];
    double nom, fwd, rev;
    int ret = 0;
    emcmot_command_t emcmotCommand;

    /* check axis range */
    if (axis < 0 || axis >= EMCMOT_MAX_AXIS) {
	fprintf(stderr, "axis out of range for compensation\n");
	return -1;
    }

    /* open input comp file */
    if (NULL == (fp = fopen(file, "r"))) {
	fprintf(stderr, "can't open compensation file %s\n", file);
	return -1;
    }

    while (!feof(fp)) {
	if (NULL == fgets(buffer, LINELEN, fp)) {
	    break;
	}
	/*
	   expecting nominal-forward-reverse triplets, e.g., 
	   0.000000 0.000000 -0.001279 
	   0.100000 0.098742  0.051632 
	   0.200000 0.171529  0.194216 */
	if (3 != sscanf(buffer, "%lf %lf %lf", &nom, &fwd, &rev)) {
	    break;
	} else {
	    emcmotCommand.command = EMCMOT_SET_JOINT_COMP;
    	    emcmotCommand.comp_nominal = nom;
    	    emcmotCommand.comp_forward = fwd;
    	    emcmotCommand.comp_reverse = rev;
	    ret |= usrmotWriteEmcmotCommand(&emcmotCommand);
	}
    }
    fclose(fp);

#if 0
/* should move to RT */
    if (total > 1) {
	emcmotComp[axis]->avgint = (emcmotComp[axis]->nominal[total - 1] -
	    emcmotComp[axis]->nominal[0]) / (total - 1);
    }

    /* ->total is the flag to emcmot that the comp table is valid, so only
       set this to be >1 if the data is really valid: total > 1 and avgint >
       0 */
    if (total > 1 && emcmotComp[axis]->avgint > DBL_MIN) {
	emcmotComp[axis]->total = total;
    } else {
	fprintf(stderr, "compensation file %s has too few distinct points\n",
	    file);
	return -1;
    }

    /* leave alter alone */
#endif
    return ret;
}

int usrmotAlter(int axis, double alter)
{
/*! \todo FIXME - the comp stuff is temporarily disabled, so "alter" must
   be disabled too.  BTW, what the heck is "alter" anyway?
*/
return -1;
/*! \todo Another #if 0 */
#if 0
    /* check axis range */
    if (axis < 0 || axis >= EMCMOT_MAX_AXIS) {
	fprintf(stderr, "axis out of range for alter\n");
	return -1;
    }

    /* first check if comp pointer is valid */
    if (emcmotComp[axis] == 0) {
	fprintf(stderr, "compensation data structure not present\n");
	return -1;
    }

    /* set alter value */
    emcmotComp[axis]->alter = alter;

    return 0;
#endif
}

int usrmotQueryAlter(int axis, double *alter)
{
/*! \todo FIXME - the comp stuff is temporarily disabled, so "alter" must
   be disabled too.  BTW, what the heck is "alter" anyway?
*/
return -1;
/*! \todo Another #if 0 */
#if 0
    /* check axis range */
    if (axis < 0 || axis >= EMCMOT_MAX_AXIS) {
	fprintf(stderr, "axis out of range for alter query\n");
	return -1;
    }

    /* first check if comp pointer is valid */
    if (emcmotComp[axis] == 0) {
	fprintf(stderr, "compensation data structure not present\n");
	return -1;
    }

    /* set alter value */
    *alter = emcmotComp[axis]->alter;

    return 0;
#endif
}

int usrmotPrintComp(int axis)
{
/*! \todo FIXME - the comp stuff is temporarily disabled */
return -1;
/*! \todo Another #if 0 */
#if 0
    int t;

    /* check axis range */
    if (axis < 0 || axis >= EMCMOT_MAX_AXIS) {
	fprintf(stderr, "axis out of range for compensation\n");
	return -1;
    }

    /* first check if comp pointer is valid */
    if (emcmotComp[axis] == 0) {
	fprintf(stderr, "compensation data structure not present\n");
	return -1;
    }

    printf("total:  %d\n", emcmotComp[axis]->total);
    printf("avgint: %f\n", emcmotComp[axis]->avgint);
    printf("alter:  %f\n", emcmotComp[axis]->alter);
    for (t = 0; t < emcmotComp[axis]->total; t++) {
	printf("%f\t%f\t%f\n",
	    emcmotComp[axis]->nominal[t],
	    emcmotComp[axis]->forward[t], emcmotComp[axis]->reverse[t]);
    }

    return 0;
#endif
}
