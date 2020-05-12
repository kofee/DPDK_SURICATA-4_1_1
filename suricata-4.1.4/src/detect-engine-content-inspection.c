/* Copyright (C) 2007-2017 Open Information Security Foundation
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

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Performs content inspection on any buffer supplied.
 */

#include "suricata-common.h"
#include "suricata.h"

#include "decode.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-parse.h"
#include "detect-content.h"
#include "detect-pcre.h"
#include "detect-isdataat.h"
#include "detect-bytetest.h"
#include "detect-bytejump.h"
#include "detect-byte-extract.h"
#include "detect-replace.h"
#include "detect-engine-content-inspection.h"
#include "detect-uricontent.h"
#include "detect-urilen.h"
#include "detect-bsize.h"
#include "detect-lua.h"
#include "detect-base64-decode.h"
#include "detect-base64-data.h"

#include "app-layer-dcerpc.h"

#include "util-spm.h"
#include "util-debug.h"
#include "util-print.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-profiling.h"

#ifdef HAVE_LUA
#include "util-lua.h"
#endif

/**
 * \brief Run the actual payload match functions
 *
 * The following keywords are inspected:
 * - content, including all the http and dce modified contents
 * - isdaatat
 * - pcre
 * - bytejump
 * - bytetest
 * - byte_extract
 * - urilen
 * -
 *
 * All keywords are evaluated against the buffer with buffer_len.
 *
 * For accounting the last match in relative matching the
 * det_ctx->buffer_offset int is used.
 *
 * \param de_ctx          Detection engine context
 * \param det_ctx         Detection engine thread context
 * \param s               Signature to inspect
 * \param sm              SigMatch to inspect
 * \param f               Flow (for pcre flowvar storage)
 * \param buffer          Ptr to the buffer to inspect
 * \param buffer_len      Length of the payload
 * \param stream_start_offset Indicates the start of the current buffer in
 *                            the whole buffer stream inspected.  This
 *                            applies if the current buffer is inspected
 *                            in chunks.
 * \param inspection_mode Refers to the engine inspection mode we are currently
 *                        inspecting.  Can be payload, stream, one of the http
 *                        buffer inspection modes or dce inspection mode.
 * \param data            Used to send some custom data.  For example in
 *                        payload inspection mode, data contains packet ptr,
 *                        and under dce inspection mode, contains dce state.
 *
 *  \retval 0 no match
 *  \retval 1 match
 */
int DetectEngineContentInspection(DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
                                  const Signature *s, const SigMatchData *smd,
                                  Flow *f,
                                  uint8_t *buffer, uint32_t buffer_len,
                                  uint32_t stream_start_offset, uint8_t flags,
                                  uint8_t inspection_mode, void *data)
{
    SCEnter();
    KEYWORD_PROFILING_START;

    det_ctx->inspection_recursion_counter++;

    if (det_ctx->inspection_recursion_counter == de_ctx->inspection_recursion_limit) {
        det_ctx->discontinue_matching = 1;
        KEYWORD_PROFILING_END(det_ctx, smd->type, 0);
        SCReturnInt(0);
    }

    if (smd == NULL || buffer_len == 0) {
        KEYWORD_PROFILING_END(det_ctx, smd->type, 0);
        SCReturnInt(0);
    }

    /* \todo unify this which is phase 2 of payload inspection unification */
    if (smd->type == DETECT_CONTENT) {

        DetectContentData *cd = (DetectContentData *)smd->ctx;


        /* we might have already have this content matched by the mpm.
         * (if there is any other reason why we'd want to avoid checking
         *  it here, please fill it in) */
        //if (cd->flags & DETECT_CONTENT_NO_DOUBLE_INSPECTION_REQUIRED) {
        //    goto match;
        //}

        /* rule parsers should take care of this */
#ifdef DEBUG
        BUG_ON(cd->depth != 0 && cd->depth <= cd->offset);
#endif

        /* search for our pattern, checking the matches recursively.
         * if we match we look for the next SigMatch as well */
        uint8_t *found = NULL;
        uint32_t offset = 0;
        uint32_t depth = buffer_len;
        uint32_t prev_offset = 0; /**< used in recursive searching */
        uint32_t prev_buffer_offset = det_ctx->buffer_offset;

        do {
            if ((cd->flags & DETECT_CONTENT_DISTANCE) ||
                (cd->flags & DETECT_CONTENT_WITHIN)) {


                offset = prev_buffer_offset;
                depth = buffer_len;

                int distance = cd->distance;
                if (cd->flags & DETECT_CONTENT_DISTANCE) {
                    if (cd->flags & DETECT_CONTENT_DISTANCE_BE) {
                        distance = det_ctx->bj_values[cd->distance];
                    }
                    if (distance < 0 && (uint32_t)(abs(distance)) > offset)
                        offset = 0;
                    else
                        offset += distance;


                               distance, offset, depth);
                }

                if (cd->flags & DETECT_CONTENT_WITHIN) {
                    if (cd->flags & DETECT_CONTENT_WITHIN_BE) {
                        if ((int32_t)depth > (int32_t)(prev_buffer_offset + det_ctx->bj_values[cd->within] + distance)) {
                            depth = prev_buffer_offset + det_ctx->bj_values[cd->within] + distance;
                        }
                    } else {
                        if ((int32_t)depth > (int32_t)(prev_buffer_offset + cd->within + distance)) {
                            depth = prev_buffer_offset + cd->within + distance;
                        }


                                   cd->within, prev_buffer_offset, depth);
                    }

                    if (stream_start_offset != 0 && prev_buffer_offset == 0) {
                        if (depth <= stream_start_offset) {
                            goto no_match;
                        } else if (depth >= (stream_start_offset + buffer_len)) {
                            ;
                        } else {
                            depth = depth - stream_start_offset;
                        }
                    }
                }

                if (cd->flags & DETECT_CONTENT_DEPTH_BE) {
                    if ((det_ctx->bj_values[cd->depth] + prev_buffer_offset) < depth) {
                        depth = prev_buffer_offset + det_ctx->bj_values[cd->depth];
                    }
                } else {
                    if (cd->depth != 0) {
                        if ((cd->depth + prev_buffer_offset) < depth) {
                            depth = prev_buffer_offset + cd->depth;
                        }


                    }
                }

                if (cd->flags & DETECT_CONTENT_OFFSET_BE) {
                    if (det_ctx->bj_values[cd->offset] > offset)
                        offset = det_ctx->bj_values[cd->offset];
                } else {
                    if (cd->offset > offset) {
                        offset = cd->offset;

                    }
                }
            } else { /* implied no relative matches */
                /* set depth */
                if (cd->flags & DETECT_CONTENT_DEPTH_BE) {
                    depth = det_ctx->bj_values[cd->depth];
                } else {
                    if (cd->depth != 0) {
                        depth = cd->depth;
                    }
                }

                if (stream_start_offset != 0 && cd->flags & DETECT_CONTENT_DEPTH) {
                    if (depth <= stream_start_offset) {
                        goto no_match;
                    } else if (depth >= (stream_start_offset + buffer_len)) {
                        ;
                    } else {
                        depth = depth - stream_start_offset;
                    }
                }

                /* set offset */
                if (cd->flags & DETECT_CONTENT_OFFSET_BE)
                    offset = det_ctx->bj_values[cd->offset];
                else
                    offset = cd->offset;
                prev_buffer_offset = 0;
            }

            /* update offset with prev_offset if we're searching for
             * matches after the first occurence. */

            if (prev_offset != 0)
                offset = prev_offset;



            if (depth > buffer_len)
                depth = buffer_len;

            /* if offset is bigger than depth we can never match on a pattern.
             * We can however, "match" on a negated pattern. */
            if (offset > depth || depth == 0) {
                if (cd->flags & DETECT_CONTENT_NEGATED) {
                    goto match;
                } else {
                    goto no_match;
                }
            }

            uint8_t *sbuffer = buffer + offset;
            uint32_t sbuffer_len = depth - offset;
            uint32_t match_offset = 0;

#ifdef DEBUG
            BUG_ON(sbuffer_len > buffer_len);
#endif
            if (cd->flags & DETECT_CONTENT_ENDS_WITH && depth < buffer_len) {

                found = NULL;
            } else if (cd->content_len > sbuffer_len) {
                found = NULL;
            } else {
                /* do the actual search */
                found = SpmScan(cd->spm_ctx, det_ctx->spm_thread_ctx, sbuffer,
                        sbuffer_len);
            }

            /* next we evaluate the result in combination with the
             * negation flag. */


            if (found == NULL && !(cd->flags & DETECT_CONTENT_NEGATED)) {
                if ((cd->flags & (DETECT_CONTENT_DISTANCE|DETECT_CONTENT_WITHIN)) == 0) {
                    /* independent match from previous matches, so failure is fatal */
                    det_ctx->discontinue_matching = 1;
                }

                goto no_match;
            } else if (found == NULL && (cd->flags & DETECT_CONTENT_NEGATED)) {
                goto match;
            } else if (found != NULL && (cd->flags & DETECT_CONTENT_NEGATED)) {

                /* don't bother carrying recursive matches now, for preceding
                 * relative keywords */
                if (DETECT_CONTENT_IS_SINGLE(cd))
                    det_ctx->discontinue_matching = 1;
                goto no_match;
            } else {
                match_offset = (uint32_t)((found - buffer) + cd->content_len);

                det_ctx->buffer_offset = match_offset;

                if ((cd->flags & DETECT_CONTENT_ENDS_WITH) == 0 || match_offset == buffer_len) {
                    /* Match branch, add replace to the list if needed */
                    if (cd->flags & DETECT_CONTENT_REPLACE) {
                        if (inspection_mode == DETECT_ENGINE_CONTENT_INSPECTION_MODE_PAYLOAD) {
                            /* we will need to replace content if match is confirmed */
                            det_ctx->replist = DetectReplaceAddToList(det_ctx->replist, found, cd);
                        } else {
                            SCLogWarning(SC_ERR_INVALID_VALUE, "Can't modify payload without packet");
                        }
                    }

                    /* if this is the last match we're done */
                    if (smd->is_last) {
                        goto match;
                    }


                    KEYWORD_PROFILING_END(det_ctx, smd->type, 1);

                    /* see if the next buffer keywords match. If not, we will
                     * search for another occurence of this content and see
                     * if the others match then until we run out of matches */
                    int r = DetectEngineContentInspection(de_ctx, det_ctx, s, smd+1,
                            f, buffer, buffer_len, stream_start_offset, flags,
                            inspection_mode, data);
                    if (r == 1) {
                        SCReturnInt(1);
                    }


                    if (det_ctx->discontinue_matching) {

                        goto no_match;
                    }

                    /* no match and no reason to look for another instance */
                    if ((cd->flags & DETECT_CONTENT_WITHIN_NEXT) == 0) {

                        det_ctx->discontinue_matching = 1;
                        goto no_match;
                    }


                }
                /* set the previous match offset to the start of this match + 1 */
                prev_offset = (match_offset - (cd->content_len - 1));

            }

        } while(1);

    } else if (smd->type == DETECT_ISDATAAT) {


        const DetectIsdataatData *id = (DetectIsdataatData *)smd->ctx;
        uint32_t dataat = id->dataat;
        if (id->flags & ISDATAAT_OFFSET_BE) {
            uint64_t be_value = det_ctx->bj_values[dataat];
            if (be_value >= 100000000) {
                if ((id->flags & ISDATAAT_NEGATED) == 0) {

                    goto no_match;
                }

                goto match;
            }
            dataat = (uint32_t)be_value;

        }

        if (id->flags & ISDATAAT_RELATIVE) {
            if (det_ctx->buffer_offset + dataat > buffer_len) {

                if (id->flags & ISDATAAT_NEGATED)
                    goto match;
                goto no_match;
            } else {

                if (id->flags & ISDATAAT_NEGATED)
                    goto no_match;
                goto match;
            }
        } else {
            if (dataat < buffer_len) {

                if (id->flags & ISDATAAT_NEGATED)
                    goto no_match;
                goto match;
            } else {

                if (id->flags & ISDATAAT_NEGATED)
                    goto match;
                goto no_match;
            }
        }

    } else if (smd->type == DETECT_PCRE) {

        DetectPcreData *pe = (DetectPcreData *)smd->ctx;
        uint32_t prev_buffer_offset = det_ctx->buffer_offset;
        uint32_t prev_offset = 0;
        int r = 0;

        det_ctx->pcre_match_start_offset = 0;
        do {
            Packet *p = NULL;
            if (inspection_mode == DETECT_ENGINE_CONTENT_INSPECTION_MODE_PAYLOAD)
                p = (Packet *)data;
            r = DetectPcrePayloadMatch(det_ctx, s, smd, p, f,
                                       buffer, buffer_len);
            if (r == 0) {
                goto no_match;
            }

            if (!(pe->flags & DETECT_PCRE_RELATIVE_NEXT)) {

                goto match;
            }
            KEYWORD_PROFILING_END(det_ctx, smd->type, 1);

            /* save it, in case we need to do a pcre match once again */
            prev_offset = det_ctx->pcre_match_start_offset;

            /* see if the next payload keywords match. If not, we will
             * search for another occurence of this pcre and see
             * if the others match, until we run out of matches */
            r = DetectEngineContentInspection(de_ctx, det_ctx, s, smd+1,
                    f, buffer, buffer_len, stream_start_offset, flags,
                    inspection_mode, data);
            if (r == 1) {
                SCReturnInt(1);
            }

            if (det_ctx->discontinue_matching)
                goto no_match;

            det_ctx->buffer_offset = prev_buffer_offset;
            det_ctx->pcre_match_start_offset = prev_offset;
        } while (1);

    } else if (smd->type == DETECT_BYTETEST) {
        DetectBytetestData *btd = (DetectBytetestData *)smd->ctx;
        uint8_t btflags = btd->flags;
        int32_t offset = btd->offset;
        uint64_t value = btd->value;
        if (btflags & DETECT_BYTETEST_OFFSET_BE) {
            offset = det_ctx->bj_values[offset];
        }
        if (btflags & DETECT_BYTETEST_VALUE_BE) {
            value = det_ctx->bj_values[value];
        }

        /* if we have dce enabled we will have to use the endianness
         * specified by the dce header */
        if (btflags & DETECT_BYTETEST_DCE && data != NULL) {
            DCERPCState *dcerpc_state = (DCERPCState *)data;
            /* enable the endianness flag temporarily.  once we are done
             * processing we reset the flags to the original value*/
            btflags |= ((dcerpc_state->dcerpc.dcerpchdr.packed_drep[0] & 0x10) ?
                      DETECT_BYTETEST_LITTLE: 0);
        }

        if (DetectBytetestDoMatch(det_ctx, s, smd->ctx, buffer, buffer_len, btflags,
                                  offset, value) != 1) {
            goto no_match;
        }

        goto match;

    } else if (smd->type == DETECT_BYTEJUMP) {
        DetectBytejumpData *bjd = (DetectBytejumpData *)smd->ctx;
        uint8_t bjflags = bjd->flags;
        int32_t offset = bjd->offset;

        if (bjflags & DETECT_BYTEJUMP_OFFSET_BE) {
            offset = det_ctx->bj_values[offset];
        }

        /* if we have dce enabled we will have to use the endianness
         * specified by the dce header */
        if (bjflags & DETECT_BYTEJUMP_DCE && data != NULL) {
            DCERPCState *dcerpc_state = (DCERPCState *)data;
            /* enable the endianness flag temporarily.  once we are done
             * processing we reset the flags to the original value*/
            bjflags |= ((dcerpc_state->dcerpc.dcerpchdr.packed_drep[0] & 0x10) ?
                      DETECT_BYTEJUMP_LITTLE: 0);
        }

        if (DetectBytejumpDoMatch(det_ctx, s, smd->ctx, buffer, buffer_len,
                                  bjflags, offset) != 1) {
            goto no_match;
        }

        goto match;

    } else if (smd->type == DETECT_BYTE_EXTRACT) {

        DetectByteExtractData *bed = (DetectByteExtractData *)smd->ctx;
        uint8_t endian = bed->endian;

        /* if we have dce enabled we will have to use the endianness
         * specified by the dce header */
        if ((bed->flags & DETECT_BYTE_EXTRACT_FLAG_ENDIAN) &&
            endian == DETECT_BYTE_EXTRACT_ENDIAN_DCE && data != NULL) {

            DCERPCState *dcerpc_state = (DCERPCState *)data;
            /* enable the endianness flag temporarily.  once we are done
             * processing we reset the flags to the original value*/
            endian |= ((dcerpc_state->dcerpc.dcerpchdr.packed_drep[0] == 0x10) ?
                       DETECT_BYTE_EXTRACT_ENDIAN_LITTLE : DETECT_BYTE_EXTRACT_ENDIAN_BIG);
        }

        if (DetectByteExtractDoMatch(det_ctx, smd, s, buffer,
                                     buffer_len,
                                     &det_ctx->bj_values[bed->local_id],
                                     endian) != 1) {
            goto no_match;
        }

        goto match;

    } else if (smd->type == DETECT_BSIZE) {

        bool eof = (flags & DETECT_CI_FLAGS_END);
        const uint64_t data_size = buffer_len + stream_start_offset;
        int r = DetectBsizeMatch(smd->ctx, data_size, eof);
        if (r < 0) {
            det_ctx->discontinue_matching = 1;
            goto no_match;

        } else if (r == 0) {
            goto no_match;
        }
        goto match;

    } else if (smd->type == DETECT_AL_URILEN) {


        int r = 0;
        DetectUrilenData *urilend = (DetectUrilenData *) smd->ctx;

        switch (urilend->mode) {
            case DETECT_URILEN_EQ:
                if (buffer_len == urilend->urilen1)
                    r = 1;
                break;
            case DETECT_URILEN_LT:
                if (buffer_len < urilend->urilen1)
                    r = 1;
                break;
            case DETECT_URILEN_GT:
                if (buffer_len > urilend->urilen1)
                    r = 1;
                break;
            case DETECT_URILEN_RA:
                if (buffer_len > urilend->urilen1 &&
                    buffer_len < urilend->urilen2) {
                    r = 1;
                }
                break;
        }

        if (r == 1) {
            goto match;
        }

        det_ctx->discontinue_matching = 0;

        goto no_match;
#ifdef HAVE_LUA
    }
    else if (smd->type == DETECT_LUA) {


        if (DetectLuaMatchBuffer(det_ctx, s, smd, buffer, buffer_len,
                    det_ctx->buffer_offset, f) != 1)
        {

            goto no_match;
        }

        goto match;
#endif /* HAVE_LUA */
    } else if (smd->type == DETECT_BASE64_DECODE) {
        if (DetectBase64DecodeDoMatch(det_ctx, s, smd, buffer, buffer_len)) {
            if (s->sm_arrays[DETECT_SM_LIST_BASE64_DATA] != NULL) {
                KEYWORD_PROFILING_END(det_ctx, smd->type, 1);
                if (DetectBase64DataDoMatch(de_ctx, det_ctx, s, f)) {
                    /* Base64 is a terminal list. */
                    goto final_match;
                }
            }
        }
    } else {

#ifdef DEBUG
        BUG_ON(1);
#endif
    }

no_match:
    KEYWORD_PROFILING_END(det_ctx, smd->type, 0);
    SCReturnInt(0);

match:
    /* this sigmatch matched, inspect the next one. If it was the last,
     * the buffer portion of the signature matched. */
    if (!smd->is_last) {
        KEYWORD_PROFILING_END(det_ctx, smd->type, 1);
        int r = DetectEngineContentInspection(de_ctx, det_ctx, s, smd+1,
                f, buffer, buffer_len, stream_start_offset, flags,
                inspection_mode, data);
        SCReturnInt(r);
    }
final_match:
    KEYWORD_PROFILING_END(det_ctx, smd->type, 1);
    SCReturnInt(1);
}

#ifdef UNITTESTS
#include "tests/detect-engine-content-inspection.c"
#endif
