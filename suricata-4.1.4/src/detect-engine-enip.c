/* Copyright (C) 2015 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/** \file
 *
 *  \author Kevin Wong <kwong@solananetworks.com>
 *
 *  Based on detect-engine-modbus.c
 */

#include "suricata-common.h"

#include "app-layer.h"

#include "detect.h"
#include "detect-cipservice.h"
#include "detect-engine-enip.h"

#include "flow.h"

#include "util-debug.h"

#if 0
/**
 * \brief Print fields from ENIP Packet
 * @param enip_data
 */
void PrintENIPAL(ENIPTransaction *enip_data)
{


            enip_data->header.command, enip_data->header.length,
            enip_data->header.session, enip_data->header.status);
    //

            enip_data->encap_data_header.interface_handle,
            enip_data->encap_data_header.timeout,
            enip_data->encap_data_header.item_count);

            enip_data->encap_addr_item.type, enip_data->encap_addr_item.length);

            enip_data->encap_data_item.type, enip_data->encap_data_item.length,
            enip_data->encap_data_item.sequence_count);

    CIPServiceEntry *svc = NULL;

    int count = 0;
    TAILQ_FOREACH(svc, &enip_data->service_list, next)
    {
        //
        count++;
    }
}
#endif

/**
 * \brief Matches the rule to the CIP segment in ENIP Packet
 * @param svc - the CIP service entry
 * * @param cipserviced - the CIP service rule
 */
static int CIPPathMatch(CIPServiceEntry *svc, DetectCipServiceData *cipserviced)
{
    uint16_t class = 0;
    uint16_t attrib = 0;
    int found_class = 0;

    SegmentEntry *seg = NULL;
    TAILQ_FOREACH(seg, &svc->segment_list, next)
    {
        switch(seg->segment)
        {
            case PATH_CLASS_8BIT:
                class = seg->value;
                if (cipserviced->cipclass == class)
                {
                    if (cipserviced->tokens == 2)
                    {// if rule only has class
                        return 1;
                    } else
                    {
                        found_class = 1;
                    }
                }
                break;
            case PATH_INSTANCE_8BIT:
                break;
            case PATH_ATTR_8BIT: //single attribute
                attrib = seg->value;
                if ((cipserviced->tokens == 3) &&
                        (cipserviced->cipclass == class) &&
                        (cipserviced->cipattribute == attrib) &&
                        (cipserviced->matchattribute == 1))
                { // if rule has class & attribute, matched all here
                    return 1;
                }
                if ((cipserviced->tokens == 3) &&
                        (cipserviced->cipclass == class) &&
                        (cipserviced->matchattribute == 0))
                { // for negation rule on attribute
                    return 1;
                }
                break;
            case PATH_CLASS_16BIT:
                class = seg->value;
                if (cipserviced->cipclass == class)
                {
                    if (cipserviced->tokens == 2)
                    {// if rule only has class
                        return 1;
                    } else
                    {
                        found_class = 1;
                    }
                }
                break;
            case PATH_INSTANCE_16BIT:
                break;
            default:
                return 0;
        }
    }

    if (found_class == 0)
    { // if haven't matched class yet, no need to check attribute
        return 0;
    }

    if ((svc->service == CIP_SET_ATTR_LIST) ||
            (svc->service == CIP_GET_ATTR_LIST))
    {
        AttributeEntry *attr = NULL;
        TAILQ_FOREACH    (attr, &svc->attrib_list, next)
        {
            if (cipserviced->cipattribute == attr->attribute)
            {
                return 1;
            }
        }
    }

    return 0;
}

/**
 * \brief Matches the rule to the ENIP Transaction
 * @param enip_data - the ENIP transation
 * * @param cipserviced - the CIP service rule
 */

static int CIPServiceMatch(ENIPTransaction *enip_data,
        DetectCipServiceData *cipserviced)
{
    int count = 1;
    CIPServiceEntry *svc = NULL;
    //
    TAILQ_FOREACH(svc, &enip_data->service_list, next)
    {

                count, svc->service,  svc->direction);

        if (cipserviced->cipservice == svc->service)
        { // compare service
            //

            if (cipserviced->tokens > 1)
            { //if rule params have class and attribute


                if ((svc->service == CIP_SET_ATTR_LIST) || (svc->service
                                == CIP_SET_ATTR_SINGLE) || (svc->service
                                == CIP_GET_ATTR_LIST) || (svc->service
                                == CIP_GET_ATTR_SINGLE))
                { //decode path
                    if (CIPPathMatch(svc, cipserviced) == 1)
                    {
                        if (svc->direction == 1) return 0; //don't match responses

                        return 1;
                    }
                }
            } else
            {
                if (svc->direction == 1) return 0; //don't match responses

                //
                return 1;
            }
        }
        count++;
    }
    return 0;
}

/** \brief Do the content inspection & validation for a signature
 *
 *  \param de_ctx   Detection engine context
 *  \param det_ctx  Detection engine thread context
 *  \param s        Signature to inspect ( and sm: SigMatch to inspect)
 *  \param f        Flow
 *  \param flags    App layer flags
 *  \param alstate  App layer state
 *  \param txv      Pointer to ENIP Transaction structure
 *
 *  \retval 0 no match or 1 match
 */
int DetectEngineInspectCIP(ThreadVars *tv,
        DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
        const Signature *s, const SigMatchData *smd, Flow *f, uint8_t flags,
        void *alstate, void *txv, uint64_t tx_id)
{
    SCEnter();


    ENIPTransaction *tx = (ENIPTransaction *) txv;
    DetectCipServiceData *cipserviced = (DetectCipServiceData *) smd->ctx;

    if (cipserviced == NULL)
    {

        SCReturnInt(0);
    }
    //

    if (CIPServiceMatch(tx, cipserviced) == 1)
    {
        //
        SCReturnInt(1);
    }

    SCReturnInt(0);
}

/** \brief Do the content inspection & validation for a signature
 *
 *  \param de_ctx   Detection engine context
 *  \param det_ctx  Detection engine thread context
 *  \param s        Signature to inspect ( and sm: SigMatch to inspect)
 *  \param f        Flow
 *  \param flags    App layer flags
 *  \param alstate  App layer state
 *  \param txv      Pointer to ENIP Transaction structure
 *
 *  \retval 0 no match or 1 match
 */

int DetectEngineInspectENIP(ThreadVars *tv,
        DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
        const Signature *s, const SigMatchData *smd,
        Flow *f, uint8_t flags, void *alstate, void *txv, uint64_t tx_id)
{
    SCEnter();

    ENIPTransaction *tx = (ENIPTransaction *) txv;
    DetectEnipCommandData *enipcmdd = (DetectEnipCommandData *) smd->ctx;

    if (enipcmdd == NULL)
    {

        SCReturnInt(0);
    }

    //

    if (enipcmdd->enipcommand == tx->header.command)
    {
        //
        SCReturnInt(1);
    }

    SCReturnInt(0);
}

#ifdef UNITTESTS /* UNITTESTS */
#include "app-layer-parser.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "flow-util.h"
#include "stream-tcp.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

static uint8_t listIdentity[] = {/* List ID */    0x00, 0x63,
                                 /* Length */     0x00, 0x00,
                                 /* Session */    0x00, 0x00, 0x00, 0x00,
                                 /* Status */     0x00, 0x00, 0x00, 0x00,
                                 /*  Delay*/      0x00,
                                 /* Context */    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 /* Quantity of coils */ 0x00, 0x00, 0x00, 0x00,};

/** \test Test code function. */
static int DetectEngineInspectENIPTest01(void)
{
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();
    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = NULL;
    Flow f;
    Packet *p = NULL;
    Signature *s = NULL;
    TcpSession ssn;
    ThreadVars tv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&f, 0, sizeof(Flow));
    memset(&ssn, 0, sizeof(TcpSession));

    p = UTHBuildPacket(listIdentity, sizeof(listIdentity), IPPROTO_TCP);
    FAIL_IF_NULL(p);

    FLOW_INITIALIZE(&f);
    f.alproto   = ALPROTO_ENIP;
    f.protoctx  = (void *)&ssn;
    f.proto     = IPPROTO_TCP;
    f.flags     |= FLOW_IPV4;

    p->flow         = &f;
    p->flags        |= PKT_HAS_FLOW | PKT_STREAM_EST;
    p->flowflags    |= FLOW_PKT_TOSERVER | FLOW_PKT_ESTABLISHED;

    StreamTcpInitConfig(TRUE);

    de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert enip any any -> any any "
            "(msg:\"Testing enip command\"; "
            "enipcommand:99 ; sid:1;)");
    FAIL_IF_NULL(s);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&tv, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_ENIP, STREAM_TOSERVER,
            listIdentity, sizeof(listIdentity));
    FAIL_IF(r != 0);

    ENIPState    *enip_state = f.alstate;
    FAIL_IF_NULL(enip_state);

    /* do detect */
    SigMatchSignatures(&tv, de_ctx, det_ctx, p);

    FAIL_IF(!(PacketAlertCheck(p, 1)));

    AppLayerParserThreadCtxFree(alp_tctx);
    DetectEngineThreadCtxDeinit(&tv, det_ctx);
    DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePacket(p);

    PASS;
}

#endif /* UNITTESTS */

void DetectEngineInspectENIPRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectEngineInspectENIPTest01", DetectEngineInspectENIPTest01);
#endif /* UNITTESTS */
    return;
}
