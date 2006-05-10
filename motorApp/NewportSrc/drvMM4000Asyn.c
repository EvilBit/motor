#include <stddef.h>
#include "epicsThread.h"
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "drvMM4000Asyn.h"
#include "paramLib.h"

#include "epicsFindSymbol.h"
#include "epicsTime.h"
#include "epicsThread.h"
#include "epicsEvent.h"
#include "epicsString.h"
#include "epicsMutex.h"
#include "ellLib.h"
#include "asynDriver.h"
#include "asynOctetSyncIO.h"

#include "drvSup.h"
#include "epicsExport.h"
#define DEFINE_MOTOR_PROTOTYPES 1
#include "motor_interface.h"

motorAxisDrvSET_t motorMM4000 = 
  {
    20,
    motorAxisReport,            /**< Standard EPICS driver report function (optional) */
    motorAxisInit,              /**< Standard EPICS dirver initialisation function (optional) */
    motorAxisSetLog,            /**< Defines an external logging function (optional) */
    motorAxisSetLogParam,       /**< Defines an external logging function user parameter (optional) */
    motorAxisOpen,              /**< Driver open function */
    motorAxisClose,             /**< Driver close function */
    motorAxisSetCallback,       /**< Provides a callback function the driver can call when the status updates */
    motorAxisPrimitive,         /**< Passes a controller dependedent string */
    motorAxisSetDouble,         /**< Pointer to function to set a double value */
    motorAxisSetInteger,        /**< Pointer to function to set an integer value */
    motorAxisGetDouble,         /**< Pointer to function to get a double value */
    motorAxisGetInteger,        /**< Pointer to function to get an integer value */
    motorAxisHome,              /**< Pointer to function to execute a more to reference or home */
    motorAxisMove,              /**< Pointer to function to execute a position move */
    motorAxisVelocityMove,      /**< Pointer to function to execute a velocity mode move */
    motorAxisStop               /**< Pointer to function to stop motion */
  };

epicsExportAddress(drvet, motorMM4000);

typedef enum {
    MM4000,
    MM4005
} MM_model;

typedef struct {
    epicsMutexId MM4000Lock;
    asynUser *pasynUser;
    int numAxes;
    char firmwareVersion[100];
    MM_model model;
    double movingPollPeriod;
    double idlePollPeriod;
    epicsEventId pollEventId;
    AXIS_HDL pAxis;  /* array of axes */
} MM4000Controller;

typedef struct motorAxisHandle
{
    MM4000Controller *pController;
    PARAMS params;
    double currentPosition;
    double stepSize;
    double highLimit;
    double lowLimit;
    double homePreset;
    int closedLoop;
    int axisStatus;
    int card;
    int axis;
    int maxDigits;
    void *logParam;
    epicsMutexId mutexId;
} motorAxis;

typedef struct
{
    AXIS_HDL pFirst;
    epicsThreadId motorThread;
    motorAxisLogFunc print;
    epicsTimeStamp now;
} motorMM4000_t;


static int motorMM4000LogMsg(void * param, const motorAxisLogMask_t logMask, const char *pFormat, ...);
static int sendOnly(MM4000Controller *pController, char *outputString);
static int sendAndReceive(MM4000Controller *pController, char *outputString, char *inputString, int inputSize);

#define PRINT   (drv.print)
#define FLOW    motorAxisTraceFlow
#define ERROR   motorAxisTraceError
#define IODRIVER  motorAxisTraceIODriver

#define MM4000_MAX_AXES 8
#define BUFFER_SIZE 100 /* Size of input and output buffers */
#define TIMEOUT 2.0     /* Timeout for I/O in seconds */

#define MM4000_HOME       0x20  /* Home LS. */
#define MM4000_LOW_LIMIT  0x10  /* Minus Travel Limit. */
#define MM4000_HIGH_LIMIT 0x08  /* Plus Travel Limit. */
#define MM4000_DIRECTION  0x04  /* Motor direction: 0 - minus; 1 - plus. */
#define MM4000_POWER_ON   0x02  /* Motor power 0 - ON; 1 - OFF. */
#define MM4000_MOVING     0x01  /* In-motion indicator. */


#define TCP_TIMEOUT 2.0
static motorMM4000_t drv={ NULL, NULL, motorMM4000LogMsg, { 0, 0 } };
static int numMM4000Controllers;
/* Pointer to array of controller strutures */
static MM4000Controller *pMM4000Controller=NULL;

#define MAX(a,b) ((a)>(b)? (a): (b))
#define MIN(a,b) ((a)<(b)? (a): (b))

static char* getMM4000Error(AXIS_HDL pAxis, int status, char *buffer);

static void motorAxisReportAxis(AXIS_HDL pAxis, int level)
{
    if (level > 0)
    {
        printf("Axis %d\n", pAxis->axis);
        printf("   axisStatus:  0x%x\n", pAxis->axisStatus);
        printf("   high limit:  %f\n", pAxis->highLimit);
        printf("   low limit:   %f\n", pAxis->lowLimit);
        printf("   home preset: %f\n", pAxis->homePreset);
        printf("   step size:   %f\n", pAxis->stepSize);
        printf("   max digits:  %d\n", pAxis->maxDigits);
    }
}

static void motorAxisReport(int level)
{
    int i, j;

    for(i=0; i<numMM4000Controllers; i++) {
        printf("Controller %d firmware version: %s\n", i, pMM4000Controller[i].firmwareVersion);
        if (level) {
            printf("    model: %d\n", pMM4000Controller[i].model);
            printf("    moving poll period: %f\n", pMM4000Controller[i].movingPollPeriod);
            printf("    idle poll period: %f\n", pMM4000Controller[i].idlePollPeriod);
            printf("Controller %d firmware version: %s\n", i, pMM4000Controller[i].firmwareVersion);
        }
        for(j=0; j<pMM4000Controller[i].numAxes; j++) {
           motorAxisReportAxis(&pMM4000Controller[i].pAxis[j], level);
        }
    }   
}


static int motorAxisInit(void)
{
  return MOTOR_AXIS_OK;
}

static int motorAxisSetLog(motorAxisLogFunc logFunc)
{
  if (logFunc == NULL) drv.print=motorMM4000LogMsg;
  else drv.print = logFunc;

  return MOTOR_AXIS_OK;
}

static int motorAxisSetLogParam(AXIS_HDL pAxis, void * param)
{
  if (pAxis == NULL) return MOTOR_AXIS_ERROR;
  else
    {
      pAxis->logParam = param;
    }
  return MOTOR_AXIS_OK;
}

static AXIS_HDL motorAxisOpen(int card, int axis, char * param)
{
  AXIS_HDL pAxis;

  if (card > numMM4000Controllers) return(NULL);
  if (axis > pMM4000Controller[card].numAxes) return(NULL);
  pAxis = &pMM4000Controller[card].pAxis[axis];
  return pAxis;
}

static int motorAxisClose(AXIS_HDL pAxis)
{
  return MOTOR_AXIS_OK;
}

static int motorAxisGetInteger(AXIS_HDL pAxis, motorAxisParam_t function, int * value)
{
  if (pAxis == NULL) return MOTOR_AXIS_ERROR;
  else
    {
      return motorParam->getInteger(pAxis->params, (paramIndex) function, value);
    }
}

static int motorAxisGetDouble(AXIS_HDL pAxis, motorAxisParam_t function, double * value)
{
  if (pAxis == NULL) return MOTOR_AXIS_ERROR;
  else
    {
      return motorParam->getDouble(pAxis->params, (paramIndex) function, value);
    }
}

static int motorAxisSetCallback(AXIS_HDL pAxis, motorAxisCallbackFunc callback, void * param)
{
  if (pAxis == NULL) return MOTOR_AXIS_ERROR;
  else
    {
      return motorParam->setCallback(pAxis->params, callback, param);
    }
}

static int motorAxisPrimitive(AXIS_HDL pAxis, int length, char * string)
{
  return MOTOR_AXIS_OK;
}

static int motorAxisSetDouble(AXIS_HDL pAxis, motorAxisParam_t function, double value)
{
    int ret_status = MOTOR_AXIS_ERROR;
    int status;
    double deviceValue;
    char buff[100];

    if (pAxis == NULL) return MOTOR_AXIS_ERROR;
    else
    {
        switch (function)
        {
        case motorAxisPosition:
        {
            PRINT(pAxis->logParam, ERROR, "motorAxisSetDouble: MM4000 does not support setting position\n");
            break;
        }
        case motorAxisEncoderRatio:
        {
            PRINT(pAxis->logParam, ERROR, "motorAxisSetDouble: MM4000 does not support setting encoder ratio\n");
            break;
        }
        case motorAxisResolution:
        {
            PRINT(pAxis->logParam, ERROR, "motorAxisSetDouble: MM4000 does not support setting resolution\n");
            break;
        }
        case motorAxisLowLimit:
        {
            deviceValue = value*pAxis->stepSize;
            sprintf(buff, "%dSR%.*f", pAxis->axis+1, pAxis->maxDigits,  deviceValue);
            ret_status = sendOnly(pAxis->pController, buff);
            break;
        }
        case motorAxisHighLimit:
        {
            deviceValue = value*pAxis->stepSize;
            sprintf(buff, "%dSL%.*f", pAxis->axis+1, pAxis->maxDigits,  deviceValue);
            ret_status = sendOnly(pAxis->pController, buff);
            break;
        }
        case motorAxisPGain:
        {
            PRINT(pAxis->logParam, ERROR, "MM4000 does not support setting proportional gain\n");
            break;
        }
        case motorAxisIGain:
        {
            PRINT(pAxis->logParam, ERROR, "MM4000 does not support setting integral gain\n");
            break;
        }
        case motorAxisDGain:
        {
            PRINT(pAxis->logParam, ERROR, "MM4000 does not support setting derivative gain\n");
            break;
        }
        case motorAxisClosedLoop:
        {
            PRINT(pAxis->logParam, ERROR, "MM4000 does not support changing closed loop or torque\n");
            break;
        }
        default:
            PRINT(pAxis->logParam, ERROR, "motorAxisSetDouble: unknown function %d\n", function);
            break;
        }
    }
    if (ret_status != MOTOR_AXIS_ERROR) status = motorParam->setDouble(pAxis->params, function, value);
    return ret_status;
}

static int motorAxisSetInteger(AXIS_HDL pAxis, motorAxisParam_t function, int value)
{
    int ret_status = MOTOR_AXIS_ERROR;
    int status;

    if (pAxis == NULL) return MOTOR_AXIS_ERROR;

    switch (function) {
    case motorAxisClosedLoop:
        if (value) {
            /* The MM4000 only allows turning on and off ALL motors (MO and MF commands), not individual axes */
            /* Don't implement */
            ret_status = MOTOR_AXIS_OK;
        } else {
            ret_status = MOTOR_AXIS_OK;
        }
        break;
    default:
        PRINT(pAxis->logParam, ERROR, "motorAxisSetInteger: unknown function %d\n", function);
        break;
    }
    if (ret_status != MOTOR_AXIS_ERROR) status = motorParam->setInteger(pAxis->params, function, value);
    return ret_status;
}


static int motorAxisMove(AXIS_HDL pAxis, double position, int relative, 
                          double min_velocity, double max_velocity, double acceleration)
{
    int status;
    char buff[100];
    char *moveCommand;

    if (pAxis == NULL) return MOTOR_AXIS_ERROR;

    PRINT(pAxis->logParam, FLOW, "Set card %d, axis %d move to %f, min vel=%f, max_vel=%f, accel=%f\n",
          pAxis->card, pAxis->axis, position, min_velocity, max_velocity, acceleration);

    if (relative) {
        moveCommand = "PR";
    } else {
        moveCommand = "PA";
    }
    sprintf(buff, "%dAC%.*f;%dVA%.*f;%d%s%.*f;",
            pAxis->axis+1, pAxis->maxDigits, acceleration * pAxis->stepSize,
            pAxis->axis+1, pAxis->maxDigits, max_velocity * pAxis->stepSize,
            pAxis->axis+1, moveCommand, pAxis->maxDigits, position * pAxis->stepSize);
    status = sendOnly(pAxis->pController, buff);
    if (status) return MOTOR_AXIS_ERROR;

    /* Send a signal to the poller task which will make it do a poll, and switch to the moving poll rate */
    epicsEventSignal(pAxis->pController->pollEventId);

    return MOTOR_AXIS_OK;
}

static int motorAxisHome(AXIS_HDL pAxis, double min_velocity, double max_velocity, double acceleration, int forwards)
{
    int status;
    char buff[100];

    if (pAxis == NULL) return MOTOR_AXIS_ERROR;

    PRINT(pAxis->logParam, FLOW, "motorAxisHome: set card %d, axis %d to home\n",
          pAxis->card, pAxis->axis);

    sprintf(buff, "%dAC%.*f; %dVA%.*f;%dOR;",
            pAxis->axis+1, pAxis->maxDigits, acceleration * pAxis->stepSize,
            pAxis->axis+1, pAxis->maxDigits, max_velocity * pAxis->stepSize,
            pAxis->axis+1);
    status = sendOnly(pAxis->pController, buff);
    if (status) return(MOTOR_AXIS_ERROR);

    /* Send a signal to the poller task which will make it do a poll, and switch to the moving poll rate */
    epicsEventSignal(pAxis->pController->pollEventId);
    return MOTOR_AXIS_OK;
}


static int motorAxisVelocityMove(AXIS_HDL pAxis, double min_velocity, double velocity, double acceleration)
{
    int status;

    if (pAxis == NULL) return(MOTOR_AXIS_ERROR);

    /* MM4000 does not have a jog command.  Simulate with move absolute
     * to the appropriate software limit. We can move to MM4000 soft limits.
     * If the record soft limits are set tighter than the MM4000 limits
     * the record will prevent JOG motion beyond its soft limits
     */
    if (velocity > 0.)
        status = motorAxisMove(pAxis,  pAxis->highLimit/pAxis->stepSize, 0, min_velocity, velocity, acceleration); 
    else
        status = motorAxisMove(pAxis,  pAxis->lowLimit/pAxis->stepSize, 0, min_velocity, -velocity, acceleration); 

    return status;
}

static int motorAxisProfileMove(AXIS_HDL pAxis, int npoints, double positions[], double times[], int relative, int trigger)
{
  return MOTOR_AXIS_ERROR;
}

static int motorAxisTriggerProfile(AXIS_HDL pAxis)
{
  return MOTOR_AXIS_ERROR;
}

static int motorAxisStop(AXIS_HDL pAxis, double acceleration)
{
    int status;
    char buff[100];

    if (pAxis == NULL) return MOTOR_AXIS_ERROR;
    
    PRINT(pAxis->logParam, FLOW, "Set card %d, axis %d to stop with accel=%f\n",
          pAxis->card, pAxis->axis, acceleration);

    sprintf(buff, "%dAC%.*f;%dST;",
            pAxis->axis+1, pAxis->maxDigits, acceleration * pAxis->stepSize,
            pAxis->axis+1);
    status = sendOnly(pAxis->pController, buff);
    if (status) return MOTOR_AXIS_ERROR;
    return MOTOR_AXIS_OK;
}


static void MM4000Poller(MM4000Controller *pController)
{
    /* This is the task that polls the MM4000 */
    double timeout;
    AXIS_HDL pAxis;
    int status;
    int i, j;
    int axisDone;
    int offset;
    int anyMoving;
    int comStatus;
    int forcedFastPolls=0;
    char *p, *tokSave;
    char statusAllString[100];
    char positionAllString[100];
    char buff[100];

    timeout = pController->idlePollPeriod;
    epicsEventSignal(pController->pollEventId);  /* Force on poll at startup */

    while(1) {
        if (timeout != 0.) status = epicsEventWaitWithTimeout(pController->pollEventId, timeout);
        else               status = epicsEventWait(pController->pollEventId);
        if (status == epicsEventWaitOK) {
            /* We got an event, rather than a timeout.  This is because other software
             * knows that an axis should have changed state (started moving, etc.).
             * Force a minimum number of fast polls, because the controller status
             * might not have changed the first few polls
             */
            forcedFastPolls = 10;
         }
        
        anyMoving = 0;
        comStatus = sendAndReceive(pController, "MS;", statusAllString, sizeof(statusAllString));
        if (comStatus == 0) comStatus = sendAndReceive(pController, "TP;", 
                                                       positionAllString, sizeof(positionAllString));
        for (i=0; i<pController->numAxes; i++) {
            pAxis = &pController->pAxis[i];
            if (!pAxis->mutexId) break;
            epicsMutexLock(pAxis->mutexId);
            if (comStatus != 0) {
                PRINT(pAxis->logParam, ERROR, "MM4000Poller: error reading status=%d\n", comStatus);
                motorParam->setInteger(pAxis->params, motorAxisCommError, 1);
            } else {
                motorParam->setInteger(pAxis->params, motorAxisCommError, 0);
                /*
                 * Parse the status string
                 * Status string format: 1MSx,2MSy,3MSz,... where x, y and z are the status
                 * bytes for the motors
                 */
                offset = pAxis->axis*5 + 3;  /* Offset in status string */
                pAxis->axisStatus = statusAllString[offset];
                if (pAxis->axisStatus & MM4000_MOVING) {
                    axisDone = 0;
                    anyMoving = 1;
                } else {
                    axisDone = 1;
                }
                motorParam->setInteger(pAxis->params, motorAxisDone, axisDone);
                if (pAxis->axisStatus & MM4000_HOME) {
                    motorParam->setInteger(pAxis->params, motorAxisHomeSignal, 1);
                } else {
                    motorParam->setInteger(pAxis->params, motorAxisHomeSignal, 0);
                }
                if (pAxis->axisStatus & MM4000_HIGH_LIMIT) {
                    motorParam->setInteger(pAxis->params, motorAxisHighHardLimit, 1);
                } else {
                    motorParam->setInteger(pAxis->params, motorAxisHighHardLimit, 0);
                }
                if (pAxis->axisStatus & MM4000_LOW_LIMIT) {
                    motorParam->setInteger(pAxis->params, motorAxisLowHardLimit, 1);
                } else {
                    motorParam->setInteger(pAxis->params, motorAxisLowHardLimit, 0);
                }
                /*
                 * Parse motor position
                 * Position string format: 1TP5.012,2TP1.123,3TP-100.567,...
                 * Skip to substring for this motor, convert to double
                 */

                strcpy(buff, positionAllString);
                tokSave = NULL;
                p = epicsStrtok_r(buff, ",", &tokSave);
                for (j=0; j < pAxis->axis; j++) 
                    p = epicsStrtok_r(NULL, ",", &tokSave);
                pAxis->currentPosition = atof(p+3);
                motorParam->setDouble(pAxis->params, motorAxisPosition,    (pAxis->currentPosition/pAxis->stepSize));
                motorParam->setDouble(pAxis->params, motorAxisEncoderPosn, (pAxis->currentPosition/pAxis->stepSize));
                PRINT(pAxis->logParam, IODRIVER, "MM4000Poller: axis %d axisStatus=%x, position=%f\n", 
                      pAxis->axis, pAxis->axisStatus, pAxis->currentPosition);

                /* We would like a way to query the actual velocity, but this is not possible.  If we could we could
                 * set the direction, and Moving flags */
            }

            motorParam->callCallback(pAxis->params);
            epicsMutexUnlock(pAxis->mutexId);

        } /* Next axis */
        if (forcedFastPolls > 0) {
            timeout = pController->movingPollPeriod;
            forcedFastPolls--;
        } else if (anyMoving) {
            timeout = pController->movingPollPeriod;
        } else {
            timeout = pController->idlePollPeriod;
        }
    } /* End while */
}

static int motorMM4000LogMsg(void * param, const motorAxisLogMask_t mask, const char *pFormat, ...)
{

    va_list     pvar;
    int         nchar;

    va_start(pvar, pFormat);
    nchar = vfprintf(stdout,pFormat,pvar);
    va_end (pvar);
    printf("\n");
    return(nchar);
}


int MM4000AsynSetup(int num_controllers)   /* number of MM4000 controllers in system.  */
{

    if (num_controllers < 1) {
        printf("MM4000Setup, num_controllers must be > 0\n");
        return MOTOR_AXIS_ERROR;
    }
    numMM4000Controllers = num_controllers;
    pMM4000Controller = (MM4000Controller *)calloc(numMM4000Controllers, sizeof(MM4000Controller)); 
    return MOTOR_AXIS_OK;
}


int MM4000AsynConfig(int card,             /* Controller number */
                     const char *portName, /* asyn port name of serial or GPIB port */
                     int asynAddress,      /* asyn subaddress for GPIB */
                     int numAxes,          /* Number of axes this controller supports */
                     int movingPollPeriod, /* Time to poll (msec) when an axis is in motion */
                     int idlePollPeriod)   /* Time to poll (msec) when an axis is idle. 0 for no polling */

{
    AXIS_HDL pAxis;
    int axis;
    MM4000Controller *pController;
    char threadName[20];
    int status;
    int totalAxes;
    int loopState;
    int digits;
    int modelNum;
    char *p, *tokSave;
    char inputBuff[BUFFER_SIZE];
    char outputBuff[BUFFER_SIZE];

    if (numMM4000Controllers < 1) {
        printf("MM4000Config: no MM4000 controllers allocated, call MM4000Setup first\n");
        return MOTOR_AXIS_ERROR;
    }
    if ((card < 0) || (card >= numMM4000Controllers)) {
        printf("MM4000Config: card must in range 0 to %d\n", numMM4000Controllers-1);
        return MOTOR_AXIS_ERROR;
    }
    if ((numAxes < 1) || (numAxes > MM4000_MAX_AXES)) {
        printf("MM4000Config: numAxes must in range 1 to %d\n", MM4000_MAX_AXES);
        return MOTOR_AXIS_ERROR;
    }

    pController = &pMM4000Controller[card];
    pController->pAxis = (AXIS_HDL) calloc(numAxes, sizeof(motorAxis));
    pController->numAxes = numAxes;
    pController->movingPollPeriod = movingPollPeriod/1000.;
    pController->idlePollPeriod = idlePollPeriod/1000.;

    status = pasynOctetSyncIO->connect(portName, asynAddress, &pController->pasynUser, NULL);

    if (status != asynSuccess) {
        printf("MM4000AsynConfig: cannot connect to asyn port %s\n", portName);
        return MOTOR_AXIS_ERROR;
    }

    sendAndReceive(pController, "VE;", inputBuff, sizeof(inputBuff));
    strcpy(pController->firmwareVersion, &inputBuff[2]);  /* Skip "VE" */

    /* Set Motion Master model indicator. */
    p = strstr(pController->firmwareVersion, "MM");
    if (p == NULL) {
        printf("MM4000AsynConfig: invalid model = %s\n", pController->firmwareVersion);
        return MOTOR_AXIS_ERROR;
    }
    modelNum = atoi(p+2);
    if (modelNum == 4000)
        pController->model = MM4000;
    else if (modelNum == 4005 || modelNum == 4006)
        pController->model = MM4005;
    else {
        printf("MM4000AsynConfig: invalid model = %s\n", pController->firmwareVersion);
        return MOTOR_AXIS_ERROR;
    }
    
    sendAndReceive(pController, "TP;", inputBuff, sizeof(inputBuff));

    /* The return string will tell us how many axes this controller has */
    for (totalAxes = 0, tokSave = NULL, p = epicsStrtok_r(inputBuff, ",", &tokSave);
             p != 0; p = epicsStrtok_r(NULL, ",", &tokSave), totalAxes++)
        ;

    if (totalAxes < numAxes) {
        printf("MM4000AsynConfig: actual number of axes=%d < numAxes=%d\n", totalAxes, numAxes);
        return MOTOR_AXIS_ERROR;
    }

    for (axis=0; axis<numAxes; axis++) {
        pAxis = &pController->pAxis[axis];
        pAxis->pController = pController;
        pAxis->card = card;
        pAxis->axis = axis;
        pAxis->mutexId = epicsMutexMustCreate();
        pAxis->params = motorParam->create(0, MOTOR_AXIS_NUM_PARAMS);

        /* Determine if encoder present based on open/closed loop mode. */
        sprintf(outputBuff, "%dTC", axis+1);
        sendAndReceive(pController, outputBuff, inputBuff, sizeof(inputBuff));
        loopState = atoi(&inputBuff[3]);    /* Skip first 3 characters */
        if (loopState != 0) {
            pAxis->closedLoop = 1;
        }

        /* Determine drive resolution. */
        sprintf(outputBuff, "%dTU", axis+1);
        sendAndReceive(pController, outputBuff, inputBuff, sizeof(inputBuff));
        pAxis->stepSize = atof(&inputBuff[3]);
        digits = (int) -log10(pAxis->stepSize) + 2;
        if (digits < 1) digits = 1;
        pAxis->maxDigits = digits;

        /* Save home preset position. */
        sprintf(outputBuff, "%dXH", axis+1);
        sendAndReceive(pController, outputBuff, inputBuff, sizeof(inputBuff));
        pAxis->homePreset = atof(&inputBuff[3]);

        /* Determine low limit */
        sprintf(outputBuff, "%dTL", axis+1);
        sendAndReceive(pController, outputBuff, inputBuff, sizeof(inputBuff));
        pAxis->lowLimit = atof(&inputBuff[3]);

        /* Determine high limit */
        sprintf(outputBuff, "%dTR", axis+1);
        sendAndReceive(pController, outputBuff, inputBuff, sizeof(inputBuff));
        pAxis->highLimit = atof(&inputBuff[3]);
    }
    
    pController->pollEventId = epicsEventMustCreate(epicsEventEmpty);

    /* Create the poller thread for this controller */
    epicsSnprintf(threadName, sizeof(threadName), "MM4000:%d", card);
    epicsThreadCreate(threadName,
                      epicsThreadPriorityMedium,
                      epicsThreadGetStackSize(epicsThreadStackMedium),
                      (EPICSTHREADFUNC) MM4000Poller, (void *) pController);

    return MOTOR_AXIS_OK;
}

int sendOnly(MM4000Controller *pController, char *outputBuff)
{
    int nRequested=strlen(outputBuff);
    int nActual;
    asynStatus status;

    status = pasynOctetSyncIO->write(pController->pasynUser, outputBuff, 
                                     nRequested, TIMEOUT, &nActual);
    if (nActual != nRequested) status = asynError;
    if (status != asynSuccess) {
        asynPrint(pController->pasynUser, ASYN_TRACE_ERROR, 
                  "drvMM4000Asyn:sendOnly: error sending command %d, sent=%d, status=%d\n",
                  outputBuff, nActual, status); 
    }
    return(status);
}


int sendAndReceive(MM4000Controller *pController, char *outputBuff, char *inputBuff, int inputSize) 
{
    int nWriteRequested=strlen(outputBuff);
    int nWrite;
    int nRead;
    int eomReason;
    asynStatus status;
    
    status = pasynOctetSyncIO->writeRead(pController->pasynUser, 
                                         outputBuff, nWriteRequested, 
                                         inputBuff, inputSize,
                                         TIMEOUT, &nWrite, &nRead, &eomReason);
    if (nWrite != nWriteRequested) status = asynError;
    if (status != asynSuccess) {
        asynPrint(pController->pasynUser, ASYN_TRACE_ERROR,
                  "drvMM4000Asyn:sendAndReceive error calling writeRead, output=%s status=%d, error=%s\n",
                  outputBuff, status, pController->pasynUser->errorMessage);
    }
    return(status);
}
