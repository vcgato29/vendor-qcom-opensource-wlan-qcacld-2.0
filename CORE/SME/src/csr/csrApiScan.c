/*
 * Copyright (c) 2011-2015 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/** ------------------------------------------------------------------------- *
    ------------------------------------------------------------------------- *


    \file csrApiScan.c

    Implementation for the Common Scan interfaces.
========================================================================== */

#include "aniGlobal.h"

#include "palApi.h"
#include "csrInsideApi.h"
#include "smeInside.h"
#include "smsDebug.h"

#include "csrSupport.h"
#include "wlan_qct_tl.h"

#include "vos_diag_core_log.h"
#include "vos_diag_core_event.h"

#include "vos_nvitem.h"
#include "vos_memory.h"
#include "wlan_qct_wda.h"
#include "vos_utils.h"

#define MIN_CHN_TIME_TO_FIND_GO 100
#define MAX_CHN_TIME_TO_FIND_GO 100
#define DIRECT_SSID_LEN 7
#define MIN_11P_CHANNEL 170

/*
 * Purpose of HIDDEN_TIMER
 * When we remove hidden ssid from the profile i.e., forget the SSID via GUI
 * that SSID shouldn't see in the profile for above requirement we used timer
 * limit, logic is explained below timer value is initialized to current time
 * when it receives corresponding probe response of hidden SSID
 * (The probe request is received regularly till SSID in the profile.
 * Once it is removed from profile probe request is not sent.) when we receive
 * probe response for broadcast probe request, during update SSID with saved
 * SSID we will diff current time with saved SSID time if it is greater than
 * 1 min then we are not updating with old one.
 */

#define HIDDEN_TIMER (1*60*1000)

/* Must be less than 100, represent the percentage of new RSSI */
#define CSR_SCAN_RESULT_RSSI_WEIGHT     80

#define MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL 140
#define MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL 120

#define MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC 30
#define MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC 20

#define CSR_SCAN_IS_OVER_BSS_LIMIT(pMac)  \
   ( (pMac)->scan.nBssLimit <= (csrLLCount(&(pMac)->scan.scanResultList)) )

//*** This is temporary work around. It need to call CCM api to get to CFG later
/// Get string parameter value
extern tSirRetStatus wlan_cfgGetStr(tpAniSirGlobal, tANI_U16, tANI_U8*, tANI_U32*);

void csrScanGetResultTimerHandler(void *);
void csrScanResultAgingTimerHandler(void *pv);
static void csrScanResultCfgAgingTimerHandler(void *pv);
void csrScanIdleScanTimerHandler(void *);
static void csrSetDefaultScanTiming( tpAniSirGlobal pMac, tSirScanType scanType, tCsrScanRequest *pScanRequest);
#ifdef WLAN_AP_STA_CONCURRENCY
static void csrStaApConcTimerHandler(void *);
#endif
tANI_BOOLEAN csrIsSupportedChannel(tpAniSirGlobal pMac, tANI_U8 channelId);
eHalStatus csrScanChannels( tpAniSirGlobal pMac, tSmeCmd *pCommand );
void csrSetCfgValidChannelList( tpAniSirGlobal pMac, tANI_U8 *pChannelList, tANI_U8 NumChannels );
void csrSaveTxPowerToCfg( tpAniSirGlobal pMac, tDblLinkList *pList, tANI_U32 cfgId );
void csrSetCfgCountryCode( tpAniSirGlobal pMac, tANI_U8 *countryCode );
void csrPurgeChannelPower( tpAniSirGlobal pMac, tDblLinkList *pChannelList );
//if bgPeriod is 0, background scan is disabled. It is in millisecond units
eHalStatus csrSetCfgBackgroundScanPeriod(tpAniSirGlobal pMac, tANI_U32 bgPeriod);
eHalStatus csrProcessSetBGScanParam(tpAniSirGlobal pMac, tSmeCmd *pCommand);
void csrReleaseScanCommand(tpAniSirGlobal pMac, tSmeCmd *pCommand, eCsrScanStatus scanStatus);
static tANI_BOOLEAN csrScanValidateScanResult( tpAniSirGlobal pMac, tANI_U8 *pChannels,
                                               tANI_U8 numChn, tSirBssDescription *pBssDesc,
                                               tDot11fBeaconIEs **ppIes );
eHalStatus csrSetBGScanChannelList( tpAniSirGlobal pMac, tANI_U8 *pAdjustChannels, tANI_U8 NumAdjustChannels);
void csrReleaseCmdSingle(tpAniSirGlobal pMac, tSmeCmd *pCommand);
tANI_BOOLEAN csrRoamIsValidChannel( tpAniSirGlobal pMac, tANI_U8 channel );
void csrPruneChannelListForMode( tpAniSirGlobal pMac, tCsrChannel *pChannelList );

#define CSR_IS_SOCIAL_CHANNEL(channel) (((channel) == 1) || ((channel) == 6) || ((channel) == 11) )



static void csrReleaseScanCmdPendingList(tpAniSirGlobal pMac)
{
    tListElem *pEntry;
    tSmeCmd *pCommand;

    while((pEntry = csrLLRemoveHead( &pMac->scan.scanCmdPendingList, LL_ACCESS_LOCK)) != NULL)
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        if ( eSmeCsrCommandMask & pCommand->command )
        {
            csrAbortCommand( pMac, pCommand, eANI_BOOLEAN_TRUE );
        }
        else
        {
            smsLog(pMac, LOGE, FL("Error: Received command : %d"),pCommand->command);
        }
    }
}
//pResult is invalid calling this function.
void csrFreeScanResultEntry( tpAniSirGlobal pMac, tCsrScanResult *pResult )
{
    if( NULL != pResult->Result.pvIes )
    {
        vos_mem_free(pResult->Result.pvIes);
    }
    vos_mem_free(pResult);
}


static eHalStatus csrLLScanPurgeResult(tpAniSirGlobal pMac, tDblLinkList *pList)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tListElem *pEntry;
    tCsrScanResult *pBssDesc;

    csrLLLock(pList);

    while((pEntry = csrLLRemoveHead(pList, LL_ACCESS_NOLOCK)) != NULL)
    {
        pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
        csrFreeScanResultEntry( pMac, pBssDesc );
    }

    csrLLUnlock(pList);

    return (status);
}

eHalStatus csrScanOpen( tpAniSirGlobal pMac )
{
    eHalStatus status;

    do
    {
        csrLLOpen(pMac->hHdd, &pMac->scan.scanResultList);
        csrLLOpen(pMac->hHdd, &pMac->scan.tempScanResults);
        csrLLOpen(pMac->hHdd, &pMac->scan.channelPowerInfoList24);
        csrLLOpen(pMac->hHdd, &pMac->scan.channelPowerInfoList5G);
#ifdef WLAN_AP_STA_CONCURRENCY
        csrLLOpen(pMac->hHdd, &pMac->scan.scanCmdPendingList);
#endif
        pMac->scan.fFullScanIssued = eANI_BOOLEAN_FALSE;
        pMac->scan.nBssLimit = CSR_MAX_BSS_SUPPORT;
        status = vos_timer_init(&pMac->scan.hTimerGetResult, VOS_TIMER_TYPE_SW, csrScanGetResultTimerHandler, pMac);
        if (!HAL_STATUS_SUCCESS(status))
        {
            smsLog(pMac, LOGE, FL("cannot allocate memory for getResult timer"));
            break;
        }
#ifdef WLAN_AP_STA_CONCURRENCY
        status = vos_timer_init(&pMac->scan.hTimerStaApConcTimer, VOS_TIMER_TYPE_SW, csrStaApConcTimerHandler, pMac);
        if (!HAL_STATUS_SUCCESS(status))
        {
            smsLog(pMac, LOGE, FL("cannot allocate memory for hTimerStaApConcTimer timer"));
            break;
        }
#endif
        status = vos_timer_init(&pMac->scan.hTimerIdleScan, VOS_TIMER_TYPE_SW, csrScanIdleScanTimerHandler, pMac);
        if (!HAL_STATUS_SUCCESS(status))
        {
            smsLog(pMac, LOGE, FL("cannot allocate memory for idleScan timer"));
            break;
        }
        status = vos_timer_init(&pMac->scan.hTimerResultAging, VOS_TIMER_TYPE_SW, csrScanResultAgingTimerHandler, pMac);
        if (!HAL_STATUS_SUCCESS(status))
        {
            smsLog(pMac, LOGE, FL("cannot allocate memory for ResultAging timer"));
            break;
        }
        status = vos_timer_init(&pMac->scan.hTimerResultCfgAging, VOS_TIMER_TYPE_SW,
                                csrScanResultCfgAgingTimerHandler, pMac);
        if (!HAL_STATUS_SUCCESS(status))
        {
            smsLog(pMac, LOGE, FL("cannot allocate memory for CFG ResultAging timer"));
            break;
        }
    }while(0);

    return (status);
}


eHalStatus csrScanClose( tpAniSirGlobal pMac )
{
    csrLLScanPurgeResult(pMac, &pMac->scan.tempScanResults);
    csrLLScanPurgeResult(pMac, &pMac->scan.scanResultList);
#ifdef WLAN_AP_STA_CONCURRENCY
    csrReleaseScanCmdPendingList(pMac);
#endif
    csrLLClose(&pMac->scan.scanResultList);
    csrLLClose(&pMac->scan.tempScanResults);
#ifdef WLAN_AP_STA_CONCURRENCY
    csrLLClose(&pMac->scan.scanCmdPendingList);
#endif
    csrPurgeChannelPower(pMac, &pMac->scan.channelPowerInfoList24);
    csrPurgeChannelPower(pMac, &pMac->scan.channelPowerInfoList5G);
    csrLLClose(&pMac->scan.channelPowerInfoList24);
    csrLLClose(&pMac->scan.channelPowerInfoList5G);
    csrScanDisable(pMac);
    vos_timer_destroy(&pMac->scan.hTimerResultAging);
    vos_timer_destroy(&pMac->scan.hTimerResultCfgAging);
    vos_timer_destroy(&pMac->scan.hTimerGetResult);
#ifdef WLAN_AP_STA_CONCURRENCY
    vos_timer_destroy(&pMac->scan.hTimerStaApConcTimer);
#endif
    vos_timer_destroy(&pMac->scan.hTimerIdleScan);
    return eHAL_STATUS_SUCCESS;
}


eHalStatus csrScanEnable( tpAniSirGlobal pMac )
{

    pMac->scan.fScanEnable = eANI_BOOLEAN_TRUE;
    pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;

    return eHAL_STATUS_SUCCESS;
}


eHalStatus csrScanDisable( tpAniSirGlobal pMac )
{

    csrScanStopTimers(pMac);
    pMac->scan.fScanEnable = eANI_BOOLEAN_FALSE;

    return eHAL_STATUS_SUCCESS;
}


//Set scan timing parameters according to state of other driver sessions
//No validation of the parameters is performed.
static void csrSetDefaultScanTiming( tpAniSirGlobal pMac, tSirScanType scanType, tCsrScanRequest *pScanRequest)
{
#ifdef WLAN_AP_STA_CONCURRENCY
    if(csrIsAnySessionConnected(pMac))
    {
        //If multi-session, use the appropriate default scan times
        if(scanType == eSIR_ACTIVE_SCAN)
        {
            pScanRequest->maxChnTime = pMac->roam.configParam.nActiveMaxChnTimeConc;
            pScanRequest->minChnTime = pMac->roam.configParam.nActiveMinChnTimeConc;
        }
        else
        {
            pScanRequest->maxChnTime = pMac->roam.configParam.nPassiveMaxChnTimeConc;
            pScanRequest->minChnTime = pMac->roam.configParam.nPassiveMinChnTimeConc;
        }
        pScanRequest->maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
        pScanRequest->minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;

        pScanRequest->restTime = pMac->roam.configParam.nRestTimeConc;

        //Return so that fields set above will not be overwritten.
        return;
    }
#endif

    //This portion of the code executed if multi-session not supported
    //(WLAN_AP_STA_CONCURRENCY not defined) or no multi-session.
    //Use the "regular" (non-concurrency) default scan timing.
    if(pScanRequest->scanType == eSIR_ACTIVE_SCAN)
    {
        pScanRequest->maxChnTime = pMac->roam.configParam.nActiveMaxChnTime;
        pScanRequest->minChnTime = pMac->roam.configParam.nActiveMinChnTime;
    }
    else
    {
        pScanRequest->maxChnTime = pMac->roam.configParam.nPassiveMaxChnTime;
        pScanRequest->minChnTime = pMac->roam.configParam.nPassiveMinChnTime;
    }
        pScanRequest->maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
        pScanRequest->minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;

#ifdef WLAN_AP_STA_CONCURRENCY
    //No rest time if no sessions are connected.
    pScanRequest->restTime = 0;
#endif
}

#ifdef WLAN_AP_STA_CONCURRENCY
//Return SUCCESS is the command is queued, else returns eHAL_STATUS_FAILURE
eHalStatus csrQueueScanRequest(tpAniSirGlobal pMac, tANI_U8 sessionId,
                               tSmeCmd *pScanCmd)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;

    tANI_BOOLEAN fNoCmdPending;
    tSmeCmd *pQueueScanCmd=NULL;
    tSmeCmd *pSendScanCmd=NULL;
    tANI_U8  nNumChanCombinedConc = 0;
    if (NULL == pScanCmd)
    {
        smsLog (pMac, LOGE, FL("Scan Req cmd is NULL"));
        return eHAL_STATUS_FAILURE;
    }
    /* split scan if any one of the following:
     * - STA session is connected and the scan is not a P2P search
     * - any P2P session is connected
     * Do not split scans if no concurrent infra connections are
     * active and if the scan is a BG scan triggered by LFR (OR)
     * any scan if LFR is in the middle of a BG scan. Splitting
     * the scan is delaying the time it takes for LFR to find
     * candidates and resulting in disconnects.
     */

    if(csrIsStaSessionConnected(pMac) &&
       !csrIsP2pSessionConnected(pMac))
    {
      nNumChanCombinedConc = pMac->roam.configParam.nNumStaChanCombinedConc;
    }
    else if(csrIsP2pSessionConnected(pMac))
    {
      nNumChanCombinedConc = pMac->roam.configParam.nNumP2PChanCombinedConc;
    }
    if ( (csrIsStaSessionConnected(pMac) &&
#ifdef FEATURE_WLAN_LFR
         (csrIsConcurrentInfraConnected(pMac) ||
          ((pScanCmd->u.scanCmd.reason != eCsrScanBgScan) &&
           (pMac->roam.neighborRoamInfo[sessionId].neighborRoamState !=
            eCSR_NEIGHBOR_ROAM_STATE_CFG_CHAN_LIST_SCAN))) &&
#endif
         (pScanCmd->u.scanCmd.u.scanRequest.p2pSearch != 1)) ||
            (csrIsP2pSessionConnected(pMac)) )
    {
        tCsrScanRequest scanReq;
        tANI_U8 numChn = pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
        tCsrChannelInfo *pChnInfo = &scanReq.ChannelInfo;
        tANI_U8    channelToScan[WNI_CFG_VALID_CHANNEL_LIST_LEN];
        tANI_BOOLEAN bMemAlloc = eANI_BOOLEAN_FALSE;

        if (numChn == 0)
        {

            numChn = pMac->scan.baseChannels.numChannels;

            pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = vos_mem_malloc(numChn);
            if ( NULL == pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList  )
            {
                smsLog( pMac, LOGE, FL(" Failed to get memory for channel list ") );
                return eHAL_STATUS_FAILURE;
            }
            bMemAlloc = eANI_BOOLEAN_TRUE;
            vos_mem_copy(pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList,
                         pMac->scan.baseChannels.channelList, numChn);
            status = eHAL_STATUS_SUCCESS;
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                vos_mem_free(pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList);
                pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = NULL;
                smsLog( pMac, LOGE, FL(" Failed to copy memory to channel list ") );
                return eHAL_STATUS_FAILURE;
            }
            pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = numChn;
        }

        //Whenever we get a scan request with multiple channels we break it up into 2 requests
        //First request  for first channel to scan and second request to scan remaining channels
        if ( numChn > nNumChanCombinedConc)
        {
            vos_mem_set(&scanReq, sizeof(tCsrScanRequest), 0);

            pQueueScanCmd = csrGetCommandBuffer(pMac); //optimize this to use 2 command buffer only
            if (!pQueueScanCmd)
            {
                if (bMemAlloc)
                {
                    vos_mem_free(pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList);
                    pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = NULL;

                }
                smsLog( pMac, LOGE, FL(" Failed to get Queue command buffer") );
                return eHAL_STATUS_FAILURE;
            }
            pQueueScanCmd->command = pScanCmd->command;
            pQueueScanCmd->sessionId = pScanCmd->sessionId;
            pQueueScanCmd->u.scanCmd.callback = pScanCmd->u.scanCmd.callback;
            pQueueScanCmd->u.scanCmd.pContext = pScanCmd->u.scanCmd.pContext;
            pQueueScanCmd->u.scanCmd.reason = pScanCmd->u.scanCmd.reason;
            pQueueScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID++; //let it wrap around

            /* First copy all the parameters to local variable of scan request */
            csrScanCopyRequest(pMac, &scanReq, &pScanCmd->u.scanCmd.u.scanRequest);

            /* Now modify the elements of local var scan request required to be modified for split scan */
            if(scanReq.ChannelInfo.ChannelList != NULL)
            {
                vos_mem_free(scanReq.ChannelInfo.ChannelList);
                scanReq.ChannelInfo.ChannelList = NULL;
            }

            pChnInfo->numOfChannels = pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels - nNumChanCombinedConc;

            VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_WARN,
                    FL(" &channelToScan %p pScanCmd(%p) pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList(%p)numChn(%d)"),
                    &channelToScan[0], pScanCmd,
                    pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList, numChn);

            vos_mem_copy(&channelToScan[0],
                     &pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[
                     nNumChanCombinedConc],
                     pChnInfo->numOfChannels * sizeof(tANI_U8));

            pChnInfo->ChannelList = &channelToScan[0];

            scanReq.BSSType = eCSR_BSS_TYPE_ANY;
            //Modify callers parameters in case of concurrency
            if (!pScanCmd->u.scanCmd.u.scanRequest.bcnRptReqScan)
                scanReq.scanType = eSIR_ACTIVE_SCAN;
            //Use concurrency values for min/maxChnTime.
            //We know csrIsAnySessionConnected(pMac) returns TRUE here
            csrSetDefaultScanTiming(pMac, scanReq.scanType, &scanReq);

            status = csrScanCopyRequest(pMac, &pQueueScanCmd->u.scanCmd.u.scanRequest, &scanReq);

            if(!HAL_STATUS_SUCCESS(status))
            {
                if (bMemAlloc)
                {
                    vos_mem_free(pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList);
                    pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = NULL;

                }
                if( scanReq.pIEField != NULL)
                {
                    vos_mem_free(scanReq.pIEField);
                    scanReq.pIEField = NULL;
                }
                smsLog( pMac, LOGE, FL(" Failed to get copy csrScanRequest = %d"), status );
                return eHAL_STATUS_FAILURE;
            }
            /* Clean the local scan variable */
            scanReq.ChannelInfo.ChannelList = NULL;
            scanReq.ChannelInfo.numOfChannels = 0;
            csrScanFreeRequest(pMac, &scanReq);

            /* setup the command to scan 2 channels */
            pSendScanCmd = pScanCmd;
            pSendScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = nNumChanCombinedConc;
            pSendScanCmd->u.scanCmd.u.scanRequest.BSSType = eCSR_BSS_TYPE_ANY;
            if (!pSendScanCmd->u.scanCmd.u.scanRequest.bcnRptReqScan)
                pSendScanCmd->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
            //Use concurrency values for min/maxChnTime.
            //We know csrIsAnySessionConnected(pMac) returns TRUE here
            csrSetDefaultScanTiming(pMac, pSendScanCmd->u.scanCmd.u.scanRequest.scanType, &pSendScanCmd->u.scanCmd.u.scanRequest);
            pSendScanCmd->u.scanCmd.callback = NULL;
        } else {
            pSendScanCmd = pScanCmd;
            pSendScanCmd->u.scanCmd.u.scanRequest.BSSType = eCSR_BSS_TYPE_ANY;
            if (!pSendScanCmd->u.scanCmd.u.scanRequest.bcnRptReqScan)
                pSendScanCmd->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
            //Use concurrency values for min/maxChnTime.
            //We know csrIsAnySessionConnected(pMac) returns TRUE here
            csrSetDefaultScanTiming(pMac, pSendScanCmd->u.scanCmd.u.scanRequest.scanType, &pSendScanCmd->u.scanCmd.u.scanRequest);
        }

        fNoCmdPending = csrLLIsListEmpty( &pMac->scan.scanCmdPendingList, LL_ACCESS_LOCK );

        //Logic Below is as follows
        // If the scanCmdPendingList is empty then we directly send that command
        // to smeCommandQueue else we buffer it in our scanCmdPendingList Queue
        if( fNoCmdPending )
        {
            if (pQueueScanCmd != NULL)
            {
                csrLLInsertTail( &pMac->scan.scanCmdPendingList, &pQueueScanCmd->Link, LL_ACCESS_LOCK );
            }

            if (pSendScanCmd != NULL)
            {
                return csrQueueSmeCommand(pMac, pSendScanCmd, eANI_BOOLEAN_FALSE);
            }
        }
        else
        {
            if (pSendScanCmd != NULL)
            {
                csrLLInsertTail( &pMac->scan.scanCmdPendingList, &pSendScanCmd->Link, LL_ACCESS_LOCK );
            }

            if (pQueueScanCmd != NULL)
            {
                csrLLInsertTail( &pMac->scan.scanCmdPendingList, &pQueueScanCmd->Link, LL_ACCESS_LOCK );
            }
        }
    }
    else
    {  //No concurrency case
        smsLog( pMac, LOG2, FL("Queuing scan command (reason=%d, roamState=%d"
                " numOfChannels=%d)"),
                pScanCmd->u.scanCmd.reason,
                pMac->roam.neighborRoamInfo[sessionId].neighborRoamState,
                pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels);
        return csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_FALSE);
    }

    return ( status );
}
#endif

/* ---------------------------------------------------------------------------
    \fn csrScan2GOnyRequest
    \brief This function will update the scan request with only
           2.4GHz valid channel list.
    \param pMac
    \param pScanCmd
    \param pScanRequest
    \return None
  -------------------------------------------------------------------------------*/
static void csrScan2GOnyRequest(tpAniSirGlobal pMac,tSmeCmd *pScanCmd,
                                tCsrScanRequest *pScanRequest)
{
    tANI_U8 index, channelId, channelListSize = 0;
    tANI_U8 channelList2G[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    static tANI_U8 validchannelList[CSR_MAX_2_4_GHZ_SUPPORTED_CHANNELS] = {0};

    VOS_ASSERT(pScanCmd && pScanRequest);
    if((pScanCmd == NULL) || (pScanRequest == NULL))
    {
        smsLog( pMac, LOGE, FL(" pScanCmd or pScanRequest is NULL "));
        return;
    }

    if ((pScanCmd->u.scanCmd.scanID != FIRST_SCAN_ID) ||
       (eCSR_SCAN_REQUEST_FULL_SCAN != pScanRequest->requestType))
           return;

    smsLog( pMac, LOG1, FL("Scanning only 2G Channels during first scan"));
    /* Construct valid Supported 2.4 GHz Channel List */
    for( index = 0; index < ARRAY_SIZE(channelList2G); index++ )
    {
        channelId = channelList2G[index];
        if ( csrIsSupportedChannel( pMac, channelId ) )
        {
            validchannelList[channelListSize++] = channelId;
        }
    }

    pScanRequest->ChannelInfo.numOfChannels = channelListSize;
    pScanRequest->ChannelInfo.ChannelList = validchannelList;
}

eHalStatus csrScanRequest(tpAniSirGlobal pMac, tANI_U16 sessionId,
              tCsrScanRequest *pScanRequest, tANI_U32 *pScanRequestID,
              csrScanCompleteCallback callback, void *pContext)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tSmeCmd *pScanCmd = NULL;
    eCsrConnectState ConnectState;

    if(pScanRequest == NULL)
    {
        smsLog( pMac, LOGE, FL(" pScanRequest is NULL"));
        VOS_ASSERT(0);
        return status;
    }

    /* During group formation, the P2P client scans for GO with the specific SSID.
     * There will be chances of GO switching to other channels because of scan or
     * to STA channel in case of STA+GO MCC scenario. So to increase the possibility
     * of client to find the GO, the dwell time of scan is increased to 100ms.
     */
    if(pScanRequest->p2pSearch)
    {
        if ((pScanRequest->SSIDs.numOfSSIDs) && (NULL != pScanRequest->SSIDs.SSIDList))
        {
            //If the scan request is for specific SSId the length of SSID will be
            //greater than 7 as SSID for p2p search contains "DIRECT-")
            if(pScanRequest->SSIDs.SSIDList->SSID.length > DIRECT_SSID_LEN)
            {
               smsLog( pMac, LOG1, FL("P2P: Increasing the min and max Dwell"
                     " time to %d for specific SSID scan %.*s"),
                     MAX_CHN_TIME_TO_FIND_GO,
                     pScanRequest->SSIDs.SSIDList->SSID.length,
                     pScanRequest->SSIDs.SSIDList->SSID.ssId);
                pScanRequest->maxChnTime = MAX_CHN_TIME_TO_FIND_GO;
                pScanRequest->minChnTime = MIN_CHN_TIME_TO_FIND_GO;
            }
        }
    }

    do
    {
        if(pMac->scan.fScanEnable)
        {
            pScanCmd = csrGetCommandBuffer(pMac);
            if(pScanCmd)
            {
                vos_mem_set(&pScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
                pScanCmd->command = eSmeCommandScan;
                pScanCmd->sessionId = sessionId;
                if (pScanCmd->sessionId >= CSR_ROAM_SESSION_MAX)
                    smsLog( pMac, LOGE, FL("Invalid Sme Session ID = %d"), sessionId);
                pScanCmd->u.scanCmd.callback = callback;
                pScanCmd->u.scanCmd.pContext = pContext;
                if(eCSR_SCAN_REQUEST_11D_SCAN == pScanRequest->requestType)
                {
                    pScanCmd->u.scanCmd.reason = eCsrScan11d1;
                }
                else if((eCSR_SCAN_REQUEST_FULL_SCAN == pScanRequest->requestType) ||
                        (eCSR_SCAN_P2P_DISCOVERY == pScanRequest->requestType)
#ifdef SOFTAP_CHANNEL_RANGE
                        ||(eCSR_SCAN_SOFTAP_CHANNEL_RANGE == pScanRequest->requestType)
#endif
                 )
                {
                    pScanCmd->u.scanCmd.reason = eCsrScanUserRequest;
                }
                else if(eCSR_SCAN_HO_BG_SCAN == pScanRequest->requestType)
                {
                    pScanCmd->u.scanCmd.reason = eCsrScanBgScan;
                }
                else if(eCSR_SCAN_HO_PROBE_SCAN == pScanRequest->requestType)
                {
                    pScanCmd->u.scanCmd.reason = eCsrScanProbeBss;
                }
                else if(eCSR_SCAN_P2P_FIND_PEER == pScanRequest->requestType)
                {
                    pScanCmd->u.scanCmd.reason = eCsrScanP2PFindPeer;
                }
                else
                {
                    pScanCmd->u.scanCmd.reason = eCsrScanIdleScan;
                }
                if(pScanRequest->minChnTime == 0 && pScanRequest->maxChnTime == 0)
                {
                    //The caller doesn't set the time correctly. Set it here
                   csrSetDefaultScanTiming(pMac, pScanRequest->scanType,
                                          pScanRequest);
                   smsLog(pMac, LOG1, FL("Setting default min %d and max %d"
                         " ChnTime"), pScanRequest->minChnTime,
                         pScanRequest->maxChnTime);
                }
#ifdef WLAN_AP_STA_CONCURRENCY
                if(pScanRequest->restTime == 0)
                {
                    //Need to set restTime only if at least one session is connected
                    if(csrIsAnySessionConnected(pMac))
                    {
                        pScanRequest->restTime = pMac->roam.configParam.nRestTimeConc;
                        if(pScanRequest->scanType == eSIR_ACTIVE_SCAN)
                        {
                            pScanRequest->maxChnTime = pMac->roam.configParam.nActiveMaxChnTimeConc;
                            pScanRequest->minChnTime = pMac->roam.configParam.nActiveMinChnTimeConc;
                        }
                        else
                        {
                            pScanRequest->maxChnTime = pMac->roam.configParam.nPassiveMaxChnTimeConc;
                            pScanRequest->minChnTime = pMac->roam.configParam.nPassiveMinChnTimeConc;
                        }
                    }
                }
#endif
                /* Increase dwell time in case P2P Search and Miracast is not present*/
                if(pScanRequest->p2pSearch &&
                    pScanRequest->ChannelInfo.numOfChannels == P2P_SOCIAL_CHANNELS
                    && (!(pMac->sme.miracast_value))) {
                    pScanRequest->maxChnTime += P2P_SEARCH_DWELL_TIME_INCREASE;
                }

                 /*For Standalone wlan : channel time will remain the same.
                   For BTC with A2DP up: Channel time = Channel time * 2, if station is not already associated.
                   This has been done to provide a larger scan window for faster connection during btc.Else Scan is seen
                   to take a long time.
                   For BTC with A2DP up: Channel time will not be doubled, if station is already associated.
                 */
                status = csrRoamGetConnectState(pMac,sessionId,&ConnectState);
                if (HAL_STATUS_SUCCESS(status) &&
                    pMac->btc.fA2DPUp &&
                   (eCSR_ASSOC_STATE_TYPE_INFRA_ASSOCIATED != ConnectState) &&
                   (eCSR_ASSOC_STATE_TYPE_IBSS_CONNECTED != ConnectState))
                {
                    pScanRequest->maxChnTime = pScanRequest->maxChnTime << 1;
                    pScanRequest->minChnTime = pScanRequest->minChnTime << 1;
                    smsLog( pMac, LOG1, FL("BTC A2DP up, doubling max and min"
                          " ChnTime (Max=%d Min=%d)"),
                          pScanRequest->maxChnTime,
                          pScanRequest->minChnTime);
                }

                pScanRequest->maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
                pScanRequest->minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
                //Need to make the following atomic
                pScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID++; //let it wrap around

                if(pScanRequestID)
                {
                    *pScanRequestID = pScanCmd->u.scanCmd.scanID;
                }

                // If it is the first scan request from HDD, CSR checks if it is for 11d.
                // If it is not, CSR will save the scan request in the pending cmd queue
                // & issue an 11d scan request to PE.
                if (((FIRST_SCAN_ID == pScanCmd->u.scanCmd.scanID)
                   && (eCSR_SCAN_REQUEST_11D_SCAN != pScanRequest->requestType))
#ifdef SOFTAP_CHANNEL_RANGE
                   && (eCSR_SCAN_SOFTAP_CHANNEL_RANGE != pScanRequest->requestType)
#endif
                   && (eANI_BOOLEAN_FALSE == pMac->scan.fEnableBypass11d)
                   )
                {
                    tSmeCmd *p11dScanCmd;
                    tCsrScanRequest scanReq;
                    tCsrChannelInfo *pChnInfo = &scanReq.ChannelInfo;

                    vos_mem_set(&scanReq, sizeof(tCsrScanRequest), 0);

                    p11dScanCmd = csrGetCommandBuffer(pMac);
                    if (p11dScanCmd)
                    {
                        tANI_U32 numChn = pMac->scan.baseChannels.numChannels;

                        vos_mem_set(&p11dScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
                        pChnInfo->ChannelList = vos_mem_malloc(numChn);
                        if ( NULL == pChnInfo->ChannelList )
                        {
                           smsLog(pMac, LOGE, FL("Failed to allocate memory"));
                           status = eHAL_STATUS_FAILURE;
                           break;
                        }
                        vos_mem_copy(pChnInfo->ChannelList,
                                    pMac->scan.baseChannels.channelList,
                                    numChn);

                        pChnInfo->numOfChannels = (tANI_U8)numChn;
                        p11dScanCmd->command = eSmeCommandScan;
                        p11dScanCmd->u.scanCmd.callback = pMac->scan.callback11dScanDone;
                        p11dScanCmd->u.scanCmd.pContext = NULL;
                        p11dScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID++;
                        scanReq.BSSType = eCSR_BSS_TYPE_ANY;

                        if ( csrIs11dSupported(pMac) )
                        {
                            scanReq.bcnRptReqScan = pScanRequest->bcnRptReqScan;
                            if (pScanRequest->bcnRptReqScan)
                                scanReq.scanType = pScanRequest->scanType ?
                                                   eSIR_PASSIVE_SCAN :
                                                   pScanRequest->scanType;
                            else
                                scanReq.scanType = eSIR_PASSIVE_SCAN;
                            scanReq.requestType = eCSR_SCAN_REQUEST_11D_SCAN;
                            p11dScanCmd->u.scanCmd.reason = eCsrScan11d1;
                            scanReq.maxChnTime = pMac->roam.configParam.nPassiveMaxChnTime;
                            scanReq.minChnTime = pMac->roam.configParam.nPassiveMinChnTime;
                        }
                        else
                        {
                            scanReq.bcnRptReqScan = pScanRequest->bcnRptReqScan;
                            if (pScanRequest->bcnRptReqScan)
                                scanReq.scanType = pScanRequest->scanType;
                            else
                                scanReq.scanType = eSIR_ACTIVE_SCAN;
                            scanReq.requestType = eCSR_SCAN_IDLE_MODE_SCAN;
                            p11dScanCmd->u.scanCmd.reason = eCsrScanIdleScan;
                            scanReq.maxChnTime = pMac->roam.configParam.nActiveMaxChnTime;
                            scanReq.minChnTime = pMac->roam.configParam.nActiveMinChnTime;

                            scanReq.maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
                            scanReq.minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
                        }
                        if (pMac->roam.configParam.nInitialDwellTime)
                        {
                            scanReq.maxChnTime =
                                     pMac->roam.configParam.nInitialDwellTime;
                            smsLog(pMac, LOG1, FL("11d scan, updating"
                                   "dwell time for first scan %u"),
                                    scanReq.maxChnTime);
                        }

                        status = csrScanCopyRequest(pMac, &p11dScanCmd->u.scanCmd.u.scanRequest, &scanReq);
                        //Free the channel list
                        vos_mem_free(pChnInfo->ChannelList);
                        pChnInfo->ChannelList = NULL;

                        if (HAL_STATUS_SUCCESS(status))
                        {
                            //Start process the command
#ifdef WLAN_AP_STA_CONCURRENCY
                            if (!pMac->fScanOffload)
                                status = csrQueueScanRequest(pMac, sessionId,
                                                             p11dScanCmd);
                            else
                                status = csrQueueSmeCommand(pMac, p11dScanCmd,
                                                            eANI_BOOLEAN_FALSE);
#else
                            status = csrQueueSmeCommand(pMac, p11dScanCmd, eANI_BOOLEAN_FALSE);
#endif
                            if( !HAL_STATUS_SUCCESS( status ) )
                            {
                                smsLog( pMac, LOGE, FL(" Failed to send message"
                                      " status = %d"), status);
                                break;
                            }
                        }
                        else
                        {
                            smsLog(pMac, LOGE, FL("csrScanCopyRequest failed"));
                            break;
                        }
                    }
                    else
                    {
                        //error
                        smsLog( pMac, LOGE, FL("p11dScanCmd failed") );
                        break;
                    }
                }

                //Scan only 2G Channels if set in ini file
                //This is mainly to reduce the First Scan duration
                //Once we turn on Wifi
                if(pMac->scan.fFirstScanOnly2GChnl)
                {
                    csrScan2GOnyRequest(pMac, pScanCmd, pScanRequest);
                }

                if (pMac->roam.configParam.nInitialDwellTime)
                {
                    pScanRequest->maxChnTime =
                            pMac->roam.configParam.nInitialDwellTime;
                    pMac->roam.configParam.nInitialDwellTime = 0;
                    smsLog(pMac, LOG1,
                                 FL("updating dwell time for first scan %u"),
                                 pScanRequest->maxChnTime);
                }

                status = csrScanCopyRequest(pMac, &pScanCmd->u.scanCmd.u.scanRequest, pScanRequest);
                /*
                 * Reset the variable after the first scan is queued after
                 * loading the driver. The purpose of this parameter is that
                 * DFS channels are skipped during the first scan after loading
                 * the driver. The above API builds the target scan request in
                 * which this variable is used.
                 */
                pMac->roam.configParam.initial_scan_no_dfs_chnl = 0;
                if(HAL_STATUS_SUCCESS(status))
                {
                  tCsrScanRequest *pTempScanReq =
                   &pScanCmd->u.scanCmd.u.scanRequest;
                  pMac->scan.scanProfile.numOfChannels =
                   pTempScanReq->ChannelInfo.numOfChannels;

                  smsLog(pMac, LOG1, FL(" SId=%d scanId=%d"
                        " Scan reason=%u numSSIDs=%d"
                        " numChan=%d P2P search=%d minCT=%d maxCT=%d"
                        " minCBtc=%d maxCBtx=%d uIEFieldLen=%d"),
                        sessionId, pScanCmd->u.scanCmd.scanID,
                        pScanCmd->u.scanCmd.reason,
                        pTempScanReq->SSIDs.numOfSSIDs,
                        pTempScanReq->ChannelInfo.numOfChannels,
                        pTempScanReq->p2pSearch,
                        pTempScanReq->minChnTime,
                        pTempScanReq->maxChnTime,
                        pTempScanReq->minChnTimeBtc,
                        pTempScanReq->maxChnTimeBtc,
                        pTempScanReq->uIEFieldLen);

                    //Start process the command
#ifdef WLAN_AP_STA_CONCURRENCY
                    if (!pMac->fScanOffload)
                        status = csrQueueScanRequest(pMac, sessionId, pScanCmd);
                    else
                        status = csrQueueSmeCommand(pMac, pScanCmd,
                                                    eANI_BOOLEAN_FALSE);
#else
                    status = csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_FALSE);
#endif
                    if( !HAL_STATUS_SUCCESS( status ) )
                    {
                        smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                        break;
                    }
                }
                else
                {
                    smsLog( pMac, LOGE, FL(" fail to copy request status = %d"), status );
                    break;
                }
            }
            else
            {
                smsLog( pMac, LOGE, FL(" pScanCmd is NULL"));
                break;
            }
        }
        else
        {
             smsLog( pMac, LOGE, FL("SId: %d Scanning not enabled"
                   " Scan type=%u, numOfSSIDs=%d P2P search=%d"),
                   sessionId, pScanRequest->requestType,
                   pScanRequest->SSIDs.numOfSSIDs,
                   pScanRequest->p2pSearch );
        }
    } while(0);
    if(!HAL_STATUS_SUCCESS(status) && pScanCmd)
    {
        if( eCsrScanIdleScan == pScanCmd->u.scanCmd.reason )
        {
            //Set the flag back for restarting idle scan
            pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;
        }
        smsLog( pMac, LOGE, FL(" SId: %d Failed with status=%d"
              " Scan reason=%u numOfSSIDs=%d"
              " P2P search=%d scanId=%d"),
              sessionId, status, pScanCmd->u.scanCmd.reason,
              pScanRequest->SSIDs.numOfSSIDs, pScanRequest->p2pSearch,
              pScanCmd->u.scanCmd.scanID );
        csrReleaseCommandScan(pMac, pScanCmd);
    }

    return (status);
}


eHalStatus csrScanRequestResult(tpAniSirGlobal pMac)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pScanCmd;

    if(pMac->scan.fScanEnable)
    {
        pScanCmd = csrGetCommandBuffer(pMac);
        if(pScanCmd)
        {
            pScanCmd->command = eSmeCommandScan;
            vos_mem_set(&pScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
            pScanCmd->u.scanCmd.callback = NULL;
            pScanCmd->u.scanCmd.pContext = NULL;
            pScanCmd->u.scanCmd.reason = eCsrScanGetResult;
            //Need to make the following atomic
            pScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID; //let it wrap around
            status = csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_FALSE);
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                csrReleaseCommandScan(pMac, pScanCmd);
            }
        }
        else
        {
            //log error
            smsLog(pMac, LOGE, FL("can not obtain a common buffer"));
            status = eHAL_STATUS_RESOURCES;
        }
    }

    return (status);
}

#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
eHalStatus csrScanRequestLfrResult(tpAniSirGlobal pMac, tANI_U32 sessionId,
                                   csrScanCompleteCallback callback, void *pContext)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pScanCmd;

    if (pMac->scan.fScanEnable)
    {
        pScanCmd = csrGetCommandBuffer(pMac);
        if (pScanCmd)
        {
            pScanCmd->command = eSmeCommandScan;
            pScanCmd->sessionId = sessionId;
            vos_mem_set(&pScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
            pScanCmd->u.scanCmd.callback = callback;
            pScanCmd->u.scanCmd.pContext = pContext;
            pScanCmd->u.scanCmd.reason = eCsrScanGetLfrResult;
            //Need to make the following atomic
            pScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID; //let it wrap around
            status = csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_TRUE);
            if ( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d\n"), status );
                csrReleaseCommandScan(pMac, pScanCmd);
            }
        }
        else
        {
            //log error
            smsLog(pMac, LOGE, FL("can not obtain a common buffer\n"));
            status = eHAL_STATUS_RESOURCES;
        }
    }

    return (status);
}
#endif //WLAN_FEATURE_ROAM_SCAN_OFFLOAD

eHalStatus csrScanAllChannels(tpAniSirGlobal pMac, eCsrRequestType reqType)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tANI_U32 scanId;
    tCsrScanRequest scanReq;

    vos_mem_set(&scanReq, sizeof(tCsrScanRequest), 0);
    scanReq.BSSType = eCSR_BSS_TYPE_ANY;
    scanReq.scanType = eSIR_ACTIVE_SCAN;
    scanReq.requestType = reqType;
    scanReq.maxChnTime = pMac->roam.configParam.nActiveMaxChnTime;
    scanReq.minChnTime = pMac->roam.configParam.nActiveMinChnTime;
    scanReq.maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
    scanReq.minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
    //Scan with invalid sessionId.
    //This results in SME using the first available session to scan.
    status = csrScanRequest(pMac, CSR_SESSION_ID_INVALID, &scanReq,
                            &scanId, NULL, NULL);

    return (status);
}




eHalStatus csrIssueRoamAfterLostlinkScan(tpAniSirGlobal pMac, tANI_U32 sessionId, eCsrRoamReason reason)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tScanResultHandle hBSSList = NULL;
    tCsrScanResultFilter *pScanFilter = NULL;
    tANI_U32 roamId = 0;
    tCsrRoamProfile *pProfile = NULL;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    do
    {
        smsLog(pMac, LOG1, " csrIssueRoamAfterLostlinkScan called");
        if(pSession->fCancelRoaming)
        {
            smsLog(pMac, LOGW, " lost link roaming is canceled");
            csrScanStartIdleScan(pMac);
            status = eHAL_STATUS_SUCCESS;
            break;
        }
        //Here is the profile we need to connect to
        pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
        if ( NULL == pScanFilter)
                status = eHAL_STATUS_FAILURE;
        else
                status = eHAL_STATUS_SUCCESS;
        if (!HAL_STATUS_SUCCESS(status))
            break;
        vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
        if(NULL == pSession->pCurRoamProfile)
        {
            pScanFilter->EncryptionType.numEntries = 1;
            pScanFilter->EncryptionType.encryptionType[0] = eCSR_ENCRYPT_TYPE_NONE;
        }
        else
        {
            //We have to make a copy of pCurRoamProfile because it will be free inside csrRoamIssueConnect
            pProfile = vos_mem_malloc(sizeof(tCsrRoamProfile));
            if ( NULL == pProfile )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if (!HAL_STATUS_SUCCESS(status))
                  break;
            vos_mem_set(pProfile, sizeof(tCsrRoamProfile), 0);
            status = csrRoamCopyProfile(pMac, pProfile, pSession->pCurRoamProfile);
            if(!HAL_STATUS_SUCCESS(status))
                break;
            status = csrRoamPrepareFilterFromProfile(pMac, pProfile, pScanFilter);
        }//We have a profile
        roamId = GET_NEXT_ROAM_ID(&pMac->roam);
        if(HAL_STATUS_SUCCESS(status))
        {
            status = csrScanGetResult(pMac, pScanFilter, &hBSSList);
            if(HAL_STATUS_SUCCESS(status))
            {
                if(eCsrLostLink1 == reason)
                {
                    //we want to put the last connected BSS to the very beginning, if possible
                    csrMoveBssToHeadFromBSSID(pMac, &pSession->connectedProfile.bssid, hBSSList);
                }
                status = csrRoamIssueConnect(pMac, sessionId, pProfile, hBSSList, reason,
                                                roamId, eANI_BOOLEAN_TRUE, eANI_BOOLEAN_TRUE);
                if(!HAL_STATUS_SUCCESS(status))
                {
                    csrScanResultPurge(pMac, hBSSList);
                }
            }//Have scan result
        }
    }while(0);
    if(pScanFilter)
    {
        //we need to free memory for filter if profile exists
        csrFreeScanFilter(pMac, pScanFilter);
        vos_mem_free(pScanFilter);
    }
    if(NULL != pProfile)
    {
        csrReleaseProfile(pMac, pProfile);
        vos_mem_free(pProfile);
    }

    return (status);
}


eHalStatus csrScanGetScanChnInfo(tpAniSirGlobal pMac, tANI_U8 sessionId,
                                 void *pContext, void *callback,
                                 tANI_U32 scanID)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pScanCmd;

    if(pMac->scan.fScanEnable)
    {
        pScanCmd = csrGetCommandBuffer(pMac);
        if(pScanCmd)
        {
            pScanCmd->command = eSmeCommandScan;
            vos_mem_set(&pScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
            pScanCmd->u.scanCmd.callback = callback;
            pScanCmd->u.scanCmd.pContext = pContext;
            pScanCmd->u.scanCmd.reason = eCsrScanGetScanChnInfo;
            if (callback)
            {
                //use same scanID as maintained in pAdapter
                pScanCmd->u.scanCmd.scanID = scanID;
            }
            else
            {
                //Need to make the following atomic
                pScanCmd->u.scanCmd.scanID =
                                   pMac->scan.nextScanID++; //let it wrap around
            }

            pScanCmd->sessionId = sessionId;
            status = csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_FALSE);
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                csrReleaseCommandScan(pMac, pScanCmd);
            }
        }
        else
        {
            //log error
            smsLog(pMac, LOGE, FL("can not obtain a common buffer"));
            status = eHAL_STATUS_RESOURCES;
        }
    }

    return (status);
}


eHalStatus csrScanHandleFailedLostlink1(tpAniSirGlobal pMac, tANI_U32 sessionId)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, "Lost link scan 1 failed");
    if(pSession->fCancelRoaming)
    {
        csrScanStartIdleScan(pMac);
    }
    else if(pSession->pCurRoamProfile)
    {
        //We fail lostlink1 but there may be other BSS in the cached result fit the profile. Give it a try first
        if(pSession->pCurRoamProfile->SSIDs.numOfSSIDs == 0 ||
            pSession->pCurRoamProfile->SSIDs.numOfSSIDs > 1)
        {
            /* Try lost link scan2 */
            status = csrScanRequestLostLink2(pMac, sessionId);
        }
        else if(!pSession->pCurRoamProfile->ChannelInfo.ChannelList ||
                pSession->pCurRoamProfile->ChannelInfo.ChannelList[0] == 0)
        {
            /* Go straight to lost link scan3 */
            status = csrScanRequestLostLink3(pMac, sessionId);
        }
        else
        {
            /* We are done with lost link */
            if(csrRoamCompleteRoaming(pMac, sessionId, eANI_BOOLEAN_FALSE, eCSR_ROAM_RESULT_FAILURE))
            {
                csrScanStartIdleScan(pMac);
            }
            status = eHAL_STATUS_SUCCESS;
        }
    }
    else
    {
        status = csrScanRequestLostLink3(pMac, sessionId);
    }

    return (status);
}



eHalStatus csrScanHandleFailedLostlink2(tpAniSirGlobal pMac, tANI_U32 sessionId)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, "Lost link scan 2 failed");
    if(pSession->fCancelRoaming)
    {
        csrScanStartIdleScan(pMac);
    }
    else if(!pSession->pCurRoamProfile || !pSession->pCurRoamProfile->ChannelInfo.ChannelList ||
                pSession->pCurRoamProfile->ChannelInfo.ChannelList[0] == 0)
    {
        /* Try lost link scan3 */
        status = csrScanRequestLostLink3(pMac, sessionId);
    }
    else
    {
        /* We are done with lost link */
        if(csrRoamCompleteRoaming(pMac, sessionId, eANI_BOOLEAN_FALSE, eCSR_ROAM_RESULT_FAILURE))
        {
            csrScanStartIdleScan(pMac);
        }
    }

    return (status);
}



eHalStatus csrScanHandleFailedLostlink3(tpAniSirGlobal pMac, tANI_U32 sessionId)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;

    smsLog(pMac, LOGW, "Lost link scan 3 failed");
    if(eANI_BOOLEAN_TRUE == csrRoamCompleteRoaming(pMac, sessionId, eANI_BOOLEAN_FALSE, eCSR_ROAM_RESULT_FAILURE))
    {
        /* We are done with lost link */
        csrScanStartIdleScan(pMac);
    }

    return (status);
}




//Lostlink1 scan is to actively scan the last connected profile's SSID on all matched BSS channels.
//If no roam profile (it should not), it is like lostlinkscan3
eHalStatus csrScanRequestLostLink1( tpAniSirGlobal pMac, tANI_U32 sessionId )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pCommand = NULL;
    tANI_U8 bAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    tCsrScanResultFilter *pScanFilter = NULL;
    tScanResultHandle hBSSList = NULL;
    tCsrScanResultInfo *pScanResult = NULL;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, FL(" called"));
    do
    {
        pCommand = csrGetCommandBuffer(pMac);
        if(!pCommand)
        {
            status = eHAL_STATUS_RESOURCES;
            break;
        }
        vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
        pCommand->command = eSmeCommandScan;
        pCommand->sessionId = (tANI_U8)sessionId;
        pCommand->u.scanCmd.reason = eCsrScanLostLink1;
        pCommand->u.scanCmd.callback = NULL;
        pCommand->u.scanCmd.pContext = NULL;
        pCommand->u.scanCmd.u.scanRequest.maxChnTime = pMac->roam.configParam.nActiveMaxChnTime;
        pCommand->u.scanCmd.u.scanRequest.minChnTime = pMac->roam.configParam.nActiveMinChnTime;
        pCommand->u.scanCmd.u.scanRequest.maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
        pCommand->u.scanCmd.u.scanRequest.minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
        pCommand->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
        if(pSession->connectedProfile.SSID.length)
        {
            pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList = vos_mem_malloc(sizeof(tCsrSSIDInfo));
            if ( NULL == pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if(!HAL_STATUS_SUCCESS(status))
            {
                break;
            }
            pCommand->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs = 1;
            vos_mem_copy(&pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList[0].SSID,
                         &pSession->connectedProfile.SSID, sizeof(tSirMacSSid));
        }
        else
        {
            pCommand->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs = 0;
        }
        if(pSession->pCurRoamProfile)
        {
            pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
            if ( NULL == pScanFilter )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if(!HAL_STATUS_SUCCESS(status))
            {
                break;
            }
            vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
            status = csrRoamPrepareFilterFromProfile(pMac, pSession->pCurRoamProfile, pScanFilter);
            if(!HAL_STATUS_SUCCESS(status))
            {
                break;
            }
            //Don't change variable status here because whether we can get result or not, the command goes to PE.
            //The status is also used to indicate whether the command is queued. Not success meaning not queue
            if(HAL_STATUS_SUCCESS((csrScanGetResult(pMac, pScanFilter, &hBSSList))) && hBSSList)
            {
                tANI_U8 i, nChn = 0;
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList =
                               vos_mem_malloc(WNI_CFG_VALID_CHANNEL_LIST_LEN);
                if ( NULL == pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList )
                        status = eHAL_STATUS_FAILURE;
                else
                        status = eHAL_STATUS_SUCCESS;
                if(!HAL_STATUS_SUCCESS(status))
                {
                    break;
                }
                while(((pScanResult = csrScanResultGetNext(pMac, hBSSList)) != NULL) &&
                    nChn < WNI_CFG_VALID_CHANNEL_LIST_LEN)
                {
                    for(i = 0; i < nChn; i++)
                    {
                        if(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[i] ==
                                        pScanResult->BssDescriptor.channelId)
                        {
                            break;
                        }
                    }
                    if(i == nChn)
                    {
                        pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[nChn++] = pScanResult->BssDescriptor.channelId;
                    }
                }
                //Include the last connected BSS' channel
                if(csrRoamIsChannelValid(pMac, pSession->connectedProfile.operationChannel))
                {
                    for(i = 0; i < nChn; i++)
                    {
                        if(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[i] ==
                                        pSession->connectedProfile.operationChannel)
                        {
                            break;
                        }
                    }
                    if(i == nChn)
                    {
                        pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[nChn++] = pSession->connectedProfile.operationChannel;
                    }
                }
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = nChn;
            }
            else
            {
                if(csrRoamIsChannelValid(pMac, pSession->connectedProfile.operationChannel))
                {
                    pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = vos_mem_malloc(1);
                    if ( NULL == pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList )
                        status = eHAL_STATUS_FAILURE;
                    else
                        status = eHAL_STATUS_SUCCESS;
                    //just try the last connected channel
                    if(HAL_STATUS_SUCCESS(status))
                    {
                        pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[0] = pSession->connectedProfile.operationChannel;
                        pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = 1;
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }
        vos_mem_copy(&pCommand->u.scanCmd.u.scanRequest.bssid, bAddr, sizeof(tCsrBssid));
        status = csrQueueSmeCommand(pMac, pCommand, eANI_BOOLEAN_FALSE);
        if( !HAL_STATUS_SUCCESS( status ) )
        {
            smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
            break;
        }
    } while( 0 );

    if(!HAL_STATUS_SUCCESS(status))
    {
        smsLog(pMac, LOGW, " csrScanRequestLostLink1 failed with status %d", status);
        if(pCommand)
        {
            csrReleaseCommandScan(pMac, pCommand);
        }
        status = csrScanHandleFailedLostlink1( pMac, sessionId );
    }
    if(pScanFilter)
    {
        csrFreeScanFilter(pMac, pScanFilter);
        vos_mem_free(pScanFilter);
    }
    if(hBSSList)
    {
        csrScanResultPurge(pMac, hBSSList);
    }

    return( status );
}


//Lostlink2 scan is to actively scan the all SSIDs of the last roaming profile's on all matched BSS channels.
//Since MAC doesn't support multiple SSID, we scan all SSIDs and filter them afterwards
eHalStatus csrScanRequestLostLink2( tpAniSirGlobal pMac, tANI_U32 sessionId )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tANI_U8 bAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    tCsrScanResultFilter *pScanFilter = NULL;
    tScanResultHandle hBSSList = NULL;
    tCsrScanResultInfo *pScanResult = NULL;
    tSmeCmd *pCommand = NULL;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, FL(" called"));
    do
    {
        pCommand = csrGetCommandBuffer(pMac);
        if(!pCommand)
        {
            status = eHAL_STATUS_RESOURCES;
            break;
        }
        vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
        pCommand->command = eSmeCommandScan;
        pCommand->sessionId = (tANI_U8)sessionId;
        pCommand->u.scanCmd.reason = eCsrScanLostLink2;
        pCommand->u.scanCmd.callback = NULL;
        pCommand->u.scanCmd.pContext = NULL;
        pCommand->u.scanCmd.u.scanRequest.maxChnTime = pMac->roam.configParam.nActiveMaxChnTime;
        pCommand->u.scanCmd.u.scanRequest.minChnTime = pMac->roam.configParam.nActiveMinChnTime;
        pCommand->u.scanCmd.u.scanRequest.maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
        pCommand->u.scanCmd.u.scanRequest.minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
        pCommand->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
        if(pSession->pCurRoamProfile)
        {
            pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
            if ( NULL == pScanFilter )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if (!HAL_STATUS_SUCCESS(status))
            {
                break;
            }
            vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
            status = csrRoamPrepareFilterFromProfile(pMac, pSession->pCurRoamProfile, pScanFilter);
            if(!HAL_STATUS_SUCCESS(status))
            {
                break;
            }
            status = csrScanGetResult(pMac, pScanFilter, &hBSSList);
            if(!HAL_STATUS_SUCCESS(status))
            {
                break;
            }
            if(hBSSList)
            {
                tANI_U8 i, nChn = 0;
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList =
                                   vos_mem_malloc(WNI_CFG_VALID_CHANNEL_LIST_LEN);
                if ( NULL == pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList )
                        status = eHAL_STATUS_FAILURE;
                else
                        status = eHAL_STATUS_SUCCESS;
                if (!HAL_STATUS_SUCCESS(status))
                {
                    break;
                }
                while(((pScanResult = csrScanResultGetNext(pMac, hBSSList)) != NULL) &&
                    nChn < WNI_CFG_VALID_CHANNEL_LIST_LEN)
                {
                    for(i = 0; i < nChn; i++)
                    {
                        if(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[i] ==
                                        pScanResult->BssDescriptor.channelId)
                        {
                            break;
                        }
                    }
                    if(i == nChn)
                    {
                        pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[nChn++] = pScanResult->BssDescriptor.channelId;
                    }
                }
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = nChn;
            }
        }
        vos_mem_copy(&pCommand->u.scanCmd.u.scanRequest.bssid, bAddr, sizeof(tCsrBssid));
        //Put to the head in pending queue
        status = csrQueueSmeCommand(pMac, pCommand, eANI_BOOLEAN_TRUE);
        if( !HAL_STATUS_SUCCESS( status ) )
        {
            smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
            break;
        }
    } while( 0 );

    if(!HAL_STATUS_SUCCESS(status))
    {
        smsLog(pMac, LOGW, " csrScanRequestLostLink2 failed with status %d", status);
        if(pCommand)
        {
            csrReleaseCommandScan(pMac, pCommand);
        }
        status = csrScanHandleFailedLostlink2( pMac, sessionId );
    }
    if(pScanFilter)
    {
        csrFreeScanFilter(pMac, pScanFilter);
        vos_mem_free(pScanFilter);
    }
    if(hBSSList)
    {
        csrScanResultPurge(pMac, hBSSList);
    }

    return( status );
}


//To actively scan all valid channels
eHalStatus csrScanRequestLostLink3( tpAniSirGlobal pMac, tANI_U32 sessionId )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pCommand;
    tANI_U8 bAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    smsLog(pMac, LOGW, FL(" called"));
    do
    {
        pCommand = csrGetCommandBuffer(pMac);
        if(!pCommand)
        {
            status = eHAL_STATUS_RESOURCES;
            break;
        }
        vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
        pCommand->command = eSmeCommandScan;
        pCommand->sessionId = (tANI_U8)sessionId;
        pCommand->u.scanCmd.reason = eCsrScanLostLink3;
        pCommand->u.scanCmd.callback = NULL;
        pCommand->u.scanCmd.pContext = NULL;
        pCommand->u.scanCmd.u.scanRequest.maxChnTime = pMac->roam.configParam.nActiveMaxChnTime;
        pCommand->u.scanCmd.u.scanRequest.minChnTime = pMac->roam.configParam.nActiveMinChnTime;
        pCommand->u.scanCmd.u.scanRequest.maxChnTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
        pCommand->u.scanCmd.u.scanRequest.minChnTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
        pCommand->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
        vos_mem_copy(&pCommand->u.scanCmd.u.scanRequest.bssid, bAddr, sizeof(tCsrBssid));
        //Put to the head of pending queue
        status = csrQueueSmeCommand(pMac, pCommand, eANI_BOOLEAN_TRUE);
        if( !HAL_STATUS_SUCCESS( status ) )
        {
            smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
            break;
        }
    } while( 0 );
    if(!HAL_STATUS_SUCCESS(status))
    {
        smsLog(pMac, LOGW, " csrScanRequestLostLink3 failed with status %d", status);
        if(csrRoamCompleteRoaming(pMac, sessionId, eANI_BOOLEAN_FALSE, eCSR_ROAM_RESULT_FAILURE))
        {
            csrScanStartIdleScan(pMac);
        }
        if(pCommand)
        {
            csrReleaseCommandScan(pMac, pCommand);
        }
    }

    return( status );
}


eHalStatus csrScanHandleSearchForSSID(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tScanResultHandle hBSSList = CSR_INVALID_SCANRESULT_HANDLE;
    tCsrScanResultFilter *pScanFilter = NULL;
    tCsrRoamProfile *pProfile = pCommand->u.scanCmd.pToRoamProfile;
    tANI_U32 sessionId = pCommand->sessionId;
    do
    {
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
        /* If this scan is for LFR */
        if (pMac->roam.neighborRoamInfo[sessionId].uOsRequestedHandoff) {
            /* Notify LFR state m/c */
            if (eHAL_STATUS_SUCCESS != csrNeighborRoamSssidScanDone(pMac,
                                                     sessionId,
                                                     eHAL_STATUS_SUCCESS)) {
                csrNeighborRoamStartLfrScan(pMac, sessionId);
            }
            status = eHAL_STATUS_SUCCESS;
            break;
        }
#endif
        //If there is roam command waiting, ignore this roam because the newer roam command is the one to execute
        if(csrIsRoamCommandWaitingForSession(pMac, sessionId))
        {
            smsLog(pMac, LOGW, FL(" aborts because roam command waiting"));
            break;
        }
        if(pProfile == NULL)
            break;
        pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
        if ( NULL == pScanFilter )
                status = eHAL_STATUS_FAILURE;
        else
                status = eHAL_STATUS_SUCCESS;
        if (!HAL_STATUS_SUCCESS(status))
            break;
        vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
        status = csrRoamPrepareFilterFromProfile(pMac, pProfile, pScanFilter);
        if(!HAL_STATUS_SUCCESS(status))
            break;
        status = csrScanGetResult(pMac, pScanFilter, &hBSSList);
        if(!HAL_STATUS_SUCCESS(status))
            break;
        status = csrRoamIssueConnect(pMac, sessionId, pProfile, hBSSList, eCsrHddIssued,
                                    pCommand->u.scanCmd.roamId, eANI_BOOLEAN_TRUE, eANI_BOOLEAN_TRUE);
        if(!HAL_STATUS_SUCCESS(status))
        {
            break;
        }
    }while(0);
    if(!HAL_STATUS_SUCCESS(status))
    {
        if(CSR_INVALID_SCANRESULT_HANDLE != hBSSList)
        {
            csrScanResultPurge(pMac, hBSSList);
        }
        //We haven't done anything to this profile
        csrRoamCallCallback(pMac, sessionId, NULL, pCommand->u.scanCmd.roamId,
                     eCSR_ROAM_ASSOCIATION_FAILURE, eCSR_ROAM_RESULT_FAILURE);
        //In case we have nothing else to do, restart idle scan
        if(csrIsConnStateDisconnected(pMac, sessionId) && !csrIsRoamCommandWaiting(pMac))
        {
            status = csrScanStartIdleScan(pMac);
        }
    }
    if (pScanFilter)
    {
        csrFreeScanFilter(pMac, pScanFilter);
        vos_mem_free(pScanFilter);
    }

    return (status);
}


eHalStatus csrScanHandleSearchForSSIDFailure(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tANI_U32 sessionId = pCommand->sessionId;
    tCsrRoamProfile *pProfile = pCommand->u.scanCmd.pToRoamProfile;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if (!pSession) {
        smsLog(pMac, LOGE, FL("Session %d not found"), sessionId);
        return eHAL_STATUS_FAILURE;
    }

#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
    /* If this scan is for LFR */
    if (pMac->roam.neighborRoamInfo[sessionId].uOsRequestedHandoff) {
        /* Notify LFR state m/c */
        if (eHAL_STATUS_SUCCESS != csrNeighborRoamSssidScanDone(pMac,
                                                sessionId,
                                                eHAL_STATUS_FAILURE)) {
            csrNeighborRoamStartLfrScan(pMac, sessionId);
        }
        return eHAL_STATUS_SUCCESS;
    }
#endif

#if defined(WLAN_DEBUG)
    if(pCommand->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs == 1)
    {
        char str[36];
        vos_mem_copy(str,
                     pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList[0].SSID.ssId,
                     pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList[0].SSID.length);
        str[pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList[0].SSID.length] = 0;
        smsLog(pMac, LOGW, FL(" SSID = %s"), str);
    }
#endif
    //Check whether it is for start ibss. No need to do anything if it is a JOIN request
    if(pProfile && CSR_IS_START_IBSS(pProfile))
    {
        status = csrRoamIssueConnect(pMac, sessionId, pProfile, NULL, eCsrHddIssued,
                                        pCommand->u.scanCmd.roamId, eANI_BOOLEAN_TRUE, eANI_BOOLEAN_TRUE);
        if(!HAL_STATUS_SUCCESS(status))
        {
            smsLog(pMac, LOGE, FL("failed to issue startIBSS command with status = 0x%08X"), status);
            csrRoamCallCallback(pMac, sessionId, NULL, pCommand->u.scanCmd.roamId, eCSR_ROAM_FAILED, eCSR_ROAM_RESULT_FAILURE);
        }
    }
    else
    {
        eCsrRoamResult roamResult = eCSR_ROAM_RESULT_FAILURE;

        if(csrIsConnStateDisconnected(pMac, sessionId) &&
          !csrIsRoamCommandWaitingForSession(pMac, sessionId))
        {
            status = csrScanStartIdleScan(pMac);
        }
        if((NULL == pProfile) || !csrIsBssTypeIBSS(pProfile->BSSType))
        {
            //Only indicate assoc_completion if we indicate assoc_start.
            if(pSession->bRefAssocStartCnt > 0)
            {
                tCsrRoamInfo *pRoamInfo = NULL, roamInfo;
                vos_mem_set(&roamInfo, sizeof(tCsrRoamInfo), 0);
                pRoamInfo = &roamInfo;
                if(pCommand->u.roamCmd.pRoamBssEntry)
                {
                    tCsrScanResult *pScanResult =
                                GET_BASE_ADDR(pCommand->u.roamCmd.pRoamBssEntry,
                                tCsrScanResult, Link);
                    roamInfo.pBssDesc = &pScanResult->Result.BssDescriptor;
                }
                roamInfo.statusCode = pSession->joinFailStatusCode.statusCode;
                roamInfo.reasonCode = pSession->joinFailStatusCode.reasonCode;
                pSession->bRefAssocStartCnt--;
                csrRoamCallCallback(pMac, sessionId, pRoamInfo,
                                    pCommand->u.scanCmd.roamId,
                                    eCSR_ROAM_ASSOCIATION_COMPLETION,
                                    eCSR_ROAM_RESULT_FAILURE);
            }
            else
            {
                csrRoamCallCallback(pMac, sessionId, NULL,
                                    pCommand->u.scanCmd.roamId,
                                    eCSR_ROAM_ASSOCIATION_FAILURE,
                                    eCSR_ROAM_RESULT_FAILURE);
            }
        }
        else
        {
            roamResult = eCSR_ROAM_RESULT_IBSS_START_FAILED;
        }
        csrRoamCompletion(pMac, sessionId, NULL, pCommand, roamResult, eANI_BOOLEAN_FALSE);
    }

    return (status);
}


//After scan for cap changes, issue a roaming command to either reconnect to the AP or pick another one to connect
eHalStatus csrScanHandleCapChangeScanComplete(tpAniSirGlobal pMac, tANI_U32 sessionId)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tScanResultHandle hBSSList = NULL;
    tCsrScanResultFilter *pScanFilter = NULL;
    tANI_U32 roamId = 0;
    tCsrRoamProfile *pProfile = NULL;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    do
    {
        //Here is the profile we need to connect to
        pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
        if ( NULL == pScanFilter )
                status = eHAL_STATUS_FAILURE;
        else
                status = eHAL_STATUS_SUCCESS;
        if (!HAL_STATUS_SUCCESS(status))
            break;
        vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
        if (NULL == pSession) break;
        if (NULL == pSession->pCurRoamProfile)
        {
            pScanFilter->EncryptionType.numEntries = 1;
            pScanFilter->EncryptionType.encryptionType[0] = eCSR_ENCRYPT_TYPE_NONE;
        }
        else
        {
            //We have to make a copy of pCurRoamProfile because it will be free inside csrRoamIssueConnect
            pProfile = vos_mem_malloc(sizeof(tCsrRoamProfile));
            if ( NULL == pProfile )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if(!HAL_STATUS_SUCCESS(status))
                break;
            status = csrRoamCopyProfile(pMac, pProfile, pSession->pCurRoamProfile);
            if(!HAL_STATUS_SUCCESS(status))
                break;
            status = csrRoamPrepareFilterFromProfile(pMac, pProfile, pScanFilter);
        }//We have a profile
        roamId = GET_NEXT_ROAM_ID(&pMac->roam);
        if(HAL_STATUS_SUCCESS(status))
        {
            status = csrScanGetResult(pMac, pScanFilter, &hBSSList);
            if(HAL_STATUS_SUCCESS(status))
            {
                //we want to put the last connected BSS to the very beginning, if possible
                csrMoveBssToHeadFromBSSID(pMac, &pSession->connectedProfile.bssid, hBSSList);
                status = csrRoamIssueConnect(pMac, sessionId, pProfile, hBSSList,
                                            eCsrCapsChange, 0, eANI_BOOLEAN_TRUE, eANI_BOOLEAN_TRUE);
                if(!HAL_STATUS_SUCCESS(status))
                {
                    csrScanResultPurge(pMac, hBSSList);
                }
            }//Have scan result
            else
            {
                smsLog(pMac, LOGW, FL("cannot find matching BSS of "
                       MAC_ADDRESS_STR),
                       MAC_ADDR_ARRAY(pSession->connectedProfile.bssid));
                //Disconnect
                csrRoamDisconnectInternal(pMac, sessionId, eCSR_DISCONNECT_REASON_UNSPECIFIED);
            }
        }
    }while(0);
    if(pScanFilter)
    {
        csrFreeScanFilter(pMac, pScanFilter);
        vos_mem_free(pScanFilter);
    }
    if(NULL != pProfile)
    {
        csrReleaseProfile(pMac, pProfile);
        vos_mem_free(pProfile);
    }

    return (status);
}



eHalStatus csrScanResultPurge(tpAniSirGlobal pMac, tScanResultHandle hScanList)
{
    eHalStatus status = eHAL_STATUS_INVALID_PARAMETER;
    tScanResultList *pScanList = (tScanResultList *)hScanList;

    if(pScanList)
    {
        status = csrLLScanPurgeResult(pMac, &pScanList->List);
        csrLLClose(&pScanList->List);
        vos_mem_free(pScanList);
    }
    return (status);
}

/**
 * csr_get_altered_rssi() - Artificially increase/decrease RSSI
 * @mac_ctx:         Global MAC Context pointer.
 * @rssi:            Actual RSSI of the AP.
 * @channel_id:      Channel on which the AP is parked.
 * @bssid:           BSSID of the AP to connect to.
 *
 * This routine will apply the boost and penalty parameters
 * if the channel_id is of 5G band and it will also apply
 * the preferred bssid score if there is a match between
 * the bssid and the global preferred bssid list.
 *
 * Return:          The modified RSSI Value
 */
static int csr_get_altered_rssi(tpAniSirGlobal mac_ctx, int rssi,
		uint8_t channel_id, tCsrBssid *bssid)
{
	int modified_rssi;
	int boost_factor;
	int penalty_factor;
	int i;
	struct roam_ext_params *roam_params;
	modified_rssi = rssi;
	roam_params = &mac_ctx->roam.configParam.roam_params;
	VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_DEBUG,
			FL("LFR: Actual Rssi:%d"), rssi);
	/* If the 5G pref feature is enabled, apply the roaming
	 * parameters to boost or penalize the rssi.
	 * Boost Factor = boost_factor * (Actual RSSI - boost Threshold)
	 * Penalty Factor = penalty factor * (penalty threshold - Actual RSSI)
	*/
	if (CSR_IS_SELECT_5G_PREFERRED(mac_ctx)) {
		if (CSR_IS_CHANNEL_5GHZ(channel_id)) {
			if (rssi > roam_params->raise_rssi_thresh_5g) {
				/* Check and boost the threshold*/
				boost_factor = roam_params->raise_factor_5g *
				(rssi - roam_params->raise_rssi_thresh_5g);
				/* Check and penalize the threshold */
				modified_rssi +=
					CSR_MIN(roam_params->max_raise_rssi_5g,
					boost_factor);
			} else if(rssi < roam_params->drop_rssi_thresh_5g) {
				penalty_factor = roam_params->drop_factor_5g *
				(roam_params->drop_rssi_thresh_5g - rssi);
				modified_rssi -=
					CSR_MAX(roam_params->max_drop_rssi_5g,
					penalty_factor);
			}
		}
	}
	/* Check if there are preferred bssid and then apply the
	 * preferred score*/
	if (roam_params->num_bssid_favored) {
		for (i=0; i<roam_params->num_bssid_favored; i++) {
			if (csrIsMacAddressEqual(mac_ctx,
				&roam_params->bssid_favored[i],
				bssid)) {
				modified_rssi +=
				roam_params->bssid_favored_factor[i];
			}
		}
	}
	VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_DEBUG,
		FL("LFR: Modified Rssi:%d"), modified_rssi);
	return modified_rssi;
}

/**
 * csrGetBssPreferValue() - Get BSS Preference Value
 * @pMac:         Global MAC Context pointer.
 * @rssi:         Actual RSSI of the AP.
 * @roaming_scan  if the scan is because of roaming
 * @bssid:        BSSID of the AP to connect to.
 * @channel_id:   Channel on which the AP is parked.
 *
 *
 * This routine helps in determining the preference value
 * of a particular BSS in the scan result which is further
 * used in the sorting logic of the final candidate AP's.
 *
 * Return:          The preference Value for a BSS.
 */
static tANI_U32 csrGetBssPreferValue(tpAniSirGlobal pMac, int rssi,
		int roaming_scan, tCsrBssid *bssid, int channel_id)
{
	tANI_U32 ret = 0;
	int i;

	if (roaming_scan)
		rssi = csr_get_altered_rssi(pMac, rssi, channel_id, bssid);

	i = CSR_NUM_RSSI_CAT - 1;
	while(i >= 0)
	{
		if(rssi >= pMac->roam.configParam.RSSICat[i])
		{
			ret = pMac->roam.configParam.BssPreferValue[i];
			break;
		}
		i--;
	};

	return (ret);
}


//Return a CapValue base on the capabilities of a BSS
static tANI_U32 csrGetBssCapValue(tpAniSirGlobal pMac, tSirBssDescription *pBssDesc, tDot11fBeaconIEs *pIes)
{
    tANI_U32 ret = CSR_BSS_CAP_VALUE_NONE;
#if  defined (WLAN_FEATURE_VOWIFI_11R) || defined (FEATURE_WLAN_ESE) || defined(FEATURE_WLAN_LFR)
    if(CSR_IS_ROAM_PREFER_5GHZ(pMac) || CSR_IS_SELECT_5G_PREFERRED(pMac))
    {
        if((pBssDesc) && CSR_IS_CHANNEL_5GHZ(pBssDesc->channelId))
        {
            ret += CSR_BSS_CAP_VALUE_5GHZ;
        }
    }
#endif
    /* if strict select 5GHz is non-zero then ignore the capability checking */
    if (pIes && !CSR_IS_SELECT_5GHZ_MARGIN(pMac))
    {
        //We only care about 11N capability
        if (pIes->VHTCaps.present)
            ret += CSR_BSS_CAP_VALUE_VHT;
        else if (pIes->HTCaps.present)
            ret += CSR_BSS_CAP_VALUE_HT;
        if(CSR_IS_QOS_BSS(pIes))
        {
            ret += CSR_BSS_CAP_VALUE_WMM;
            //Give advantage to UAPSD
            if(CSR_IS_UAPSD_BSS(pIes))
            {
                ret += CSR_BSS_CAP_VALUE_UAPSD;
            }
        }
    }

    return (ret);
}

/**
 * csr_is_better_rssi() - Is bss1 better than bss2
 * @mac_ctx:             Global MAC Context pointer.
 * @bss1:                Pointer to the first BSS.
 * @bss2:                Pointer to the second BSS.
 * @roaming_scan_filter  if the scan is because of roaming
 *
 *
 * This routine helps in determining the preference value
 * of a particular BSS in the scan result which is further
 * used in the sorting logic of the final candidate AP's.
 *
 * Return:          true, if bss1 is better than bss2
 *                  false, if bss2 is better than bss1.
 */
static bool csr_is_better_rssi(tpAniSirGlobal mac_ctx,
		tCsrScanResult *bss1, tCsrScanResult *bss2,
		int roaming_scan_filter)
{
	bool ret;
	int rssi1, rssi2;

	rssi1 = bss1->Result.BssDescriptor.rssi;
	rssi2 = bss2->Result.BssDescriptor.rssi;
	if (!roaming_scan_filter)
		if (CSR_IS_BETTER_RSSI(rssi1, rssi2))
			ret = true;
		else
			ret = false;
	else {
		/* Apply the boost and penlty logic and check
		 * which is the best RSSI*/
		rssi1 = csr_get_altered_rssi(mac_ctx, rssi1,
				bss1->Result.BssDescriptor.channelId,
				&bss1->Result.BssDescriptor.bssId);
		rssi2 = csr_get_altered_rssi(mac_ctx, rssi2,
				bss2->Result.BssDescriptor.channelId,
				&bss2->Result.BssDescriptor.bssId);
		if (CSR_IS_BETTER_RSSI(rssi1, rssi2))
			ret = true;
		else
			ret = false;
	}
	return ret;
}
/* To check whether pBss1 is better than pBss2 */
static tANI_BOOLEAN csrIsBetterBss(tpAniSirGlobal mac_ctx,
		tCsrScanResult *pBss1, tCsrScanResult *pBss2,
		int roam_scan_filter)
{
	tANI_BOOLEAN ret;

	if(CSR_IS_BETTER_PREFER_VALUE(pBss1->preferValue, pBss2->preferValue))
		ret = eANI_BOOLEAN_TRUE;
	else if(CSR_IS_EQUAL_PREFER_VALUE
		(pBss1->preferValue, pBss2->preferValue)) {
		if(CSR_IS_BETTER_CAP_VALUE(pBss1->capValue, pBss2->capValue))
			ret = eANI_BOOLEAN_TRUE;
		else if (CSR_IS_EQUAL_CAP_VALUE
			(pBss1->capValue, pBss2->capValue)) {
			if (csr_is_better_rssi
				(mac_ctx, pBss1, pBss2, roam_scan_filter))
				ret = eANI_BOOLEAN_TRUE;
			else
				ret = eANI_BOOLEAN_FALSE;
		}
		else
			ret = eANI_BOOLEAN_FALSE;
	}
	else
		ret = eANI_BOOLEAN_FALSE;

	return (ret);
}


#ifdef FEATURE_WLAN_LFR
//Add the channel to the occupiedChannels array
static void csrScanAddToOccupiedChannels(
        tpAniSirGlobal pMac,
        tCsrScanResult *pResult,
        tANI_U8 sessionId,
        tCsrChannel *pOccupiedChannels,
        tDot11fBeaconIEs *pIes)
{
    eHalStatus status;
    tANI_U8   channel;
    tANI_U8 numOccupiedChannels = pOccupiedChannels->numChannels;
    tANI_U8 *pOccupiedChannelList = pOccupiedChannels->channelList;

    channel = pResult->Result.BssDescriptor.channelId;

    if (!csrIsChannelPresentInList(pOccupiedChannelList, numOccupiedChannels, channel)
        && csrNeighborRoamConnectedProfileMatch(pMac, sessionId, pResult, pIes))
    {
        status = csrAddToChannelListFront(pOccupiedChannelList, numOccupiedChannels, channel);
        if(HAL_STATUS_SUCCESS(status))
        {
            pOccupiedChannels->numChannels++;
            smsLog(pMac, LOG2, FL("Added channel %d to the list (count=%d)"),
                                  channel, pOccupiedChannels->numChannels);
            if (pOccupiedChannels->numChannels > CSR_BG_SCAN_OCCUPIED_CHANNEL_LIST_LEN)
                pOccupiedChannels->numChannels = CSR_BG_SCAN_OCCUPIED_CHANNEL_LIST_LEN;
        }
    }
}
#endif

//Put the BSS into the scan result list
//pIes can not be NULL
static void csrScanAddResult(tpAniSirGlobal pMac, tCsrScanResult *pResult,
                                tDot11fBeaconIEs *pIes, tANI_U32 sessionId)
{
#ifdef FEATURE_WLAN_LFR
    tpCsrNeighborRoamControlInfo pNeighborRoamInfo =
                                        &pMac->roam.neighborRoamInfo[sessionId];
#endif
    pResult->preferValue = csrGetBssPreferValue(pMac,
        (int)pResult->Result.BssDescriptor.rssi, 0, NULL, 0);
    pResult->capValue =
            csrGetBssCapValue(pMac, &pResult->Result.BssDescriptor, pIes);
    csrLLInsertTail( &pMac->scan.scanResultList, &pResult->Link, LL_ACCESS_LOCK );
#ifdef FEATURE_WLAN_LFR
    if(0 == pNeighborRoamInfo->cfgParams.channelInfo.numOfChannels)
    {
        /* Build the occupied channel list, only if "gNeighborScanChannelList" is
           NOT set in the cfg.ini file */
        csrScanAddToOccupiedChannels(pMac, pResult, sessionId,
                                     &pMac->scan.occupiedChannels[sessionId],
                                     pIes);
    }
#endif
}


eHalStatus csrScanGetResult(tpAniSirGlobal pMac, tCsrScanResultFilter *pFilter, tScanResultHandle *phResult)
{
    eHalStatus status;
    tScanResultList *pRetList;
    tCsrScanResult *pResult, *pBssDesc;
    tANI_U32 count = 0;
    tListElem *pEntry;
    tANI_U32 bssLen, allocLen;
    eCsrEncryptionType uc = eCSR_ENCRYPT_TYPE_NONE, mc = eCSR_ENCRYPT_TYPE_NONE;
    eCsrAuthType auth = eCSR_AUTH_TYPE_OPEN_SYSTEM;
    tDot11fBeaconIEs *pIes, *pNewIes;
    tANI_BOOLEAN fMatch;
    tANI_U16 i = 0;
    struct roam_ext_params *roam_params = NULL;
    uint8_t scan_filter_for_roam = 0;

    if(phResult)
    {
        *phResult = CSR_INVALID_SCANRESULT_HANDLE;
    }
    if (pFilter)
       scan_filter_for_roam = pFilter->scan_filter_for_roam;

    if (pMac->roam.configParam.nSelect5GHzMargin ||
       (CSR_IS_SELECT_5G_PREFERRED(pMac) && scan_filter_for_roam))
    {
        VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_DEBUG,
                  FL("LFR: Filter is prepared for roaming"));
        pMac->scan.inScanResultBestAPRssi = -128;
        roam_params = &pMac->roam.configParam.roam_params;
#ifdef WLAN_DEBUG_ROAM_OFFLOAD
        VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
                  FL("nSelect5GHzMargin"));
#endif
        csrLLLock(&pMac->scan.scanResultList);
  /* For roaming scans and 5G preferred roaming, there is no
   * need to check the filter match and also re-program the
   * RSSI bucket categories, since we use the RSSI values
   * while setting the preference value for the BSS.
   * There is no need to check the match for roaming since
   * it is already done.*/
  if(!(CSR_IS_SELECT_5G_PREFERRED(pMac)
               && scan_filter_for_roam)) {
        /* Find out the best AP Rssi going thru the scan results */
        pEntry = csrLLPeekHead(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
        while ( NULL != pEntry)
        {
            pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
            fMatch = FALSE;


            if (pFilter)
            for(i = 0; i < pFilter->SSIDs.numOfSSIDs; i++)
            {
                fMatch = csrIsSsidMatch( pMac, pFilter->SSIDs.SSIDList[i].SSID.ssId, pFilter->SSIDs.SSIDList[i].SSID.length,
                                        pBssDesc->Result.ssId.ssId,
                                        pBssDesc->Result.ssId.length, eANI_BOOLEAN_TRUE );
                if (fMatch)
                {
                    pIes = (tDot11fBeaconIEs *)( pBssDesc->Result.pvIes );

                    //At this time, pBssDescription->Result.pvIes may be NULL
                    if( !pIes && (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
                                  &pBssDesc->Result.BssDescriptor, &pIes))) )
                    {
                        continue;
                    }

                    smsLog(pMac, LOG1, FL("SSID Matched"));

                    if ( pFilter->bOSENAssociation )
                    {
                        fMatch = TRUE;
                    }
                    else
                    {
#ifdef WLAN_FEATURE_11W
                        fMatch = csrIsSecurityMatch(pMac, &pFilter->authType,
                                                &pFilter->EncryptionType,
                                                &pFilter->mcEncryptionType,
                                                &pFilter->MFPEnabled,
                                                &pFilter->MFPRequired,
                                                &pFilter->MFPCapable,
                                                &pBssDesc->Result.BssDescriptor,
                                                pIes, NULL, NULL, NULL );
#else
                        fMatch = csrIsSecurityMatch(pMac, &pFilter->authType,
                                                &pFilter->EncryptionType,
                                                &pFilter->mcEncryptionType,
                                                NULL, NULL, NULL,
                                                &pBssDesc->Result.BssDescriptor,
                                                pIes, NULL, NULL, NULL );
#endif
                    }
                    if ((pBssDesc->Result.pvIes == NULL) && pIes)
                         vos_mem_free(pIes);

                    if (fMatch)
                        smsLog(pMac, LOG1, FL(" Security Matched"));
                }
            }

            if (fMatch && (pBssDesc->Result.BssDescriptor.rssi > pMac->scan.inScanResultBestAPRssi))
            {
                smsLog(pMac, LOG1, FL("Best AP Rssi changed from %d to %d"),
                                       pMac->scan.inScanResultBestAPRssi,
                                       pBssDesc->Result.BssDescriptor.rssi);
                pMac->scan.inScanResultBestAPRssi = pBssDesc->Result.BssDescriptor.rssi;
            }
            pEntry = csrLLNext(&pMac->scan.scanResultList, pEntry, LL_ACCESS_NOLOCK);
        }
   }

        if ((-128 != pMac->scan.inScanResultBestAPRssi) ||
             (CSR_IS_SELECT_5G_PREFERRED(pMac) &&
              scan_filter_for_roam))
        {
            smsLog(pMac, LOG1, FL("Best AP Rssi is %d"), pMac->scan.inScanResultBestAPRssi);
            /* Modify Rssi category based on best AP Rssi */
	    if (-128 != pMac->scan.inScanResultBestAPRssi)
            csrAssignRssiForCategory(pMac, pMac->scan.inScanResultBestAPRssi, pMac->roam.configParam.bCatRssiOffset);

            pEntry = csrLLPeekHead(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
            while ( NULL != pEntry)
            {
                pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );

                /* re-assign preference value based on (modified rssi bucket (or)
                 * prefer 5G feature.*/
                pBssDesc->preferValue = csrGetBssPreferValue(pMac,
                  (int)pBssDesc->Result.BssDescriptor.rssi,
                  scan_filter_for_roam,
                  &pBssDesc->Result.BssDescriptor.bssId,
                  pBssDesc->Result.BssDescriptor.channelId);

                smsLog(pMac, LOG2, FL("BSSID("MAC_ADDRESS_STR
                       ") Rssi(%d) Chnl(%d) PrefVal(%u) SSID=%.*s"),
                       MAC_ADDR_ARRAY(pBssDesc->Result.BssDescriptor.bssId),
                       pBssDesc->Result.BssDescriptor.rssi,
                       pBssDesc->Result.BssDescriptor.channelId,
                       pBssDesc->preferValue,
                       pBssDesc->Result.ssId.length, pBssDesc->Result.ssId.ssId);

                pEntry = csrLLNext(&pMac->scan.scanResultList, pEntry, LL_ACCESS_NOLOCK);
            }
        }

        csrLLUnlock(&pMac->scan.scanResultList);
    }

    pRetList = vos_mem_malloc(sizeof(tScanResultList));
    if ( NULL == pRetList )
        status = eHAL_STATUS_FAILURE;
    else
    {
        status = eHAL_STATUS_SUCCESS;
        vos_mem_set(pRetList, sizeof(tScanResultList), 0);
        csrLLOpen(pMac->hHdd, &pRetList->List);
        pRetList->pCurEntry = NULL;

        csrLLLock(&pMac->scan.scanResultList);
        pEntry = csrLLPeekHead( &pMac->scan.scanResultList, LL_ACCESS_NOLOCK );
        while( pEntry )
        {
            pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
            pIes = (tDot11fBeaconIEs *)( pBssDesc->Result.pvIes );
            /*
             * If pBssDesc->Result.pvIes is NULL, we need to free any memory
             * allocated by csrMatchBSS for any error condition, otherwise,
             * it will be freed later.
             */
            fMatch = eANI_BOOLEAN_FALSE;
            pNewIes = NULL;

            if(pFilter)
            {
                fMatch = csrMatchBSS(pMac, &pBssDesc->Result.BssDescriptor, pFilter, &auth, &uc, &mc, &pIes);
#ifdef WLAN_DEBUG_ROAM_OFFLOAD
                VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_DEBUG,
                          FL("csrMatchBSS fmatch %d"), fMatch);
#endif
                if( NULL != pIes )
                {
                    //Only save it when matching
                    if(fMatch)
                    {
                        if( !pBssDesc->Result.pvIes )
                        {
                            //csrMatchBSS allocates the memory. Simply pass it and it is freed later
                            pNewIes = pIes;
                        }
                        else
                        {
                            //The pIes is allocated by someone else. make a copy
                            //Only to save parsed IEs if caller provides a filter. Most likely the caller
                            //is using to for association, hence save the parsed IEs
                            pNewIes = vos_mem_malloc(sizeof(tDot11fBeaconIEs));
                            if ( NULL == pNewIes )
                                status = eHAL_STATUS_FAILURE;
                            else
                                status = eHAL_STATUS_SUCCESS;
                            if ( HAL_STATUS_SUCCESS( status ) )
                            {
                                vos_mem_copy(pNewIes, pIes, sizeof( tDot11fBeaconIEs ));
                            }
                            else
                            {
                                smsLog(pMac, LOGE, FL(" fail to allocate memory for IEs"));
                                //Need to free memory allocated by csrMatchBSS
                                if( !pBssDesc->Result.pvIes )
                                {
                                    vos_mem_free(pIes);
                                }
                                break;
                            }
                        }
                    }//fMatch
                    else if( !pBssDesc->Result.pvIes )
                    {
                        vos_mem_free(pIes);
                    }
                }
            }
            if(NULL == pFilter || fMatch)
            {
                bssLen = pBssDesc->Result.BssDescriptor.length + sizeof(pBssDesc->Result.BssDescriptor.length);
                allocLen = sizeof( tCsrScanResult ) + bssLen;
                pResult = vos_mem_malloc(allocLen);
                if ( NULL == pResult )
                        status = eHAL_STATUS_FAILURE;
                else
                        status = eHAL_STATUS_SUCCESS;
                if(!HAL_STATUS_SUCCESS(status))
                {
                    smsLog(pMac, LOGE, FL("  fail to allocate memory for scan result, len=%d"), allocLen);
                    if(pNewIes)
                    {
                        vos_mem_free(pNewIes);
                    }
                    break;
                }
                vos_mem_set(pResult, allocLen, 0);
                pResult->capValue = pBssDesc->capValue;
                pResult->preferValue = pBssDesc->preferValue;
                pResult->ucEncryptionType = uc;
                pResult->mcEncryptionType = mc;
                pResult->authType = auth;
                pResult->Result.ssId = pBssDesc->Result.ssId;
                pResult->Result.timer = pBssDesc->Result.timer;
                //save the pIes for later use
                pResult->Result.pvIes = pNewIes;
                //save bss description
                vos_mem_copy(&pResult->Result.BssDescriptor,
                             &pBssDesc->Result.BssDescriptor, bssLen);
                //No need to lock pRetList because it is locally allocated and no outside can access it at this time
                if(csrLLIsListEmpty(&pRetList->List, LL_ACCESS_NOLOCK))
                {
                    csrLLInsertTail(&pRetList->List, &pResult->Link, LL_ACCESS_NOLOCK);
                }
                else
                {
                    //To sort the list
                    tListElem *pTmpEntry;
                    tCsrScanResult *pTmpResult;

                    pTmpEntry = csrLLPeekHead(&pRetList->List, LL_ACCESS_NOLOCK);
                    while(pTmpEntry)
                    {
                        pTmpResult = GET_BASE_ADDR( pTmpEntry, tCsrScanResult, Link );
                       if(csrIsBetterBss(pMac, pResult, pTmpResult,
                                          scan_filter_for_roam))
                        {
                           VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_DEBUG,
                             FL("BSSID("MAC_ADDRESS_STR") better than BSSID("MAC_ADDRESS_STR")"),
                             MAC_ADDR_ARRAY
                             (pResult->Result.BssDescriptor.bssId),
                             MAC_ADDR_ARRAY
                             (pTmpResult->Result.BssDescriptor.bssId));
                            csrLLInsertEntry(&pRetList->List, pTmpEntry, &pResult->Link, LL_ACCESS_NOLOCK);
                            //To indicate we are done
                            pResult = NULL;
                            break;
                        }
                        pTmpEntry = csrLLNext(&pRetList->List, pTmpEntry, LL_ACCESS_NOLOCK);
                    }
                    if(pResult != NULL)
                    {
                        /* This one is'nt better than anyone or the first one */
                           VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_DEBUG,
                             FL("Insert BSSID("MAC_ADDRESS_STR") to TAIL"),
                             MAC_ADDR_ARRAY
                             (pResult->Result.BssDescriptor.bssId));
                        csrLLInsertTail(&pRetList->List, &pResult->Link, LL_ACCESS_NOLOCK);
                    }
                }
                count++;
            }
            pEntry = csrLLNext( &pMac->scan.scanResultList, pEntry, LL_ACCESS_NOLOCK );
        }//while
        csrLLUnlock(&pMac->scan.scanResultList);

        smsLog(pMac, LOG2, FL("return %d BSS"), csrLLCount(&pRetList->List));

        if( !HAL_STATUS_SUCCESS(status) || (phResult == NULL) )
        {
            //Fail or No one wants the result.
            csrScanResultPurge(pMac, (tScanResultHandle)pRetList);
        }
        else
        {
            if(0 == count)
            {
                //We are here meaning the there is no match
                csrLLClose(&pRetList->List);
                vos_mem_free(pRetList);
                status = eHAL_STATUS_E_NULL_VALUE;
            }
            else if(phResult)
            {
                *phResult = pRetList;
            }
        }
    }//Allocated pRetList

    return (status);
}

/*
 * NOTE: This routine is being added to make
 * sure that scan results are not being flushed
 * while roaming. If the scan results are flushed,
 * we are unable to recover from
 * csrRoamRoamingStateDisassocRspProcessor.
 * If it is needed to remove this routine,
 * first ensure that we recover gracefully from
 * csrRoamRoamingStateDisassocRspProcessor if
 * csrScanGetResult returns with a failure because
 * of not being able to find the roaming BSS.
 */
tANI_U8 csrScanFlushDenied(tpAniSirGlobal pMac, tANI_U8 sessionId)
{
    switch(pMac->roam.neighborRoamInfo[sessionId].neighborRoamState) {
        case eCSR_NEIGHBOR_ROAM_STATE_REPORT_SCAN:
        case eCSR_NEIGHBOR_ROAM_STATE_PREAUTHENTICATING:
        case eCSR_NEIGHBOR_ROAM_STATE_PREAUTH_DONE:
        case eCSR_NEIGHBOR_ROAM_STATE_REASSOCIATING:
            return (pMac->roam.neighborRoamInfo[sessionId].neighborRoamState);
        default:
            return 0;
    }
}

eHalStatus csrScanFlushResult(tpAniSirGlobal pMac, tANI_U8 sessionId)
{
    tANI_U8 isFlushDenied = csrScanFlushDenied(pMac, sessionId);
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSirMbMsg *pMsg;
    tANI_U16 msgLen;

    if (isFlushDenied) {
        smsLog(pMac, LOGW, "%s: scan flush denied in roam state %d",
                __func__, isFlushDenied);
        return eHAL_STATUS_FAILURE;
    }

    /* prepare and send clear cached scan results msg to lim */
    msgLen = (tANI_U16)(sizeof( tSirMbMsg ));
    pMsg = vos_mem_malloc(msgLen);
    if ( NULL != pMsg ) {
        vos_mem_set((void *)pMsg, msgLen, 0);
        pMsg->type = pal_cpu_to_be16((tANI_U16)eWNI_SME_CLEAR_LIM_SCAN_CACHE);
        pMsg->msgLen = pal_cpu_to_be16(msgLen);
        palSendMBMessage(pMac->hHdd, pMsg);
    } else {
        status = eHAL_STATUS_FAILED_ALLOC;
    }

    csrLLScanPurgeResult( pMac, &pMac->scan.tempScanResults );
    csrLLScanPurgeResult( pMac, &pMac->scan.scanResultList );
    return( status );
}

eHalStatus csrScanFlushSelectiveResult(tpAniSirGlobal pMac, v_BOOL_t flushP2P)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tListElem *pEntry,*pFreeElem;
    tCsrScanResult *pBssDesc;
    tDblLinkList *pList = &pMac->scan.scanResultList;

    csrLLLock(pList);

    pEntry = csrLLPeekHead( pList, LL_ACCESS_NOLOCK );
    while( pEntry != NULL)
    {
        pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
        if( flushP2P == vos_mem_compare( pBssDesc->Result.ssId.ssId,
                                         "DIRECT-", 7) )
        {
            pFreeElem = pEntry;
            pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
            csrLLRemoveEntry(pList, pFreeElem, LL_ACCESS_NOLOCK);
            csrFreeScanResultEntry( pMac, pBssDesc );
            continue;
        }
        pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
    }

    csrLLUnlock(pList);

    return (status);
}

void csrScanFlushBssEntry(tpAniSirGlobal pMac,
                             tpSmeCsaOffloadInd pCsaOffloadInd)
{
    tListElem *pEntry,*pFreeElem;
    tCsrScanResult *pBssDesc;
    tDblLinkList *pList = &pMac->scan.scanResultList;

    csrLLLock(pList);

    pEntry = csrLLPeekHead( pList, LL_ACCESS_NOLOCK );
    while( pEntry != NULL)
    {
        pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
        if( vos_mem_compare(pBssDesc->Result.BssDescriptor.bssId,
                            pCsaOffloadInd->bssId, sizeof(tSirMacAddr)) )
        {
            pFreeElem = pEntry;
            pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
            csrLLRemoveEntry(pList, pFreeElem, LL_ACCESS_NOLOCK);
            csrFreeScanResultEntry( pMac, pBssDesc );
            smsLog( pMac, LOG1, FL("Removed BSS entry:%pM"),
                    pCsaOffloadInd->bssId);
            continue;
        }

        pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
    }

    csrLLUnlock(pList);
}

/**
 * csrCheck11dChannel
 *
 *FUNCTION:
 * This function is called from csrScanFilterResults function and
 * compare channel number with given channel list.
 *
 *LOGIC:
 * Check Scan result channel number with CFG channel list
 *
 *ASSUMPTIONS:
 *
 *
 *NOTE:
 *
 * @param  channelId      channel number
 * @param  pChannelList   Pointer to channel list
 * @param  numChannels    Number of channel in channel list
 *
 * @return Status
 */

eHalStatus csrCheck11dChannel(tANI_U8 channelId, tANI_U8 *pChannelList, tANI_U32 numChannels)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tANI_U8 i = 0;

    for (i = 0; i < numChannels; i++)
    {
        if(pChannelList[ i ] == channelId)
        {
            status = eHAL_STATUS_SUCCESS;
            break;
        }
    }
    return status;
}

/**
 * csrScanFilterResults
 *
 *FUNCTION:
 * This function is called from csrApplyCountryInformation function and
 * filter scan result based on valid channel list number.
 *
 *LOGIC:
 * Get scan result from scan list and Check Scan result channel number
 * with 11d channel list if channel number is found in 11d channel list
 * then do not remove scan result entry from scan list
 *
 *ASSUMPTIONS:
 *
 *
 *NOTE:
 *
 * @param  pMac        Pointer to Global MAC structure
 *
 * @return Status
 */

eHalStatus csrScanFilterResults(tpAniSirGlobal pMac)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tListElem *pEntry,*pTempEntry;
    tCsrScanResult *pBssDesc;
    tANI_U32 len = sizeof(pMac->roam.validChannelList);

    /* Get valid channels list from CFG */
    if (!HAL_STATUS_SUCCESS(csrGetCfgValidChannels(pMac,
                                      pMac->roam.validChannelList, &len)))
    {
        smsLog( pMac, LOGE, "Failed to get Channel list from CFG");
    }

    csrLLLock(&pMac->scan.scanResultList);

    pEntry = csrLLPeekHead(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
    while( pEntry )
    {
        pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
        pTempEntry = csrLLNext(&pMac->scan.scanResultList, pEntry,
                                                            LL_ACCESS_NOLOCK);
        if(csrCheck11dChannel(pBssDesc->Result.BssDescriptor.channelId,
                                              pMac->roam.validChannelList, len))
        {
            /* Remove Scan result which does not have 11d channel */
            if( csrLLRemoveEntry(&pMac->scan.scanResultList, pEntry,
                                                              LL_ACCESS_NOLOCK))
            {
                csrFreeScanResultEntry( pMac, pBssDesc );
            }
        }
        else
        {
            smsLog( pMac, LOG1, FL("%d is a Valid channel"),
                    pBssDesc->Result.BssDescriptor.channelId);
        }
        pEntry = pTempEntry;
    }

    csrLLUnlock(&pMac->scan.scanResultList);
    csrLLLock(&pMac->scan.tempScanResults);

    pEntry = csrLLPeekHead(&pMac->scan.tempScanResults, LL_ACCESS_NOLOCK);
    while( pEntry )
    {
        pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
        pTempEntry = csrLLNext(&pMac->scan.tempScanResults, pEntry,
                                                            LL_ACCESS_NOLOCK);
        if(csrCheck11dChannel(pBssDesc->Result.BssDescriptor.channelId,
                              pMac->roam.validChannelList, len))
        {
            /* Remove Scan result which does not have 11d channel */
            if(csrLLRemoveEntry(&pMac->scan.tempScanResults, pEntry,
                        LL_ACCESS_NOLOCK))
            {
                csrFreeScanResultEntry( pMac, pBssDesc );
            }
        }
        else
        {
            smsLog( pMac, LOG1, FL("%d is a Valid channel"),
                    pBssDesc->Result.BssDescriptor.channelId);
        }
        pEntry = pTempEntry;
    }

    csrLLUnlock(&pMac->scan.tempScanResults);
    return status;
}


eHalStatus csrScanCopyResultList(tpAniSirGlobal pMac, tScanResultHandle hIn, tScanResultHandle *phResult)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tScanResultList *pRetList, *pInList = (tScanResultList *)hIn;
    tCsrScanResult *pResult, *pScanResult;
    tANI_U32 count = 0;
    tListElem *pEntry;
    tANI_U32 bssLen, allocLen;

    if(phResult)
    {
        *phResult = CSR_INVALID_SCANRESULT_HANDLE;
    }
    pRetList = vos_mem_malloc(sizeof(tScanResultList));
    if ( NULL == pRetList )
        status = eHAL_STATUS_FAILURE;
    else
    {
        vos_mem_set(pRetList, sizeof(tScanResultList), 0);
        csrLLOpen(pMac->hHdd, &pRetList->List);
        pRetList->pCurEntry = NULL;
        csrLLLock(&pMac->scan.scanResultList);
        csrLLLock(&pInList->List);

        pEntry = csrLLPeekHead( &pInList->List, LL_ACCESS_NOLOCK );
        while( pEntry )
        {
            pScanResult = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
            bssLen = pScanResult->Result.BssDescriptor.length + sizeof(pScanResult->Result.BssDescriptor.length);
            allocLen = sizeof( tCsrScanResult ) + bssLen;
            pResult = vos_mem_malloc(allocLen);
            if ( NULL == pResult )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if (!HAL_STATUS_SUCCESS(status))
            {
                csrScanResultPurge(pMac, (tScanResultHandle *)pRetList);
                count = 0;
                break;
            }
            vos_mem_set(pResult, allocLen , 0);
            vos_mem_copy(&pResult->Result.BssDescriptor, &pScanResult->Result.BssDescriptor, bssLen);
            if( pScanResult->Result.pvIes )
            {
                pResult->Result.pvIes = vos_mem_malloc(sizeof( tDot11fBeaconIEs ));
                if ( NULL == pResult->Result.pvIes )
                        status = eHAL_STATUS_FAILURE;
                else
                        status = eHAL_STATUS_SUCCESS;
                if (!HAL_STATUS_SUCCESS(status))
                {
                    //Free the memory we allocate above first
                    vos_mem_free(pResult);
                    csrScanResultPurge(pMac, (tScanResultHandle *)pRetList);
                    count = 0;
                    break;
                }
                vos_mem_copy(pResult->Result.pvIes, pScanResult->Result.pvIes,
                             sizeof( tDot11fBeaconIEs ));
            }
            csrLLInsertTail(&pRetList->List, &pResult->Link, LL_ACCESS_LOCK);
            count++;
            pEntry = csrLLNext( &pInList->List, pEntry, LL_ACCESS_NOLOCK );
        }//while
        csrLLUnlock(&pInList->List);
        csrLLUnlock(&pMac->scan.scanResultList);

        if(HAL_STATUS_SUCCESS(status))
        {
            if(0 == count)
            {
                csrLLClose(&pRetList->List);
                vos_mem_free(pRetList);
                status = eHAL_STATUS_E_NULL_VALUE;
            }
            else if(phResult)
            {
                *phResult = pRetList;
            }
        }
    }//Allocated pRetList

    return (status);
}



eHalStatus csrScanningStateMsgProcessor( tpAniSirGlobal pMac, void *pMsgBuf )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSirMbMsg *pMsg = (tSirMbMsg *)pMsgBuf;

    if((eWNI_SME_SCAN_RSP == pMsg->type) || (eWNI_SME_GET_SCANNED_CHANNEL_RSP == pMsg->type))
    {
        status = csrScanSmeScanResponse( pMac, pMsgBuf );
    }
    else
    {
        if(pMsg->type == eWNI_SME_UPPER_LAYER_ASSOC_CNF)
        {
            tCsrRoamSession  *pSession;
            tSirSmeAssocIndToUpperLayerCnf *pUpperLayerAssocCnf;
            tCsrRoamInfo roamInfo;
            tCsrRoamInfo *pRoamInfo = NULL;
            tANI_U32 sessionId;
            eHalStatus status;
            smsLog( pMac, LOG1, FL("Scanning : ASSOCIATION confirmation can be given to upper layer "));
            vos_mem_set(&roamInfo, sizeof(tCsrRoamInfo), 0);
            pRoamInfo = &roamInfo;
            pUpperLayerAssocCnf = (tSirSmeAssocIndToUpperLayerCnf *)pMsgBuf;
            status = csrRoamGetSessionIdFromBSSID( pMac, (tCsrBssid *)pUpperLayerAssocCnf->bssId, &sessionId );
            pSession = CSR_GET_SESSION(pMac, sessionId);

            if(!pSession)
            {
                smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
                return eHAL_STATUS_FAILURE;
            }

            pRoamInfo->statusCode = eSIR_SME_SUCCESS; //send the status code as Success
            pRoamInfo->u.pConnectedProfile = &pSession->connectedProfile;
            pRoamInfo->staId = (tANI_U8)pUpperLayerAssocCnf->aid;
            pRoamInfo->rsnIELen = (tANI_U8)pUpperLayerAssocCnf->rsnIE.length;
            pRoamInfo->prsnIE = pUpperLayerAssocCnf->rsnIE.rsnIEdata;
            pRoamInfo->addIELen = (tANI_U8)pUpperLayerAssocCnf->addIE.length;
            pRoamInfo->paddIE = pUpperLayerAssocCnf->addIE.addIEdata;
            vos_mem_copy(pRoamInfo->peerMac, pUpperLayerAssocCnf->peerMacAddr, sizeof(tSirMacAddr));
            vos_mem_copy(&pRoamInfo->bssid, pUpperLayerAssocCnf->bssId, sizeof(tCsrBssid));
            pRoamInfo->wmmEnabledSta = pUpperLayerAssocCnf->wmmEnabledSta;
            if(CSR_IS_INFRA_AP(pRoamInfo->u.pConnectedProfile) )
            {
                pMac->roam.roamSession[sessionId].connectState = eCSR_ASSOC_STATE_TYPE_INFRA_CONNECTED;
                pRoamInfo->fReassocReq = pUpperLayerAssocCnf->reassocReq;
                status = csrRoamCallCallback(pMac, sessionId, pRoamInfo, 0, eCSR_ROAM_INFRA_IND, eCSR_ROAM_RESULT_INFRA_ASSOCIATION_CNF);
            }
            if(CSR_IS_WDS_AP( pRoamInfo->u.pConnectedProfile))
            {
                vos_sleep( 100 );
                pMac->roam.roamSession[sessionId].connectState = eCSR_ASSOC_STATE_TYPE_WDS_CONNECTED;//Sta
                status = csrRoamCallCallback(pMac, sessionId, pRoamInfo, 0, eCSR_ROAM_WDS_IND, eCSR_ROAM_RESULT_WDS_ASSOCIATION_IND);//Sta
            }

        }
        else
        {

            if( csrIsAnySessionInConnectState( pMac ) )
            {
                //In case of we are connected, we need to check whether connect status changes
                //because scan may also run while connected.
                csrRoamCheckForLinkStatusChange( pMac, ( tSirSmeRsp * )pMsgBuf );
            }
            else
            {
                smsLog( pMac, LOGW, "Message [0x%04x] received in state, when expecting Scan Response", pMsg->type );
            }
        }
    }

    return (status);
}



void csrCheckNSaveWscIe(tpAniSirGlobal pMac, tSirBssDescription *pNewBssDescr, tSirBssDescription *pOldBssDescr)
{
    int idx, len;
    tANI_U8 *pbIe;

    //If failed to remove, assuming someone else got it.
    if((pNewBssDescr->fProbeRsp != pOldBssDescr->fProbeRsp) &&
       (0 == pNewBssDescr->WscIeLen))
    {
        idx = 0;
        len = pOldBssDescr->length - sizeof(tSirBssDescription) +
                sizeof(tANI_U16) + sizeof(tANI_U32) - DOT11F_IE_WSCPROBERES_MIN_LEN - 2;
        pbIe = (tANI_U8 *)pOldBssDescr->ieFields;
        //Save WPS IE if it exists
        pNewBssDescr->WscIeLen = 0;
        while(idx < len)
        {
            if((DOT11F_EID_WSCPROBERES == pbIe[0]) &&
                (0x00 == pbIe[2]) && (0x50 == pbIe[3]) && (0xf2 == pbIe[4]) && (0x04 == pbIe[5]))
            {
                /* Found it */
                if((DOT11F_IE_WSCPROBERES_MAX_LEN - 2) >= pbIe[1])
                {
                    vos_mem_copy(pNewBssDescr->WscIeProbeRsp, pbIe, pbIe[1] + 2);
                    pNewBssDescr->WscIeLen = pbIe[1] + 2;
                }
                break;
            }
            idx += pbIe[1] + 2;
            pbIe += pbIe[1] + 2;
        }
    }
}



//pIes may be NULL
tANI_BOOLEAN csrRemoveDupBssDescription( tpAniSirGlobal pMac, tSirBssDescription *pSirBssDescr,
                                         tDot11fBeaconIEs *pIes, tAniSSID *pSsid, v_TIME_t *timer, tANI_BOOLEAN fForced )
{
    tListElem *pEntry;

    tCsrScanResult *pBssDesc;
    tANI_BOOLEAN fRC = FALSE;

    // Walk through all the chained BssDescriptions.  If we find a chained BssDescription that
    // matches the BssID of the BssDescription passed in, then these must be duplicate scan
    // results for this Bss.  In that case, remove the 'old' Bss description from the linked list.
    csrLLLock(&pMac->scan.scanResultList);
    pEntry = csrLLPeekHead(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);

    while( pEntry )
    {
        pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );

        // we have a duplicate scan results only when BSSID, SSID, Channel and NetworkType
        // matches
        if ( csrIsDuplicateBssDescription( pMac, &pBssDesc->Result.BssDescriptor,
                                                        pSirBssDescr, pIes, fForced ) )
        {
            int32_t rssi_new, rssi_old;

            rssi_new = (int32_t) pSirBssDescr->rssi;
            rssi_old = (int32_t) pBssDesc->Result.BssDescriptor.rssi;
            rssi_new = ((rssi_new * CSR_SCAN_RESULT_RSSI_WEIGHT) +
                         rssi_old * (100 - CSR_SCAN_RESULT_RSSI_WEIGHT)) / 100;
            pSirBssDescr->rssi = (tANI_S8) rssi_new;

            rssi_new = (int32_t) pSirBssDescr->rssi_raw;
            rssi_old = (int32_t) pBssDesc->Result.BssDescriptor.rssi_raw;
            rssi_new = ((rssi_new * CSR_SCAN_RESULT_RSSI_WEIGHT) +
                         rssi_old * (100 - CSR_SCAN_RESULT_RSSI_WEIGHT)) / 100;
            pSirBssDescr->rssi_raw = (tANI_S8) rssi_new;

            // Remove the 'old' entry from the list....
            if(csrLLRemoveEntry(&pMac->scan.scanResultList, pEntry,
                      LL_ACCESS_NOLOCK))
            {
                // !we need to free the memory associated with this node
                //If failed to remove, assuming someone else got it.
                *pSsid = pBssDesc->Result.ssId;
                *timer = pBssDesc->Result.timer;
                csrCheckNSaveWscIe(pMac, pSirBssDescr, &pBssDesc->Result.BssDescriptor);

                csrFreeScanResultEntry( pMac, pBssDesc );
            }
            else
            {
                smsLog( pMac, LOGW, FL( "  fail to remove entry" ) );
            }
            fRC = TRUE;

            // If we found a match, we can stop looking through the list.
            break;
        }

        pEntry = csrLLNext(&pMac->scan.scanResultList, pEntry,
                            LL_ACCESS_NOLOCK);
    }
    csrLLUnlock(&pMac->scan.scanResultList);

    return fRC;
}


eHalStatus csrAddPMKIDCandidateList( tpAniSirGlobal pMac, tANI_U32 sessionId,
                                     tSirBssDescription *pBssDesc, tDot11fBeaconIEs *pIes )
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, "csrAddPMKIDCandidateList called pMac->scan.NumPmkidCandidate = %d", pSession->NumPmkidCandidate);
    if( pIes )
    {
        // check if this is a RSN BSS
        if( pIes->RSN.present )
        {
            // Check if the BSS is capable of doing pre-authentication
            if( pSession->NumPmkidCandidate < CSR_MAX_PMKID_ALLOWED )
            {

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
                {
                    WLAN_VOS_DIAG_EVENT_DEF(secEvent, vos_event_wlan_security_payload_type);
                    vos_mem_set(&secEvent, sizeof(vos_event_wlan_security_payload_type), 0);
                    secEvent.eventId = WLAN_SECURITY_EVENT_PMKID_CANDIDATE_FOUND;
                    secEvent.encryptionModeMulticast =
                        (v_U8_t)diagEncTypeFromCSRType(pSession->connectedProfile.mcEncryptionType);
                    secEvent.encryptionModeUnicast =
                        (v_U8_t)diagEncTypeFromCSRType(pSession->connectedProfile.EncryptionType);
                    vos_mem_copy(secEvent.bssid, pSession->connectedProfile.bssid, 6);
                    secEvent.authMode =
                        (v_U8_t)diagAuthTypeFromCSRType(pSession->connectedProfile.AuthType);
                    WLAN_VOS_DIAG_EVENT_REPORT(&secEvent, EVENT_WLAN_SECURITY);
                }
#endif//#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR

                // if yes, then add to PMKIDCandidateList
                vos_mem_copy(pSession->PmkidCandidateInfo[pSession->NumPmkidCandidate].BSSID,
                             pBssDesc->bssId, VOS_MAC_ADDR_SIZE);
                /* Bit 0 of first byte - PreAuthentication Capability */
                if ( (pIes->RSN.RSN_Cap[0] >> 0) & 0x1 )
                {
                    pSession->PmkidCandidateInfo[pSession->NumPmkidCandidate].preAuthSupported
                                                                          = eANI_BOOLEAN_TRUE;
                }
                else
                {
                    pSession->PmkidCandidateInfo[pSession->NumPmkidCandidate].preAuthSupported
                                                                          = eANI_BOOLEAN_FALSE;
                }
                pSession->NumPmkidCandidate++;
            }
            else
            {
                status = eHAL_STATUS_FAILURE;
            }
        }
    }

    return (status);
}

//This function checks whether new AP is found for the current connected profile
//If it is found, it return the sessionId, else it return invalid sessionID
eHalStatus csrProcessBSSDescForPMKIDList(tpAniSirGlobal pMac,
                                         tSirBssDescription *pBssDesc,
                                         tDot11fBeaconIEs *pIes,
                                         tANI_U8 sessionId)
{
    tCsrRoamSession *pSession;
    tDot11fBeaconIEs *pIesLocal = pIes;
    eHalStatus status = eHAL_STATUS_FAILURE;

    if (pIesLocal ||
        HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(
                        pMac, pBssDesc, &pIesLocal))) {
        if (CSR_IS_SESSION_VALID(pMac, sessionId)) {
            pSession = CSR_GET_SESSION(pMac, sessionId);
            if (csrIsConnStateConnectedInfra(pMac, sessionId) &&
               (eCSR_AUTH_TYPE_RSN == pSession->connectedProfile.AuthType)) {
                if (csrMatchBSSToConnectProfile(pMac,
                                                &pSession->connectedProfile,
                                                pBssDesc, pIesLocal)) {
                    /* This new BSS fits the current profile connected */
                    if (!HAL_STATUS_SUCCESS(csrAddPMKIDCandidateList(pMac,
                                      sessionId, pBssDesc, pIesLocal))) {
                        smsLog(pMac, LOGE,
                               FL("csrAddPMKIDCandidateList failed"));
                    } else {
                        status = eHAL_STATUS_SUCCESS;
                    }
                }
            }
        }

        if (!pIes) {
            vos_mem_free(pIesLocal);
        }
    }
    return status;
}

#ifdef FEATURE_WLAN_WAPI
eHalStatus csrAddBKIDCandidateList( tpAniSirGlobal pMac, tANI_U32 sessionId,
                                    tSirBssDescription *pBssDesc, tDot11fBeaconIEs *pIes )
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, "csrAddBKIDCandidateList called pMac->scan.NumBkidCandidate = %d",
                                             pSession->NumBkidCandidate);
    if( pIes )
    {
        // check if this is a WAPI BSS
        if( pIes->WAPI.present )
        {
            // Check if the BSS is capable of doing pre-authentication
            if( pSession->NumBkidCandidate < CSR_MAX_BKID_ALLOWED )
            {

                // if yes, then add to BKIDCandidateList
                vos_mem_copy(pSession->BkidCandidateInfo[pSession->NumBkidCandidate].BSSID,
                             pBssDesc->bssId, VOS_MAC_ADDR_SIZE);
                if ( pIes->WAPI.preauth )
                {
                    pSession->BkidCandidateInfo[pSession->NumBkidCandidate].preAuthSupported
                                                                         = eANI_BOOLEAN_TRUE;
                }
                else
                {
                    pSession->BkidCandidateInfo[pSession->NumBkidCandidate].preAuthSupported
                                                                        = eANI_BOOLEAN_FALSE;
                }
                pSession->NumBkidCandidate++;
            }
            else
            {
                status = eHAL_STATUS_FAILURE;
            }
        }
    }

    return (status);
}

//This function checks whether new AP is found for the current connected profile
//if so add to BKIDCandidateList
tANI_BOOLEAN csrProcessBSSDescForBKIDList(tpAniSirGlobal pMac, tSirBssDescription *pBssDesc,
                                          tDot11fBeaconIEs *pIes)
{
    tANI_BOOLEAN fRC = FALSE;
    tDot11fBeaconIEs *pIesLocal = pIes;
    tANI_U32 sessionId;
    tCsrRoamSession *pSession;

    if( pIesLocal || HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, pBssDesc, &pIesLocal)) )
    {
        for( sessionId = 0; sessionId < CSR_ROAM_SESSION_MAX; sessionId++ )
        {
            if( CSR_IS_SESSION_VALID( pMac, sessionId) )
            {
                pSession = CSR_GET_SESSION( pMac, sessionId );
                if( csrIsConnStateConnectedInfra( pMac, sessionId ) &&
                    eCSR_AUTH_TYPE_WAPI_WAI_CERTIFICATE == pSession->connectedProfile.AuthType)
                {
                    if(csrMatchBSSToConnectProfile(pMac, &pSession->connectedProfile,pBssDesc, pIesLocal))
                    {
                        //this new BSS fits the current profile connected
                        if(HAL_STATUS_SUCCESS(csrAddBKIDCandidateList(pMac, sessionId, pBssDesc, pIesLocal)))
                        {
                            fRC = TRUE;
                        }
                    }
                }
            }
        }
        if(!pIes)
        {
            vos_mem_free(pIesLocal);
        }

    }
    return fRC;
}

#endif


static void csrMoveTempScanResultsToMainList(tpAniSirGlobal pMac,
                                             tANI_U8 reason,
                                             tANI_U8 sessionId)
{
    tListElem *pEntry;
    tCsrScanResult *pBssDescription;
    tANI_BOOLEAN    fDupBss;
#ifdef FEATURE_WLAN_WAPI
    tANI_BOOLEAN fNewWapiBSSForCurConnection = eANI_BOOLEAN_FALSE;
#endif /* FEATURE_WLAN_WAPI */
    tDot11fBeaconIEs *pIesLocal = NULL;
    tAniSSID tmpSsid;
    v_TIME_t timer=0;

    tmpSsid.length = 0;

    // remove the BSS descriptions from temporary list
    while ((pEntry = csrLLRemoveTail(&pMac->scan.tempScanResults,
                                      LL_ACCESS_LOCK)) != NULL)
    {
        pBssDescription = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );

        smsLog( pMac, LOGW, "...Bssid= "MAC_ADDRESS_STR" chan= %d, rssi = -%d",
                      MAC_ADDR_ARRAY(pBssDescription->Result.BssDescriptor.bssId),
                      pBssDescription->Result.BssDescriptor.channelId,
                pBssDescription->Result.BssDescriptor.rssi * (-1) );

        //At this time, pBssDescription->Result.pvIes may be NULL
        pIesLocal = (tDot11fBeaconIEs *)( pBssDescription->Result.pvIes );
        if( !pIesLocal && (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, &pBssDescription->Result.BssDescriptor, &pIesLocal))) )
        {
            smsLog(pMac, LOGE, FL("  Cannot pared IEs"));
            csrFreeScanResultEntry(pMac, pBssDescription);
            continue;
        }
        fDupBss = csrRemoveDupBssDescription( pMac, &pBssDescription->Result.BssDescriptor, pIesLocal, &tmpSsid, &timer, FALSE );
        //Check whether we have reach out limit, but don't lose the LFR candidates came from FW
        if( CSR_SCAN_IS_OVER_BSS_LIMIT(pMac)
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
            && !( eCsrScanGetLfrResult == reason )
#endif
          )
        {
            //Limit reach
            smsLog(pMac, LOGW, FL("  BSS limit reached"));
            //Free the resources
            if( (pBssDescription->Result.pvIes == NULL) && pIesLocal )
            {
                vos_mem_free(pIesLocal);
            }
            csrFreeScanResultEntry(pMac, pBssDescription);
            //Continue because there may be duplicated BSS
            continue;
        }
        // check for duplicate scan results
        if ( !fDupBss )
        {
            if (HAL_STATUS_SUCCESS(csrProcessBSSDescForPMKIDList(pMac,
                            &pBssDescription->Result.BssDescriptor,
                            pIesLocal, sessionId))) {
                /* Found a new BSS */
                csrRoamCallCallback(pMac, sessionId, NULL, 0,
                           eCSR_ROAM_SCAN_FOUND_NEW_BSS, eCSR_ROAM_RESULT_NONE);
            }
        }
        else
        {
            //Check if the new one has SSID it it, if not, use the older SSID if it exists.
            if( (0 == pBssDescription->Result.ssId.length) && tmpSsid.length )
            {
                //New BSS has a hidden SSID and old one has the SSID. Keep the SSID only
                //if diff of saved SSID time and current time is less than 1 min to avoid
                //side effect of saving SSID with old one is that if AP changes its SSID while remain
                //hidden, we may never see it and also to address the requirement of
                //When we remove hidden ssid from the profile i.e., forget the SSID via
                // GUI that SSID shouldn't see in the profile
                if( (vos_timer_get_system_time() - timer) <= HIDDEN_TIMER)
                {
                   pBssDescription->Result.timer = timer;
                   pBssDescription->Result.ssId = tmpSsid;
                }
            }
        }

        //Find a good AP for 11d info
        if ( csrIs11dSupported( pMac ) )
        {
            // check if country information element is present
            if (pIesLocal->Country.present)
            {
                csrAddVoteForCountryInfo(pMac, pIesLocal->Country.country);
                smsLog(pMac, LOGW, FL("11d AP Bssid " MAC_ADDRESS_STR
                                " chan= %d, rssi = -%d, countryCode %c%c"),
                                MAC_ADDR_ARRAY( pBssDescription->Result.BssDescriptor.bssId),
                                pBssDescription->Result.BssDescriptor.channelId,
                                pBssDescription->Result.BssDescriptor.rssi * (-1),
                                pIesLocal->Country.country[0],pIesLocal->Country.country[1] );
            }
        }

        // append to main list
        csrScanAddResult(pMac, pBssDescription, pIesLocal, sessionId);
        if ( (pBssDescription->Result.pvIes == NULL) && pIesLocal )
        {
            vos_mem_free(pIesLocal);
        }
    }

    /* We don't need to update CC while connected to an AP
       which is advertising CC already */
    if (csrIs11dSupported(pMac))
    {
        tCsrRoamSession *pSession;
        tANI_U32 i;

        for (i = 0; i < CSR_ROAM_SESSION_MAX; i++ )
        {
            if (CSR_IS_SESSION_VALID( pMac, i ) )
            {
                pSession = CSR_GET_SESSION( pMac, i );
                if (csrIsConnStateConnected(pMac, i))
                {
                    smsLog(pMac, LOGW, FL("No need for updating CC in"
                                         "connected state"));
                    goto end;
                }
            }
        }
        csrElectedCountryInfo(pMac);
        csrLearnCountryInformation( pMac, NULL, NULL, eANI_BOOLEAN_TRUE );
    }

end:
    //If we can find the current 11d info in any of the scan results, or
    // a good enough AP with the 11d info from the scan results then no need to
    // get into ambiguous state
    if(pMac->scan.fAmbiguous11dInfoFound)
    {
      if((pMac->scan.fCurrent11dInfoMatch))
      {
        pMac->scan.fAmbiguous11dInfoFound = eANI_BOOLEAN_FALSE;
      }
    }

#ifdef FEATURE_WLAN_WAPI
    if(fNewWapiBSSForCurConnection)
    {
        //remember it first
        csrRoamCallCallback(pMac, sessionId, NULL, 0, eCSR_ROAM_SCAN_FOUND_NEW_BSS, eCSR_ROAM_RESULT_NEW_WAPI_BSS);
    }
#endif /* FEATURE_WLAN_WAPI */

    return;
}


static tCsrScanResult *
csrScanSaveBssDescription(tpAniSirGlobal pMac,
                           tSirBssDescription *pBSSDescription,
                           tDot11fBeaconIEs *pIes,
                           tANI_U8 sessionId)
{
    tCsrScanResult *pCsrBssDescription = NULL;
    tANI_U32 cbBSSDesc;
    tANI_U32 cbAllocated;

    // figure out how big the BSS description is (the BSSDesc->length does NOT
    // include the size of the length field itself).
    cbBSSDesc = pBSSDescription->length + sizeof( pBSSDescription->length );

    cbAllocated = sizeof( tCsrScanResult ) + cbBSSDesc;

    pCsrBssDescription = vos_mem_malloc(cbAllocated);
    if ( NULL != pCsrBssDescription )
    {
        vos_mem_set(pCsrBssDescription, cbAllocated, 0);
        pCsrBssDescription->AgingCount = (tANI_S32)pMac->roam.configParam.agingCount;
        smsLog(pMac, LOGW,
           FL(" Set Aging Count = %d for BSS "MAC_ADDRESS_STR" "),
           pCsrBssDescription->AgingCount,
           MAC_ADDR_ARRAY(pCsrBssDescription->Result.BssDescriptor.bssId));
        vos_mem_copy(&pCsrBssDescription->Result.BssDescriptor, pBSSDescription, cbBSSDesc);
#if defined(VOSS_ENSBALED)
        if ( NULL != pCsrBssDescription->Result.pvIes)
        {
           VOS_ASSERT( pCsrBssDescription->Result.pvIes == NULL );
           return NULL;
        }
#endif
        csrScanAddResult(pMac, pCsrBssDescription, pIes, sessionId);
    }

    return( pCsrBssDescription );
}

// Append a Bss Description...
tCsrScanResult *
csrScanAppendBssDescription(tpAniSirGlobal pMac,
                            tSirBssDescription *pSirBssDescription,
                            tDot11fBeaconIEs *pIes,
                            tANI_BOOLEAN fForced,
                            tANI_U8 sessionId)
{
    tCsrScanResult *pCsrBssDescription = NULL;
    tAniSSID tmpSsid;
    v_TIME_t timer = 0;
    int result;

    tmpSsid.length = 0;
    result = csrRemoveDupBssDescription( pMac, pSirBssDescription, pIes, &tmpSsid, &timer, fForced );
    pCsrBssDescription = csrScanSaveBssDescription(pMac, pSirBssDescription,
                                                    pIes, sessionId);
    if (result && (pCsrBssDescription != NULL))
    {
        //Check if the new one has SSID it it, if not, use the older SSID if it exists.
        if( (0 == pCsrBssDescription->Result.ssId.length) && tmpSsid.length )
        {
            //New BSS has a hidden SSID and old one has the SSID. Keep the SSID only
            //if diff of saved SSID time and current time is less than 1 min to avoid
            //side effect of saving SSID with old one is that if AP changes its SSID while remain
            //hidden, we may never see it and also to address the requirement of
            //When we remove hidden ssid from the profile i.e., forget the SSID via
            // GUI that SSID shouldn't see in the profile
            if((vos_timer_get_system_time()-timer) <= HIDDEN_TIMER)
            {
              pCsrBssDescription->Result.ssId = tmpSsid;
              pCsrBssDescription->Result.timer = timer;
            }
        }
    }


    return( pCsrBssDescription );
}



void csrPurgeChannelPower( tpAniSirGlobal pMac, tDblLinkList *pChannelList )
{
    tCsrChannelPowerInfo *pChannelSet;
    tListElem *pEntry;

    csrLLLock(pChannelList);
    // Remove the channel sets from the learned list and put them in the free list
    while( ( pEntry = csrLLRemoveHead( pChannelList, LL_ACCESS_NOLOCK ) ) != NULL)
    {
        pChannelSet = GET_BASE_ADDR( pEntry, tCsrChannelPowerInfo, link );
        if( pChannelSet )
        {
            vos_mem_free(pChannelSet);
        }
    }
    csrLLUnlock(pChannelList);
    return;
}


/*
 * Save the channelList into the ultimate storage as the final stage of channel
 * Input: pCountryInfo -- the country code (e.g. "USI"), channel list, and power limit are all stored inside this data structure
 */
eHalStatus csrSaveToChannelPower2G_5G( tpAniSirGlobal pMac, tANI_U32 tableSize, tSirMacChanInfo *channelTable )
{
    tANI_U32 i = tableSize / sizeof( tSirMacChanInfo );
    tSirMacChanInfo *pChannelInfo;
    tCsrChannelPowerInfo *pChannelSet;
    tANI_BOOLEAN f2GHzInfoFound = FALSE;
    tANI_BOOLEAN f2GListPurged = FALSE, f5GListPurged = FALSE;

    pChannelInfo = channelTable;
    /* At-least 3 bytes have to be remaining  -- from "countryString" */
    while ( i-- )
    {
        pChannelSet = vos_mem_malloc(sizeof(tCsrChannelPowerInfo));
        if ( NULL != pChannelSet )
        {
            vos_mem_set(pChannelSet, sizeof(tCsrChannelPowerInfo), 0);
            pChannelSet->firstChannel = pChannelInfo->firstChanNum;
            pChannelSet->numChannels = pChannelInfo->numChannels;

            // Now set the inter-channel offset based on the frequency band the channel set lies in
            if( (CSR_IS_CHANNEL_24GHZ(pChannelSet->firstChannel)) &&
                    ((pChannelSet->firstChannel + (pChannelSet->numChannels - 1)) <= CSR_MAX_24GHz_CHANNEL_NUMBER) )

            {
                pChannelSet->interChannelOffset = 1;
                f2GHzInfoFound = TRUE;
            }
            else if ( (CSR_IS_CHANNEL_5GHZ(pChannelSet->firstChannel)) &&
                ((pChannelSet->firstChannel + ((pChannelSet->numChannels - 1) * 4)) <= CSR_MAX_5GHz_CHANNEL_NUMBER) )
            {
                pChannelSet->interChannelOffset = 4;
                f2GHzInfoFound = FALSE;
            }
            else
            {
                smsLog( pMac, LOGW, FL("Invalid Channel %d Present in Country IE"),
                        pChannelSet->firstChannel);
                vos_mem_free(pChannelSet);
                return eHAL_STATUS_FAILURE;
            }

            pChannelSet->txPower = CSR_ROAM_MIN( pChannelInfo->maxTxPower, pMac->roam.configParam.nTxPowerCap );

            if( f2GHzInfoFound )
            {
                if( !f2GListPurged )
                {
                    // purge previous results if found new
                    csrPurgeChannelPower( pMac, &pMac->scan.channelPowerInfoList24 );
                    f2GListPurged = TRUE;
                }

                if(CSR_IS_OPERATING_BG_BAND(pMac))
                {
                    // add to the list of 2.4 GHz channel sets
                    csrLLInsertTail( &pMac->scan.channelPowerInfoList24, &pChannelSet->link, LL_ACCESS_LOCK );
                }
                else {
                    smsLog( pMac, LOGW, FL("Adding 11B/G channels in 11A mode -- First Channel is %d"),
                                pChannelSet->firstChannel);
                      vos_mem_free(pChannelSet);
                }
            }
            else
            {
                // 5GHz info found
                if( !f5GListPurged )
                {
                    // purge previous results if found new
                    csrPurgeChannelPower( pMac, &pMac->scan.channelPowerInfoList5G );
                    f5GListPurged = TRUE;
                }

                if(CSR_IS_OPERATING_A_BAND(pMac))
                {
                    // add to the list of 5GHz channel sets
                    csrLLInsertTail( &pMac->scan.channelPowerInfoList5G, &pChannelSet->link, LL_ACCESS_LOCK );
                }
                else {
                    smsLog( pMac, LOGW, FL("Adding 11A channels in B/G mode -- First Channel is %d"),
                                pChannelSet->firstChannel);
                    vos_mem_free(pChannelSet);
                }
            }
        }

        pChannelInfo++;                // move to next entry
    }

    return eHAL_STATUS_SUCCESS;
}

static  void csrClearDfsChannelList( tpAniSirGlobal pMac )
{
    tSirMbMsg *pMsg;
    tANI_U16 msgLen;

    msgLen = (tANI_U16)(sizeof( tSirMbMsg ));
    pMsg = vos_mem_malloc(msgLen);
    if ( NULL != pMsg )
    {
       vos_mem_set((void *)pMsg, msgLen, 0);
       pMsg->type = pal_cpu_to_be16((tANI_U16)eWNI_SME_CLEAR_DFS_CHANNEL_LIST);
       pMsg->msgLen = pal_cpu_to_be16(msgLen);
       palSendMBMessage(pMac->hHdd, pMsg);
    }
}

void csrApplyPower2Current( tpAniSirGlobal pMac )
{
    smsLog( pMac, LOG3, FL(" Updating Cfg with power settings"));
    csrSaveTxPowerToCfg( pMac, &pMac->scan.channelPowerInfoList24, WNI_CFG_MAX_TX_POWER_2_4 );
    csrSaveTxPowerToCfg( pMac, &pMac->scan.channelPowerInfoList5G, WNI_CFG_MAX_TX_POWER_5 );
}


void csrApplyChannelPowerCountryInfo( tpAniSirGlobal pMac, tCsrChannel *pChannelList, tANI_U8 *countryCode, tANI_BOOLEAN updateRiva)
{
    int i;
    tANI_U8 numChannels = 0;
    tANI_U8 tempNumChannels = 0;
    tCsrChannel ChannelList;

    if (pChannelList->numChannels)
    {
        tempNumChannels = CSR_MIN(pChannelList->numChannels, WNI_CFG_VALID_CHANNEL_LIST_LEN);

        for (i = 0; i < tempNumChannels; i++)
        {
            ChannelList.channelList[numChannels] = pChannelList->channelList[i];
            numChannels++;
        }

        ChannelList.numChannels = numChannels;
        csrSetCfgValidChannelList(pMac, ChannelList.channelList, ChannelList.numChannels);

        // extend scan capability
        //  build a scan list based on the channel list : channel# + active/passive scan
        csrSetCfgScanControlList(pMac, countryCode, &ChannelList);
        /*Send msg to Lim to clear DFS channel list */
        csrClearDfsChannelList(pMac);
#ifdef FEATURE_WLAN_SCAN_PNO
        if (updateRiva)
        {
            VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO, FL("  Sending 11d PNO info to Riva"));
            // Send HAL UpdateScanParams message
            pmcUpdateScanParams(pMac, &(pMac->roam.configParam), &ChannelList, TRUE);
        }
#endif // FEATURE_WLAN_SCAN_PNO
    }
    else
    {
        smsLog( pMac, LOGE, FL("  11D channel list is empty"));
    }
    csrApplyPower2Current( pMac );     // Store the channel+power info in the global place: Cfg
    csrSetCfgCountryCode(pMac, countryCode);
}


void csrResetCountryInformation( tpAniSirGlobal pMac, tANI_BOOLEAN fForce, tANI_BOOLEAN updateRiva )
{
    if( fForce || (csrIs11dSupported( pMac ) && (!pMac->scan.f11dInfoReset)))
    {

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
    {
        vos_log_802_11d_pkt_type *p11dLog;
        int Index;

        WLAN_VOS_DIAG_LOG_ALLOC(p11dLog, vos_log_802_11d_pkt_type, LOG_WLAN_80211D_C);
        if(p11dLog)
        {
            p11dLog->eventId = WLAN_80211D_EVENT_RESET;
            vos_mem_copy(p11dLog->countryCode, pMac->scan.countryCodeCurrent, 3);
            p11dLog->numChannel = pMac->scan.base20MHzChannels.numChannels;
            if(p11dLog->numChannel <= VOS_LOG_MAX_NUM_CHANNEL)
            {
                vos_mem_copy(p11dLog->Channels,
                             pMac->scan.base20MHzChannels.channelList,
                             p11dLog->numChannel);
                for (Index=0; Index < pMac->scan.base20MHzChannels.numChannels; Index++)
                {
                    p11dLog->TxPwr[Index] = CSR_ROAM_MIN( pMac->scan.defaultPowerTable[Index].pwr, pMac->roam.configParam.nTxPowerCap );
                }
            }
            if(!pMac->roam.configParam.Is11dSupportEnabled)
            {
                p11dLog->supportMultipleDomain = WLAN_80211D_DISABLED;
            }
            else if(pMac->roam.configParam.fEnforceDefaultDomain)
            {
                p11dLog->supportMultipleDomain = WLAN_80211D_NOT_SUPPORT_MULTI_DOMAIN;
            }
            else
            {
                p11dLog->supportMultipleDomain = WLAN_80211D_SUPPORT_MULTI_DOMAIN;
            }
            WLAN_VOS_DIAG_LOG_REPORT(p11dLog);
        }
    }
#endif //#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR

        csrPruneChannelListForMode(pMac, &pMac->scan.baseChannels);
        csrPruneChannelListForMode(pMac, &pMac->scan.base20MHzChannels);

        csrSaveChannelPowerForBand(pMac, eANI_BOOLEAN_FALSE);
        csrSaveChannelPowerForBand(pMac, eANI_BOOLEAN_TRUE);
        // ... and apply the channel list, power settings, and the country code.
        csrApplyChannelPowerCountryInfo( pMac, &pMac->scan.base20MHzChannels, pMac->scan.countryCodeCurrent, updateRiva );
        // clear the 11d channel list
        vos_mem_set(&pMac->scan.channels11d, sizeof(pMac->scan.channels11d), 0);
        pMac->scan.f11dInfoReset = eANI_BOOLEAN_TRUE;
        pMac->scan.f11dInfoApplied = eANI_BOOLEAN_FALSE;
    }

    return;
}


eHalStatus csrResetCountryCodeInformation(tpAniSirGlobal pMac, tANI_BOOLEAN *pfRestartNeeded)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tANI_BOOLEAN fRestart = eANI_BOOLEAN_FALSE;

    //Use the Country code and domain from EEPROM
    vos_mem_copy(pMac->scan.countryCodeCurrent, pMac->scan.countryCodeDefault,
                 WNI_CFG_COUNTRY_CODE_LEN);
    csrSetRegulatoryDomain(pMac, pMac->scan.domainIdCurrent, &fRestart);
    if( ((eANI_BOOLEAN_FALSE == fRestart) || (pfRestartNeeded == NULL) )
          && !csrIsInfraConnected(pMac))
    {
        //Only reset the country info if we don't need to restart
        csrResetCountryInformation(pMac, eANI_BOOLEAN_TRUE, eANI_BOOLEAN_TRUE);
    }
    if(pfRestartNeeded)
    {
        *pfRestartNeeded = fRestart;
    }

    return (status);
}

void csrClearVotesForCountryInfo(tpAniSirGlobal pMac)
{
    pMac->scan.countryCodeCount = 0;
    vos_mem_set(pMac->scan.votes11d,
                 sizeof(tCsrVotes11d) * CSR_MAX_NUM_COUNTRY_CODE, 0);
}

void csrAddVoteForCountryInfo(tpAniSirGlobal pMac, tANI_U8 *pCountryCode)
{
    tANI_BOOLEAN match = FALSE;
    tANI_U8 i;

    /* convert to UPPER here so we are assured
     * the strings are always in upper case.
     */
    for( i = 0; i < 3; i++ )
    {
        pCountryCode[ i ] = (tANI_U8)csrToUpper( pCountryCode[ i ] );
    }

    /* Some of the 'old' Cisco 350 series AP's advertise NA as the
     * country code (for North America ??). NA is not a valid country code
     * or domain so let's allow this by changing it to the proper
     * country code (which is US).  We've also seen some NETGEAR AP's
     * that have "XX " as the country code with valid 2.4 GHz US channel
     * information.  If we cannot find the country code advertised in the
     * 11d information element, let's default to US.
     */

    if ( !HAL_STATUS_SUCCESS(csrGetRegulatoryDomainForCountry( pMac,
                  pCountryCode, NULL,COUNTRY_QUERY ) ) )
    {
        pCountryCode[ 0 ] = '0';
        pCountryCode[ 1 ] = '0';
    }

    /* We've seen some of the AP's improperly put a 0 for the
     * third character of the country code. spec says valid charcters are
     * 'O' (for outdoor), 'I' for Indoor, or ' ' (space; for either).
     * if we see a 0 in this third character, let's change it to a ' '.
     */
    if ( 0 == pCountryCode[ 2 ] )
    {
        pCountryCode[ 2 ] = ' ';
    }

    for (i = 0; i < pMac->scan.countryCodeCount; i++)
    {
        match = (vos_mem_compare(pMac->scan.votes11d[i].countryCode,
                          pCountryCode, 2));
        if(match)
        {
            break;
        }
    }

    if (match)
    {
        pMac->scan.votes11d[i].votes++;
    }
    else
    {
        vos_mem_copy( pMac->scan.votes11d[pMac->scan.countryCodeCount].countryCode,
                       pCountryCode, 3 );
        pMac->scan.votes11d[pMac->scan.countryCodeCount].votes = 1;
        pMac->scan.countryCodeCount++;
    }

    return;
}

tANI_BOOLEAN csrElectedCountryInfo(tpAniSirGlobal pMac)
{
    tANI_BOOLEAN fRet = FALSE;
    tANI_U8 maxVotes = 0;
    tANI_U8 i, j=0;

    if (!pMac->scan.countryCodeCount)
    {
        return fRet;
    }
    maxVotes = pMac->scan.votes11d[0].votes;
    fRet = TRUE;

    for(i = 1; i < pMac->scan.countryCodeCount; i++)
    {
        /* If we have a tie for max votes for 2 different country codes,
         * pick random.we can put some more intelligence - TBD
         */
        if (maxVotes < pMac->scan.votes11d[i].votes)
        {
            VOS_TRACE( VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                     " Votes for Country %c%c : %d\n",
                    pMac->scan.votes11d[i].countryCode[0],
                    pMac->scan.votes11d[i].countryCode[1],
                    pMac->scan.votes11d[i].votes);

            maxVotes = pMac->scan.votes11d[i].votes;
            j = i;
            fRet = TRUE;
        }

    }
    if (fRet)
    {
        vos_mem_copy(pMac->scan.countryCodeElected,
            pMac->scan.votes11d[j].countryCode, WNI_CFG_COUNTRY_CODE_LEN);
        VOS_TRACE( VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                 "Selected Country is %c%c With count %d\n",
                      pMac->scan.votes11d[j].countryCode[0],
                      pMac->scan.votes11d[j].countryCode[1],
                      pMac->scan.votes11d[j].votes);
    }
    return fRet;
}

eHalStatus csrSetCountryCode(tpAniSirGlobal pMac, tANI_U8 *pCountry, tANI_BOOLEAN *pfRestartNeeded)
{
    eHalStatus status = eHAL_STATUS_INVALID_PARAMETER;
    v_REGDOMAIN_t domainId;

    if(pCountry)
    {
        status = csrGetRegulatoryDomainForCountry(pMac, pCountry, &domainId, COUNTRY_USER);
        if(HAL_STATUS_SUCCESS(status))
        {
            status = csrSetRegulatoryDomain(pMac, domainId, pfRestartNeeded);
            if(HAL_STATUS_SUCCESS(status))
            {
                //We don't need to check the pMac->roam.configParam.fEnforceDefaultDomain flag here,
                //csrSetRegulatoryDomain will fail if the country doesn't fit our domain criteria.
                vos_mem_copy(pMac->scan.countryCodeCurrent, pCountry, WNI_CFG_COUNTRY_CODE_LEN);
                if((pfRestartNeeded == NULL) || !(*pfRestartNeeded))
                {
                    //Simply set it to cfg. If we need to restart, restart will apply it to the CFG
                    csrSetCfgCountryCode(pMac, pCountry);
                }
            }
        }
    }

    return (status);
}



//caller allocated memory for pNumChn and pChnPowerInfo
//As input, *pNumChn has the size of the array of pChnPowerInfo
//Upon return, *pNumChn has the number of channels assigned.
void csrGetChannelPowerInfo( tpAniSirGlobal pMac, tDblLinkList *pList,
                             tANI_U32 *pNumChn, tChannelListWithPower *pChnPowerInfo)
{
    tListElem *pEntry;
    tANI_U32 chnIdx = 0, idx;
    tCsrChannelPowerInfo *pChannelSet;

    //Get 2.4Ghz first
    pEntry = csrLLPeekHead( pList, LL_ACCESS_LOCK );
    while( pEntry && (chnIdx < *pNumChn) )
    {
        pChannelSet = GET_BASE_ADDR( pEntry, tCsrChannelPowerInfo, link );
        if ( 1 != pChannelSet->interChannelOffset )
        {
            for( idx = 0; (idx < pChannelSet->numChannels) && (chnIdx < *pNumChn); idx++ )
            {
                pChnPowerInfo[chnIdx].chanId = (tANI_U8)(pChannelSet->firstChannel + ( idx * pChannelSet->interChannelOffset ));
                pChnPowerInfo[chnIdx++].pwr = pChannelSet->txPower;
            }
        }
        else
        {
            for( idx = 0; (idx < pChannelSet->numChannels) && (chnIdx < *pNumChn); idx++ )
            {
                pChnPowerInfo[chnIdx].chanId = (tANI_U8)(pChannelSet->firstChannel + idx);
                pChnPowerInfo[chnIdx++].pwr = pChannelSet->txPower;
            }
        }

        pEntry = csrLLNext( pList, pEntry, LL_ACCESS_LOCK );
    }
    *pNumChn = chnIdx;

    return ;
}



void csrApplyCountryInformation( tpAniSirGlobal pMac, tANI_BOOLEAN fForce )
{
    v_REGDOMAIN_t domainId;
    eHalStatus status = eHAL_STATUS_SUCCESS;

    do
    {
        if( !csrIs11dSupported( pMac ) || 0 == pMac->scan.channelOf11dInfo) break;
        if( pMac->scan.fAmbiguous11dInfoFound )
        {
            /* Ambiguous info found; Restore the default domain as well */
            if(HAL_STATUS_SUCCESS(csrGetRegulatoryDomainForCountry(
                                         pMac, pMac->scan.countryCodeCurrent,
                                         &domainId, COUNTRY_QUERY)))
            {
                pMac->scan.domainIdCurrent = domainId;
            }
            else
            {
                smsLog(pMac, LOGE, FL(" failed to get domain from currentCountryCode %02X%02X"),
                    pMac->scan.countryCodeCurrent[0], pMac->scan.countryCodeCurrent[1]);
            }
            csrResetCountryInformation( pMac, eANI_BOOLEAN_FALSE, eANI_BOOLEAN_TRUE );
            break;
        }
        if ( pMac->scan.f11dInfoApplied && !fForce ) break;
        if(HAL_STATUS_SUCCESS(csrGetRegulatoryDomainForCountry(
                                        pMac, pMac->scan.countryCode11d,
                                        &domainId, COUNTRY_QUERY)))
        {
            //Check whether we need to enforce default domain
            if( ( !pMac->roam.configParam.fEnforceDefaultDomain ) ||
                (pMac->scan.domainIdCurrent == domainId) )
            {

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
                {
                    vos_log_802_11d_pkt_type *p11dLog;
                    tChannelListWithPower chnPwrInfo[WNI_CFG_VALID_CHANNEL_LIST_LEN];
                    tANI_U32 nChnInfo = WNI_CFG_VALID_CHANNEL_LIST_LEN, nTmp;

                    WLAN_VOS_DIAG_LOG_ALLOC(p11dLog, vos_log_802_11d_pkt_type, LOG_WLAN_80211D_C);
                    if(p11dLog)
                    {
                        p11dLog->eventId = WLAN_80211D_EVENT_COUNTRY_SET;
                        vos_mem_copy(p11dLog->countryCode, pMac->scan.countryCode11d, 3);
                        p11dLog->numChannel = pMac->scan.channels11d.numChannels;
                        if(p11dLog->numChannel <= VOS_LOG_MAX_NUM_CHANNEL)
                        {
                            vos_mem_copy(p11dLog->Channels,
                                         pMac->scan.channels11d.channelList,
                                         p11dLog->numChannel);
                            csrGetChannelPowerInfo(pMac, &pMac->scan.channelPowerInfoList24,
                                                    &nChnInfo, chnPwrInfo);
                            nTmp = nChnInfo;
                            nChnInfo = WNI_CFG_VALID_CHANNEL_LIST_LEN - nTmp;
                            csrGetChannelPowerInfo(pMac, &pMac->scan.channelPowerInfoList5G,
                                                    &nChnInfo, &chnPwrInfo[nTmp]);
                            for(nTmp = 0; nTmp < p11dLog->numChannel; nTmp++)
                            {
                                for(nChnInfo = 0; nChnInfo < WNI_CFG_VALID_CHANNEL_LIST_LEN; nChnInfo++)
                                {
                                    if(p11dLog->Channels[nTmp] == chnPwrInfo[nChnInfo].chanId)
                                    {
                                        p11dLog->TxPwr[nTmp] = chnPwrInfo[nChnInfo].pwr;
                                        break;
                                    }
                                }
                            }
                        }
                        if(!pMac->roam.configParam.Is11dSupportEnabled)
                        {
                            p11dLog->supportMultipleDomain = WLAN_80211D_DISABLED;
                        }
                        else if(pMac->roam.configParam.fEnforceDefaultDomain)
                        {
                            p11dLog->supportMultipleDomain = WLAN_80211D_NOT_SUPPORT_MULTI_DOMAIN;
                        }
                        else
                        {
                            p11dLog->supportMultipleDomain = WLAN_80211D_SUPPORT_MULTI_DOMAIN;
                        }
                        WLAN_VOS_DIAG_LOG_REPORT(p11dLog);
                    }
                }
#endif //#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
                if(pMac->scan.domainIdCurrent != domainId)
                {
                   smsLog(pMac, LOGW, FL("Domain Changed Old %d, new %d"),
                                      pMac->scan.domainIdCurrent, domainId);
                   status = WDA_SetRegDomain(pMac, domainId, eSIR_TRUE);
                }
                if (status != eHAL_STATUS_SUCCESS)
                {
                    smsLog( pMac, LOGE, FL("  fail to set regId %d"), domainId );
                }
                pMac->scan.domainIdCurrent = domainId;

                // switch to active scans using this new channel list
                pMac->scan.curScanType = eSIR_ACTIVE_SCAN;
                pMac->scan.f11dInfoApplied = eANI_BOOLEAN_TRUE;
                pMac->scan.f11dInfoReset = eANI_BOOLEAN_FALSE;
            }
        }

    } while( 0 );

    return;
}



tANI_BOOLEAN csrSave11dCountryString( tpAniSirGlobal pMac, tANI_U8 *pCountryCode,
                     tANI_BOOLEAN fForce)
{
    tANI_BOOLEAN fCountryStringChanged = FALSE, fUnknownCountryCode = FALSE;
    tANI_U32 i;
    v_REGDOMAIN_t regd;
    tANI_BOOLEAN fCountryNotPresentInDriver = FALSE;

    // convert to UPPER here so we are assured the strings are always in upper case.
    for( i = 0; i < 3; i++ )
    {
        pCountryCode[ i ] = (tANI_U8)csrToUpper( pCountryCode[ i ] );
    }

    // Some of the 'old' Cisco 350 series AP's advertise NA as the country code (for North America ??).
    // NA is not a valid country code or domain so let's allow this by changing it to the proper
    // country code (which is US).  We've also seen some NETGEAR AP's that have "XX " as the country code
    // with valid 2.4 GHz US channel information.  If we cannot find the country code advertised in the
    // 11d information element, let's default to US.
    if ( !HAL_STATUS_SUCCESS(csrGetRegulatoryDomainForCountry(pMac,
                                                      pCountryCode,
                                                      &regd,
                                                      COUNTRY_QUERY) ) )
    {
        // Check the enforcement first
        if( pMac->roam.configParam.fEnforceDefaultDomain || pMac->roam.configParam.fEnforceCountryCodeMatch )
        {
            fUnknownCountryCode = TRUE;
        }
        else
        {
            fCountryNotPresentInDriver = TRUE;
        }
    }
    //right now, even if we don't find the CC in driver we set to world. Making
    //sure countryCode11d doesn't get updated with the invalid CC, instead
    //reflect the world CC
    else if (REGDOMAIN_WORLD == regd)
    {
        fCountryNotPresentInDriver = TRUE;
    }

    /*
     * We've seen some of the AP's improperly put a 0 for the third character
     * of the country code. Spec says valid characters are 'O' (for outdoor),
     * 'I' for Indoor, or ' ' (space; for either).
     * if we see a 0 in this third character, let's change it to a ' '.
     */
    if ( 0 == pCountryCode[ 2 ] )
    {
        pCountryCode[ 2 ] = ' ';
    }

    if( !fUnknownCountryCode )
    {
        fCountryStringChanged = (!vos_mem_compare(pMac->scan.countryCode11d, pCountryCode, 2));


        if(( 0 == pMac->scan.countryCode11d[ 0 ] && 0 == pMac->scan.countryCode11d[ 1 ] )
             || (fForce))
        {
            if (!fCountryNotPresentInDriver)
            {
                // this is the first .11d information
                vos_mem_copy(pMac->scan.countryCode11d, pCountryCode,
                         sizeof( pMac->scan.countryCode11d ));

            }
            else
            {
                pMac->scan.countryCode11d[0] = '0';
                pMac->scan.countryCode11d[1] = '0';
            }
        }
    }

    return( fCountryStringChanged );
}


void csrSaveChannelPowerForBand( tpAniSirGlobal pMac, tANI_BOOLEAN fPopulate5GBand )
{
    tANI_U32 Index, count=0;
    tSirMacChanInfo *pChanInfo;
    tSirMacChanInfo *pChanInfoStart;
    tANI_S32 maxChannelIndex;

    maxChannelIndex = ( pMac->scan.base20MHzChannels.numChannels < WNI_CFG_VALID_CHANNEL_LIST_LEN ) ?
                      pMac->scan.base20MHzChannels.numChannels : WNI_CFG_VALID_CHANNEL_LIST_LEN ;

    pChanInfo = vos_mem_malloc(sizeof(tSirMacChanInfo) * WNI_CFG_VALID_CHANNEL_LIST_LEN);
    if ( NULL != pChanInfo )
    {
        vos_mem_set(pChanInfo, sizeof(tSirMacChanInfo) * WNI_CFG_VALID_CHANNEL_LIST_LEN, 0);
        pChanInfoStart = pChanInfo;
        for (Index=0; Index < maxChannelIndex; Index++)
        {
            if ((fPopulate5GBand && (CSR_IS_CHANNEL_5GHZ(pMac->scan.defaultPowerTable[Index].chanId))) ||
                (!fPopulate5GBand && (CSR_IS_CHANNEL_24GHZ(pMac->scan.defaultPowerTable[Index].chanId))) )
            {
                if(count >= WNI_CFG_VALID_CHANNEL_LIST_LEN)
                {
                    smsLog( pMac, LOGW, FL(" csrSaveChannelPowerForBand, count exceeded, count =  %d"), count);
                    break;
                }
                pChanInfo->firstChanNum = pMac->scan.defaultPowerTable[Index].chanId;
                pChanInfo->numChannels  = 1;
                pChanInfo->maxTxPower   = CSR_ROAM_MIN( pMac->scan.defaultPowerTable[Index].pwr, pMac->roam.configParam.nTxPowerCap );
                pChanInfo++;
                count++;
            }
        }
        if(count)
        {
            csrSaveToChannelPower2G_5G( pMac, count * sizeof(tSirMacChanInfo), pChanInfoStart );
        }
        vos_mem_free(pChanInfoStart);
    }
}


void csrSetOppositeBandChannelInfo( tpAniSirGlobal pMac )
{
    tANI_BOOLEAN fPopulate5GBand = FALSE;

    do
    {
        // if this is not a dual band product, then we don't need to set the opposite
        // band info.  We only work in one band so no need to look in the other band.
        if ( !CSR_IS_OPEARTING_DUAL_BAND( pMac ) ) break;
        // if we found channel info on the 5.0 band and...
        if ( CSR_IS_CHANNEL_5GHZ( pMac->scan.channelOf11dInfo ) )
        {
            // and the 2.4 band is empty, then populate the 2.4 channel info
            if ( !csrLLIsListEmpty( &pMac->scan.channelPowerInfoList24, LL_ACCESS_LOCK ) ) break;
            fPopulate5GBand = FALSE;
        }
        else
        {
            // else, we found channel info in the 2.4 GHz band.  If the 5.0 band is empty
            // set the 5.0 band info from the 2.4 country code.
            if ( !csrLLIsListEmpty( &pMac->scan.channelPowerInfoList5G, LL_ACCESS_LOCK ) ) break;
            fPopulate5GBand = TRUE;
        }
        csrSaveChannelPowerForBand( pMac, fPopulate5GBand );

    } while( 0 );
}


tANI_BOOLEAN csrIsSupportedChannel(tpAniSirGlobal pMac, tANI_U8 channelId)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE;
    tANI_U32 i;

    //Make sure it is a channel that is in our supported list.
    for ( i = 0; i < pMac->scan.baseChannels.numChannels; i++ )
    {
        if ( channelId == pMac->scan.baseChannels.channelList[i] )
        {
            fRet = eANI_BOOLEAN_TRUE;
            break;
        }
    }

    //If it is configured to limit a set of the channels
    if( fRet && pMac->roam.configParam.fEnforce11dChannels )
    {
        fRet = eANI_BOOLEAN_FALSE;
        for ( i = 0; i < pMac->scan.base20MHzChannels.numChannels; i++ )
        {
            if ( channelId == pMac->scan.base20MHzChannels.channelList[i] )
            {
                fRet = eANI_BOOLEAN_TRUE;
                break;
            }
        }
    }

    return (fRet);
}



//bSize specify the buffer size of pChannelList
tANI_U8 csrGetChannelListFromChannelSet( tpAniSirGlobal pMac, tANI_U8 *pChannelList, tANI_U8 bSize, tCsrChannelPowerInfo *pChannelSet )
{
    tANI_U8 i, j = 0, chnId;

    bSize = CSR_MIN(bSize, pChannelSet->numChannels);
    for( i = 0; i < bSize; i++ )
    {
        chnId = (tANI_U8)(pChannelSet->firstChannel + ( i * pChannelSet->interChannelOffset ));
        if ( csrIsSupportedChannel( pMac, chnId ) )
        {
            pChannelList[j++] = chnId;
        }
    }

    return (j);
}



//bSize -- specify the buffer size of pChannelList
void csrConstructCurrentValidChannelList( tpAniSirGlobal pMac, tDblLinkList *pChannelSetList,
                                            tANI_U8 *pChannelList, tANI_U8 bSize, tANI_U8 *pNumChannels )
{
    tListElem *pEntry;
    tCsrChannelPowerInfo *pChannelSet;
    tANI_U8 numChannels;
    tANI_U8 *pChannels;

    if( pChannelSetList && pChannelList && pNumChannels )
    {
        pChannels = pChannelList;
        *pNumChannels = 0;
        pEntry = csrLLPeekHead( pChannelSetList, LL_ACCESS_LOCK );
        while( pEntry )
        {
            pChannelSet = GET_BASE_ADDR( pEntry, tCsrChannelPowerInfo, link );
            numChannels = csrGetChannelListFromChannelSet( pMac, pChannels, bSize, pChannelSet );
            pChannels += numChannels;
            *pNumChannels += numChannels;
            pEntry = csrLLNext( pChannelSetList, pEntry, LL_ACCESS_LOCK );
        }
    }
}


/*
  * 802.11D only: Gather 11d IE via beacon or Probe response and store them in pAdapter->channels11d
*/
tANI_BOOLEAN csrLearnCountryInformation( tpAniSirGlobal pMac, tSirBssDescription *pSirBssDesc,
                                         tDot11fBeaconIEs *pIes, tANI_BOOLEAN fForce)
{
    eHalStatus status;
    tANI_U8 *pCountryCodeSelected;
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE;
    v_REGDOMAIN_t domainId;
    tDot11fBeaconIEs *pIesLocal = pIes;
    tANI_BOOLEAN useVoting = eANI_BOOLEAN_FALSE;

    if (VOS_STA_SAP_MODE == vos_get_conparam ())
        return eHAL_STATUS_SUCCESS;

    if ((NULL == pSirBssDesc) && (NULL == pIes))
        useVoting = eANI_BOOLEAN_TRUE;

    do
    {
        // check if .11d support is enabled
        if( !csrIs11dSupported( pMac ) ) break;

        if (eANI_BOOLEAN_FALSE == useVoting)
        {
            if( !pIesLocal &&
                (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
                                     pSirBssDesc, &pIesLocal))))
            {
                break;
            }
            // check if country information element is present
            if(!pIesLocal->Country.present)
            {
                //No country info
                break;
            }

            if( HAL_STATUS_SUCCESS(csrGetRegulatoryDomainForCountry
                (pMac, pIesLocal->Country.country, &domainId,
                COUNTRY_QUERY)) &&
                ( domainId == REGDOMAIN_WORLD))
            {
                break;
            }
        } //useVoting == eANI_BOOLEAN_FALSE

        if (eANI_BOOLEAN_FALSE == useVoting)
            pCountryCodeSelected = pIesLocal->Country.country;
        else
            pCountryCodeSelected = pMac->scan.countryCodeElected;

        status = csrGetRegulatoryDomainForCountry(pMac,
                       pCountryCodeSelected, &domainId, COUNTRY_IE);
        if ( status != eHAL_STATUS_SUCCESS )
        {
            smsLog( pMac, LOGE, FL("  fail to get regId %d"), domainId );
            fRet = eANI_BOOLEAN_FALSE;
            break;
        }

        /* updating 11d Country Code with Country code selected. */
        vos_mem_copy(pMac->scan.countryCode11d,
                             pCountryCodeSelected,
                             WNI_CFG_COUNTRY_CODE_LEN);

#ifndef CONFIG_ENABLE_LINUX_REG
        // Checking for Domain Id change
        if ( domainId != pMac->scan.domainIdCurrent )
        {
            vos_mem_copy(pMac->scan.countryCode11d,
                                  pCountryCodeSelected,
                                  sizeof( pMac->scan.countryCode11d ) );
            /* Set Current Country code and Current Regulatory domain */
            status = csrSetRegulatoryDomain(pMac, domainId, NULL);
            if (eHAL_STATUS_SUCCESS != status)
            {
                smsLog(pMac, LOGE, "Set Reg Domain Fail %d", status);
                fRet = eANI_BOOLEAN_FALSE;
                break;
            }
            //csrSetRegulatoryDomain will fail if the country doesn't fit our domain criteria.
            vos_mem_copy(pMac->scan.countryCodeCurrent,
                            pCountryCodeSelected, WNI_CFG_COUNTRY_CODE_LEN);
            //Simply set it to cfg.
            csrSetCfgCountryCode(pMac, pCountryCodeSelected);

            /* overwrite the defualt country code */
            vos_mem_copy(pMac->scan.countryCodeDefault,
                                      pMac->scan.countryCodeCurrent,
                                      WNI_CFG_COUNTRY_CODE_LEN);
            /* Set Current RegDomain */
            status = WDA_SetRegDomain(pMac, domainId, eSIR_TRUE);
            if ( status != eHAL_STATUS_SUCCESS )
            {
                smsLog( pMac, LOGE, FL("  fail to Set regId %d"), domainId );
                fRet = eANI_BOOLEAN_FALSE;
                break;
            }
             /* set to default domain ID */
            pMac->scan.domainIdCurrent = domainId;
            /* get the channels based on new cc */
            status = csrInitGetChannels( pMac );

            if ( status != eHAL_STATUS_SUCCESS )
            {
                smsLog( pMac, LOGE, FL("  fail to get Channels "));
                fRet = eANI_BOOLEAN_FALSE;
                break;
            }
        }
#endif
        fRet = eANI_BOOLEAN_TRUE;

    } while( 0 );

    if( !pIes && pIesLocal )
    {
        //locally allocated
        vos_mem_free(pIesLocal);
    }

    return( fRet );
}


static void csrSaveScanResults(tpAniSirGlobal pMac, tANI_U8 reason,
                               tANI_U8 sessionId)
{
    // initialize this to FALSE. profMoveInterimScanResultsToMainList() routine
    // will set this to the channel where an .11d beacon is seen
    pMac->scan.channelOf11dInfo = 0;
    // if we get any ambiguous .11d information then this will be set to TRUE
    pMac->scan.fAmbiguous11dInfoFound = eANI_BOOLEAN_FALSE;
    //Tush
    // if we get any ambiguous .11d information, then this will be set to TRUE
    // only if the applied 11d info could be found in one of the scan results
    pMac->scan.fCurrent11dInfoMatch = eANI_BOOLEAN_FALSE;
    // move the scan results from interim list to the main scan list
    csrMoveTempScanResultsToMainList(pMac, reason, sessionId);

    // Now check if we gathered any domain/country specific information
    // If so, we should update channel list and apply Tx power settings
    if( csrIs11dSupported(pMac) )
    {
        csrApplyCountryInformation( pMac, FALSE );
    }
}


void csrReinitScanCmd(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    switch (pCommand->u.scanCmd.reason)
    {
    case eCsrScanSetBGScanParam:
    case eCsrScanAbortBgScan:
        if(pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList)
        {
            vos_mem_free(pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList);
            pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList = NULL;
        }
        break;
    case eCsrScanBGScanAbort:
    case eCsrScanBGScanEnable:
    case eCsrScanGetScanChnInfo:
        break;
    case eCsrScanAbortNormalScan:
    default:
        csrScanFreeRequest(pMac, &pCommand->u.scanCmd.u.scanRequest);
        break;
    }
    if(pCommand->u.scanCmd.pToRoamProfile)
    {
        csrReleaseProfile(pMac, pCommand->u.scanCmd.pToRoamProfile);
        vos_mem_free(pCommand->u.scanCmd.pToRoamProfile);
    }
    vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
}


tANI_BOOLEAN csrGetRemainingChannelsFor11dScan( tpAniSirGlobal pMac, tANI_U8 *pChannels, tANI_U8 *pcChannels )
{
    tANI_U32 index11dChannels, index;
    tANI_U32 indexCurrentChannels;
    tANI_BOOLEAN fChannelAlreadyScanned;
    tANI_U32 len = sizeof(pMac->roam.validChannelList);

    *pcChannels = 0;
    if ( CSR_IS_11D_INFO_FOUND(pMac) && csrRoamIsChannelValid(pMac, pMac->scan.channelOf11dInfo) )
    {
        if (HAL_STATUS_SUCCESS(csrGetCfgValidChannels(pMac, (tANI_U8 *)pMac->roam.validChannelList, &len)))
        {
            //Find the channel index where we found the 11d info
            for(index = 0; index < len; index++)
            {
                if(pMac->scan.channelOf11dInfo == pMac->roam.validChannelList[index])
                    break;
            }
            //check whether we found the channel index
            if(index < len)
            {
                // Now, look through the 11d channel list and create a list of all channels in the 11d list that are
                // NOT in the current channel list.  This gives us a list of the new channels that have not been
                // scanned.  We'll scan this new list so we have a complete set of scan results on all of the domain channels
                // initially.
                for ( index11dChannels = 0; index11dChannels < pMac->scan.channels11d.numChannels; index11dChannels++ )
                {
                    fChannelAlreadyScanned = eANI_BOOLEAN_FALSE;

                    for( indexCurrentChannels = 0; indexCurrentChannels < index; indexCurrentChannels++ )
                    {
                        if ( pMac->roam.validChannelList[ indexCurrentChannels ] == pMac->scan.channels11d.channelList[ index11dChannels ] )
                        {
                            fChannelAlreadyScanned = eANI_BOOLEAN_TRUE;
                            break;
                        }
                    }

                    if ( !fChannelAlreadyScanned )
                    {
                        pChannels[ *pcChannels ] = pMac->scan.channels11d.channelList[ index11dChannels ];
                        ( *pcChannels )++;
                    }
                }
            }
        }//GetCFG
    }
    return( *pcChannels );
}


eCsrScanCompleteNextCommand csrScanGetNextCommandState( tpAniSirGlobal pMac, tSmeCmd *pCommand, tANI_BOOLEAN fSuccess )
{
    eCsrScanCompleteNextCommand NextCommand = eCsrNextScanNothing;

    switch( pCommand->u.scanCmd.reason )
    {
        case eCsrScan11d1:
            NextCommand = (fSuccess) ? eCsrNext11dScan1Success : eCsrNext11dScan1Failure;
            break;
        case eCsrScan11d2:
            NextCommand = (fSuccess) ? eCsrNext11dScan2Success : eCsrNext11dScan2Failure;
            break;
        case eCsrScan11dDone:
            NextCommand = eCsrNext11dScanComplete;
            break;
        case eCsrScanLostLink1:
            NextCommand = (fSuccess) ? eCsrNextLostLinkScan1Success : eCsrNextLostLinkScan1Failed;
            break;
        case eCsrScanLostLink2:
            NextCommand = (fSuccess) ? eCsrNextLostLinkScan2Success : eCsrNextLostLinkScan2Failed;
            break;
        case eCsrScanLostLink3:
            NextCommand = (fSuccess) ? eCsrNextLostLinkScan3Success : eCsrNextLostLinkScan3Failed;
            break;
        case eCsrScanForSsid:
            NextCommand = (fSuccess) ? eCsrNexteScanForSsidSuccess : eCsrNexteScanForSsidFailure;
            break;
        case eCsrScanForCapsChange:
            NextCommand = eCsrNextCapChangeScanComplete;    //don't care success or not
            break;
        case eCsrScanIdleScan:
            NextCommand = eCsrNextIdleScanComplete;
            break;
        default:
            NextCommand = eCsrNextScanNothing;
            break;
    }
    return( NextCommand );
}


//Return whether the pCommand is finished.
tANI_BOOLEAN csrHandleScan11d1Failure(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_TRUE;

    //Apply back the default setting and passively scan one more time.
    csrResetCountryInformation(pMac, eANI_BOOLEAN_FALSE, eANI_BOOLEAN_TRUE);
    pCommand->u.scanCmd.reason = eCsrScan11d2;
    if(HAL_STATUS_SUCCESS(csrScanChannels(pMac, pCommand)))
    {
        fRet = eANI_BOOLEAN_FALSE;
    }

    return (fRet);
}


tANI_BOOLEAN csrHandleScan11dSuccess(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_TRUE;
    tANI_U8 *pChannels;
    tANI_U8 cChannels;

    pChannels = vos_mem_malloc(WNI_CFG_VALID_CHANNEL_LIST_LEN);
    if ( NULL != pChannels )
    {
        vos_mem_set(pChannels, WNI_CFG_VALID_CHANNEL_LIST_LEN, 0);
        if ( csrGetRemainingChannelsFor11dScan( pMac, pChannels, &cChannels ) )
        {
            pCommand->u.scanCmd.reason = eCsrScan11dDone;
            if(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList)
            {
                vos_mem_free(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList);
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = NULL;
            }
            pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = vos_mem_malloc(cChannels);
            if ( NULL != pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList )
            {
                vos_mem_copy(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList,
                             pChannels, cChannels);
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = cChannels;
                pCommand->u.scanCmd.u.scanRequest.requestType = eCSR_SCAN_REQUEST_FULL_SCAN;
                pCommand->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
                if(HAL_STATUS_SUCCESS(csrScanChannels(pMac, pCommand)))
                {
                    //Reuse the same command buffer
                    fRet = eANI_BOOLEAN_FALSE;
                }
            }
        }
        vos_mem_free(pChannels);
    }

    return (fRet);
}

//Return whether the command should be removed
tANI_BOOLEAN csrScanComplete( tpAniSirGlobal pMac, tSirSmeScanRsp *pScanRsp )
{
    eCsrScanCompleteNextCommand NextCommand = eCsrNextScanNothing;
    tListElem *pEntry;
    tSmeCmd *pCommand;
    tANI_BOOLEAN fRemoveCommand = eANI_BOOLEAN_TRUE;
    tANI_BOOLEAN fSuccess;

    if (pMac->fScanOffload)
        pEntry = csrLLPeekHead(&pMac->sme.smeScanCmdActiveList, LL_ACCESS_LOCK);
    else
        pEntry = csrLLPeekHead(&pMac->sme.smeCmdActiveList, LL_ACCESS_LOCK);

    if ( pEntry )
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );

        // If the head of the queue is Active and it is a SCAN command, remove
        // and put this on the Free queue.
        if ( eSmeCommandScan == pCommand->command )
        {
            tANI_U32 sessionId = pCommand->sessionId;

            if(eSIR_SME_SUCCESS != pScanRsp->statusCode)
            {
                fSuccess = eANI_BOOLEAN_FALSE;
            }
            else
            {
                //pMac->scan.tempScanResults is not empty meaning the scan found something
                //This check only valid here because csrSaveScanresults is not yet called
                fSuccess = (!csrLLIsListEmpty(&pMac->scan.tempScanResults, LL_ACCESS_LOCK));
            }
            if (pCommand->u.scanCmd.abortScanDueToBandChange)
            {
                /*
                 * Scan aborted due to band change
                 * The scan results need to be flushed
                 */
                if (pCommand->u.scanCmd.callback
                    != pMac->scan.callback11dScanDone)
                {
                    smsLog(pMac, LOG1, FL("Filtering the scan results as the "
                                          "results may belong to wrong band"));
                    csrScanFilterResults(pMac);
                }
                else
                {
                    smsLog(pMac, LOG1, FL("11d_scan_done will flush the scan"
                                          " results"));
                }
                pCommand->u.scanCmd.abortScanDueToBandChange
                    = eANI_BOOLEAN_FALSE;
            }
            csrSaveScanResults(pMac, pCommand->u.scanCmd.reason, sessionId);

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
            {
                vos_log_scan_pkt_type *pScanLog = NULL;
                tScanResultHandle hScanResult;
                tCsrScanResultInfo *pScanResult;
                tDot11fBeaconIEs *pIes;
                int n = 0, c = 0;

                WLAN_VOS_DIAG_LOG_ALLOC(pScanLog, vos_log_scan_pkt_type, LOG_WLAN_SCAN_C);
                if(pScanLog)
                {
                    if(eCsrScanBgScan == pCommand->u.scanCmd.reason ||
                        eCsrScanProbeBss == pCommand->u.scanCmd.reason ||
                        eCsrScanSetBGScanParam == pCommand->u.scanCmd.reason)
                    {
                        pScanLog->eventId = WLAN_SCAN_EVENT_HO_SCAN_RSP;
                    }
                    else
                    {
                        if( eSIR_PASSIVE_SCAN != pMac->scan.curScanType )
                        {
                            pScanLog->eventId = WLAN_SCAN_EVENT_ACTIVE_SCAN_RSP;
                        }
                        else
                        {
                            pScanLog->eventId = WLAN_SCAN_EVENT_PASSIVE_SCAN_RSP;
                        }
                    }
                    if(eSIR_SME_SUCCESS == pScanRsp->statusCode)
                    {
                        if(HAL_STATUS_SUCCESS(csrScanGetResult(pMac, NULL, &hScanResult)))
                        {
                            while(((pScanResult = csrScanResultGetNext(pMac, hScanResult)) != NULL))
                            {
                                if( n < VOS_LOG_MAX_NUM_BSSID )
                                {
                                    if(!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, &pScanResult->BssDescriptor, &pIes)))
                                    {
                                        smsLog(pMac, LOGE, FL(" fail to parse IEs"));
                                        break;
                                    }
                                    vos_mem_copy(pScanLog->bssid[n],
                                                 pScanResult->BssDescriptor.bssId, 6);
                                    if(pIes && pIes->SSID.present && VOS_LOG_MAX_SSID_SIZE >= pIes->SSID.num_ssid)
                                    {
                                        vos_mem_copy(pScanLog->ssid[n],
                                                     pIes->SSID.ssid, pIes->SSID.num_ssid);
                                    }
                                    vos_mem_free(pIes);
                                    n++;
                                }
                                c++;
                            }
                            pScanLog->numSsid = (v_U8_t)n;
                            pScanLog->totalSsid = (v_U8_t)c;
                            csrScanResultPurge(pMac, hScanResult);
                        }
                    }
                    else
                    {
                        pScanLog->status = WLAN_SCAN_STATUS_FAILURE;
                    }
                    WLAN_VOS_DIAG_LOG_REPORT(pScanLog);
                }
            }
#endif //#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR

            NextCommand = csrScanGetNextCommandState(pMac, pCommand, fSuccess);
            //We reuse the command here instead reissue a new command
            switch(NextCommand)
            {
            case eCsrNext11dScan1Success:
            case eCsrNext11dScan2Success:
                smsLog( pMac, LOG2, FL("11dScan1/3 produced results.  Reissue Active scan..."));
                // if we found country information, no need to continue scanning further, bail out
                fRemoveCommand = eANI_BOOLEAN_TRUE;
                NextCommand = eCsrNext11dScanComplete;
                break;
            case eCsrNext11dScan1Failure:
                //We are not done yet. 11d scan fail once. We will try to reset anything and do it over again
                //The only meaningful thing for this retry is that we cannot find 11d information after a reset so
                //we clear the "old" 11d info and give it once more chance
                fRemoveCommand = csrHandleScan11d1Failure(pMac, pCommand);
                if(fRemoveCommand)
                {
                    NextCommand = eCsrNext11dScanComplete;
                }
                break;
            case eCsrNextLostLinkScan1Success:
                if(!HAL_STATUS_SUCCESS(csrIssueRoamAfterLostlinkScan(pMac, sessionId, eCsrLostLink1)))
                {
                    csrScanHandleFailedLostlink1(pMac, sessionId);
                }
                break;
            case eCsrNextLostLinkScan2Success:
                if(!HAL_STATUS_SUCCESS(csrIssueRoamAfterLostlinkScan(pMac, sessionId, eCsrLostLink2)))
                {
                    csrScanHandleFailedLostlink2(pMac, sessionId);
                }
                break;
            case eCsrNextLostLinkScan3Success:
                if(!HAL_STATUS_SUCCESS(csrIssueRoamAfterLostlinkScan(pMac, sessionId, eCsrLostLink3)))
                {
                    csrScanHandleFailedLostlink3(pMac, sessionId);
                }
                break;
            case eCsrNextLostLinkScan1Failed:
                csrScanHandleFailedLostlink1(pMac, sessionId);
                break;
            case eCsrNextLostLinkScan2Failed:
                csrScanHandleFailedLostlink2(pMac, sessionId);
                break;
            case eCsrNextLostLinkScan3Failed:
                csrScanHandleFailedLostlink3(pMac, sessionId);
                break;
            case eCsrNexteScanForSsidSuccess:
                csrScanHandleSearchForSSID(pMac, pCommand);
                break;
            case eCsrNexteScanForSsidFailure:
                csrScanHandleSearchForSSIDFailure(pMac, pCommand);
                break;
            case eCsrNextIdleScanComplete:
                pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;
                break;
            case eCsrNextCapChangeScanComplete:
                csrScanHandleCapChangeScanComplete(pMac, sessionId);
                break;
            default:

                break;
            }
        }
        else
        {
            smsLog( pMac, LOGW, FL("Scan Completion called but SCAN command is not ACTIVE ..."));
            fRemoveCommand = eANI_BOOLEAN_FALSE;
        }
    }
    else
    {
        smsLog( pMac, LOGW, FL("Scan Completion called but NO commands are ACTIVE ..."));
        fRemoveCommand = eANI_BOOLEAN_FALSE;
    }

    return( fRemoveCommand );
}



static void csrScanRemoveDupBssDescriptionFromInterimList( tpAniSirGlobal pMac,
                                                           tSirBssDescription *pSirBssDescr,
                                                           tDot11fBeaconIEs *pIes)
{
    tListElem *pEntry;
    tCsrScanResult *pCsrBssDescription;

    csrLLLock(&pMac->scan.tempScanResults);
    // Walk through all the chained BssDescriptions.  If we find a chained BssDescription that
    // matches the BssID of the BssDescription passed in, then these must be duplicate scan
    // results for this Bss.  In that case, remove the 'old' Bss description from the linked list.
    pEntry = csrLLPeekHead(&pMac->scan.tempScanResults, LL_ACCESS_NOLOCK);
    while( pEntry )
    {
        pCsrBssDescription = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );

        // we have a duplicate scan results only when BSSID, SSID, Channel and NetworkType
        // matches

        if ( csrIsDuplicateBssDescription( pMac, &pCsrBssDescription->Result.BssDescriptor,
                                             pSirBssDescr, pIes, FALSE ) )
        {
            pSirBssDescr->rssi = (tANI_S8)( (((tANI_S32)pSirBssDescr->rssi * CSR_SCAN_RESULT_RSSI_WEIGHT ) +
                                    ((tANI_S32)pCsrBssDescription->Result.BssDescriptor.rssi * (100 - CSR_SCAN_RESULT_RSSI_WEIGHT) )) / 100 );

            // Remove the 'old' entry from the list....
            if(csrLLRemoveEntry(&pMac->scan.tempScanResults, pEntry,
                          LL_ACCESS_NOLOCK))
            {
                csrCheckNSaveWscIe(pMac, pSirBssDescr, &pCsrBssDescription->Result.BssDescriptor);
                // we need to free the memory associated with this node
                csrFreeScanResultEntry( pMac, pCsrBssDescription );
            }

            // If we found a match, we can stop looking through the list.
            break;
        }

        pEntry = csrLLNext(&pMac->scan.tempScanResults, pEntry,
                                 LL_ACCESS_NOLOCK);
    }
    csrLLUnlock(&pMac->scan.tempScanResults);
}



//Caller allocated memory pfNewBssForConn to return whether new candidate for
//current connection is found. Cannot be NULL
tCsrScanResult *csrScanSaveBssDescriptionToInterimList( tpAniSirGlobal pMac,
                                                        tSirBssDescription *pBSSDescription,
                                                        tDot11fBeaconIEs *pIes)
{
    tCsrScanResult *pCsrBssDescription = NULL;
    tANI_U32 cbBSSDesc;
    tANI_U32 cbAllocated;

    // figure out how big the BSS description is (the BSSDesc->length does NOT
    // include the size of the length field itself).
    cbBSSDesc = pBSSDescription->length + sizeof( pBSSDescription->length );

    cbAllocated = sizeof( tCsrScanResult ) + cbBSSDesc;

    pCsrBssDescription = vos_mem_malloc(cbAllocated);
    if ( NULL != pCsrBssDescription )
    {
        vos_mem_set(pCsrBssDescription, cbAllocated, 0);
        pCsrBssDescription->AgingCount = (tANI_S32)pMac->roam.configParam.agingCount;
        smsLog(pMac, LOGW,
           FL(" Set Aging Count = %d for BSS "MAC_ADDRESS_STR" "),
           pCsrBssDescription->AgingCount,
           MAC_ADDR_ARRAY(pBSSDescription->bssId));
        vos_mem_copy(&pCsrBssDescription->Result.BssDescriptor, pBSSDescription, cbBSSDesc );
        //Save SSID separately for later use
        if( pIes->SSID.present && !csrIsNULLSSID(pIes->SSID.ssid, pIes->SSID.num_ssid) )
        {
            //SSID not hidden
            tANI_U32 len = pIes->SSID.num_ssid;
            if (len > SIR_MAC_MAX_SSID_LENGTH)
            {
               // truncate to fit in our struct
               len = SIR_MAC_MAX_SSID_LENGTH;
            }
            pCsrBssDescription->Result.ssId.length = len;
            pCsrBssDescription->Result.timer = vos_timer_get_system_time();
            vos_mem_copy(pCsrBssDescription->Result.ssId.ssId, pIes->SSID.ssid, len);
        }
        csrLLInsertTail( &pMac->scan.tempScanResults, &pCsrBssDescription->Link, LL_ACCESS_LOCK );
    }

    return( pCsrBssDescription );
}




tANI_BOOLEAN csrIsDuplicateBssDescription( tpAniSirGlobal pMac, tSirBssDescription *pSirBssDesc1,
                                           tSirBssDescription *pSirBssDesc2, tDot11fBeaconIEs *pIes2, tANI_BOOLEAN fForced )
{
    tANI_BOOLEAN fMatch = FALSE;
    tSirMacCapabilityInfo *pCap1, *pCap2;
    tDot11fBeaconIEs *pIes1 = NULL;
    tDot11fBeaconIEs *pIesTemp = pIes2;

    pCap1 = (tSirMacCapabilityInfo *)&pSirBssDesc1->capabilityInfo;
    pCap2 = (tSirMacCapabilityInfo *)&pSirBssDesc2->capabilityInfo;
    if(pCap1->ess == pCap2->ess)
    {
        if (pCap1->ess &&
                csrIsMacAddressEqual( pMac, (tCsrBssid *)pSirBssDesc1->bssId, (tCsrBssid *)pSirBssDesc2->bssId)&&
            (fForced || (vos_chan_to_band(pSirBssDesc1->channelId) == vos_chan_to_band((pSirBssDesc2->channelId)))))
        {
            fMatch = TRUE;
            // Check for SSID match, if exists
            do
            {
                if(!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, pSirBssDesc1, &pIes1)))
                {
                    break;
                }
                if( NULL == pIesTemp )
                {
                    if(!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, pSirBssDesc2, &pIesTemp)))
                    {
                        break;
                    }
                }
                if(pIes1->SSID.present && pIesTemp->SSID.present)
                {
                    fMatch = csrIsSsidMatch(pMac, pIes1->SSID.ssid, pIes1->SSID.num_ssid,
                                            pIesTemp->SSID.ssid, pIesTemp->SSID.num_ssid, eANI_BOOLEAN_TRUE);
                }
            }while(0);

        }
        else if (pCap1->ibss && (pSirBssDesc1->channelId == pSirBssDesc2->channelId))
        {

            do
            {
                if(!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, pSirBssDesc1, &pIes1)))
                {
                    break;
                }
                if( NULL == pIesTemp )
                {
                    if(!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac, pSirBssDesc2, &pIesTemp)))
                    {
                        break;
                    }
                }
                //Same channel cannot have same SSID for different IBSS
                if(pIes1->SSID.present && pIesTemp->SSID.present)
                {
                    fMatch = csrIsSsidMatch(pMac, pIes1->SSID.ssid, pIes1->SSID.num_ssid,
                                            pIesTemp->SSID.ssid, pIesTemp->SSID.num_ssid, eANI_BOOLEAN_TRUE);
                }
            }while(0);
        }
        /* In case of P2P devices, ess and ibss will be set to zero */
        else if (!pCap1->ess &&
                csrIsMacAddressEqual( pMac, (tCsrBssid *)pSirBssDesc1->bssId, (tCsrBssid *)pSirBssDesc2->bssId))
        {
            fMatch = TRUE;
        }
    }

    if(pIes1)
    {
        vos_mem_free(pIes1);
    }

    if( (NULL == pIes2) && pIesTemp )
    {
        //locally allocated
        vos_mem_free(pIesTemp);
    }

    return( fMatch );
}


tANI_BOOLEAN csrIsNetworkTypeEqual( tSirBssDescription *pSirBssDesc1, tSirBssDescription *pSirBssDesc2 )
{
    return( pSirBssDesc1->nwType == pSirBssDesc2->nwType );
}


//to check whether the BSS matches the dot11Mode
static tANI_BOOLEAN csrScanIsBssAllowed(tpAniSirGlobal pMac, tSirBssDescription *pBssDesc,
                                        tDot11fBeaconIEs *pIes)
{
    tANI_BOOLEAN fAllowed = eANI_BOOLEAN_FALSE;
    eCsrPhyMode phyMode;

    if(HAL_STATUS_SUCCESS(csrGetPhyModeFromBss(pMac, pBssDesc, &phyMode, pIes)))
    {
        switch(pMac->roam.configParam.phyMode)
        {
        case eCSR_DOT11_MODE_11b:
            fAllowed = (tANI_BOOLEAN)(eCSR_DOT11_MODE_11a != phyMode);
            break;
        case eCSR_DOT11_MODE_11g:
            fAllowed = (tANI_BOOLEAN)(eCSR_DOT11_MODE_11a != phyMode);
            break;
        case eCSR_DOT11_MODE_11g_ONLY:
            fAllowed = (tANI_BOOLEAN)(eCSR_DOT11_MODE_11g == phyMode);
            break;
        case eCSR_DOT11_MODE_11a:
            fAllowed = (tANI_BOOLEAN)((eCSR_DOT11_MODE_11b != phyMode) && (eCSR_DOT11_MODE_11g != phyMode));
            break;
        case eCSR_DOT11_MODE_11n_ONLY:
            fAllowed = (tANI_BOOLEAN)((eCSR_DOT11_MODE_11n == phyMode));
            break;

#ifdef WLAN_FEATURE_11AC
         case eCSR_DOT11_MODE_11ac_ONLY:
             fAllowed = (tANI_BOOLEAN)((eCSR_DOT11_MODE_11ac == phyMode));
             break;
#endif
        case eCSR_DOT11_MODE_11b_ONLY:
            fAllowed = (tANI_BOOLEAN)(eCSR_DOT11_MODE_11b == phyMode);
            break;
        case eCSR_DOT11_MODE_11n:
#ifdef WLAN_FEATURE_11AC
        case eCSR_DOT11_MODE_11ac:
#endif
        default:
            fAllowed = eANI_BOOLEAN_TRUE;
            break;
        }
    }

    return (fAllowed);
}



//Return pIes to caller for future use when returning TRUE.
static tANI_BOOLEAN csrScanValidateScanResult( tpAniSirGlobal pMac, tANI_U8 *pChannels,
                                               tANI_U8 numChn, tSirBssDescription *pBssDesc,
                                               tDot11fBeaconIEs **ppIes )
{
    tANI_BOOLEAN fValidChannel = FALSE;
    tDot11fBeaconIEs *pIes = NULL;
    tANI_U8 index;

    for( index = 0; index < numChn; index++ )
    {
        /*
         * This check relies on the fact that a single BSS description is
         * returned in each ScanRsp call, which is the way LIM implemented
         * the scan req/rsp functions. We changed to this model when we ran
         * with a large number of APs.  If this were to change, then this check
         * would have to mess with removing the bssDescription from somewhere
         * in an arbitrary index in the bssDescription array.
         */
        if ( pChannels[ index ] == pBssDesc->channelId )
        {
           fValidChannel = TRUE;
           break;
        }
    }
    *ppIes = NULL;
    if(fValidChannel)
    {
        if( HAL_STATUS_SUCCESS( csrGetParsedBssDescriptionIEs(pMac, pBssDesc, &pIes) ) )
        {
            fValidChannel = csrScanIsBssAllowed(pMac, pBssDesc, pIes);
            if( fValidChannel )
            {
                *ppIes = pIes;
            }
            else
            {
                vos_mem_free(pIes);
            }
        }
        else
        {
            fValidChannel = FALSE;
        }
    }

    return( fValidChannel );
}


//Return whether last scan result is received
static tANI_BOOLEAN csrScanProcessScanResults( tpAniSirGlobal pMac, tSmeCmd *pCommand,
                                                tSirSmeScanRsp *pScanRsp, tANI_BOOLEAN *pfRemoveCommand )
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE, fRemoveCommand = eANI_BOOLEAN_FALSE;
    tDot11fBeaconIEs *pIes = NULL;
    tANI_U32 cbParsed;
    tSirBssDescription *pSirBssDescription;
    tANI_U32 cbBssDesc;
    tANI_U32 cbScanResult = GET_FIELD_OFFSET( tSirSmeScanRsp, bssDescription )
                            + sizeof(tSirBssDescription);    //We need at least one CB
    tpCsrNeighborRoamControlInfo pNeighborRoamInfo =
                              &pMac->roam.neighborRoamInfo[pScanRsp->sessionId];

    /*
     * Don't consider the scan rsp to be valid if the status code is Scan
     * Failure.  Scan Failure is returned when the scan could not find anything.
     * So if we get scan failure return that the scan response is invalid.
     * Also check the length in the scan result for valid scan BssDescriptions.
     */
    do
    {
        if ( ( cbScanResult <= pScanRsp->length ) &&
             (( eSIR_SME_SUCCESS == pScanRsp->statusCode ) ||
              ( eSIR_SME_MORE_SCAN_RESULTS_FOLLOW == pScanRsp->statusCode ) ) )
        {
            tANI_U8 *pChannelList = NULL;
            tANI_U8 cChannels = 0;

            //Different scan type can reach this point, we need to distinguish it
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
            if( eCsrScanGetLfrResult == pCommand->u.scanCmd.reason )
            {
                pChannelList = NULL;
                cChannels = 0;
            }
            else
#endif
            if( eCsrScanSetBGScanParam == pCommand->u.scanCmd.reason )
            {
                //eCsrScanSetBGScanParam uses different structure
                tCsrBGScanRequest *pBgScanReq = &pCommand->u.scanCmd.u.bgScanRequest;

                cChannels = pBgScanReq->ChannelInfo.numOfChannels;
                pChannelList = pBgScanReq->ChannelInfo.ChannelList;
            }
            else
            {
                //the rest use generic scan request
                cChannels = pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
                pChannelList = pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList;
            }

            // if the scan result is not on one of the channels in the Valid channel list, then it
            // must have come from an AP on an overlapping channel (in the 2.4GHz band).  In this case,
            // let's drop the scan result.
            //
            // The other situation is where the scan request is for a scan on a particular channel set
            // and the scan result is from a

            // if the NumChannels is 0, then we are supposed to be scanning all channels.  Use the full channel
            // list as the 'valid' channel list.  Otherwise, use the specific channel list in the scan parms
            // as the valid channels.
            if ( 0 == cChannels )
            {
                tANI_U32 len = sizeof(pMac->roam.validChannelList);

                if (HAL_STATUS_SUCCESS(csrGetCfgValidChannels(pMac, (tANI_U8 *)pMac->roam.validChannelList, &len)))
                {
                    pChannelList = pMac->roam.validChannelList;
                    cChannels = (tANI_U8)len;
                }
                else
                {
                    //Cannot continue
                    smsLog( pMac, LOGE, "CSR: Processing internal SCAN results...csrGetCfgValidChannels failed" );
                    break;
                }
            }

            smsLog( pMac, LOG2, "CSR: Processing internal SCAN results..." );
            cbParsed = GET_FIELD_OFFSET( tSirSmeScanRsp, bssDescription );
            pSirBssDescription = pScanRsp->bssDescription;
            while( cbParsed < pScanRsp->length )
            {
                if ( csrScanValidateScanResult( pMac, pChannelList, cChannels, pSirBssDescription, &pIes ) )
                {
                    csrScanRemoveDupBssDescriptionFromInterimList(pMac, pSirBssDescription, pIes);
                    csrScanSaveBssDescriptionToInterimList( pMac, pSirBssDescription, pIes );
                    if( eSIR_PASSIVE_SCAN == pMac->scan.curScanType )
                    {
                        if( csrIs11dSupported( pMac) )
                        {
                            //Check whether the BSS is acceptable base on 11d info and our configs.
                            if( csrMatchCountryCode( pMac, NULL, pIes ) )
                            {
                                //Double check whether the channel is acceptable by us.
                                if( csrIsSupportedChannel( pMac, pSirBssDescription->channelId ) )
                                {
                                    pMac->scan.curScanType = eSIR_ACTIVE_SCAN;
                                }
                            }
                        }
                        else
                        {
                            pMac->scan.curScanType = eSIR_ACTIVE_SCAN;
                        }
                    }
                    //Free the resource
                    vos_mem_free(pIes);
                }
                // skip over the BSS description to the next one...
                cbBssDesc = pSirBssDescription->length + sizeof( pSirBssDescription->length );

                cbParsed += cbBssDesc;
                pSirBssDescription = (tSirBssDescription *)((tANI_U8 *)pSirBssDescription + cbBssDesc );

            } //while
        }
        else
        {
            smsLog( pMac, LOGW, " Scanrsp fail (0x%08X), length = %d (expected %d)",
                    pScanRsp->statusCode, pScanRsp->length, cbScanResult);
            //HO bg scan/probe failed no need to try autonomously
            if(eCsrScanBgScan == pCommand->u.scanCmd.reason ||
               eCsrScanProbeBss == pCommand->u.scanCmd.reason ||
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
               eCsrScanGetLfrResult == pCommand->u.scanCmd.reason ||
#endif
               eCsrScanSetBGScanParam == pCommand->u.scanCmd.reason)
            {
                fRemoveCommand = eANI_BOOLEAN_TRUE;
            }
        }
    }while(0);
    if ( eSIR_SME_MORE_SCAN_RESULTS_FOLLOW != pScanRsp->statusCode )
    {
        smsLog(pMac, LOGE, "Found %d BSS, statusCode %d",
                           csrLLCount(&pMac->scan.tempScanResults),
                           pScanRsp->statusCode);
        smsLog(pMac, LOG1, "scan reason is %d", pCommand->u.scanCmd.reason);
#ifdef FEATURE_WLAN_DIAG_SUPPORT
        csr_diag_event_report(pMac, eCSR_EVENT_SCAN_COMPLETE, eSIR_SUCCESS,
                              eSIR_SUCCESS);
        if (csrLLCount(&pMac->scan.tempScanResults) > 0)
            csr_diag_event_report(pMac, eCSR_EVENT_SCAN_RES_FOUND, eSIR_SUCCESS,
                                  eSIR_SUCCESS);
#endif
        fRemoveCommand = csrScanComplete( pMac, pScanRsp );
        fRet = eANI_BOOLEAN_TRUE;
    }//if ( eSIR_SME_MORE_SCAN_RESULTS_FOLLOW != pScanRsp->statusCode )
    if(pfRemoveCommand)
    {
        *pfRemoveCommand = fRemoveCommand;
    }

#ifdef WLAN_AP_STA_CONCURRENCY
    if (pMac->fScanOffload)
        return fRet;

    if (!csrLLIsListEmpty( &pMac->scan.scanCmdPendingList, LL_ACCESS_LOCK ))
    {
        /* Pending scan commands in the list because the previous scan command
         * was split into a scan command on one channel + a scan command for all
         * remaining channels.
         *
         * Start timer to trigger processing of the next scan command.
         * NOTE for LFR:
         * Do not split scans if no concurrent infra connections are
         * active and if the scan is a BG scan triggered by LFR (OR)
         * any scan if LFR is in the middle of a BG scan. Splitting
         * the scan is delaying the time it takes for LFR to find
         * candidates and resulting in disconnects.
         */
        if ((csrIsStaSessionConnected(pMac) &&
#ifdef FEATURE_WLAN_LFR
            (csrIsConcurrentInfraConnected(pMac) ||
            ((pCommand->u.scanCmd.reason != eCsrScanBgScan) &&
            (pNeighborRoamInfo->neighborRoamState !=
                       eCSR_NEIGHBOR_ROAM_STATE_CFG_CHAN_LIST_SCAN))) &&
#endif
            (pCommand->u.scanCmd.u.scanRequest.p2pSearch != 1)) ||
            (csrIsP2pSessionConnected(pMac)))
        {
            /* if active connected sessions present then continue to split scan
             * with specified interval between consecutive scans */
            csrSetDefaultScanTiming(pMac, pCommand->u.scanCmd.u.scanRequest.scanType, &(pCommand->u.scanCmd.u.scanRequest));
            vos_timer_start(&pMac->scan.hTimerStaApConcTimer,
                pCommand->u.scanCmd.u.scanRequest.restTime);
        } else {
            /* if no connected sessions present then initiate next scan command immediately */
            /* minimum timer granularity is 10ms */
            vos_timer_start(&pMac->scan.hTimerStaApConcTimer, 10);
        }
    }
#endif
    return (fRet);
}


tANI_BOOLEAN csrScanIsWildCardScan( tpAniSirGlobal pMac, tSmeCmd *pCommand )
{
    tANI_U8 bssid[VOS_MAC_ADDR_SIZE] = {0, 0, 0, 0, 0, 0};
    tANI_BOOLEAN f = vos_mem_compare(pCommand->u.scanCmd.u.scanRequest.bssid,
                                     bssid, sizeof(tCsrBssid));

    //It is not a wild card scan if the bssid is not broadcast and the number of SSID is 1.
    return ((tANI_BOOLEAN)( (f || (0xff == pCommand->u.scanCmd.u.scanRequest.bssid[0])) &&
        (pCommand->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs != 1) ));
}

#ifdef FEATURE_WLAN_SCAN_PNO
eHalStatus csrSavePnoScanResults(tpAniSirGlobal pMac, tSirSmeScanRsp *pScanRsp,
                                 tANI_U8 sessionId)
{
    tSirBssDescription *pSirBssDescription;
    tANI_U32 cbScanResult = GET_FIELD_OFFSET( tSirSmeScanRsp, bssDescription )
                            + sizeof(tSirBssDescription);  //We need at least one CB
    tCsrScanResult *pScanResult = NULL;
    tAniSSID tmpSsid;
    v_TIME_t timer;
    tANI_U32 cbParsed;
    tANI_U32 cbBssDesc;
    tANI_U16 ieLen;

    if ((cbScanResult > pScanRsp->length ) ||
        (( eSIR_SME_SUCCESS != pScanRsp->statusCode ) &&
         ( eSIR_SME_MORE_SCAN_RESULTS_FOLLOW != pScanRsp->statusCode ) ) )
                  return eHAL_STATUS_FAILURE;

    cbParsed = GET_FIELD_OFFSET( tSirSmeScanRsp, bssDescription );
    pSirBssDescription = pScanRsp->bssDescription;

    while( cbParsed < pScanRsp->length )
    {
        // Check whether we have reach out limit
        if ( CSR_SCAN_IS_OVER_BSS_LIMIT(pMac) )
        {
            smsLog( pMac, LOGW, FL( "BSS limit reached"));
            return eHAL_STATUS_RESOURCES;
        }

        ieLen = (pSirBssDescription->length + sizeof( pSirBssDescription->length )
                        - GET_FIELD_OFFSET( tSirBssDescription, ieFields ));

        pScanResult = vos_mem_malloc(sizeof(tCsrScanResult) + ieLen);
        if (NULL == pScanResult)
        {
            smsLog(pMac, LOGE, FL(" Fail to allocate memory for frame"));
            return eHAL_STATUS_RESOURCES;
        }

        vos_mem_set(pScanResult, sizeof(tCsrScanResult) + ieLen, 0);

        if (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
            pSirBssDescription, (tDot11fBeaconIEs **)&pScanResult->Result.pvIes)))
        {
            smsLog(pMac, LOGE, FL("  Cannot parse IEs"));
            csrFreeScanResultEntry(pMac, pScanResult);
            return eHAL_STATUS_RESOURCES;
        }

        cbBssDesc = pSirBssDescription->length +
                    sizeof( pSirBssDescription->length );

        vos_mem_copy(&pScanResult->Result.BssDescriptor, pSirBssDescription,
                     cbBssDesc);

        // Remove duplicate entry
        csrRemoveDupBssDescription( pMac, &pScanResult->Result.BssDescriptor,
                                    (tDot11fBeaconIEs *)pScanResult->Result.pvIes,
                                    &tmpSsid , &timer, FALSE );
        //Add to scan cache
        csrScanAddResult(pMac, pScanResult,
                         (tDot11fBeaconIEs *)pScanResult->Result.pvIes,
                         sessionId);

        // skip over the BSS description to the next one...
        cbParsed += cbBssDesc;
        pSirBssDescription = (tSirBssDescription *)((tANI_U8 *)pSirBssDescription +
                              cbBssDesc );
    }

    return eHAL_STATUS_SUCCESS;
}
#endif

eHalStatus csrScanSmeScanResponse( tpAniSirGlobal pMac, void *pMsgBuf )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tListElem *pEntry;
    tSmeCmd *pCommand;
    eCsrScanStatus scanStatus;
    tSirSmeScanRsp *pScanRsp = (tSirSmeScanRsp *)pMsgBuf;
    tSmeGetScanChnRsp *pScanChnInfo;
    tANI_BOOLEAN fRemoveCommand = eANI_BOOLEAN_TRUE;
    eCsrScanReason reason = eCsrScanOther;

    if (pMac->fScanOffload)
        pEntry = csrLLPeekHead(&pMac->sme.smeScanCmdActiveList,
                               LL_ACCESS_LOCK);
    else
        pEntry = csrLLPeekHead(&pMac->sme.smeCmdActiveList, LL_ACCESS_LOCK);

    if ( pEntry )
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        if ( eSmeCommandScan == pCommand->command )
        {
            scanStatus = (eSIR_SME_SUCCESS == pScanRsp->statusCode) ? eCSR_SCAN_SUCCESS : eCSR_SCAN_FAILURE;
            reason = pCommand->u.scanCmd.reason;
            switch(pCommand->u.scanCmd.reason)
            {
            case eCsrScanAbortBgScan:
            case eCsrScanAbortNormalScan:
            case eCsrScanBGScanAbort:
            case eCsrScanBGScanEnable:
                break;
            case eCsrScanGetScanChnInfo:
                pScanChnInfo = (tSmeGetScanChnRsp *)pMsgBuf;
                /*
                 * status code not available in tSmeGetScanChnRsp, so
                 * by default considering it to be success
                 */
                scanStatus = eSIR_SME_SUCCESS;
                csrScanAgeResults(pMac, pScanChnInfo);
                break;
            case eCsrScanForCapsChange:
                csrScanProcessScanResults( pMac, pCommand, pScanRsp, &fRemoveCommand );
                break;
            case eCsrScanP2PFindPeer:
              scanStatus = ((eSIR_SME_SUCCESS == pScanRsp->statusCode) && (pScanRsp->length > 50)) ? eCSR_SCAN_FOUND_PEER : eCSR_SCAN_FAILURE;
              csrScanProcessScanResults( pMac, pCommand, pScanRsp, NULL );
              break;
            case eCsrScanSetBGScanParam:
            default:
                if(csrScanProcessScanResults( pMac, pCommand, pScanRsp, &fRemoveCommand ))
                {
                    /*
                     * Not to get channel info if the scan is not a
                     * wild card scan because it may cause scan results got
                     * aged out incorrectly.
                     */
                    if( csrScanIsWildCardScan( pMac, pCommand ) && (!pCommand->u.scanCmd.u.scanRequest.p2pSearch)
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
                        && (pCommand->u.scanCmd.reason != eCsrScanGetLfrResult)
#endif
                      )
                    {
                        //Get the list of channels scanned
                       if( pCommand->u.scanCmd.reason != eCsrScanUserRequest)
                       {
                           csrScanGetScanChnInfo(pMac, pCommand->sessionId,
                                                 NULL, NULL,
                                                 pCommand->u.scanCmd.scanID);
                       }
                       else
                       {
                           csrScanGetScanChnInfo(pMac,
                                   pCommand->sessionId,
                                   pCommand->u.scanCmd.pContext,
                                   pCommand->u.scanCmd.callback,
                                   pCommand->u.scanCmd.scanID);
                           pCommand->u.scanCmd.callback = NULL;
                       }
                    }
                }
                break;
            }//switch
            if(fRemoveCommand)
            {

                csrReleaseScanCommand(pMac, pCommand, scanStatus);

                }
            smeProcessPendingQueue( pMac );
        }
#ifdef FEATURE_WLAN_SCAN_PNO
        else if (pMac->pnoOffload &&
                 !HAL_STATUS_SUCCESS(csrSavePnoScanResults(pMac, pScanRsp,
                                                    pScanRsp->sessionId)))
        {
            smsLog( pMac, LOGE, "CSR: Unable to store scan results for PNO" );
            status = eHAL_STATUS_FAILURE;
        }
#endif
        else
        {
            smsLog( pMac, LOGE, "CSR: Scan Completion called but SCAN command is not ACTIVE ..." );
            status = eHAL_STATUS_FAILURE;
        }
    }
#ifdef FEATURE_WLAN_SCAN_PNO
    else if (pMac->pnoOffload &&
             !HAL_STATUS_SUCCESS(csrSavePnoScanResults(pMac, pScanRsp,
                                                pScanRsp->sessionId)))
    {
        smsLog( pMac, LOGE, "CSR: Unable to store scan results for PNO" );
        status = eHAL_STATUS_FAILURE;
    }
#endif
    else if (pMac->pnoOffload == FALSE)
    {
        smsLog( pMac, LOGE, "CSR: Scan Completion called but NO commands are ACTIVE ..." );
        status = eHAL_STATUS_FAILURE;
    }

    return (status);
}




tCsrScanResultInfo *csrScanResultGetFirst(tpAniSirGlobal pMac, tScanResultHandle hScanResult)
{
    tListElem *pEntry;
    tCsrScanResult *pResult;
    tCsrScanResultInfo *pRet = NULL;
    tScanResultList *pResultList = (tScanResultList *)hScanResult;

    if(pResultList)
    {
        csrLLLock(&pResultList->List);
        pEntry = csrLLPeekHead(&pResultList->List, LL_ACCESS_NOLOCK);
        if(pEntry)
        {
            pResult = GET_BASE_ADDR(pEntry, tCsrScanResult, Link);
            pRet = &pResult->Result;
        }
        pResultList->pCurEntry = pEntry;
        csrLLUnlock(&pResultList->List);
    }

    return pRet;
}


tCsrScanResultInfo *csrScanResultGetNext(tpAniSirGlobal pMac, tScanResultHandle hScanResult)
{
    tListElem *pEntry = NULL;
    tCsrScanResult *pResult = NULL;
    tCsrScanResultInfo *pRet = NULL;
    tScanResultList *pResultList = (tScanResultList *)hScanResult;

    if(pResultList)
    {
        csrLLLock(&pResultList->List);
        if(NULL == pResultList->pCurEntry)
        {
            pEntry = csrLLPeekHead(&pResultList->List, LL_ACCESS_NOLOCK);
        }
        else
        {
            pEntry = csrLLNext(&pResultList->List, pResultList->pCurEntry, LL_ACCESS_NOLOCK);
        }
        if(pEntry)
        {
            pResult = GET_BASE_ADDR(pEntry, tCsrScanResult, Link);
            pRet = &pResult->Result;
        }
        pResultList->pCurEntry = pEntry;
        csrLLUnlock(&pResultList->List);
    }

    return pRet;
}


//This function moves the first BSS that matches the bssid to the head of the result
eHalStatus csrMoveBssToHeadFromBSSID(tpAniSirGlobal pMac, tCsrBssid *bssid, tScanResultHandle hScanResult)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tScanResultList *pResultList = (tScanResultList *)hScanResult;
    tCsrScanResult *pResult = NULL;
    tListElem *pEntry = NULL;

    if(pResultList && bssid)
    {
        csrLLLock(&pResultList->List);
        pEntry = csrLLPeekHead(&pResultList->List, LL_ACCESS_NOLOCK);
        while(pEntry)
        {
            pResult = GET_BASE_ADDR(pEntry, tCsrScanResult, Link);
            if (vos_mem_compare(bssid, pResult->Result.BssDescriptor.bssId, sizeof(tCsrBssid)))
            {
                status = eHAL_STATUS_SUCCESS;
                csrLLRemoveEntry(&pResultList->List, pEntry, LL_ACCESS_NOLOCK);
                csrLLInsertHead(&pResultList->List, pEntry, LL_ACCESS_NOLOCK);
                break;
            }
            pEntry = csrLLNext(&pResultList->List, pResultList->pCurEntry, LL_ACCESS_NOLOCK);
        }
        csrLLUnlock(&pResultList->List);
    }

    return (status);
}


//Remove the BSS if possible.
//Return -- TRUE == the BSS is remove. False == Fail to remove it
//This function is called when list lock is held. Be caution what functions it can call.
tANI_BOOLEAN csrScanAgeOutBss(tpAniSirGlobal pMac, tCsrScanResult *pResult)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE;
    tANI_U32 i;
    tCsrRoamSession *pSession;
    tANI_BOOLEAN isConnBssfound = eANI_BOOLEAN_FALSE;

    for( i = 0; i < CSR_ROAM_SESSION_MAX; i++ )
    {
        if( CSR_IS_SESSION_VALID( pMac, i ) )
        {
            pSession = CSR_GET_SESSION( pMac, i );
            //Not to remove the BSS we are connected to.
            if(csrIsConnStateConnectedInfra(pMac, i) && (NULL != pSession->pConnectBssDesc) &&
              (csrIsDuplicateBssDescription(pMac, &pResult->Result.BssDescriptor,
                                             pSession->pConnectBssDesc, NULL, FALSE))
              )
              {
                isConnBssfound = eANI_BOOLEAN_TRUE;
                break;
              }
        }
    }

    if( isConnBssfound )
    {
        /* Reset the counter so that aging out of connected BSS won't
           happen too soon */
        pResult->AgingCount = (tANI_S32)pMac->roam.configParam.agingCount;
        smsLog(pMac, LOGW,
           FL(" Connected BSS, Set Aging Count=%d for BSS "MAC_ADDRESS_STR" "),
           pResult->AgingCount,
           MAC_ADDR_ARRAY(pResult->Result.BssDescriptor.bssId));
        pResult->Result.BssDescriptor.nReceivedTime = (tANI_TIMESTAMP)palGetTickCount(pMac->hHdd);

        return (fRet);
    }
    else
    {
        smsLog(pMac, LOGW, "Aging out BSS "MAC_ADDRESS_STR" Channel %d",
               MAC_ADDR_ARRAY(pResult->Result.BssDescriptor.bssId),
               pResult->Result.BssDescriptor.channelId);
        //No need to hold the spin lock because caller should hold the lock for pMac->scan.scanResultList
        if(csrLLRemoveEntry(&pMac->scan.scanResultList, &pResult->Link,
                                 LL_ACCESS_NOLOCK))
        {
            if (csrIsMacAddressEqual(pMac,
                       (tCsrBssid *) pResult->Result.BssDescriptor.bssId,
                       (tCsrBssid *) pMac->scan.currentCountryBssid))
            {
                smsLog(pMac, LOGW, "Aging out 11d BSS "MAC_ADDRESS_STR,
                       MAC_ADDR_ARRAY(pResult->Result.BssDescriptor.bssId));
                pMac->scan.currentCountryRSSI = -128;
            }
            csrFreeScanResultEntry(pMac, pResult);
            fRet = eANI_BOOLEAN_TRUE;
        }
    }

    return (fRet);
}


eHalStatus csrScanAgeResults(tpAniSirGlobal pMac, tSmeGetScanChnRsp *pScanChnInfo)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tListElem *pEntry, *tmpEntry;
    tCsrScanResult *pResult;
    tLimScanChn *pChnInfo;
    tANI_U8 i;

    csrLLLock(&pMac->scan.scanResultList);
    for(i = 0; i < pScanChnInfo->numChn; i++)
    {
        pChnInfo = &pScanChnInfo->scanChn[i];
        pEntry = csrLLPeekHead(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
        while( pEntry )
        {
            tmpEntry = csrLLNext(&pMac->scan.scanResultList,
                                          pEntry, LL_ACCESS_NOLOCK);
            pResult = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
            if(pResult->Result.BssDescriptor.channelId == pChnInfo->channelId)
            {
                if(pResult->AgingCount <= 0)
                {
                    smsLog(pMac, LOGW, " age out due to ref count");
                    csrScanAgeOutBss(pMac, pResult);
                }
                else
                {
                    pResult->AgingCount--;
                    smsLog(pMac, LOGW,
                     FL("Decremented AgingCount=%d for BSS "MAC_ADDRESS_STR""),
                     pResult->AgingCount,
                     MAC_ADDR_ARRAY(pResult->Result.BssDescriptor.bssId));
                }
            }
            pEntry = tmpEntry;
        }
    }
    csrLLUnlock(&pMac->scan.scanResultList);

    return (status);
}


eHalStatus csrSendMBScanReq( tpAniSirGlobal pMac, tANI_U16 sessionId,
                    tCsrScanRequest *pScanReq, tScanReqParam *pScanReqParam )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSirSmeScanReq *pMsg;
    tANI_U16 msgLen;
    tANI_U8 bssid[VOS_MAC_ADDR_SIZE] = {0, 0, 0, 0, 0, 0};
    tSirScanType scanType = pScanReq->scanType;
    tANI_U32 minChnTime;    //in units of milliseconds
    tANI_U32 maxChnTime;    //in units of milliseconds
    tANI_U32 i;
    tANI_U8 selfMacAddr[VOS_MAC_ADDR_SIZE];
    tANI_U8 *pSelfMac = NULL;

    msgLen = (tANI_U16)(sizeof( tSirSmeScanReq ) - sizeof( pMsg->channelList.channelNumber ) +
                        ( sizeof( pMsg->channelList.channelNumber ) * pScanReq->ChannelInfo.numOfChannels )) +
                   ( pScanReq->uIEFieldLen ) ;

    pMsg = vos_mem_malloc(msgLen);
    if ( NULL == pMsg )
        status = eHAL_STATUS_FAILURE;
    else
        status = eHAL_STATUS_SUCCESS;
    if (HAL_STATUS_SUCCESS(status))
    {
        do
        {
            vos_mem_set(pMsg, msgLen, 0);
            pMsg->messageType = pal_cpu_to_be16((tANI_U16)eWNI_SME_SCAN_REQ);
            pMsg->length = pal_cpu_to_be16(msgLen);
            //ToDO: Fill in session info when we need to do scan base on session.
            if ((pMac->fScanOffload) && (sessionId != CSR_SESSION_ID_INVALID))
            {
                pMsg->sessionId = sessionId;
            }
            else
            {
                /* if sessionId == CSR_SESSION_ID_INVALID, then send the scan
                   request on first available session */
                pMsg->sessionId = 0;
            }
            if (pMsg->sessionId >= CSR_ROAM_SESSION_MAX)
                smsLog( pMac, LOGE, FL(" Invalid Sme Session ID = %d"), pMsg->sessionId );
            pMsg->transactionId = 0;
            pMsg->dot11mode = (tANI_U8) csrTranslateToWNICfgDot11Mode(pMac, csrFindBestPhyMode( pMac, pMac->roam.configParam.phyMode ));
            pMsg->bssType = pal_cpu_to_be32(csrTranslateBsstypeToMacType(pScanReq->BSSType));

            if ( CSR_IS_SESSION_VALID( pMac, sessionId ) )
            {
              pSelfMac = (tANI_U8 *)&pMac->roam.roamSession[sessionId].selfMacAddr;
            }
            else
            {
              // Since we don't have session for the scanning, we find a valid session. In case we fail to
              // do so, get the WNI_CFG_STA_ID
              for( i = 0; i < CSR_ROAM_SESSION_MAX; i++ )
              {
                if( CSR_IS_SESSION_VALID( pMac, i ) )
                {
                  pSelfMac = (tANI_U8 *)&pMac->roam.roamSession[i].selfMacAddr;
                  break;
                }
              }
              if( CSR_ROAM_SESSION_MAX == i )
              {
                tANI_U32 len = VOS_MAC_ADDR_SIZE;
                pSelfMac = selfMacAddr;
                status = ccmCfgGetStr( pMac, WNI_CFG_STA_ID, pSelfMac, &len );
                if( !HAL_STATUS_SUCCESS( status ) ||
                    ( len < VOS_MAC_ADDR_SIZE ) )
                {
                  smsLog( pMac, LOGE, FL(" Can not get self MAC address from CFG status = %d"), status );
                  //Force failed status
                  status = eHAL_STATUS_FAILURE;
                  break;
                }
              }
            }
            vos_mem_copy((tANI_U8 *)pMsg->selfMacAddr, pSelfMac, sizeof(tSirMacAddr));

            //sirCopyMacAddr
            vos_mem_copy((tANI_U8 *)pMsg->bssId, (tANI_U8 *)&pScanReq->bssid, sizeof(tSirMacAddr));
            if ( vos_mem_compare(pScanReq->bssid, bssid, sizeof(tCsrBssid)))
            {
                vos_mem_set(pMsg->bssId, sizeof(tSirMacAddr), 0xff);
            }
            else
            {
                vos_mem_copy(pMsg->bssId, pScanReq->bssid, VOS_MAC_ADDR_SIZE);
            }
            minChnTime = pScanReq->minChnTime;
            maxChnTime = pScanReq->maxChnTime;

            //Verify the scan type first, if the scan is active scan, we need to make sure we
            //are allowed to do so.
            /* if 11d is enabled & we don't see any beacon around, scan type falls
               back to passive. But in BT AMP STA mode we need to send out a
               directed probe*/
            if( (eSIR_PASSIVE_SCAN != scanType) && (eCSR_SCAN_P2P_DISCOVERY != pScanReq->requestType)
                && (eCSR_BSS_TYPE_WDS_STA != pScanReq->BSSType)
                && (eANI_BOOLEAN_FALSE == pMac->scan.fEnableBypass11d))
            {
                scanType = pMac->scan.curScanType;
                if(eSIR_PASSIVE_SCAN == pMac->scan.curScanType)
                {
                    if(minChnTime < pMac->roam.configParam.nPassiveMinChnTime)
                    {
                        minChnTime = pMac->roam.configParam.nPassiveMinChnTime;
                    }
                    if(maxChnTime < pMac->roam.configParam.nPassiveMaxChnTime)
                    {
                        maxChnTime = pMac->roam.configParam.nPassiveMaxChnTime;
                    }
                }
            }
            pMsg->scanType = pal_cpu_to_be32(scanType);

        pMsg->numSsid =
         (pScanReq->SSIDs.numOfSSIDs < SIR_SCAN_MAX_NUM_SSID) ?
         pScanReq->SSIDs.numOfSSIDs : SIR_SCAN_MAX_NUM_SSID;
            if((pScanReq->SSIDs.numOfSSIDs != 0) && ( eSIR_PASSIVE_SCAN != scanType ))
            {
                for (i = 0; i < pMsg->numSsid; i++)
                {
                    vos_mem_copy(&pMsg->ssId[i],
                                 &pScanReq->SSIDs.SSIDList[i].SSID, sizeof(tSirMacSSid));
                }
            }
            else
            {
                //Otherwise we scan all SSID and let the result filter later
                for (i = 0; i < SIR_SCAN_MAX_NUM_SSID; i++)
                {
                    pMsg->ssId[i].length = 0;
                }
            }

            pMsg->minChannelTime = pal_cpu_to_be32(minChnTime);
            pMsg->maxChannelTime = pal_cpu_to_be32(maxChnTime);
            pMsg->minChannelTimeBtc = pMac->roam.configParam.nActiveMinChnTimeBtc;
            pMsg->maxChannelTimeBtc = pMac->roam.configParam.nActiveMaxChnTimeBtc;
            //hidden SSID option
            pMsg->hiddenSsid = pScanReqParam->hiddenSsid;
            //rest time
            pMsg->restTime = pScanReq->restTime;
            pMsg->returnAfterFirstMatch = pScanReqParam->bReturnAfter1stMatch;
            // All the scan results caching will be done by Roaming
            // We do not want LIM to do any caching of scan results,
            // so delete the LIM cache on all scan requests
            pMsg->returnFreshResults = pScanReqParam->freshScan;
            //Always ask for unique result
            pMsg->returnUniqueResults = pScanReqParam->fUniqueResult;
            pMsg->channelList.numChannels = (tANI_U8)pScanReq->ChannelInfo.numOfChannels;
            if(pScanReq->ChannelInfo.numOfChannels)
            {
                //Assuming the channelNumber is tANI_U8 (1 byte)
                vos_mem_copy(pMsg->channelList.channelNumber,
                             pScanReq->ChannelInfo.ChannelList,
                             pScanReq->ChannelInfo.numOfChannels);
            }

            pMsg->uIEFieldLen = (tANI_U16) pScanReq->uIEFieldLen;
            pMsg->uIEFieldOffset = (tANI_U16)(sizeof( tSirSmeScanReq ) - sizeof( pMsg->channelList.channelNumber ) +
                  ( sizeof( pMsg->channelList.channelNumber ) * pScanReq->ChannelInfo.numOfChannels )) ;
            if(pScanReq->uIEFieldLen != 0)
            {
                vos_mem_copy((tANI_U8 *)pMsg+pMsg->uIEFieldOffset, pScanReq->pIEField,
                              pScanReq->uIEFieldLen);
            }
            pMsg->p2pSearch = pScanReq->p2pSearch;

            if (pScanReq->requestType == eCSR_SCAN_HO_BG_SCAN)
            {
                pMsg->backgroundScanMode = eSIR_ROAMING_SCAN;
            }

        }while(0);
        smsLog(pMac, LOG1, FL("domainIdCurrent %d scanType %d bssType %d requestType %d numChannels %d  "),
               pMac->scan.domainIdCurrent, pMsg->scanType, pMsg->bssType,
               pScanReq->requestType, pMsg->channelList.numChannels);

        for(i = 0; i < pMsg->channelList.numChannels; i++)
        {
            smsLog(pMac, LOG1, FL("channelNumber[%d]= %d"), i, pMsg->channelList.channelNumber[i]);
        }

        if(HAL_STATUS_SUCCESS(status))
        {
            status = palSendMBMessage(pMac->hHdd, pMsg);
        }
        else
        {
            smsLog( pMac, LOGE, FL(" failed to send down scan req with status = %d"), status );
            vos_mem_free(pMsg);
        }
    }//Success allocated memory
    else
    {
        smsLog( pMac, LOGE, FL(" memory allocation failure"));
    }
    if(!HAL_STATUS_SUCCESS(status))
    {
      smsLog( pMac, LOG1, FL("Failed: SId: %d FirstMatch = %d"
            " UniqueResult = %d freshScan = %d hiddenSsid = %d"),
            sessionId, pScanReqParam->bReturnAfter1stMatch,
            pScanReqParam->fUniqueResult, pScanReqParam->freshScan,
            pScanReqParam->hiddenSsid );
      smsLog( pMac, LOG1, FL("scanType = %u BSSType = %u numOfSSIDs = %d"
            " numOfChannels = %d requestType = %d p2pSearch = %d\n"),
            pScanReq->scanType, pScanReq->BSSType,
            pScanReq->SSIDs.numOfSSIDs,
            pScanReq->ChannelInfo.numOfChannels, pScanReq->requestType,
            pScanReq->p2pSearch );
     }

    return( status );
}

eHalStatus csrSendMBScanResultReq( tpAniSirGlobal pMac, tANI_U32 sessionId, tScanReqParam *pScanReqParam )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSirSmeScanReq *pMsg;
    tANI_U16 msgLen;

    msgLen = (tANI_U16)(sizeof( tSirSmeScanReq ));
    pMsg = vos_mem_malloc(msgLen);
    if ( NULL == pMsg )
       status = eHAL_STATUS_FAILURE;
    else
    {
        vos_mem_set(pMsg, msgLen, 0);
        pMsg->messageType = pal_cpu_to_be16((tANI_U16)eWNI_SME_SCAN_REQ);
        pMsg->length = pal_cpu_to_be16(msgLen);
        pMsg->sessionId = sessionId;
        pMsg->transactionId = 0;
        pMsg->returnFreshResults = pScanReqParam->freshScan;
        //Always ask for unique result
        pMsg->returnUniqueResults = pScanReqParam->fUniqueResult;
        pMsg->returnAfterFirstMatch = pScanReqParam->bReturnAfter1stMatch;
        status = palSendMBMessage(pMac->hHdd, pMsg);
        if (!HAL_STATUS_SUCCESS(status))
        {
            smsLog( pMac, LOGE, FL(" failed to send down scan req with status = %d\n"), status );
        }

    }

    return( status );
}



eHalStatus csrScanChannels( tpAniSirGlobal pMac, tSmeCmd *pCommand )
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tScanReqParam scanReq;

    do
    {
        /*
         * Don't delete cached results. Rome rssi based scan candidates
         * may land up in scan cache instead of LFR cache.
         * They will be deleted upon query.
         */
        scanReq.freshScan = SIR_BG_SCAN_RETURN_FRESH_RESULTS;
        scanReq.fUniqueResult = TRUE;
        scanReq.hiddenSsid = SIR_SCAN_NO_HIDDEN_SSID;
        if(eCsrScanForSsid == pCommand->u.scanCmd.reason)
        {
            scanReq.bReturnAfter1stMatch = CSR_SCAN_RETURN_AFTER_FIRST_MATCH;
        }
        else
        {
            // Basically do scan on all channels even for 11D 1st scan case.
            scanReq.bReturnAfter1stMatch = CSR_SCAN_RETURN_AFTER_ALL_CHANNELS;
        }
        if((eCsrScanBgScan == pCommand->u.scanCmd.reason)||
           (eCsrScanProbeBss == pCommand->u.scanCmd.reason))
        {
            scanReq.hiddenSsid = SIR_SCAN_HIDDEN_SSID_PE_DECISION;
        }

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
        {
            vos_log_scan_pkt_type *pScanLog = NULL;

            WLAN_VOS_DIAG_LOG_ALLOC(pScanLog, vos_log_scan_pkt_type, LOG_WLAN_SCAN_C);
            if(pScanLog)
            {
                if(eCsrScanBgScan == pCommand->u.scanCmd.reason ||
                    eCsrScanProbeBss == pCommand->u.scanCmd.reason)
                {
                    pScanLog->eventId = WLAN_SCAN_EVENT_HO_SCAN_REQ;
                }
                else
                {
                    if( (eSIR_PASSIVE_SCAN != pCommand->u.scanCmd.u.scanRequest.scanType) &&
                        (eSIR_PASSIVE_SCAN != pMac->scan.curScanType) )
                    {
                        pScanLog->eventId = WLAN_SCAN_EVENT_ACTIVE_SCAN_REQ;
                    }
                    else
                    {
                        pScanLog->eventId = WLAN_SCAN_EVENT_PASSIVE_SCAN_REQ;
                    }
                }
                pScanLog->minChnTime = (v_U8_t)pCommand->u.scanCmd.u.scanRequest.minChnTime;
                pScanLog->maxChnTime = (v_U8_t)pCommand->u.scanCmd.u.scanRequest.maxChnTime;
                pScanLog->numChannel = (v_U8_t)pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
                if(pScanLog->numChannel && (pScanLog->numChannel < VOS_LOG_MAX_NUM_CHANNEL))
                {
                    vos_mem_copy(pScanLog->channels,
                                 pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList,
                                 pScanLog->numChannel);
                }
                WLAN_VOS_DIAG_LOG_REPORT(pScanLog);
            }
        }
#endif //#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR

        csrClearVotesForCountryInfo(pMac);
        status = csrSendMBScanReq(pMac, pCommand->sessionId,
                                &pCommand->u.scanCmd.u.scanRequest, &scanReq);
    }while(0);

    return( status );
}


eHalStatus csrScanRetrieveResult(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tScanReqParam scanReq;

    do
    {
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
        if (eCsrScanGetLfrResult == pCommand->u.scanCmd.reason)
        {
            //to get the LFR candidates from PE cache
            scanReq.freshScan = SIR_BG_SCAN_RETURN_LFR_CACHED_RESULTS|SIR_BG_SCAN_PURGE_LFR_RESULTS;
            scanReq.fUniqueResult = TRUE;
            scanReq.bReturnAfter1stMatch = CSR_SCAN_RETURN_AFTER_ALL_CHANNELS;
        }
        else
#endif
        {
           //not a fresh scan
           scanReq.freshScan = SIR_BG_SCAN_PURGE_RESUTLS;
           scanReq.fUniqueResult = TRUE;
           scanReq.bReturnAfter1stMatch = CSR_SCAN_RETURN_AFTER_ALL_CHANNELS;
        }
        status = csrSendMBScanResultReq(pMac, pCommand->sessionId, &scanReq);
    }while(0);

    return (status);
}



eHalStatus csrProcessScanCommand( tpAniSirGlobal pMac, tSmeCmd *pCommand )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tCsrChannelInfo newChannelInfo = {0, NULL};
    int i, j;
    tANI_U8 *pChannel = NULL;
    tANI_U32 len = 0;

    // Transition to Scanning state...
    if (!pMac->fScanOffload)
    {
        for( i = 0; i < CSR_ROAM_SESSION_MAX; i++ )
        {
            pCommand->u.scanCmd.lastRoamState[i] =
                csrRoamStateChange( pMac, eCSR_ROAMING_STATE_SCANNING, i);
            smsLog( pMac, LOG3, "starting SCAN command from %d state...."
                    " reason is %d", pCommand->u.scanCmd.lastRoamState[i],
                    pCommand->u.scanCmd.reason );
        }
    }
    else
    {
        pCommand->u.scanCmd.lastRoamState[pCommand->sessionId] =
            csrRoamStateChange(pMac, eCSR_ROAMING_STATE_SCANNING,
                               pCommand->sessionId);
        smsLog( pMac, LOG3,
                "starting SCAN command from %d state.... reason is %d",
                pCommand->u.scanCmd.lastRoamState[pCommand->sessionId],
                pCommand->u.scanCmd.reason );
    }

    switch(pCommand->u.scanCmd.reason)
    {
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
    case eCsrScanGetLfrResult:
#endif
    case eCsrScanGetResult:
    case eCsrScanForCapsChange:     //For cap change, LIM already save BSS description
        status = csrScanRetrieveResult(pMac, pCommand);
        break;
    case eCsrScanSetBGScanParam:
        status = csrProcessSetBGScanParam(pMac, pCommand);
        break;
    case eCsrScanBGScanAbort:
        status = csrSetCfgBackgroundScanPeriod(pMac, 0);
        break;
    case eCsrScanBGScanEnable:
        status = csrSetCfgBackgroundScanPeriod(pMac, pMac->roam.configParam.bgScanInterval);
        break;
    case eCsrScanGetScanChnInfo:
        status = csrScanGetScanChannelInfo(pMac, pCommand->sessionId);
        break;
    case eCsrScanUserRequest:
        if(pMac->roam.configParam.fScanTwice)
        {
            //We scan 2.4 channel twice
            if(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels &&
               (NULL != pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList))
            {
                len = pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
                /* Allocate twice the channel */
                newChannelInfo.ChannelList = vos_mem_malloc(len * 2);
                pChannel = pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList;
            }
            else
            {
                //get the valid channel list to scan all.
                len = sizeof(pMac->roam.validChannelList);

                if (HAL_STATUS_SUCCESS(csrGetCfgValidChannels(pMac, (tANI_U8 *)pMac->roam.validChannelList, &len)))
                {
                    //allocate twice the channel
                    newChannelInfo.ChannelList = (tANI_U8 *)vos_mem_malloc(len * 2);
                    pChannel = pMac->roam.validChannelList;
                }
            }
            if(NULL == newChannelInfo.ChannelList)
            {
                newChannelInfo.numOfChannels = 0;
            }
            else
            {
                j = 0;
                for(i = 0; i < len; i++)
                {
                    newChannelInfo.ChannelList[j++] = pChannel[i];
                    if(CSR_MAX_24GHz_CHANNEL_NUMBER >= pChannel[i])
                    {
                        newChannelInfo.ChannelList[j++] = pChannel[i];
                    }
                }
                if(NULL != pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList)
                {
                    //pChannel points to the channellist from the command, free it.
                    vos_mem_free(pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList);
                    pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = NULL;
                }
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = j;
                pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = newChannelInfo.ChannelList;
            }
        } //if(pMac->roam.configParam.fScanTwice)

        status = csrScanChannels(pMac, pCommand);

        break;
    default:
        status = csrScanChannels(pMac, pCommand);
        break;
    }

    if(!HAL_STATUS_SUCCESS(status))
    {
        csrReleaseScanCommand(pMac, pCommand, eCSR_SCAN_FAILURE);
    }

    return (status);
}


eHalStatus csrScanSetBGScanparams(tpAniSirGlobal pMac, tCsrBGScanRequest *pScanReq)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pCommand = NULL;

    if(pScanReq)
    {
        do
        {
            pCommand = csrGetCommandBuffer(pMac);
            if(!pCommand)
            {
                status = eHAL_STATUS_RESOURCES;
                break;
            }
            vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
            pCommand->command = eSmeCommandScan;
            pCommand->u.scanCmd.reason = eCsrScanSetBGScanParam;
            pCommand->u.scanCmd.callback = NULL;
            pCommand->u.scanCmd.pContext = NULL;
            vos_mem_copy(&pCommand->u.scanCmd.u.bgScanRequest, pScanReq, sizeof(tCsrBGScanRequest));
            //we have to do the follow
            if(pScanReq->ChannelInfo.numOfChannels == 0)
            {
                pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList = NULL;
            }
            else
            {
                pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList
                                 = vos_mem_malloc(pScanReq->ChannelInfo.numOfChannels);
                if ( NULL != pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList )
                {
                    vos_mem_copy(pCommand->u.scanCmd.u.bgScanRequest.ChannelInfo.ChannelList,
                                 pScanReq->ChannelInfo.ChannelList,
                                 pScanReq->ChannelInfo.numOfChannels);
                }
                else
                {
                    smsLog(pMac, LOGE, FL("ran out of memory"));
                    csrReleaseCommandScan(pMac, pCommand);
                    return eHAL_STATUS_FAILURE;
                }
            }

            //scan req for SSID
            if(pScanReq->SSID.length)
            {
               vos_mem_copy(pCommand->u.scanCmd.u.bgScanRequest.SSID.ssId,
                            pScanReq->SSID.ssId, pScanReq->SSID.length);
               pCommand->u.scanCmd.u.bgScanRequest.SSID.length = pScanReq->SSID.length;

            }
            pCommand->u.scanCmd.u.bgScanRequest.maxChnTime= pScanReq->maxChnTime;
            pCommand->u.scanCmd.u.bgScanRequest.minChnTime = pScanReq->minChnTime;
            pCommand->u.scanCmd.u.bgScanRequest.scanInterval = pScanReq->scanInterval;


            status = csrQueueSmeCommand(pMac, pCommand, eANI_BOOLEAN_FALSE);
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                csrReleaseCommandScan( pMac, pCommand );
                break;
            }
        }while(0);
    }

    return (status);
}

eHalStatus csrScanBGScanAbort( tpAniSirGlobal pMac )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pCommand = NULL;

    do
    {
        pCommand = csrGetCommandBuffer(pMac);
        if(!pCommand)
        {
            status = eHAL_STATUS_RESOURCES;
            break;
        }
        vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
        pCommand->command = eSmeCommandScan;
        pCommand->u.scanCmd.reason = eCsrScanBGScanAbort;
        pCommand->u.scanCmd.callback = NULL;
        pCommand->u.scanCmd.pContext = NULL;
        status = csrQueueSmeCommand(pMac, pCommand, eANI_BOOLEAN_FALSE);
        if( !HAL_STATUS_SUCCESS( status ) )
        {
            smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
            csrReleaseCommandScan( pMac, pCommand );
            break;
        }
    }while(0);

    return (status);
}


//This will enable the background scan with the non-zero interval
eHalStatus csrScanBGScanEnable(tpAniSirGlobal pMac)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSmeCmd *pCommand = NULL;

    if(pMac->roam.configParam.bgScanInterval)
    {
        do
        {
            pCommand = csrGetCommandBuffer(pMac);
            if(!pCommand)
            {
                status = eHAL_STATUS_RESOURCES;
                break;
            }
            vos_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
            pCommand->command = eSmeCommandScan;
            pCommand->u.scanCmd.reason = eCsrScanBGScanEnable;
            pCommand->u.scanCmd.callback = NULL;
            pCommand->u.scanCmd.pContext = NULL;
            status = csrQueueSmeCommand(pMac, pCommand, eANI_BOOLEAN_FALSE);
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                csrReleaseCommandScan( pMac, pCommand );
                break;
            }
        }while(0);
        //BG scan results are reported automatically by PE to SME once the scan is done.
        //No need to fetch the results explicitly.
        //csrScanStartGetResultTimer(pMac);
        csrScanStartResultAgingTimer(pMac);
    }
    else
    {
        //We don't have BG scan so stop the aging timer
        csrScanStopResultAgingTimer(pMac);
        smsLog(pMac, LOGE, FL("cannot continue because the bgscan interval is 0"));
        status = eHAL_STATUS_INVALID_PARAMETER;
    }

    return (status);
}


eHalStatus csrScanCopyRequest(tpAniSirGlobal pMac, tCsrScanRequest *pDstReq, tCsrScanRequest *pSrcReq)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tANI_U32 len = sizeof(pMac->roam.validChannelList);
    tANI_U32 index = 0;
    tANI_U32 new_index = 0;
    eNVChannelEnabledType NVchannel_state;
    uint8_t skip_dfs_chnl = pMac->roam.configParam.initial_scan_no_dfs_chnl;

    do
    {
        status = csrScanFreeRequest(pMac, pDstReq);
        if(HAL_STATUS_SUCCESS(status))
        {
            vos_mem_copy(pDstReq, pSrcReq, sizeof(tCsrScanRequest));
            /* Re-initialize the pointers to NULL since we did a copy */
            pDstReq->pIEField = NULL;
            pDstReq->ChannelInfo.ChannelList = NULL;
            pDstReq->SSIDs.SSIDList = NULL;

            if(pSrcReq->uIEFieldLen == 0)
            {
                pDstReq->pIEField = NULL;
            }
            else
            {
                pDstReq->pIEField = vos_mem_malloc(pSrcReq->uIEFieldLen);
                if ( NULL == pDstReq->pIEField )
                {
                   status = eHAL_STATUS_FAILURE;
                   smsLog(pMac, LOGE, FL("No memory for scanning IE fields"));
                   break;
                }
                else
                {
                   status = eHAL_STATUS_SUCCESS;
                   vos_mem_copy(pDstReq->pIEField, pSrcReq->pIEField,
                               pSrcReq->uIEFieldLen);
                   pDstReq->uIEFieldLen = pSrcReq->uIEFieldLen;
                }
            }//Allocate memory for IE field
            {
                if(pSrcReq->ChannelInfo.numOfChannels == 0)
                {
                    pDstReq->ChannelInfo.ChannelList = NULL;
                        pDstReq->ChannelInfo.numOfChannels = 0;
                }
                else
                {
                    pDstReq->ChannelInfo.ChannelList = vos_mem_malloc(
                                         pSrcReq->ChannelInfo.numOfChannels
                                         * sizeof(*pDstReq->ChannelInfo.ChannelList));
                    if ( NULL == pDstReq->ChannelInfo.ChannelList )
                    {  status = eHAL_STATUS_FAILURE;
                       pDstReq->ChannelInfo.numOfChannels = 0;
                       smsLog(pMac, LOGE, FL("No memory for scanning Channel"
                             " List"));
                       break;
                    }

                    if((pSrcReq->scanType == eSIR_PASSIVE_SCAN) && (pSrcReq->requestType == eCSR_SCAN_REQUEST_11D_SCAN))
                    {
                       for ( index = 0; index < pSrcReq->ChannelInfo.numOfChannels ; index++ )
                       {
                          NVchannel_state = vos_nv_getChannelEnabledState(
                                  pSrcReq->ChannelInfo.ChannelList[index]);
                          if (pSrcReq->ChannelInfo.ChannelList[index] < MIN_11P_CHANNEL &&
                              ((NV_CHANNEL_ENABLE == NVchannel_state) ||
                                  ((NV_CHANNEL_DFS == NVchannel_state) &&
                                    !skip_dfs_chnl)))
                          {
                             pDstReq->ChannelInfo.ChannelList[new_index] =
                                 pSrcReq->ChannelInfo.ChannelList[index];
                             new_index++;
                          }
                       }
                       pDstReq->ChannelInfo.numOfChannels = new_index;
                    }
                    else if(HAL_STATUS_SUCCESS(csrGetCfgValidChannels(pMac, pMac->roam.validChannelList, &len)))
                    {
                        new_index = 0;
                        pMac->roam.numValidChannels = len;
                        for ( index = 0; index < pSrcReq->ChannelInfo.numOfChannels ; index++ )
                        {
                            /* Allow scan on valid channels only.
                             * If it is p2p scan and valid channel list doesnt contain
                             * social channels, enforce scan on social channels because
                             * that is the only way to find p2p peers.
                             * This can happen only if band is set to 5Ghz mode.
                             */
                            if(pSrcReq->ChannelInfo.ChannelList[index] < MIN_11P_CHANNEL &&
                               ((csrRoamIsValidChannel(pMac, pSrcReq->ChannelInfo.ChannelList[index])) ||
                               ((eCSR_SCAN_P2P_DISCOVERY == pSrcReq->requestType) &&
                                CSR_IS_SOCIAL_CHANNEL(pSrcReq->ChannelInfo.ChannelList[index]))))
                            {
                                if( ((pSrcReq->skipDfsChnlInP2pSearch ||
                                    skip_dfs_chnl) &&
                                    (NV_CHANNEL_DFS == vos_nv_getChannelEnabledState(pSrcReq->ChannelInfo.ChannelList[index])) )
#ifdef FEATURE_WLAN_LFR
                                     /*
                                      * If LFR is requesting a contiguous scan
                                      * (i.e. numOfChannels > 1), then ignore
                                      * DFS channels.
                                      * TODO: vos_nv_getChannelEnabledState is returning
                                      * 120, 124 and 128 as non-DFS channels. Hence, the
                                      * use of direct check for channels below.
                                      */
                                     || ((eCSR_SCAN_HO_BG_SCAN == pSrcReq->requestType) &&
                                         (pSrcReq->ChannelInfo.numOfChannels > 1) &&
                                         (CSR_IS_CHANNEL_DFS(pSrcReq->ChannelInfo.ChannelList[index])) &&
                                         !pMac->roam.configParam.allowDFSChannelRoam )
#endif
                                  )
                                {
#ifdef FEATURE_WLAN_LFR
                                    smsLog(pMac, LOG2,
                                          FL(" reqType=%d, numOfChannels=%d,"
                                           " ignoring DFS channel %d"),
                                          pSrcReq->requestType,
                                          pSrcReq->ChannelInfo.numOfChannels,
                                          pSrcReq->ChannelInfo.ChannelList[index]);
#endif
                                    continue;
                                }

                                pDstReq->ChannelInfo.ChannelList[new_index] =
                                    pSrcReq->ChannelInfo.ChannelList[index];
                                new_index++;
                            }
                        }
                        pDstReq->ChannelInfo.numOfChannels = new_index;
#ifdef FEATURE_WLAN_LFR
                        if ((eCSR_SCAN_HO_BG_SCAN == pSrcReq->requestType) &&
                                (0 == pDstReq->ChannelInfo.numOfChannels))
                        {
                            /*
                             * No valid channels found in the request.
                             * Only perform scan on the channels passed
                             * pSrcReq if it is a eCSR_SCAN_HO_BG_SCAN.
                             * Passing 0 to LIM will trigger a scan on
                             * all valid channels which is not desirable.
                             */
                           smsLog(pMac, LOGE, FL(" no valid channels found"
                                 " (request=%d)"), pSrcReq->requestType);
                           for ( index = 0; index < pSrcReq->ChannelInfo.numOfChannels ; index++ )
                           {
                               smsLog(pMac, LOGE, FL("pSrcReq index=%d"
                                     " channel=%d"), index,
                                     pSrcReq->ChannelInfo.ChannelList[index]);
                           }
                           status = eHAL_STATUS_FAILURE;
                           break;
                    }
#endif
                    }
                    else
                    {
                        smsLog(pMac, LOGE, FL("Couldn't get the valid Channel"
                                  " List, keeping requester's list"));
                        new_index = 0;
                        for ( index = 0;
                                index < pSrcReq->ChannelInfo.numOfChannels;
                                index++ )
                        {
                            if (pSrcReq->ChannelInfo.ChannelList[index] <
                                  MIN_11P_CHANNEL)
                            {
                                pDstReq->ChannelInfo.ChannelList[new_index] =
                                      pSrcReq->ChannelInfo.ChannelList[index];
                                new_index++;
                            }
                        }
                        pDstReq->ChannelInfo.numOfChannels = new_index;
                    }
                }//Allocate memory for Channel List
            }
            if(pSrcReq->SSIDs.numOfSSIDs == 0)
            {
                pDstReq->SSIDs.numOfSSIDs = 0;
                pDstReq->SSIDs.SSIDList = NULL;
            }
            else
            {
                pDstReq->SSIDs.SSIDList = vos_mem_malloc(
                              pSrcReq->SSIDs.numOfSSIDs * sizeof(*pDstReq->SSIDs.SSIDList));
                if ( NULL == pDstReq->SSIDs.SSIDList )
                        status = eHAL_STATUS_FAILURE;
                else
                        status = eHAL_STATUS_SUCCESS;
                if (HAL_STATUS_SUCCESS(status))
                {
                    pDstReq->SSIDs.numOfSSIDs = pSrcReq->SSIDs.numOfSSIDs;
                    vos_mem_copy(pDstReq->SSIDs.SSIDList,
                                 pSrcReq->SSIDs.SSIDList,
                                 pSrcReq->SSIDs.numOfSSIDs * sizeof(*pDstReq->SSIDs.SSIDList));
                }
                else
                {
                    pDstReq->SSIDs.numOfSSIDs = 0;
                    smsLog(pMac, LOGE, FL("No memory for scanning SSID List"));
                    break;
                }
            }//Allocate memory for SSID List
            pDstReq->p2pSearch = pSrcReq->p2pSearch;
            pDstReq->skipDfsChnlInP2pSearch = pSrcReq->skipDfsChnlInP2pSearch;

        }
    }while(0);

    if(!HAL_STATUS_SUCCESS(status))
    {
        csrScanFreeRequest(pMac, pDstReq);
    }

    return (status);
}


eHalStatus csrScanFreeRequest(tpAniSirGlobal pMac, tCsrScanRequest *pReq)
{

    if(pReq->ChannelInfo.ChannelList)
    {
        vos_mem_free(pReq->ChannelInfo.ChannelList);
        pReq->ChannelInfo.ChannelList = NULL;
    }
    pReq->ChannelInfo.numOfChannels = 0;
    if(pReq->pIEField)
    {
        vos_mem_free(pReq->pIEField);
        pReq->pIEField = NULL;
    }
    pReq->uIEFieldLen = 0;
    if(pReq->SSIDs.SSIDList)
    {
        vos_mem_free(pReq->SSIDs.SSIDList);
        pReq->SSIDs.SSIDList = NULL;
    }
    pReq->SSIDs.numOfSSIDs = 0;

    return eHAL_STATUS_SUCCESS;
}


void csrScanCallCallback(tpAniSirGlobal pMac, tSmeCmd *pCommand, eCsrScanStatus scanStatus)
{
    if(pCommand->u.scanCmd.callback)
    {
        pCommand->u.scanCmd.callback(pMac, pCommand->u.scanCmd.pContext,
                                     pCommand->sessionId,
                                     pCommand->u.scanCmd.scanID, scanStatus);
    } else {
        smsLog( pMac, LOG2, "%s:%d - Callback NULL!!!", __func__, __LINE__);
    }
}


void csrScanStopTimers(tpAniSirGlobal pMac)
{
    csrScanStopResultAgingTimer(pMac);
    csrScanStopIdleScanTimer(pMac);
    csrScanStopGetResultTimer(pMac);
    if(0 != pMac->scan.scanResultCfgAgingTime )
    {
        csrScanStopResultCfgAgingTimer(pMac);
    }

}


eHalStatus csrScanStartGetResultTimer(tpAniSirGlobal pMac)
{
    eHalStatus status;

    if(pMac->scan.fScanEnable)
    {
        status = vos_timer_start(&pMac->scan.hTimerGetResult, CSR_SCAN_GET_RESULT_INTERVAL/VOS_TIMER_TO_MS_UNIT);
    }
    else
    {
        status = eHAL_STATUS_FAILURE;
    }

    return (status);
}


eHalStatus csrScanStopGetResultTimer(tpAniSirGlobal pMac)
{
    return (vos_timer_stop(&pMac->scan.hTimerGetResult));
}


void csrScanGetResultTimerHandler(void *pv)
{
    tpAniSirGlobal pMac = PMAC_STRUCT( pv );

    csrScanRequestResult(pMac);

    vos_timer_start(&pMac->scan.hTimerGetResult, CSR_SCAN_GET_RESULT_INTERVAL/VOS_TIMER_TO_MS_UNIT);
}

#ifdef WLAN_AP_STA_CONCURRENCY
static void csrStaApConcTimerHandler(void *pv)
{
    tpAniSirGlobal pMac = PMAC_STRUCT( pv );
    tListElem *pEntry;
    tSmeCmd *pScanCmd;
    tANI_U32 sessionId = CSR_SESSION_ID_INVALID;

    csrLLLock(&pMac->scan.scanCmdPendingList);

    if ( NULL != ( pEntry = csrLLPeekHead( &pMac->scan.scanCmdPendingList, LL_ACCESS_NOLOCK) ) )
    {
        tCsrScanRequest scanReq;
        tSmeCmd *pSendScanCmd = NULL;
        tANI_U8 numChn = 0;
        tANI_U8 nNumChanCombinedConc = 0;
        tANI_U8 i, j;
        tCsrChannelInfo *pChnInfo = &scanReq.ChannelInfo;
        tANI_U8    channelToScan[WNI_CFG_VALID_CHANNEL_LIST_LEN];
        eHalStatus status;

        pScanCmd = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        numChn = pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
        sessionId = pScanCmd->sessionId;

        /* if any session is connected and the number of channels to scan is
         * greater than 1 then split the scan into multiple scan operations
         * on each individual channel else continue to perform scan on all
         * specified channels */

        /* split scan if number of channels to scan is greater than 1 and
         * any one of the following:
         * - STA session is connected and the scan is not a P2P search
         * - any P2P session is connected
         * Do not split scans if no concurrent infra connections are
         * active and if the scan is a BG scan triggered by LFR (OR)
         * any scan if LFR is in the middle of a BG scan. Splitting
         * the scan is delaying the time it takes for LFR to find
         * candidates and resulting in disconnects.
         */

        if((csrIsStaSessionConnected(pMac) &&
           !csrIsP2pSessionConnected(pMac)))
        {
           nNumChanCombinedConc = pMac->roam.configParam.nNumStaChanCombinedConc;
        }
        else if(csrIsP2pSessionConnected(pMac))
        {
           nNumChanCombinedConc = pMac->roam.configParam.nNumP2PChanCombinedConc;
        }

        if ( (numChn > nNumChanCombinedConc) &&
                ((csrIsStaSessionConnected(pMac) &&
#ifdef FEATURE_WLAN_LFR
                  (csrIsConcurrentInfraConnected(pMac) ||
                   ((pScanCmd->u.scanCmd.reason != eCsrScanBgScan) &&
                    (pMac->roam.neighborRoamInfo[sessionId].neighborRoamState !=
                     eCSR_NEIGHBOR_ROAM_STATE_CFG_CHAN_LIST_SCAN))) &&
#endif
                  (pScanCmd->u.scanCmd.u.scanRequest.p2pSearch != 1)) ||
              (csrIsP2pSessionConnected(pMac))))
        {
             vos_mem_set(&scanReq, sizeof(tCsrScanRequest), 0);

             pSendScanCmd = csrGetCommandBuffer(pMac); //optimize this to use 2 command buffer only
             if (!pSendScanCmd)
             {
                 smsLog( pMac, LOGE, FL(" Failed to get Queue command buffer") );
                 csrLLUnlock(&pMac->scan.scanCmdPendingList);
                 return;
             }
             pSendScanCmd->command = pScanCmd->command;
             pSendScanCmd->sessionId = pScanCmd->sessionId;
             pSendScanCmd->u.scanCmd.callback = NULL;
             pSendScanCmd->u.scanCmd.pContext = pScanCmd->u.scanCmd.pContext;
             pSendScanCmd->u.scanCmd.reason = pScanCmd->u.scanCmd.reason;
             pSendScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID++; //let it wrap around

             /* First copy all the parameters to local variable of scan request */
             csrScanCopyRequest(pMac, &scanReq, &pScanCmd->u.scanCmd.u.scanRequest);

             /* Now modify the elements of local var scan request required to be modified for split scan */
             if(scanReq.ChannelInfo.ChannelList != NULL)
             {
                 vos_mem_free(scanReq.ChannelInfo.ChannelList);
                 scanReq.ChannelInfo.ChannelList = NULL;
             }

             pChnInfo->numOfChannels = nNumChanCombinedConc;
             vos_mem_copy(&channelToScan[0],
                          &pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[0],
                          pChnInfo->numOfChannels * sizeof(tANI_U8));//just send one channel
             pChnInfo->ChannelList = &channelToScan[0];

             for (i = 0, j = nNumChanCombinedConc; i < (numChn-nNumChanCombinedConc); i++, j++)
             {
                 pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[i] =
                 pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[j]; //Move all the channels one step
             }

             pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = numChn - nNumChanCombinedConc; //reduce outstanding # of channels to be scanned

             scanReq.BSSType = eCSR_BSS_TYPE_ANY;
             //Modify callers parameters in case of concurrency
             scanReq.scanType = eSIR_ACTIVE_SCAN;
             //Use concurrency values for min/maxChnTime.
             csrSetDefaultScanTiming(pMac, scanReq.scanType, &scanReq);

             status = csrScanCopyRequest(pMac, &pSendScanCmd->u.scanCmd.u.scanRequest, &scanReq);
             if(!HAL_STATUS_SUCCESS(status))
             {
                 smsLog( pMac, LOGE, FL(" Failed to get copy csrScanRequest = %d"), status );
                 csrLLUnlock(&pMac->scan.scanCmdPendingList);
                 return;
             }
             /* Clean the local scan variable */
             scanReq.ChannelInfo.ChannelList = NULL;
             scanReq.ChannelInfo.numOfChannels = 0;
             csrScanFreeRequest(pMac, &scanReq);
        }
        else
        {
             /* no active connected session present or numChn == 1
              * scan all remaining channels */
             pSendScanCmd = pScanCmd;
             //remove this command from pending list
             if (csrLLRemoveHead( &pMac->scan.scanCmdPendingList, LL_ACCESS_NOLOCK) == NULL)
             { //In case between PeekHead and here, the entry got removed by another thread.
                 smsLog( pMac, LOGE, FL(" Failed to remove entry from scanCmdPendingList"));
             }

        }
        csrQueueSmeCommand(pMac, pSendScanCmd, eANI_BOOLEAN_FALSE);

    }

    csrLLUnlock(&pMac->scan.scanCmdPendingList);

}
#endif

eHalStatus csrScanStartResultAgingTimer(tpAniSirGlobal pMac)
{
    eHalStatus status = eHAL_STATUS_FAILURE;

    if(pMac->scan.fScanEnable)
    {
        status = vos_timer_start(&pMac->scan.hTimerResultAging, CSR_SCAN_RESULT_AGING_INTERVAL/VOS_TIMER_TO_MS_UNIT);
    }
    return (status);
}

eHalStatus csrScanStartResultCfgAgingTimer(tpAniSirGlobal pMac)
{
    eHalStatus status = eHAL_STATUS_FAILURE;

    if(pMac->scan.fScanEnable)
    {
        status = vos_timer_start(&pMac->scan.hTimerResultCfgAging, CSR_SCAN_RESULT_CFG_AGING_INTERVAL/VOS_TIMER_TO_MS_UNIT);
    }
    return (status);
}

eHalStatus csrScanStopResultAgingTimer(tpAniSirGlobal pMac)
{
    return (vos_timer_stop(&pMac->scan.hTimerResultAging));
}

eHalStatus csrScanStopResultCfgAgingTimer(tpAniSirGlobal pMac)
{
    return (vos_timer_stop(&pMac->scan.hTimerResultCfgAging));
}

//This function returns the maximum time a BSS is allowed in the scan result.
//The time varies base on connection and power saving factors.
//Not connected, No PS
//Not connected, with PS
//Connected w/o traffic, No PS
//Connected w/o traffic, with PS
//Connected w/ traffic, no PS -- Not supported
//Connected w/ traffic, with PS -- Not supported
//the return unit is in seconds.
tANI_U32 csrScanGetAgeOutTime(tpAniSirGlobal pMac)
{
    tANI_U32 nRet;

    if(pMac->scan.nAgingCountDown)
    {
        //Calculate what should be the timeout value for this
        nRet = pMac->scan.nLastAgeTimeOut * pMac->scan.nAgingCountDown;
        pMac->scan.nAgingCountDown--;
    }
    else
    {
        if( csrIsAllSessionDisconnected( pMac ) )
        {
            if(pmcIsPowerSaveEnabled(pMac, ePMC_IDLE_MODE_POWER_SAVE))
            {
                nRet = pMac->roam.configParam.scanAgeTimeNCPS;
            }
            else
            {
                nRet = pMac->roam.configParam.scanAgeTimeNCNPS;
            }
        }
        else
        {
            if(pmcIsPowerSaveEnabled(pMac, ePMC_BEACON_MODE_POWER_SAVE))
            {
                nRet = pMac->roam.configParam.scanAgeTimeCPS;
            }
            else
            {
                nRet = pMac->roam.configParam.scanAgeTimeCNPS;
            }
        }
        //If state-change causing aging time out change, we want to delay it somewhat to avoid
        //unnecessary removal of BSS. This is mostly due to transition from connect to disconnect.
        if(pMac->scan.nLastAgeTimeOut > nRet)
        {
            if(nRet)
            {
                pMac->scan.nAgingCountDown = (pMac->scan.nLastAgeTimeOut / nRet);
            }
            pMac->scan.nLastAgeTimeOut = nRet;
            nRet *= pMac->scan.nAgingCountDown;
        }
        else
        {
            pMac->scan.nLastAgeTimeOut = nRet;
        }
    }

    return (nRet);
}


void csrScanResultAgingTimerHandler(void *pv)
{
    tpAniSirGlobal pMac = PMAC_STRUCT( pv );
    tANI_BOOLEAN fDisconnected = csrIsAllSessionDisconnected(pMac);

    //no scan, no aging
    if (pMac->scan.fScanEnable &&
        (((eANI_BOOLEAN_FALSE == fDisconnected) && pMac->roam.configParam.bgScanInterval)
        || (fDisconnected && (pMac->scan.fCancelIdleScan == eANI_BOOLEAN_FALSE))
        || (pMac->fScanOffload))
        )
    {
        tListElem *pEntry, *tmpEntry;
        tCsrScanResult *pResult;
        tANI_TIMESTAMP ageOutTime = (tANI_TIMESTAMP)(csrScanGetAgeOutTime(pMac) * PAL_TICKS_PER_SECOND); //turn it into 10ms units
        tANI_TIMESTAMP curTime = (tANI_TIMESTAMP)palGetTickCount(pMac->hHdd);

        csrLLLock(&pMac->scan.scanResultList);
        pEntry = csrLLPeekHead( &pMac->scan.scanResultList, LL_ACCESS_NOLOCK );
        while( pEntry )
        {
            tmpEntry = csrLLNext(&pMac->scan.scanResultList, pEntry, LL_ACCESS_NOLOCK);
            pResult = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
            if((curTime - pResult->Result.BssDescriptor.nReceivedTime) > ageOutTime)
            {
                smsLog(pMac, LOGW, " age out due to time out");
                csrScanAgeOutBss(pMac, pResult);
            }
            pEntry = tmpEntry;
        }
        csrLLUnlock(&pMac->scan.scanResultList);
    }
    vos_timer_start(&pMac->scan.hTimerResultAging, CSR_SCAN_RESULT_AGING_INTERVAL/VOS_TIMER_TO_MS_UNIT);
}

static void csrScanResultCfgAgingTimerHandler(void *pv)
{
    tpAniSirGlobal pMac = PMAC_STRUCT( pv );
    tListElem *pEntry, *tmpEntry;
    tCsrScanResult *pResult;
    tANI_TIMESTAMP ageOutTime =  pMac->scan.scanResultCfgAgingTime * PAL_TICKS_PER_SECOND;
    tANI_TIMESTAMP curTime = (tANI_TIMESTAMP)palGetTickCount(pMac->hHdd);

    csrLLLock(&pMac->scan.scanResultList);
    pEntry = csrLLPeekHead( &pMac->scan.scanResultList, LL_ACCESS_NOLOCK );
    while( pEntry )
    {
        tmpEntry = csrLLNext(&pMac->scan.scanResultList, pEntry,
                                      LL_ACCESS_NOLOCK);
        pResult = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
        if((curTime - pResult->Result.BssDescriptor.nReceivedTime) > ageOutTime)
        {
            smsLog(pMac, LOGW, " age out due to time out");
            csrScanAgeOutBss(pMac, pResult);
        }
        pEntry = tmpEntry;
    }
    csrLLUnlock(&pMac->scan.scanResultList);
    vos_timer_start(&pMac->scan.hTimerResultCfgAging, CSR_SCAN_RESULT_CFG_AGING_INTERVAL/VOS_TIMER_TO_MS_UNIT);
}

eHalStatus csrScanStartIdleScanTimer(tpAniSirGlobal pMac, tANI_U32 interval)
{
    eHalStatus status;

    smsLog(pMac, LOG1, " csrScanStartIdleScanTimer");
    if((pMac->scan.fScanEnable) && (eANI_BOOLEAN_FALSE == pMac->scan.fCancelIdleScan) && interval)
    {
        pMac->scan.nIdleScanTimeGap += interval;
        vos_timer_stop(&pMac->scan.hTimerIdleScan);
        status = vos_timer_start(&pMac->scan.hTimerIdleScan, interval/VOS_TIMER_TO_MS_UNIT);
        if( !HAL_STATUS_SUCCESS(status) )
        {
            smsLog(pMac, LOGE, "  Fail to start Idle scan timer. status = %d interval = %d", status, interval);
            //This should not happen but set the flag to restart when ready
            pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;
        }
    }
    else
    {
        if( pMac->scan.fScanEnable && (eANI_BOOLEAN_FALSE == pMac->scan.fCancelIdleScan) )
        {
            pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;
        }
        status = eHAL_STATUS_FAILURE;
    }

    return (status);
}


eHalStatus csrScanStopIdleScanTimer(tpAniSirGlobal pMac)
{
    return (vos_timer_stop(&pMac->scan.hTimerIdleScan));
}


//Stop CSR from asking for IMPS, This function doesn't disable IMPS from CSR
void csrScanSuspendIMPS( tpAniSirGlobal pMac )
{
    csrScanCancelIdleScan(pMac);
}


//Start CSR from asking for IMPS. This function doesn't trigger CSR to request entering IMPS
//because IMPS maybe disabled.
void csrScanResumeIMPS( tpAniSirGlobal pMac )
{
    csrScanStartIdleScan( pMac );
}


void csrScanIMPSCallback(void *callbackContext, eHalStatus status)
{
    tpAniSirGlobal pMac = PMAC_STRUCT( callbackContext );

    if(eANI_BOOLEAN_FALSE == pMac->scan.fCancelIdleScan)
    {
        if(pMac->roam.configParam.IsIdleScanEnabled)
        {
            if(HAL_STATUS_SUCCESS(status))
            {
                if(csrIsAllSessionDisconnected(pMac) && !csrIsRoamCommandWaiting(pMac))
                {
                    smsLog(pMac, LOGW, FL("starts idle mode full scan"));
                    csrScanAllChannels(pMac, eCSR_SCAN_IDLE_MODE_SCAN);
                }
                else
                {
                    smsLog(pMac, LOGW, FL("cannot start idle mode full scan"));
                    //even though we are in timer handle, calling stop timer will make sure the timer
                    //doesn't get to restart.
                    csrScanStopIdleScanTimer(pMac);
                }
            }
            else
            {
                smsLog(pMac, LOGE, FL("sees not success status (%d)"), status);
            }
        }
        else
        {//we might need another flag to check if CSR needs to request imps at all

            tANI_U32 nTime = 0;

            pMac->scan.fRestartIdleScan = eANI_BOOLEAN_FALSE;
            if(!HAL_STATUS_SUCCESS(csrScanTriggerIdleScan(pMac, &nTime)))
            {
                csrScanStartIdleScanTimer(pMac, nTime);
            }
        }
    }
}


/*
 * Param: pTimeInterval -- Caller allocated memory in return, if failed,
 * to specify the next time interval for idle scan timer interval
 * Return: Not success -- meaning it cannot start IMPS, caller needs to
 * start a timer for idle scan
 */
eHalStatus csrScanTriggerIdleScan(tpAniSirGlobal pMac, tANI_U32 *pTimeInterval)
{
    eHalStatus status = eHAL_STATUS_CSR_WRONG_STATE;

    //Do not trigger IMPS in case of concurrency
    if (vos_concurrent_open_sessions_running() &&
        csrIsAnySessionInConnectState(pMac))
    {
        smsLog( pMac, LOG1, FL("Cannot request IMPS because Concurrent Sessions Running") );
        return (status);
    }

    if(pTimeInterval)
    {
        *pTimeInterval = 0;
    }

    smsLog(pMac, LOG3, FL("called"));
    if( smeCommandPending( pMac ) )
    {
        smsLog( pMac, LOG1, FL("  Cannot request IMPS because command pending") );
        //Not to enter IMPS because more work to do
        if(pTimeInterval)
        {
            *pTimeInterval = 0;
        }
        //restart when ready
        pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;

        return (status);
    }
    if (IsPmcImpsReqFailed (pMac))
    {
        if(pTimeInterval)
        {
            *pTimeInterval = 1000000; //usec
        }
        //restart when ready
        pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;

        return status;
    }
    if((pMac->scan.fScanEnable) &&
      (eANI_BOOLEAN_FALSE == pMac->scan.fCancelIdleScan)) {
        //Stop get result timer because idle scan gets scan result out of PE
        csrScanStopGetResultTimer(pMac);
        if(pTimeInterval)
        {
            *pTimeInterval = pMac->roam.configParam.impsSleepTime;
        }
        //pmcRequestImps take a period in millisecond unit.
        status = pmcRequestImps(pMac,
                   pMac->roam.configParam.impsSleepTime / VOS_TIMER_TO_MS_UNIT,
                   csrScanIMPSCallback, pMac);
        if(!HAL_STATUS_SUCCESS(status))
        {
            if(eHAL_STATUS_PMC_ALREADY_IN_IMPS != status)
            {
                //Do restart the timer if CSR thinks it cannot do IMPS
                if( !csrCheckPSReady( pMac ) )
                {
                    if(pTimeInterval)
                    {
                    *pTimeInterval = 0;
                }
                    //Set the restart flag to true because that idle scan
                    //can be restarted even though the timer will not be running
                    pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;
                }
                else
                {
                    //For not now, we do a quicker retry
                    if(pTimeInterval)
                    {
                    *pTimeInterval = CSR_IDLE_SCAN_WAIT_TIME;
                }
            }
                smsLog(pMac, LOGW, FL("call pmcRequestImps and it returns status code (%d)"), status);
            }
            else
            {
                smsLog(pMac, LOGW, FL("already in IMPS"));
                //Since CSR is the only module to request for IMPS. If it is already in IMPS, CSR assumes
                //the callback will be called in the future. Should not happen though.
                status = eHAL_STATUS_SUCCESS;
                pMac->scan.nIdleScanTimeGap = 0;
            }
        }
        else
        {
            //requested so let's reset the value
            pMac->scan.nIdleScanTimeGap = 0;
        }
    }

    return (status);
}


eHalStatus csrScanStartIdleScan(tpAniSirGlobal pMac)
{
    eHalStatus status = eHAL_STATUS_CSR_WRONG_STATE;
    tANI_U32 nTime = 0;

    smsLog(pMac, LOGW, FL("called"));
    /*
     * Idle Scan not supported with Power Save Offload
     * Enabled
     */
    if(pMac->psOffloadEnabled)
    {
        return eHAL_STATUS_SUCCESS;
    }

    if(pMac->roam.configParam.IsIdleScanEnabled)
    {
        //stop bg scan first
        csrScanBGScanAbort(pMac);
        //Stop get result timer because idle scan gets scan result out of PE
        csrScanStopGetResultTimer(pMac);
        //Enable aging timer since idle scan is going on
        csrScanStartResultAgingTimer(pMac);
    }
    pMac->scan.fCancelIdleScan = eANI_BOOLEAN_FALSE;
    status = csrScanTriggerIdleScan(pMac, &nTime);
    if(!HAL_STATUS_SUCCESS(status))
    {
        csrScanStartIdleScanTimer(pMac, nTime);
    }

    return (status);
}


void csrScanCancelIdleScan(tpAniSirGlobal pMac)
{
    /*
     * Idle Scan not supported with Power Save Offload
     * Enabled
     */
    if(pMac->psOffloadEnabled)
    {
        return;
    }

    if(eANI_BOOLEAN_FALSE == pMac->scan.fCancelIdleScan)
    {
        if (vos_concurrent_open_sessions_running()) {
            return;
        }
        smsLog(pMac, LOG1, "  csrScanCancelIdleScan");
        pMac->scan.fCancelIdleScan = eANI_BOOLEAN_TRUE;
        /* Set the restart flag in case later on it is uncanceled */
        pMac->scan.fRestartIdleScan = eANI_BOOLEAN_TRUE;
        csrScanStopIdleScanTimer(pMac);
        csrScanRemoveNotRoamingScanCommand(pMac);
    }
}


void csrScanIdleScanTimerHandler(void *pv)
{
    tpAniSirGlobal pMac = PMAC_STRUCT( pv );
    eHalStatus status;
    tANI_U32 nTime = 0;

    smsLog(pMac, LOGW, "  csrScanIdleScanTimerHandler called  ");
    pmcResetImpsFailStatus (pMac);
    status = csrScanTriggerIdleScan(pMac, &nTime);
    if(!HAL_STATUS_SUCCESS(status) && (eANI_BOOLEAN_FALSE == pMac->scan.fCancelIdleScan))
    {
        //Check whether it is time to actually do an idle scan
        if(pMac->scan.nIdleScanTimeGap >= pMac->roam.configParam.impsSleepTime)
        {
            pMac->scan.nIdleScanTimeGap = 0;
            csrScanIMPSCallback(pMac, eHAL_STATUS_SUCCESS);
        }
        else
        {
            csrScanStartIdleScanTimer(pMac, nTime);
        }
    }
}




tANI_BOOLEAN csrScanRemoveNotRoamingScanCommand(tpAniSirGlobal pMac)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE;
    tListElem *pEntry, *pEntryTmp;
    tSmeCmd *pCommand;
    tDblLinkList localList;
    tDblLinkList *pCmdList;

    vos_mem_zero(&localList, sizeof(tDblLinkList));
    if(!HAL_STATUS_SUCCESS(csrLLOpen(pMac->hHdd, &localList)))
    {
        smsLog(pMac, LOGE, FL(" failed to open list"));
        return fRet;
    }
    if (!pMac->fScanOffload)
        pCmdList = &pMac->sme.smeCmdPendingList;
    else
        pCmdList = &pMac->sme.smeScanCmdPendingList;

    csrLLLock(pCmdList);
    pEntry = csrLLPeekHead(pCmdList, LL_ACCESS_NOLOCK);
    while(pEntry)
    {
        pEntryTmp = csrLLNext(pCmdList, pEntry, LL_ACCESS_NOLOCK);
        pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
        if( eSmeCommandScan == pCommand->command )
        {
            switch( pCommand->u.scanCmd.reason )
            {
            case eCsrScanIdleScan:
                if( csrLLRemoveEntry(pCmdList, pEntry, LL_ACCESS_NOLOCK) )
                {
                    csrLLInsertTail(&localList, pEntry, LL_ACCESS_NOLOCK);
                }
                fRet = eANI_BOOLEAN_TRUE;
                break;

            default:
                break;
            } //switch
        }
        pEntry = pEntryTmp;
    }

    csrLLUnlock(pCmdList);

    while( (pEntry = csrLLRemoveHead(&localList, LL_ACCESS_NOLOCK)) )
    {
        pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
        csrReleaseCommandScan( pMac, pCommand );
    }

    csrLLClose(&localList);

    return (fRet);
}


tANI_BOOLEAN csrScanRemoveFreshScanCommand(tpAniSirGlobal pMac, tANI_U8 sessionId)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE;
    tListElem *pEntry, *pEntryTmp;
    tSmeCmd *pCommand;
    tDblLinkList localList;
    tDblLinkList *pCmdList;

    vos_mem_zero(&localList, sizeof(tDblLinkList));
    if(!HAL_STATUS_SUCCESS(csrLLOpen(pMac->hHdd, &localList)))
    {
        smsLog(pMac, LOGE, FL(" failed to open list"));
        return fRet;
    }

    if (!pMac->fScanOffload)
        pCmdList = &pMac->sme.smeCmdPendingList;
    else
        pCmdList = &pMac->sme.smeScanCmdPendingList;

    csrLLLock(pCmdList);
    pEntry = csrLLPeekHead(pCmdList, LL_ACCESS_NOLOCK);
    while(pEntry)
    {
        pEntryTmp = csrLLNext(pCmdList, pEntry, LL_ACCESS_NOLOCK);
        pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
        if( (eSmeCommandScan == pCommand->command) && (sessionId == pCommand->sessionId) )
        {
            switch(pCommand->u.scanCmd.reason)
            {
#ifdef WLAN_FEATURE_ROAM_SCAN_OFFLOAD
            case eCsrScanGetLfrResult:
#endif
            case eCsrScanGetResult:
            case eCsrScanSetBGScanParam:
            case eCsrScanBGScanAbort:
            case eCsrScanBGScanEnable:
            case eCsrScanGetScanChnInfo:
                break;
            default:
                 smsLog (pMac, LOGW, "%s: -------- abort scan command reason = %d",
                    __func__, pCommand->u.scanCmd.reason);
                //The rest are fresh scan requests
                if( csrLLRemoveEntry(pCmdList, pEntry, LL_ACCESS_NOLOCK) )
                {
                    csrLLInsertTail(&localList, pEntry, LL_ACCESS_NOLOCK);
                }
                fRet = eANI_BOOLEAN_TRUE;
                break;
            }
        }
        pEntry = pEntryTmp;
    }

    csrLLUnlock(pCmdList);

    while( (pEntry = csrLLRemoveHead(&localList, LL_ACCESS_NOLOCK)) )
    {
        pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
        if (pCommand->u.scanCmd.callback)
        {
            /* User scan request is pending,
                                 * send response with status eCSR_SCAN_ABORT*/
            pCommand->u.scanCmd.callback(pMac,
                     pCommand->u.scanCmd.pContext,
                     sessionId,
                     pCommand->u.scanCmd.scanID,
                     eCSR_SCAN_ABORT);
        }
        csrReleaseCommandScan( pMac, pCommand );
    }
    csrLLClose(&localList);

    return (fRet);
}


void csrReleaseScanCommand(tpAniSirGlobal pMac, tSmeCmd *pCommand, eCsrScanStatus scanStatus)
{
    eCsrScanReason reason = pCommand->u.scanCmd.reason;
    tANI_BOOLEAN status;

    if (!pMac->fScanOffload)
    {
        tANI_U32 i;
        for(i = 0; i < CSR_ROAM_SESSION_MAX; i++)
            csrRoamStateChange(pMac, pCommand->u.scanCmd.lastRoamState[i], i);
    }
    else
    {
        csrRoamStateChange(pMac,
                pCommand->u.scanCmd.lastRoamState[pCommand->sessionId],
                pCommand->sessionId);
    }

    csrScanCallCallback(pMac, pCommand, scanStatus);

    smsLog(pMac, LOG3, "   Remove Scan command reason = %d", reason);
    if (pMac->fScanOffload)
    {
        status = csrLLRemoveEntry(&pMac->sme.smeScanCmdActiveList,
                                  &pCommand->Link, LL_ACCESS_LOCK);
    }
    else
    {
        status = csrLLRemoveEntry(&pMac->sme.smeCmdActiveList,
                                  &pCommand->Link, LL_ACCESS_LOCK);
    }

    if(status)
    {
        csrReleaseCommandScan( pMac, pCommand );
    }
    else
    {
        smsLog(pMac, LOGE,
                " ********csrReleaseScanCommand cannot release command reason %d",
                pCommand->u.scanCmd.reason );
    }
}


eHalStatus csrScanGetPMKIDCandidateList(tpAniSirGlobal pMac, tANI_U32 sessionId,
                                        tPmkidCandidateInfo *pPmkidList, tANI_U32 *pNumItems )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, "  pMac->scan.NumPmkidCandidate = %d", pSession->NumPmkidCandidate);
    csrResetPMKIDCandidateList(pMac, sessionId);
    if(csrIsConnStateConnected(pMac, sessionId) && pSession->pCurRoamProfile)
    {
        tCsrScanResultFilter *pScanFilter;
        tCsrScanResultInfo *pScanResult;
        tScanResultHandle hBSSList;
        tANI_U32 nItems = *pNumItems;

        *pNumItems = 0;
        pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
        if ( NULL == pScanFilter )
           status = eHAL_STATUS_FAILURE;
        else
        {
            vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
            //Here is the profile we need to connect to
            status = csrRoamPrepareFilterFromProfile(pMac, pSession->pCurRoamProfile, pScanFilter);
            if(HAL_STATUS_SUCCESS(status))
            {
                status = csrScanGetResult(pMac, pScanFilter, &hBSSList);
                if(HAL_STATUS_SUCCESS(status))
                {
                    while(((pScanResult = csrScanResultGetNext(pMac, hBSSList)) != NULL) && ( pSession->NumPmkidCandidate < nItems))
                    {
                        //NumPmkidCandidate adds up here
                        csrProcessBSSDescForPMKIDList(pMac,
                                      &pScanResult->BssDescriptor,
                                      (tDot11fBeaconIEs *)(pScanResult->pvIes),
                                      sessionId);
                    }
                    if(pSession->NumPmkidCandidate)
                    {
                        *pNumItems = pSession->NumPmkidCandidate;
                        vos_mem_copy(pPmkidList, pSession->PmkidCandidateInfo,
                                     pSession->NumPmkidCandidate * sizeof(tPmkidCandidateInfo));
                    }
                    csrScanResultPurge(pMac, hBSSList);
                }//Have scan result
                csrFreeScanFilter(pMac, pScanFilter);
            }
            vos_mem_free(pScanFilter);
        }
    }

    return (status);
}



#ifdef FEATURE_WLAN_WAPI
eHalStatus csrScanGetBKIDCandidateList(tpAniSirGlobal pMac, tANI_U32 sessionId,
                                       tBkidCandidateInfo *pBkidList, tANI_U32 *pNumItems )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );

    if(!pSession)
    {
        smsLog(pMac, LOGE, FL("  session %d not found "), sessionId);
        return eHAL_STATUS_FAILURE;
    }

    smsLog(pMac, LOGW, "  pMac->scan.NumBkidCandidate = %d", pSession->NumBkidCandidate);
    csrResetBKIDCandidateList(pMac, sessionId);
    if(csrIsConnStateConnected(pMac, sessionId) && pSession->pCurRoamProfile)
    {
        tCsrScanResultFilter *pScanFilter;
        tCsrScanResultInfo *pScanResult;
        tScanResultHandle hBSSList;
        tANI_U32 nItems = *pNumItems;
        *pNumItems = 0;
        pScanFilter = vos_mem_malloc(sizeof(tCsrScanResultFilter));
        if ( NULL == pScanFilter )
            status = eHAL_STATUS_FAILURE;
        else
        {
            vos_mem_set(pScanFilter, sizeof(tCsrScanResultFilter), 0);
            //Here is the profile we need to connect to
            status = csrRoamPrepareFilterFromProfile(pMac, pSession->pCurRoamProfile, pScanFilter);
            if(HAL_STATUS_SUCCESS(status))
            {
                status = csrScanGetResult(pMac, pScanFilter, &hBSSList);
                if(HAL_STATUS_SUCCESS(status))
                {
                    while(((pScanResult = csrScanResultGetNext(pMac, hBSSList)) != NULL) && ( pSession->NumBkidCandidate < nItems))
                    {
                        //pMac->scan.NumBkidCandidate adds up here
                        csrProcessBSSDescForBKIDList(pMac, &pScanResult->BssDescriptor,
                              (tDot11fBeaconIEs *)( pScanResult->pvIes ));

                    }
                    if(pSession->NumBkidCandidate)
                    {
                        *pNumItems = pSession->NumBkidCandidate;
                        vos_mem_copy(pBkidList, pSession->BkidCandidateInfo, pSession->NumBkidCandidate * sizeof(tBkidCandidateInfo));
                    }
                    csrScanResultPurge(pMac, hBSSList);
                }//Have scan result
            }
            vos_mem_free(pScanFilter);
        }
    }

    return (status);
}
#endif /* FEATURE_WLAN_WAPI */



//This function is usually used for BSSs that suppresses SSID so the profile
//shall have one and only one SSID
eHalStatus csrScanForSSID(tpAniSirGlobal pMac, tANI_U32 sessionId, tCsrRoamProfile *pProfile, tANI_U32 roamId, tANI_BOOLEAN notify)
{
    eHalStatus status = eHAL_STATUS_INVALID_PARAMETER;
    tSmeCmd *pScanCmd = NULL;
    tANI_U8 bAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    tANI_U8  index = 0;
    tANI_U32 numSsid = pProfile->SSIDs.numOfSSIDs;
    tpCsrNeighborRoamControlInfo pNeighborRoamInfo =
                              &pMac->roam.neighborRoamInfo[sessionId];

    smsLog(pMac, LOG2, FL("called"));
    //For WDS, we use the index 0. There must be at least one in there
    if( CSR_IS_WDS_STA( pProfile ) && numSsid )
    {
        numSsid = 1;
    }
    if(pMac->scan.fScanEnable && ( numSsid == 1 ) )
    {
        do
        {
            pScanCmd = csrGetCommandBuffer(pMac);
            if(!pScanCmd)
            {
                smsLog(pMac, LOGE, FL("failed to allocate command buffer"));
                break;
            }
            vos_mem_set(&pScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
            pScanCmd->u.scanCmd.pToRoamProfile = vos_mem_malloc(sizeof(tCsrRoamProfile));
            if ( NULL == pScanCmd->u.scanCmd.pToRoamProfile )
            {
                status = eHAL_STATUS_FAILURE;
            }
            else
            {
                status = csrRoamCopyProfile(pMac, pScanCmd->u.scanCmd.pToRoamProfile, pProfile);
            }
            if(!HAL_STATUS_SUCCESS(status))
                break;
            pScanCmd->u.scanCmd.roamId = roamId;
            pScanCmd->command = eSmeCommandScan;
            pScanCmd->sessionId = (tANI_U8)sessionId;
            pScanCmd->u.scanCmd.callback = NULL;
            pScanCmd->u.scanCmd.pContext = NULL;
            pScanCmd->u.scanCmd.reason = eCsrScanForSsid;//Need to check: might need a new reason for SSID scan for LFR during multisession with p2p
            pScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID++; //let it wrap around
            vos_mem_set(&pScanCmd->u.scanCmd.u.scanRequest, sizeof(tCsrScanRequest), 0);
            pScanCmd->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
            pScanCmd->u.scanCmd.u.scanRequest.BSSType = pProfile->BSSType;
            // To avoid 11b rate in probe request Set p2pSearch flag as 1 for P2P Client Mode
            if(VOS_P2P_CLIENT_MODE == pProfile->csrPersona)
            {
                pScanCmd->u.scanCmd.u.scanRequest.p2pSearch = 1;
            }
            if(pProfile->pAddIEScan)
            {
                pScanCmd->u.scanCmd.u.scanRequest.pIEField = vos_mem_malloc(
                                                    pProfile->nAddIEScanLength);
                if ( NULL == pScanCmd->u.scanCmd.u.scanRequest.pIEField )
                    status = eHAL_STATUS_FAILURE;
                else
                    status = eHAL_STATUS_SUCCESS;
                vos_mem_set(pScanCmd->u.scanCmd.u.scanRequest.pIEField,
                            pProfile->nAddIEScanLength, 0);
                if (HAL_STATUS_SUCCESS(status))
                {
                    vos_mem_copy(pScanCmd->u.scanCmd.u.scanRequest.pIEField,
                                 pProfile->pAddIEScan, pProfile->nAddIEScanLength);
                    pScanCmd->u.scanCmd.u.scanRequest.uIEFieldLen = pProfile->nAddIEScanLength;
                }
                else
                {
                    smsLog(pMac, LOGE, "No memory for scanning IE fields");
                }
            } //Allocate memory for IE field
            else
            {
                pScanCmd->u.scanCmd.u.scanRequest.uIEFieldLen = 0;
            }
            /* For one channel be good enough time to receive beacon at-least */
            if(  1 == pProfile->ChannelInfo.numOfChannels )
            {
#if  defined (WLAN_FEATURE_ROAM_SCAN_OFFLOAD)
                 if (pNeighborRoamInfo->handoffReqInfo.src == FASTREASSOC) {
                     pScanCmd->u.scanCmd.u.scanRequest.maxChnTime =
                                    MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC;
                     pScanCmd->u.scanCmd.u.scanRequest.minChnTime =
                                    MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC;
                     /* Reset this value */
                     pNeighborRoamInfo->handoffReqInfo.src = 0;
                 }
                 else
#endif
                 {
                     pScanCmd->u.scanCmd.u.scanRequest.maxChnTime = MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL;
                     pScanCmd->u.scanCmd.u.scanRequest.minChnTime = MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL;
                 }
            }
            else
            {
                 pScanCmd->u.scanCmd.u.scanRequest.maxChnTime =
                                   pMac->roam.configParam.nActiveMaxChnTime;
                 pScanCmd->u.scanCmd.u.scanRequest.minChnTime =
                                   pMac->roam.configParam.nActiveMinChnTime;
            }
            pScanCmd->u.scanCmd.u.scanRequest.maxChnTimeBtc =
                                   pMac->roam.configParam.nActiveMaxChnTimeBtc;
            pScanCmd->u.scanCmd.u.scanRequest.minChnTimeBtc =
                                   pMac->roam.configParam.nActiveMinChnTimeBtc;
            if(pProfile->BSSIDs.numOfBSSIDs == 1)
            {
                vos_mem_copy(pScanCmd->u.scanCmd.u.scanRequest.bssid,
                             pProfile->BSSIDs.bssid, sizeof(tCsrBssid));
            }
            else
            {
                vos_mem_copy(pScanCmd->u.scanCmd.u.scanRequest.bssid, bAddr, 6);
            }
            if(pProfile->ChannelInfo.numOfChannels)
            {
                pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList = vos_mem_malloc(
                                 sizeof(*pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList)
                                 * pProfile->ChannelInfo.numOfChannels);
                if ( NULL == pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList )
                    status = eHAL_STATUS_FAILURE;
                else
                    status = eHAL_STATUS_SUCCESS;
                pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = 0;
                if(HAL_STATUS_SUCCESS(status))
                {
                  csrRoamIsChannelValid(pMac, pProfile->ChannelInfo.ChannelList[0]);
                  for(index = 0; index < pProfile->ChannelInfo.numOfChannels; index++)
                  {
                     if(csrRoamIsValidChannel(pMac, pProfile->ChannelInfo.ChannelList[index]))
                     {
                        pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList[pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels]
                           = pProfile->ChannelInfo.ChannelList[index];
                        pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels++;
                     }
                     else
                     {
                         smsLog(pMac, LOGW, FL("process a channel (%d) that is invalid"), pProfile->ChannelInfo.ChannelList[index]);
                     }

                  }
               }
               else
                {
                    break;
                }

            }
            else
            {
                pScanCmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = 0;
            }
            if(pProfile->SSIDs.numOfSSIDs)
            {
                pScanCmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList = vos_mem_malloc(
                                     pProfile->SSIDs.numOfSSIDs * sizeof(tCsrSSIDInfo));
                if ( NULL == pScanCmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList )
                    status = eHAL_STATUS_FAILURE;
                else
                    status = eHAL_STATUS_SUCCESS;
                if(!HAL_STATUS_SUCCESS(status))
                {
                    break;
                }
                pScanCmd->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs = 1;
                vos_mem_copy(pScanCmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList,
                             pProfile->SSIDs.SSIDList, sizeof(tCsrSSIDInfo));
            }
            //Start process the command
            status = csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_FALSE);
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                break;
            }
        }while(0);
        if(!HAL_STATUS_SUCCESS(status))
        {
            if(pScanCmd)
            {
                csrReleaseCommandScan(pMac, pScanCmd);
                //TODO:free the memory that is allocated in this function
            }
            if(notify)
            {
            csrRoamCallCallback(pMac, sessionId, NULL, roamId, eCSR_ROAM_FAILED, eCSR_ROAM_RESULT_FAILURE);
        }
        }
    }//valid
    else
    {
        smsLog(pMac, LOGE, FL("cannot scan because scanEnable (%d) or numSSID (%d) is invalid"),
                pMac->scan.fScanEnable, pProfile->SSIDs.numOfSSIDs);
    }

    return (status);
}


/*
 * Issue a scan based on the new capability information
 * This should only happen when the associated AP changes its capability.
 * After this scan is done, CSR re-roams based on the new scan results
 */
eHalStatus csrScanForCapabilityChange(tpAniSirGlobal pMac, tSirSmeApNewCaps *pNewCaps)
{
    eHalStatus status = eHAL_STATUS_INVALID_PARAMETER;
    tSmeCmd *pScanCmd = NULL;

    if(pNewCaps)
    {
        do
        {
            pScanCmd = csrGetCommandBuffer(pMac);
            if(!pScanCmd)
            {
                smsLog(pMac, LOGE, FL("failed to allocate command buffer"));
                status = eHAL_STATUS_RESOURCES;
                break;
            }
            vos_mem_set(&pScanCmd->u.scanCmd, sizeof(tScanCmd), 0);
            status = eHAL_STATUS_SUCCESS;
            pScanCmd->u.scanCmd.roamId = 0;
            pScanCmd->command = eSmeCommandScan;
            pScanCmd->u.scanCmd.callback = NULL;
            pScanCmd->u.scanCmd.pContext = NULL;
            pScanCmd->u.scanCmd.reason = eCsrScanForCapsChange;
            pScanCmd->u.scanCmd.scanID = pMac->scan.nextScanID++; //let it wrap around
            status = csrQueueSmeCommand(pMac, pScanCmd, eANI_BOOLEAN_FALSE);
            if( !HAL_STATUS_SUCCESS( status ) )
            {
                smsLog( pMac, LOGE, FL(" fail to send message status = %d"), status );
                break;
            }
        }while(0);
        if(!HAL_STATUS_SUCCESS(status))
        {
            if(pScanCmd)
            {
                csrReleaseCommandScan(pMac, pScanCmd);
            }
        }
    }

    return (status);
}



void csrInitBGScanChannelList(tpAniSirGlobal pMac)
{
    tANI_U32 len = CSR_MIN(sizeof(pMac->roam.validChannelList), sizeof(pMac->scan.bgScanChannelList));

    vos_mem_set(pMac->scan.bgScanChannelList, len, 0);
    pMac->scan.numBGScanChannel = 0;

    if(HAL_STATUS_SUCCESS(csrGetCfgValidChannels(pMac, pMac->roam.validChannelList, &len)))
    {
        pMac->roam.numValidChannels = len;
        pMac->scan.numBGScanChannel = (tANI_U8)CSR_MIN(len, WNI_CFG_BG_SCAN_CHANNEL_LIST_LEN);
        vos_mem_copy(pMac->scan.bgScanChannelList, pMac->roam.validChannelList,
                     pMac->scan.numBGScanChannel);
        csrSetBGScanChannelList(pMac, pMac->scan.bgScanChannelList, pMac->scan.numBGScanChannel);
    }
}


//This function return TRUE if background scan channel list is adjusted.
//this function will only shrink the background scan channel list
tANI_BOOLEAN csrAdjustBGScanChannelList(tpAniSirGlobal pMac, tANI_U8 *pChannelList, tANI_U8 NumChannels,
                                        tANI_U8 *pAdjustChannels, tANI_U8 *pNumAdjustChannels)
{
    tANI_BOOLEAN fRet = eANI_BOOLEAN_FALSE;
    tANI_U8 i, j, count = *pNumAdjustChannels;

    i = 0;
    while(i < count)
    {
        for(j = 0; j < NumChannels; j++)
        {
            if(pChannelList[j] == pAdjustChannels[i])
                break;
        }
        if(j == NumChannels)
        {
            //This channel is not in the list, remove it
            fRet = eANI_BOOLEAN_TRUE;
            count--;
            if(count - i)
            {
                vos_mem_copy(&pAdjustChannels[i], &pAdjustChannels[i+1], count - i);
            }
            else
            {
                //already remove the last one. Done.
                break;
            }
        }
        else
        {
            i++;
        }
    }//while(i<count)
    *pNumAdjustChannels = count;

    return (fRet);
}


//Get the list of the base channels to scan for passively 11d info
eHalStatus csrScanGetSupportedChannels( tpAniSirGlobal pMac )
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    int n = WNI_CFG_VALID_CHANNEL_LIST_LEN;

    status = vos_nv_getSupportedChannels( pMac->scan.baseChannels.channelList, &n, NULL, NULL );
    if( HAL_STATUS_SUCCESS(status) )
    {
        pMac->scan.baseChannels.numChannels = (tANI_U8)n;
    }
    else
    {
        smsLog( pMac, LOGE, FL(" failed") );
        pMac->scan.baseChannels.numChannels = 0;
    }

    return ( status );
}

//This function use the input pChannelList to validate the current saved channel list
eHalStatus csrSetBGScanChannelList( tpAniSirGlobal pMac, tANI_U8 *pAdjustChannels, tANI_U8 NumAdjustChannels)
{
    tANI_U32 dataLen = sizeof( tANI_U8 ) * NumAdjustChannels;

    return (ccmCfgSetStr(pMac, WNI_CFG_BG_SCAN_CHANNEL_LIST, pAdjustChannels, dataLen, NULL, eANI_BOOLEAN_FALSE));
}


void csrSetCfgValidChannelList( tpAniSirGlobal pMac, tANI_U8 *pChannelList, tANI_U8 NumChannels )
{
    tANI_U32 dataLen = sizeof( tANI_U8 ) * NumChannels;
    eHalStatus status;

    VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                "%s: dump valid channel list(NumChannels(%d))",
                __func__,NumChannels);
    VOS_TRACE_HEX_DUMP(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                       pChannelList, NumChannels);
    ccmCfgSetStr(pMac, WNI_CFG_VALID_CHANNEL_LIST, pChannelList, dataLen, NULL, eANI_BOOLEAN_FALSE);

    if (pMac->fScanOffload)
    {
        VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                "Scan offload is enabled, update default chan list");
        status = csrUpdateChannelList(pMac);
        if (eHAL_STATUS_SUCCESS != status)
        {
            VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
                    "failed to update the supported channel list");
        }
    }
    return;
}



/*
 * The Tx power limits are saved in the cfg for future usage.
 */
void csrSaveTxPowerToCfg( tpAniSirGlobal pMac, tDblLinkList *pList, tANI_U32 cfgId )
{
    tListElem *pEntry;
    tANI_U32 cbLen = 0, dataLen;
    tCsrChannelPowerInfo *pChannelSet;
    tANI_U32 idx;
    tSirMacChanInfo *pChannelPowerSet;
    tANI_U8 *pBuf = NULL;

    //allocate maximum space for all channels
    dataLen = WNI_CFG_VALID_CHANNEL_LIST_LEN * sizeof(tSirMacChanInfo);
    if ( (pBuf = vos_mem_malloc(dataLen)) != NULL )
    {
        vos_mem_set(pBuf, dataLen, 0);
        pChannelPowerSet = (tSirMacChanInfo *)(pBuf);

        pEntry = csrLLPeekHead( pList, LL_ACCESS_LOCK );
        // write the tuples (startChan, numChan, txPower) for each channel found in the channel power list.
        while( pEntry )
        {
            pChannelSet = GET_BASE_ADDR( pEntry, tCsrChannelPowerInfo, link );
            if ( 1 != pChannelSet->interChannelOffset )
            {
                /*
                 * we keep the 5G channel sets internally with an inter channel
                 * offset of 4.  Expand these to the right format.
                 * (inter channel offset of 1 is the only option for the
                 * triplets that 11d advertises.
                 */
                if ((cbLen + (pChannelSet->numChannels * sizeof(tSirMacChanInfo))) >= dataLen)
                {
                    // expanding this entry will overflow our allocation
                    smsLog(pMac, LOGE,
                           "%s: Buffer overflow, start %d, num %d, offset %d",
                           __func__,
                           pChannelSet->firstChannel,
                           pChannelSet->numChannels,
                           pChannelSet->interChannelOffset);
                    break;
                }

                for( idx = 0; idx < pChannelSet->numChannels; idx++ )
                {
                    pChannelPowerSet->firstChanNum = (tSirMacChanNum)(pChannelSet->firstChannel + ( idx * pChannelSet->interChannelOffset ));
                    smsLog(pMac, LOG3, " Setting Channel Number %d", pChannelPowerSet->firstChanNum);
                    pChannelPowerSet->numChannels  = 1;
                    pChannelPowerSet->maxTxPower = CSR_ROAM_MIN( pChannelSet->txPower, pMac->roam.configParam.nTxPowerCap );
                    smsLog(pMac, LOG3, " Setting Max Transmit Power %d", pChannelPowerSet->maxTxPower);
                    cbLen += sizeof( tSirMacChanInfo );
                    pChannelPowerSet++;
                }
            }
            else
            {
                if (cbLen >= dataLen)
                {
                    // this entry will overflow our allocation
                    smsLog(pMac, LOGE,
                           "%s: Buffer overflow, start %d, num %d, offset %d",
                           __func__,
                           pChannelSet->firstChannel,
                           pChannelSet->numChannels,
                           pChannelSet->interChannelOffset);
                    break;
                }
                pChannelPowerSet->firstChanNum = pChannelSet->firstChannel;
                smsLog(pMac, LOG3, " Setting Channel Number %d", pChannelPowerSet->firstChanNum);
                pChannelPowerSet->numChannels = pChannelSet->numChannels;
                pChannelPowerSet->maxTxPower = CSR_ROAM_MIN( pChannelSet->txPower, pMac->roam.configParam.nTxPowerCap );
                smsLog(pMac, LOG3, " Setting Max Transmit Power %d, nTxPower %d", pChannelPowerSet->maxTxPower,pMac->roam.configParam.nTxPowerCap );


                cbLen += sizeof( tSirMacChanInfo );
                pChannelPowerSet++;
            }

            pEntry = csrLLNext( pList, pEntry, LL_ACCESS_LOCK );
        }

        if(cbLen)
        {
            ccmCfgSetStr(pMac, cfgId, (tANI_U8 *)pBuf, cbLen, NULL, eANI_BOOLEAN_FALSE);
        }
        vos_mem_free(pBuf);
    }//Allocate memory
}


void csrSetCfgCountryCode( tpAniSirGlobal pMac, tANI_U8 *countryCode )
{
    tANI_U8 cc[WNI_CFG_COUNTRY_CODE_LEN];
    ///v_REGDOMAIN_t DomainId;

    smsLog( pMac, LOG3, "Setting Country Code in Cfg from csrSetCfgCountryCode %s",countryCode );
    vos_mem_copy(cc, countryCode, WNI_CFG_COUNTRY_CODE_LEN);

    // don't program the bogus country codes that we created for Korea in the MAC.  if we see
    // the bogus country codes, program the MAC with the right country code.
    if ( ( 'K'  == countryCode[ 0 ] && '1' == countryCode[ 1 ]  ) ||
         ( 'K'  == countryCode[ 0 ] && '2' == countryCode[ 1 ]  ) ||
         ( 'K'  == countryCode[ 0 ] && '3' == countryCode[ 1 ]  ) ||
         ( 'K'  == countryCode[ 0 ] && '4' == countryCode[ 1 ]  )    )
    {
        // replace the alternate Korea country codes, 'K1', 'K2', .. with 'KR' for Korea
        cc[ 1 ] = 'R';
    }
    ccmCfgSetStr(pMac, WNI_CFG_COUNTRY_CODE, cc, WNI_CFG_COUNTRY_CODE_LEN, NULL, eANI_BOOLEAN_FALSE);

    //Need to let HALPHY know about the current domain so it can apply some
    //domain-specific settings (TX filter...)
    /*if(HAL_STATUS_SUCCESS(csrGetRegulatoryDomainForCountry(pMac, cc, &DomainId)))
    {
        halPhySetRegDomain(pMac, DomainId);
    }*/
}



eHalStatus csrGetCountryCode(tpAniSirGlobal pMac, tANI_U8 *pBuf, tANI_U8 *pbLen)
{
    eHalStatus status = eHAL_STATUS_INVALID_PARAMETER;
    tANI_U32 len;

    if(pBuf && pbLen && (*pbLen >= WNI_CFG_COUNTRY_CODE_LEN))
    {
        len = *pbLen;
        status = ccmCfgGetStr(pMac, WNI_CFG_COUNTRY_CODE, pBuf, &len);
        if(HAL_STATUS_SUCCESS(status))
        {
            *pbLen = (tANI_U8)len;
        }
    }

    return (status);
}


void csrSetCfgScanControlList( tpAniSirGlobal pMac, tANI_U8 *countryCode, tCsrChannel *pChannelList  )
{
    tANI_U8 i, j;
    tANI_BOOLEAN found=FALSE;
    tANI_U8 *pControlList = NULL;
    tANI_U32 len = WNI_CFG_SCAN_CONTROL_LIST_LEN;

    if ( (pControlList = vos_mem_malloc(WNI_CFG_SCAN_CONTROL_LIST_LEN)) != NULL )
    {
        vos_mem_set((void *)pControlList, WNI_CFG_SCAN_CONTROL_LIST_LEN, 0);
        if(HAL_STATUS_SUCCESS(ccmCfgGetStr(pMac, WNI_CFG_SCAN_CONTROL_LIST, pControlList, &len)))
        {
            for (i = 0; i < pChannelList->numChannels; i++)
            {
                for (j = 0; j < len; j += 2)
                {
                    if (pControlList[j] == pChannelList->channelList[i])
                    {
                        found = TRUE;
                        break;
                    }
                }

                if (found)    // insert a pair(channel#, flag)
                {
                    pControlList[j+1] = csrGetScanType(pMac, pControlList[j]);
                    found = FALSE;  // reset the flag
                }

            }
            VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                      "%s: dump scan control list",__func__);
            VOS_TRACE_HEX_DUMP(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_INFO,
                               pControlList, len);

            ccmCfgSetStr(pMac, WNI_CFG_SCAN_CONTROL_LIST, pControlList, len, NULL, eANI_BOOLEAN_FALSE);
        }//Successfully getting scan control list
        vos_mem_free(pControlList);
    }//AllocateMemory
}


//if bgPeriod is 0, background scan is disabled. It is in millisecond units
eHalStatus csrSetCfgBackgroundScanPeriod(tpAniSirGlobal pMac, tANI_U32 bgPeriod)
{
    return (ccmCfgSetInt(pMac, WNI_CFG_BACKGROUND_SCAN_PERIOD, bgPeriod, (tCcmCfgSetCallback) csrScanCcmCfgSetCallback, eANI_BOOLEAN_FALSE));
}


void csrScanCcmCfgSetCallback(tHalHandle hHal, tANI_S32 result)
{
    tListElem *pEntry = NULL;
    tSmeCmd *pCommand = NULL;
    tpAniSirGlobal pMac = PMAC_STRUCT( hHal );
    tDblLinkList *pCmdList ;

    if (!pMac->fScanOffload)
        pCmdList = &pMac->sme.smeCmdActiveList;
    else
        pCmdList = &pMac->sme.smeScanCmdActiveList;

    pEntry = csrLLPeekHead( pCmdList, LL_ACCESS_LOCK );
    if ( pEntry )
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        if ( eSmeCommandScan == pCommand->command )
        {
            eCsrScanStatus scanStatus = (CCM_IS_RESULT_SUCCESS(result)) ? eCSR_SCAN_SUCCESS : eCSR_SCAN_FAILURE;
            csrReleaseScanCommand(pMac, pCommand, scanStatus);
        }
        else
        {
            smsLog( pMac, LOGW, "CSR: Scan Completion called but SCAN command is not ACTIVE ..." );
        }
    }
    smeProcessPendingQueue( pMac );
}

eHalStatus csrProcessSetBGScanParam(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
    eHalStatus status;
    tCsrBGScanRequest *pScanReq = &pCommand->u.scanCmd.u.bgScanRequest;
    tANI_U32 dataLen = sizeof( tANI_U8 ) * pScanReq->ChannelInfo.numOfChannels;

    //***setcfg for background scan channel list
    status = ccmCfgSetInt(pMac, WNI_CFG_ACTIVE_MINIMUM_CHANNEL_TIME, pScanReq->minChnTime, NULL, eANI_BOOLEAN_FALSE);
    status = ccmCfgSetInt(pMac, WNI_CFG_ACTIVE_MAXIMUM_CHANNEL_TIME, pScanReq->maxChnTime, NULL, eANI_BOOLEAN_FALSE);
    //Not set the background scan interval if not connected because bd scan should not be run if not connected
    if(!csrIsAllSessionDisconnected(pMac))
    {
        /* If disabling BG scan here, we need to stop aging as well */
        if(pScanReq->scanInterval == 0)
        {
            //Stop aging because no new result is coming in
            csrScanStopResultAgingTimer(pMac);
        }

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
        {
            vos_log_scan_pkt_type *pScanLog = NULL;

            WLAN_VOS_DIAG_LOG_ALLOC(pScanLog, vos_log_scan_pkt_type, LOG_WLAN_SCAN_C);
            if(pScanLog)
            {
                pScanLog->eventId = WLAN_SCAN_EVENT_HO_SCAN_REQ;
                pScanLog->minChnTime = (v_U8_t)pScanReq->minChnTime;
                pScanLog->maxChnTime = (v_U8_t)pScanReq->maxChnTime;
                pScanLog->timeBetweenBgScan = (v_U8_t)pScanReq->scanInterval;
                pScanLog->numChannel = pScanReq->ChannelInfo.numOfChannels;
                if(pScanLog->numChannel && (pScanLog->numChannel < VOS_LOG_MAX_NUM_CHANNEL))
                {
                    vos_mem_copy(pScanLog->channels,
                                 pScanReq->ChannelInfo.ChannelList,
                                 pScanLog->numChannel);
                }
                WLAN_VOS_DIAG_LOG_REPORT(pScanLog);
            }
        }
#endif //#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR

        status = ccmCfgSetInt(pMac, WNI_CFG_BACKGROUND_SCAN_PERIOD, pScanReq->scanInterval, NULL, eANI_BOOLEAN_FALSE);
    }
    else
    {
        //No need to stop aging because IDLE scan is still running
        status = ccmCfgSetInt(pMac, WNI_CFG_BACKGROUND_SCAN_PERIOD, 0, NULL, eANI_BOOLEAN_FALSE);
    }

    if(pScanReq->SSID.length > WNI_CFG_SSID_LEN)
    {
        pScanReq->SSID.length = WNI_CFG_SSID_LEN;
    }

    status = ccmCfgSetStr(pMac, WNI_CFG_BG_SCAN_CHANNEL_LIST, pScanReq->ChannelInfo.ChannelList, dataLen, NULL, eANI_BOOLEAN_FALSE);
    status = ccmCfgSetStr(pMac, WNI_CFG_SSID, (tANI_U8 *)pScanReq->SSID.ssId, pScanReq->SSID.length, NULL, eANI_BOOLEAN_FALSE);



    return (status);
}


eHalStatus csrScanAbortMacScan(tpAniSirGlobal pMac, tANI_U8 sessionId,
                               eCsrAbortReason reason)
{
    eHalStatus status = eHAL_STATUS_FAILURE;
    tSirSmeScanAbortReq *pMsg;
    tANI_U16 msgLen;
    tListElem *pEntry;
    tSmeCmd *pCommand;

    if (!pMac->fScanOffload)
    {
#ifdef WLAN_AP_STA_CONCURRENCY
        csrLLLock(&pMac->scan.scanCmdPendingList);
        while(NULL !=
               (pEntry = csrLLRemoveHead(&pMac->scan.scanCmdPendingList,
                                LL_ACCESS_NOLOCK)))
        {

            pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
            csrAbortCommand( pMac, pCommand, eANI_BOOLEAN_FALSE);
        }
        csrLLUnlock(&pMac->scan.scanCmdPendingList);
#endif

        pMac->scan.fDropScanCmd = eANI_BOOLEAN_TRUE;
        csrRemoveCmdFromPendingList( pMac, &pMac->roam.roamCmdPendingList, eSmeCommandScan);
        csrRemoveCmdFromPendingList( pMac, &pMac->sme.smeCmdPendingList, eSmeCommandScan);
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_FALSE;

        pEntry = csrLLPeekHead(&pMac->sme.smeCmdActiveList, LL_ACCESS_LOCK);
    }
    else
    {
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_TRUE;
        csrRemoveCmdWithSessionIdFromPendingList(pMac,
                sessionId,
                &pMac->sme.smeScanCmdPendingList,
                eSmeCommandScan);
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_FALSE;

        pEntry = csrLLPeekHead(&pMac->sme.smeScanCmdActiveList, LL_ACCESS_LOCK);
    }

    //We need to abort scan only if we are scanning
    if(NULL != pEntry)
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        if(eSmeCommandScan == pCommand->command &&
           pCommand->sessionId == sessionId)
        {
            msgLen = (tANI_U16)(sizeof(tSirSmeScanAbortReq));
            pMsg = vos_mem_malloc(msgLen);
            if ( NULL == pMsg )
            {
               status = eHAL_STATUS_FAILURE;
               smsLog(pMac, LOGE, FL("Failed to allocate memory for SmeScanAbortReq"));
            }
            else
            {
                if(reason == eCSR_SCAN_ABORT_DUE_TO_BAND_CHANGE)
                {
                    pCommand->u.scanCmd.abortScanDueToBandChange
                        = eANI_BOOLEAN_TRUE;
                }
                vos_mem_set((void *)pMsg, msgLen, 0);
                pMsg->type = pal_cpu_to_be16((tANI_U16)eWNI_SME_SCAN_ABORT_IND);
                pMsg->msgLen = pal_cpu_to_be16(msgLen);
                pMsg->sessionId = sessionId;
                status = palSendMBMessage(pMac->hHdd, pMsg);
            }
        }
    }

    return(status);
}

void csrRemoveCmdWithSessionIdFromPendingList(tpAniSirGlobal pMac,
                                              tANI_U8 sessionId,
                                              tDblLinkList *pList,
                                              eSmeCommandType commandType)
{
    tDblLinkList localList;
    tListElem *pEntry;
    tSmeCmd   *pCommand;
    tListElem  *pEntryToRemove;

    vos_mem_zero(&localList, sizeof(tDblLinkList));
    if(!HAL_STATUS_SUCCESS(csrLLOpen(pMac->hHdd, &localList)))
    {
        smsLog(pMac, LOGE, FL(" failed to open list"));
        return;
    }

    csrLLLock(pList);
    if ((pEntry = csrLLPeekHead( pList, LL_ACCESS_NOLOCK)))
    {

        /* Have to make sure we don't loop back to the head of the list,
         * which will happen if the entry is NOT on the list */
        while (pEntry)
        {
            pEntryToRemove = pEntry;
            pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
            pCommand = GET_BASE_ADDR( pEntryToRemove, tSmeCmd, Link );
            if ((pCommand->command == commandType)  &&
                    (pCommand->sessionId == sessionId))
            {
                /* Remove that entry only */
                if (csrLLRemoveEntry( pList, pEntryToRemove, LL_ACCESS_NOLOCK))
                {
                    csrLLInsertTail(&localList, pEntryToRemove,
                                    LL_ACCESS_NOLOCK);
                }
            }
        }
    }
    csrLLUnlock(pList);

    while ((pEntry = csrLLRemoveHead(&localList, LL_ACCESS_NOLOCK)))
    {
        pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
        csrAbortCommand(pMac, pCommand, eANI_BOOLEAN_FALSE);
    }

    csrLLClose(&localList);
}

void csrRemoveCmdFromPendingList(tpAniSirGlobal pMac, tDblLinkList *pList,
                                 eSmeCommandType commandType )
{
    tDblLinkList localList;
    tListElem *pEntry;
    tSmeCmd   *pCommand;
    tListElem  *pEntryToRemove;

    vos_mem_zero(&localList, sizeof(tDblLinkList));
    if(!HAL_STATUS_SUCCESS(csrLLOpen(pMac->hHdd, &localList)))
    {
        smsLog(pMac, LOGE, FL(" failed to open list"));
        return;
    }

    csrLLLock(pList);
    if( !csrLLIsListEmpty( pList, LL_ACCESS_NOLOCK ) )
    {
        pEntry = csrLLPeekHead( pList, LL_ACCESS_NOLOCK);

        // Have to make sure we don't loop back to the head of the list, which will
        // happen if the entry is NOT on the list...
        while( pEntry )
        {
            pEntryToRemove = pEntry;
            pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
            pCommand = GET_BASE_ADDR( pEntryToRemove, tSmeCmd, Link );
            if ( pCommand->command == commandType )
            {
                // Remove that entry only
                if(csrLLRemoveEntry( pList, pEntryToRemove, LL_ACCESS_NOLOCK))
                {
                    csrLLInsertTail(&localList, pEntryToRemove, LL_ACCESS_NOLOCK);
                }
            }
        }


    }
    csrLLUnlock(pList);

    while( (pEntry = csrLLRemoveHead(&localList, LL_ACCESS_NOLOCK)) )
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        csrAbortCommand( pMac, pCommand, eANI_BOOLEAN_FALSE);
    }
    csrLLClose(&localList);

}

eHalStatus csrScanAbortScanForSSID(tpAniSirGlobal pMac, tANI_U32 sessionId)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSirSmeScanAbortReq *pMsg;
    tANI_U16 msgLen;
    tListElem *pEntry;
    tSmeCmd *pCommand;

    if (!pMac->fScanOffload)
    {
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_TRUE;
#ifdef WLAN_AP_STA_CONCURRENCY
        csrRemoveScanForSSIDFromPendingList( pMac, &pMac->scan.scanCmdPendingList, sessionId);
#endif
        csrRemoveScanForSSIDFromPendingList( pMac, &pMac->roam.roamCmdPendingList, sessionId);
        csrRemoveScanForSSIDFromPendingList( pMac, &pMac->sme.smeCmdPendingList, sessionId);
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_FALSE;
        pEntry = csrLLPeekHead(&pMac->sme.smeCmdActiveList, LL_ACCESS_LOCK);
    }
    else
    {
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_TRUE;
        csrRemoveScanForSSIDFromPendingList( pMac, &pMac->sme.smeScanCmdPendingList, sessionId);
        pMac->scan.fDropScanCmd = eANI_BOOLEAN_FALSE;
        pEntry = csrLLPeekHead(&pMac->sme.smeScanCmdActiveList, LL_ACCESS_LOCK);
    }

    if(NULL != pEntry)
    {
       pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );

       if ( (eSmeCommandScan == pCommand->command ) &&
                        (sessionId == pCommand->sessionId))
       {
          if ( eCsrScanForSsid == pCommand->u.scanCmd.reason)
          {
             msgLen = (tANI_U16)(sizeof( tSirSmeScanAbortReq ));
             pMsg = vos_mem_malloc(msgLen);
             if ( NULL == pMsg )
             {
                status = eHAL_STATUS_FAILURE;
                smsLog(pMac, LOGE, FL("Failed to allocate memory for SmeScanAbortReq"));
             }
             else
             {
                vos_mem_zero((void *)pMsg, msgLen);
                pMsg->type = pal_cpu_to_be16((tANI_U16)eWNI_SME_SCAN_ABORT_IND);
                pMsg->msgLen = pal_cpu_to_be16(msgLen);
                pMsg->sessionId = sessionId;
                status = palSendMBMessage(pMac->hHdd, pMsg);
             }
          }
       }
    }
    return( status );
}

void csrRemoveScanForSSIDFromPendingList(tpAniSirGlobal pMac, tDblLinkList *pList, tANI_U32 sessionId)
{
    tDblLinkList localList;
    tListElem *pEntry;
    tSmeCmd   *pCommand;
    tListElem  *pEntryToRemove;

    vos_mem_zero(&localList, sizeof(tDblLinkList));
    if(!HAL_STATUS_SUCCESS(csrLLOpen(pMac->hHdd, &localList)))
    {
        smsLog(pMac, LOGE, FL(" failed to open list"));
        return;
    }

    csrLLLock(pList);
    if( !csrLLIsListEmpty( pList, LL_ACCESS_NOLOCK ) )
    {
        pEntry = csrLLPeekHead( pList, LL_ACCESS_NOLOCK);

        // Have to make sure we don't loop back to the head of the list, which will
        // happen if the entry is NOT on the list...
        while( pEntry )
        {
            pEntryToRemove = pEntry;
            pEntry = csrLLNext(pList, pEntry, LL_ACCESS_NOLOCK);
            pCommand = GET_BASE_ADDR( pEntryToRemove, tSmeCmd, Link );
            if ( (eSmeCommandScan == pCommand->command ) &&
                             (sessionId == pCommand->sessionId) )
            {
               if ( eCsrScanForSsid == pCommand->u.scanCmd.reason)
               {
                 // Remove that entry only
                 if ( csrLLRemoveEntry( pList, pEntryToRemove, LL_ACCESS_NOLOCK))
                 {
                    csrLLInsertTail(&localList, pEntryToRemove, LL_ACCESS_NOLOCK);
                 }
               }
            }
        }
    }
    csrLLUnlock(pList);

    while( (pEntry = csrLLRemoveHead(&localList, LL_ACCESS_NOLOCK)) )
    {
        pCommand = GET_BASE_ADDR( pEntry, tSmeCmd, Link );
        csrAbortCommand( pMac, pCommand, eANI_BOOLEAN_FALSE);
    }
    csrLLClose(&localList);
}

eHalStatus csrScanAbortMacScanNotForConnect(tpAniSirGlobal pMac,
                                            tANI_U8 sessionId)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;

    if( !csrIsScanForRoamCommandActive( pMac ) )
    {
        //Only abort the scan if it is not used for other roam/connect purpose
        status = csrScanAbortMacScan(pMac, sessionId, eCSR_SCAN_ABORT_DEFAULT);
    }

    return (status);
}


eHalStatus csrScanGetScanChannelInfo(tpAniSirGlobal pMac, tANI_U8 sessionId)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tSirMbMsg *pMsg;
    tANI_U16 msgLen;

    if (pMac->fScanOffload)
        msgLen = (tANI_U16)(sizeof(tSirSmeGetScanChanReq));
    else
        msgLen = (tANI_U16)(sizeof(tSirMbMsg));

    pMsg = vos_mem_malloc(msgLen);
    if ( NULL == pMsg )
        status = eHAL_STATUS_FAILURE;
    else
    {
        vos_mem_set(pMsg, msgLen, 0);
        pMsg->type = eWNI_SME_GET_SCANNED_CHANNEL_REQ;
        pMsg->msgLen = msgLen;
        if (pMac->fScanOffload)
            ((tSirSmeGetScanChanReq *)pMsg)->sessionId = sessionId;
        status = palSendMBMessage(pMac->hHdd, pMsg);
    }

    return( status );
}

tANI_BOOLEAN csrRoamIsValidChannel( tpAniSirGlobal pMac, tANI_U8 channel )
{
    tANI_BOOLEAN fValid = FALSE;
    tANI_U32 idxValidChannels;
    tANI_U32 len = pMac->roam.numValidChannels;

    for ( idxValidChannels = 0; ( idxValidChannels < len ); idxValidChannels++ )
    {
       if ( channel == pMac->roam.validChannelList[ idxValidChannels ] )
       {
          fValid = TRUE;
          break;
       }
    }

    return fValid;
}

#ifdef FEATURE_WLAN_SCAN_PNO
eHalStatus csrScanSavePreferredNetworkFound(tpAniSirGlobal pMac,
            tSirPrefNetworkFoundInd *pPrefNetworkFoundInd)
{
   v_U32_t uLen = 0;
   tpSirProbeRespBeacon pParsedFrame;
   tCsrScanResult *pScanResult = NULL;
   tSirBssDescription *pBssDescr = NULL;
   tANI_BOOLEAN fDupBss;
   tDot11fBeaconIEs *pIesLocal = NULL;
   tAniSSID tmpSsid;
   v_TIME_t timer=0;
   tpSirMacMgmtHdr macHeader = (tpSirMacMgmtHdr)pPrefNetworkFoundInd->data;

   pParsedFrame =
       (tpSirProbeRespBeacon)vos_mem_malloc(sizeof(tSirProbeRespBeacon));

   if (NULL == pParsedFrame)
   {
      smsLog(pMac, LOGE, FL(" fail to allocate memory for frame"));
      return eHAL_STATUS_RESOURCES;
   }

   if ( pPrefNetworkFoundInd->frameLength <= SIR_MAC_HDR_LEN_3A )
   {
      smsLog(pMac, LOGE,
         FL("Not enough bytes in PNO indication probe resp frame! length=%d"),
         pPrefNetworkFoundInd->frameLength);
      vos_mem_free(pParsedFrame);
      return eHAL_STATUS_FAILURE;
   }

   if (sirConvertProbeFrame2Struct(pMac,
               &pPrefNetworkFoundInd->data[SIR_MAC_HDR_LEN_3A],
               pPrefNetworkFoundInd->frameLength - SIR_MAC_HDR_LEN_3A,
               pParsedFrame) != eSIR_SUCCESS ||
         !pParsedFrame->ssidPresent)
   {
      smsLog(pMac, LOGE,
         FL("Parse error ProbeResponse, length=%d"),
         pPrefNetworkFoundInd->frameLength);
      vos_mem_free(pParsedFrame);
      return eHAL_STATUS_FAILURE;
   }
   //24 byte MAC header and 12 byte to ssid IE
   if (pPrefNetworkFoundInd->frameLength >
           (SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET))
   {
      uLen = pPrefNetworkFoundInd->frameLength -
          (SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET);
   }

   pScanResult = vos_mem_malloc(sizeof(tCsrScanResult) + uLen);
   if ( NULL == pScanResult )
   {
      smsLog(pMac, LOGE, FL(" fail to allocate memory for frame"));
      vos_mem_free(pParsedFrame);
      return eHAL_STATUS_RESOURCES;
   }

   vos_mem_set(pScanResult, sizeof(tCsrScanResult) + uLen, 0);
   pBssDescr = &pScanResult->Result.BssDescriptor;
   /*
    * Length of BSS description is without length of length itself and length
    * of pointer that holds the next BSS description
    */
   pBssDescr->length = (tANI_U16)(
                     sizeof(tSirBssDescription) - sizeof(tANI_U16) -
                     sizeof(tANI_U32) + uLen);
   if (pParsedFrame->dsParamsPresent)
   {
      pBssDescr->channelId = pParsedFrame->channelNumber;
   }
   else if (pParsedFrame->HTInfo.present)
   {
      pBssDescr->channelId = pParsedFrame->HTInfo.primaryChannel;
   }
   else
   {
      pBssDescr->channelId = pParsedFrame->channelNumber;
   }

   if ((pBssDescr->channelId > 0) && (pBssDescr->channelId < 15))
   {
      int i;
      // 11b or 11g packet
      // 11g iff extended Rate IE is present or
      // if there is an A rate in suppRate IE
      for (i = 0; i < pParsedFrame->supportedRates.numRates; i++)
      {
         if (sirIsArate(pParsedFrame->supportedRates.rate[i] & 0x7f))
         {
            pBssDescr->nwType = eSIR_11G_NW_TYPE;
            break;
         }
      }
      if (pParsedFrame->extendedRatesPresent)
      {
            pBssDescr->nwType = eSIR_11G_NW_TYPE;
      }
   }
   else
   {
      // 11a packet
      pBssDescr->nwType = eSIR_11A_NW_TYPE;
   }

   pBssDescr->sinr = 0;
   pBssDescr->rssi = -1 * pPrefNetworkFoundInd->rssi;
   pBssDescr->beaconInterval = pParsedFrame->beaconInterval;
   if (!pBssDescr->beaconInterval)
   {
      smsLog(pMac, LOGW,
         FL("Bcn Interval is Zero , default to 100" MAC_ADDRESS_STR),
         MAC_ADDR_ARRAY(pBssDescr->bssId) );
      pBssDescr->beaconInterval = 100;
   }
   pBssDescr->timeStamp[0]   = pParsedFrame->timeStamp[0];
   pBssDescr->timeStamp[1]   = pParsedFrame->timeStamp[1];
   pBssDescr->capabilityInfo = *((tANI_U16 *)&pParsedFrame->capabilityInfo);
   vos_mem_copy((tANI_U8 *) &pBssDescr->bssId, (tANI_U8 *) macHeader->bssId, sizeof(tSirMacAddr));
   pBssDescr->nReceivedTime = (tANI_TIMESTAMP)palGetTickCount(pMac->hHdd);

   smsLog( pMac, LOG2, "(%s):Bssid= "MAC_ADDRESS_STR
                       " chan= %d, rssi = %d", __func__,
                       MAC_ADDR_ARRAY(pBssDescr->bssId),
                       pBssDescr->channelId,
                       pBssDescr->rssi );

   //IEs
   if (uLen)
   {
      vos_mem_copy(&pBssDescr->ieFields,
                   pPrefNetworkFoundInd->data + (SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET),
                   uLen);
   }

   pIesLocal = (tDot11fBeaconIEs *)( pScanResult->Result.pvIes );
   if ( !pIesLocal &&
       (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
            &pScanResult->Result.BssDescriptor, &pIesLocal))) )
   {
      smsLog(pMac, LOGE, FL("  Cannot parse IEs"));
      csrFreeScanResultEntry(pMac, pScanResult);
      vos_mem_free(pParsedFrame);
      return eHAL_STATUS_RESOURCES;
   }

   fDupBss = csrRemoveDupBssDescription( pMac,
           &pScanResult->Result.BssDescriptor, pIesLocal, &tmpSsid, &timer, FALSE);
   //Check whether we have reach out limit
   if ( CSR_SCAN_IS_OVER_BSS_LIMIT(pMac) )
   {
      //Limit reach
      smsLog(pMac, LOGE, FL("  BSS limit reached"));
      //Free the resources
      if( (pScanResult->Result.pvIes == NULL) && pIesLocal )
      {
         vos_mem_free(pIesLocal);
      }
      csrFreeScanResultEntry(pMac, pScanResult);
      vos_mem_free(pParsedFrame);
      return eHAL_STATUS_RESOURCES;
   }
   //Add to scan cache
   csrScanAddResult(pMac, pScanResult, pIesLocal,
                   pPrefNetworkFoundInd->sessionId);

   if( (pScanResult->Result.pvIes == NULL) && pIesLocal )
   {
      vos_mem_free(pIesLocal);
   }

   vos_mem_free(pParsedFrame);

   return eHAL_STATUS_SUCCESS;
}
#endif //FEATURE_WLAN_SCAN_PNO

#ifdef FEATURE_WLAN_LFR
void csrInitOccupiedChannelsList(tpAniSirGlobal pMac, tANI_U8 sessionId)
{
  tListElem *pEntry = NULL;
  tCsrScanResult *pBssDesc = NULL;
  tDot11fBeaconIEs *pIes = NULL;
  tpCsrNeighborRoamControlInfo pNeighborRoamInfo =
                                      &pMac->roam.neighborRoamInfo[sessionId];

  if (0 != pNeighborRoamInfo->cfgParams.channelInfo.numOfChannels)
  {
       smsLog(pMac, LOG1, FL("Ini file contains neighbor scan channel list, "
             "hence NO need to build occupied channel list (numChannels = %d)"),
              pNeighborRoamInfo->cfgParams.channelInfo.numOfChannels);
      return;
  }

  if (!csrNeighborRoamIsNewConnectedProfile(pMac, sessionId))
  {
      smsLog(pMac, LOG2, FL("donot flush occupied list since current roam "
             "profile matches previous (numChannels = %d)"),
              pMac->scan.occupiedChannels[sessionId].numChannels);
      return;
  }

  /* Empty occupied channels here */
  pMac->scan.occupiedChannels[sessionId].numChannels = 0;

  csrLLLock(&pMac->scan.scanResultList);
  pEntry = csrLLPeekHead(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
  while( pEntry )
  {
      pBssDesc = GET_BASE_ADDR( pEntry, tCsrScanResult, Link );
      pIes = (tDot11fBeaconIEs *)( pBssDesc->Result.pvIes );

      //At this time, pBssDescription->Result.pvIes may be NULL
      if( !pIes && (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
                    &pBssDesc->Result.BssDescriptor, &pIes))) )
      {
          continue;
      }

      csrScanAddToOccupiedChannels(pMac, pBssDesc, sessionId,
                                   &pMac->scan.occupiedChannels[sessionId],
                                   pIes);

      /*
       * Free the memory allocated for pIes in csrGetParsedBssDescriptionIEs
       */
      if( (pBssDesc->Result.pvIes == NULL) && pIes )
      {
          vos_mem_free(pIes);
      }

      pEntry = csrLLNext(&pMac->scan.scanResultList, pEntry, LL_ACCESS_NOLOCK);
  }//while
  csrLLUnlock(&pMac->scan.scanResultList);
}
#endif

eHalStatus csrScanCreateEntryInScanCache(tpAniSirGlobal pMac, tANI_U32 sessionId,
                                         tCsrBssid bssid, tANI_U8 channel)
{
    eHalStatus status = eHAL_STATUS_SUCCESS;
    tDot11fBeaconIEs *pNewIes = NULL;
    tCsrRoamSession *pSession = CSR_GET_SESSION( pMac, sessionId );
    tSirBssDescription *pNewBssDescriptor = NULL;
    tANI_U32 size = 0;

    if(NULL == pSession)
    {
       status = eHAL_STATUS_FAILURE;
       return status;
    }
    smsLog(pMac, LOG2, FL("csrScanCreateEntryInScanCache: Current bssid::"
                          MAC_ADDRESS_STR),
                          MAC_ADDR_ARRAY(pSession->pConnectBssDesc->bssId));
    smsLog(pMac, LOG2, FL("csrScanCreateEntryInScanCache: My bssid::"
                          MAC_ADDRESS_STR" channel %d"),
                          MAC_ADDR_ARRAY(bssid), channel);

    do
    {
        if(!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
                                                             pSession->pConnectBssDesc, &pNewIes)))
        {
            smsLog(pMac, LOGE, FL("%s: Failed to parse IEs"),
                                  __func__);
            status = eHAL_STATUS_FAILURE;
            break;
        }

        size = pSession->pConnectBssDesc->length + sizeof(pSession->pConnectBssDesc->length);
        if (size)
        {
            pNewBssDescriptor = vos_mem_malloc(size);
            if ( NULL == pNewBssDescriptor )
                status = eHAL_STATUS_FAILURE;
            else
                status = eHAL_STATUS_SUCCESS;
            if (HAL_STATUS_SUCCESS(status))
            {
                vos_mem_copy(pNewBssDescriptor, pSession->pConnectBssDesc, size);
            }
            else
            {
                smsLog(pMac, LOGE, FL("%s: memory allocation failed"),
                                      __func__);
                status = eHAL_STATUS_FAILURE;
                break;
            }

            //change the BSSID & channel as passed
            vos_mem_copy(pNewBssDescriptor->bssId, bssid, sizeof(tSirMacAddr));
            pNewBssDescriptor->channelId = channel;
            if (NULL == csrScanAppendBssDescription(pMac, pNewBssDescriptor,
                                                    pNewIes, TRUE, sessionId)) {
                smsLog(pMac, LOGE, FL("csrScanAppendBssDescription failed"));
                status = eHAL_STATUS_FAILURE;
                break;
            }
        }
        else
        {
            smsLog(pMac, LOGE, FL("%s: length of bss descriptor is 0"),
                                  __func__);
            status = eHAL_STATUS_FAILURE;
            break;
        }
        smsLog(pMac, LOGE, FL("%s: entry successfully added in scan cache"),
                              __func__);
    }while(0);

    if(pNewIes)
    {
        vos_mem_free(pNewIes);
    }
    if(pNewBssDescriptor)
    {
        vos_mem_free(pNewBssDescriptor);
    }
    return status;
}

#ifdef FEATURE_WLAN_ESE
//  Update the TSF with the difference in system time
void UpdateCCKMTSF(tANI_U32 *timeStamp0, tANI_U32 *timeStamp1, tANI_U32 *incr)
{
    tANI_U64 timeStamp64 = ((tANI_U64)*timeStamp1 << 32) | (*timeStamp0);
    timeStamp64 = (tANI_U64)(timeStamp64 + (tANI_U64)*incr);
    *timeStamp0 = (tANI_U32)(timeStamp64 & 0xffffffff);
    *timeStamp1 = (tANI_U32)((timeStamp64 >> 32) & 0xffffffff);
}
#endif

/**
 * csrScanSaveRoamOffloadApToScanCache
 * This function parses the received beacon/probe response
 * from the firmware as part of the roam synch indication.
 * The beacon or the probe response is parsed and is also
 * saved into the scan cache
 *
 * @param  pMac Pointer to Global Mac
 * @param  pRoamOffloadSynchInd Roam Synch Indication from
 *         firmware which also contains the beacon/probe
 *         response
 * @return Status
 */
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
eHalStatus csrScanSaveRoamOffloadApToScanCache(tpAniSirGlobal pMac,
            tSirRoamOffloadSynchInd *pRoamOffloadSynchInd)
{
   v_U32_t uLen = 0;
   tANI_BOOLEAN fDupBss;
   tDot11fBeaconIEs *pIesLocal = NULL;
   tAniSSID tmpSsid;
   v_TIME_t timer=0;
   tCsrScanResult *pScanResult = NULL;
   tANI_U8 sessionId = pRoamOffloadSynchInd->roamedVdevId;

   uLen = pRoamOffloadSynchInd->beaconProbeRespLength -
          (SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET);
   pScanResult = vos_mem_malloc(sizeof(tCsrScanResult) + uLen);
   if ( pScanResult == NULL )
   {
      VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
      " fail to allocate memory for frame");
      return eHAL_STATUS_RESOURCES;
   }

   vos_mem_zero(pScanResult, sizeof(tCsrScanResult) + uLen );
   vos_mem_copy(&pScanResult->Result.BssDescriptor,
                pRoamOffloadSynchInd->pbssDescription,
                (sizeof(tSirBssDescription) + uLen));
   pIesLocal = (tDot11fBeaconIEs *)( pScanResult->Result.pvIes );
   if ( !pIesLocal &&
       (!HAL_STATUS_SUCCESS(csrGetParsedBssDescriptionIEs(pMac,
            &pScanResult->Result.BssDescriptor, &pIesLocal))) )
   {
      VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
                "%s:Cannot Parse IEs", __func__);
      csrFreeScanResultEntry(pMac, pScanResult);
      return eHAL_STATUS_RESOURCES;
   }

   fDupBss = csrRemoveDupBssDescription(pMac,
                                        &pScanResult->Result.BssDescriptor,
                                        pIesLocal, &tmpSsid, &timer, TRUE);
   if ( CSR_SCAN_IS_OVER_BSS_LIMIT(pMac) )
   {
      VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
                "%s:BSS Limit Exceed", __func__);
      if( (pScanResult->Result.pvIes == NULL) && pIesLocal )
      {
            vos_mem_free(pIesLocal);
      }
      csrFreeScanResultEntry(pMac, pScanResult);
      return eHAL_STATUS_RESOURCES;
   }
   csrScanAddResult(pMac, pScanResult, pIesLocal, sessionId);
   return eHAL_STATUS_SUCCESS;
}
#endif

/**
 * csr_get_bssdescr_from_scan_handle() - This function to extract
 *                                       first bss description from scan handle
 * @result_handle: an object for the result.
 *
 * This function is written to extract first bss from scan handle.
 *
 * Return: first bss descriptor from the scan handle.
 */
tSirBssDescription*
csr_get_bssdescr_from_scan_handle(tScanResultHandle result_handle,
                                  tSirBssDescription *bss_descr)
{
   tListElem *first_element = NULL;
   tCsrScanResult *scan_result = NULL;
   tScanResultList *bss_list = (tScanResultList *)result_handle;

   if (NULL == bss_list) {
       VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
                 FL("Empty bss_list"));
       return NULL;
   }
   if (csrLLIsListEmpty(&bss_list->List, LL_ACCESS_NOLOCK)) {
       VOS_TRACE(VOS_MODULE_ID_SME, VOS_TRACE_LEVEL_ERROR,
                 FL("bss_list->List is empty"));
       vos_mem_free(bss_list);
       return NULL;
   }
   first_element = csrLLPeekHead(&bss_list->List, LL_ACCESS_NOLOCK);
   if (first_element) {
       scan_result = GET_BASE_ADDR(first_element,
                                   tCsrScanResult,
                                   Link);
       vos_mem_copy(bss_descr,
                    &scan_result->Result.BssDescriptor,
                    sizeof(tSirBssDescription));
   }
   return bss_descr;
}
