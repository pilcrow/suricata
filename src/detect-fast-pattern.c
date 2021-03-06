/* Copyright (C) 2007-2010 Open Information Security Foundation
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
 * \author Anoop Saldanha <poonaatsoc@gmail.com>
 *
 * Implements the fast_pattern keyword
 */

#include "suricata-common.h"
#include "detect.h"
#include "flow.h"
#include "detect-content.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-fast-pattern.h"

#include "util-error.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#define DETECT_FAST_PATTERN_REGEX "^(\\s*only\\s*)|\\s*([0-9]+)\\s*,\\s*([0-9]+)\\s*$"

static pcre *parse_regex = NULL;
static pcre_extra *parse_regex_study = NULL;

static int DetectFastPatternSetup(DetectEngineCtx *, Signature *, char *);
void DetectFastPatternRegisterTests(void);

/* holds the list of sm's that should be given fp support */
SCFPSupportSMType *sm_fp_support_smtype_list = NULL;
/* holds the list of sm match lists that need to be searched for a keyword
 * that has fp support */
SCFPSupportSMList *sm_fp_support_smlist_list = NULL;

/**
 * \brief Lets one add a sm list id to be searched for potential fp supported
 *        keywords later.
 *
 * \param list_id SM list id.
 */
static void SupportFastPatternForSigMatchList(int list_id)
{
    if (sm_fp_support_smlist_list != NULL) {
        SCFPSupportSMList *tmp_smlist_fp = sm_fp_support_smlist_list;
        while (tmp_smlist_fp != NULL) {
            if (tmp_smlist_fp->list_id == list_id)
                return;
            tmp_smlist_fp = tmp_smlist_fp->next;
        }
    }

    SCFPSupportSMList *new_smlist_fp = malloc(sizeof(SCFPSupportSMList));
    if (new_smlist_fp == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memset(new_smlist_fp, 0, sizeof(SCFPSupportSMList));
    new_smlist_fp->list_id = list_id;

    new_smlist_fp->next = sm_fp_support_smlist_list;
    sm_fp_support_smlist_list = new_smlist_fp;

    return;
}

/**
 * \brief Lets one add a sigmatch type for fast pattern support(explains the weird
 *        name the function has).
 *
 * \param sm_type The sigmatch for which fp support has to be added.
 */
static void SupportFastPatternForSigMatchType(uint8_t sm_type)
{
    if (sm_fp_support_smtype_list != NULL) {
        SCFPSupportSMType *tmp_smtype_fp = sm_fp_support_smtype_list;
        while (tmp_smtype_fp != NULL) {
            if (tmp_smtype_fp->sm_type == sm_type) {
                return;
            }
            tmp_smtype_fp = tmp_smtype_fp->next;
        }
    }

    SCFPSupportSMType *new_smtype_fp = malloc(sizeof(SCFPSupportSMType));
    if (new_smtype_fp == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memset(new_smtype_fp, 0, sizeof(SCFPSupportSMType));
    new_smtype_fp->sm_type = sm_type;

    new_smtype_fp->next = sm_fp_support_smtype_list;
    sm_fp_support_smtype_list = new_smtype_fp;

    return;
}

/**
 * \brief Registers the keywords(SMs) that should be given fp support.
 */
void SupportFastPatternForSigMatchTypes(void)
{
    SupportFastPatternForSigMatchType(DETECT_CONTENT);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_PMATCH);

    SupportFastPatternForSigMatchType(DETECT_URICONTENT);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_UMATCH);

    SupportFastPatternForSigMatchType(DETECT_AL_HTTP_CLIENT_BODY);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_HCBDMATCH);

    SupportFastPatternForSigMatchType(DETECT_AL_HTTP_HEADER);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_HHDMATCH);

    SupportFastPatternForSigMatchType(DETECT_AL_HTTP_RAW_HEADER);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_HRHDMATCH);

    SupportFastPatternForSigMatchType(DETECT_AL_HTTP_METHOD);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_HMDMATCH);

    SupportFastPatternForSigMatchType(DETECT_AL_HTTP_COOKIE);
    SupportFastPatternForSigMatchList(DETECT_SM_LIST_HCDMATCH);

    return;
}

/**
 * \brief Registration function for fast_pattern keyword
 */
void DetectFastPatternRegister(void)
{
    sigmatch_table[DETECT_FAST_PATTERN].name = "fast_pattern";
    sigmatch_table[DETECT_FAST_PATTERN].Match = NULL;
    sigmatch_table[DETECT_FAST_PATTERN].Setup = DetectFastPatternSetup;
    sigmatch_table[DETECT_FAST_PATTERN].Free  = NULL;
    sigmatch_table[DETECT_FAST_PATTERN].RegisterTests = DetectFastPatternRegisterTests;

    sigmatch_table[DETECT_FAST_PATTERN].flags |= SIGMATCH_PAYLOAD;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(DETECT_FAST_PATTERN_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at "
                   "offset %" PRId32 ": %s", DETECT_FAST_PATTERN_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }

    return;

 error:
    /* get some way to return an error code! */
    return;
}

//static int DetectFastPatternParseArg(

/**
 * \brief Configures the previous content context for a fast_pattern modifier
 *        keyword used in the rule.
 *
 * \param de_ctx   Pointer to the Detection Engine Context.
 * \param s        Pointer to the Signature to which the current keyword belongs.
 * \param m        Pointer to the SigMatch.
 * \param null_str Should hold an empty string always.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
static int DetectFastPatternSetup(DetectEngineCtx *de_ctx, Signature *s, char *arg)
{
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];
    const char *arg_substr = NULL;
    DetectContentData *cd = NULL;

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL &&
        s->sm_lists_tail[DETECT_SM_LIST_UMATCH] == NULL &&
        s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH] == NULL &&
        s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH] == NULL &&
        s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH] == NULL &&
        s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH] == NULL &&
        s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH] == NULL) {
        SCLogWarning(SC_WARN_COMPATIBILITY, "fast_pattern found inside the "
                     "rule, without a preceding content based keyword.  "
                     "Currently we provide fast_pattern support for content, "
                     "uricontent, http_client_body, http_header, "
                     "http_raw_header, http_method or http_cookie option");
        return -1;
    }

    SigMatch *pm = SigMatchGetLastSMFromLists(s, 12,
                                              DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                              DETECT_URICONTENT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                              DETECT_AL_HTTP_CLIENT_BODY, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                              DETECT_AL_HTTP_HEADER, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                              DETECT_AL_HTTP_RAW_HEADER, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                              DETECT_AL_HTTP_METHOD, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                              DETECT_AL_HTTP_COOKIE, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]);
    if (pm == NULL) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "fast_pattern found inside "
                   "the rule, without a content context. Please use a "
                   "content based keyword before using fast_pattern");
        return -1;
    }

    cd = pm->ctx;
    if (cd->flags & DETECT_CONTENT_NEGATED &&
        (cd->flags & DETECT_CONTENT_DISTANCE ||
         cd->flags & DETECT_CONTENT_WITHIN ||
         cd->flags & DETECT_CONTENT_OFFSET ||
         cd->flags & DETECT_CONTENT_DEPTH)) {

        /* we can't have any of these if we are having "only" */
        SCLogError(SC_ERR_INVALID_SIGNATURE, "fast_pattern; cannot be "
                   "used with negated content, along with relative modifiers.");
        goto error;
    }

    if (arg == NULL|| strcmp(arg, "") == 0) {
        cd->flags |= DETECT_CONTENT_FAST_PATTERN;
        return 0;
    }

    /* Execute the regex and populate args with captures. */
    ret = pcre_exec(parse_regex, parse_regex_study, arg,
                    strlen(arg), 0, 0, ov, MAX_SUBSTRINGS);
    /* fast pattern only */
    if (ret == 2) {
        if (cd->flags & DETECT_CONTENT_NEGATED ||
            cd->flags & DETECT_CONTENT_DISTANCE ||
            cd->flags & DETECT_CONTENT_WITHIN ||
            cd->flags & DETECT_CONTENT_OFFSET ||
            cd->flags & DETECT_CONTENT_DEPTH) {

            /* we can't have any of these if we are having "only" */
            SCLogError(SC_ERR_INVALID_SIGNATURE, "fast_pattern: only; cannot be "
                       "used with negated content or with any of the relative "
                       "modifiers like distance, within, offset, depth");
            goto error;
        }
        cd->flags |= DETECT_CONTENT_FAST_PATTERN_ONLY;

        /* fast pattern chop */
    } else if (ret == 4) {
        res = pcre_get_substring((char *)arg, ov, MAX_SUBSTRINGS,
                                 2, &arg_substr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                       "for fast_pattern offset");
            goto error;
        }
        int offset = atoi(arg_substr);
        if (offset > 65535) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Fast pattern offset exceeds "
                       "limit");
            goto error;
        }

        res = pcre_get_substring((char *)arg, ov, MAX_SUBSTRINGS,
                                 3, &arg_substr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                       "for fast_pattern offset");
            goto error;
        }
        int length = atoi(arg_substr);
        if (offset > 65535) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Fast pattern length exceeds "
                       "limit");
            goto error;
        }

        if (offset + length > 65535) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Fast pattern (length + offset) "
                       "exceeds limit pattern length limit");
            goto error;
        }

        if (offset + length > cd->content_len) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Fast pattern (length + "
                       "offset (%u)) exceeds pattern length (%u)",
                       offset + length, cd->content_len);
            goto error;
        }

        cd->fp_chop_offset = offset;
        cd->fp_chop_len = length;
        cd->flags |= DETECT_CONTENT_FAST_PATTERN_CHOP;

    } else {
        SCLogError(SC_ERR_PCRE_PARSE, "parse error, ret %" PRId32
                   ", string %s", ret, arg);
        goto error;
    }

    //int args;
    //args = 0;
    //printf("ret-%d\n", ret);
    //for (args = 0; args < ret; args++) {
    //    res = pcre_get_substring((char *)arg, ov, MAX_SUBSTRINGS,
    //                             args, &arg_substr);
    //    if (res < 0) {
    //        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
    //                   "for arg 1");
    //        goto error;
    //    }
    //    printf("%d-%s\n", args, arg_substr);
    //}

    cd->flags |= DETECT_CONTENT_FAST_PATTERN;

    return 0;

 error:
    return -1;
}

/*----------------------------------Unittests---------------------------------*/

#ifdef UNITTESTS

/**
 * \test Checks if a fast_pattern is registered in a Signature
 */
int DetectFastPatternTest01(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; tcpv4-csum:valid; fast_pattern; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_CONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature
 */
int DetectFastPatternTest02(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern; "
                               "content:boo; fast_pattern; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_CONTENT) {
            if (((DetectContentData *)sm->ctx)->flags &
                DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that we have no fast_pattern registerd for a Signature when the
 *       Signature doesn't contain a fast_pattern
 */
int DetectFastPatternTest03(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_CONTENT) {
            if ( !(((DetectContentData *)sm->ctx)->flags &
                   DETECT_CONTENT_FAST_PATTERN)) {
                result = 1;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a fast_pattern is not registered in a Signature, when we
 *       supply a fast_pattern with an argument
 */
int DetectFastPatternTest04(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:boo; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a fast_pattern is used in the mpm phase.
 */
int DetectFastPatternTest05(void)
{
    uint8_t *buf = (uint8_t *) "Oh strin1.  But what "
        "strin2.  This is strings3.  We strins_str4. we "
        "have strins_string5";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));

    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; fast_pattern; "
                               "content:strings_str4; content:strings_string5; "
                               "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    if (PacketPatternSearch(det_ctx, p) != 0)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

end:
    UTHFreePackets(&p, 1);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a fast_pattern is used in the mpm phase.
 */
int DetectFastPatternTest06(void)
{
    uint8_t *buf = (uint8_t *) "Oh this is a string1.  But what is this with "
        "string2.  This is strings3.  We have strings_str4.  We also have "
        "strings_string5";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; fast_pattern; "
                               "content:strings_str4; content:strings_string5; "
                               "sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    if (PacketPatternSearch(det_ctx, p) != 0)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

end:
    UTHFreePackets(&p, 1);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a fast_pattern is used in the mpm phase, when the payload
 *       doesn't contain the fast_pattern string within it.
 */
int DetectFastPatternTest07(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  now here comes our "
        "dark knight strings_string5.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; fast_pattern; "
                               "content:strings_str4; content:strings_string5; "
                               "sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    if (PacketPatternSearch(det_ctx, p) == 0)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

end:
    UTHFreePackets(&p, 1);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a fast_pattern is used in the mpm phase and that we get
 *       exactly 1 match for the mpm phase.
 */
int DetectFastPatternTest08(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  now here comes our "
        "dark knight strings3.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        printf("de_ctx init: ");
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; fast_pattern; "
                               "content:strings_str4; content:strings_string5; "
                               "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    uint32_t r = PacketPatternSearch(det_ctx, p);
    if (r != 1) {
        printf("expected 1, got %"PRIu32": ", r);
        goto end;
    }

    result = 1;
end:
    UTHFreePackets(&p, 1);
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}
/**
 * \test Checks that a fast_pattern is used in the mpm phase, when the payload
 *       doesn't contain the fast_pattern string within it.
 */
int DetectFastPatternTest09(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  no_strings4 _imp now here "
        "comes our dark knight strings3.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; "
                               "content:strings4_imp; fast_pattern; "
                               "content:strings_string5; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    if (PacketPatternSearch(det_ctx, p) == 0)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

end:
    UTHFreePackets(&p, 1);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a the SigInit chooses the fast_pattern with better pattern
 *       strength, when we have multiple fast_patterns in the Signature.  Also
 *       checks that we get a match for the fast_pattern from the mpm phase.
 */
int DetectFastPatternTest10(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  strings4_imp now here "
        "comes our dark knight strings5.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        printf("de_ctx init: ");
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; "
                               "content:strings4_imp; fast_pattern; "
                               "content:strings_string5; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    uint32_t r = PacketPatternSearch(det_ctx, p);
    if (r != 1) {
        printf("expected 1, got %"PRIu32": ", r);
        goto end;
    }

    result = 1;
end:
    UTHFreePackets(&p, 1);
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a the SigInit chooses the fast_pattern with better pattern
 *       strength, when we have multiple fast_patterns in the Signature.  Also
 *       checks that we get no matches for the fast_pattern from the mpm phase.
 */
int DetectFastPatternTest11(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  strings5_imp now here "
        "comes our dark knight strings5.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; fast_pattern; "
                               "content:strings4_imp; fast_pattern; "
                               "content:strings_string5; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    if (PacketPatternSearch(det_ctx, p) == 0)
        result = 1;


end:
    UTHFreePackets(&p, 1);
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
        if (det_ctx != NULL)
            DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    return result;
}

/**
 * \test Checks that we don't get a match for the mpm phase.
 */
int DetectFastPatternTest12(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  strings5_imp now here "
        "comes our dark knight strings5.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; "
                               "content:strings4_imp; "
                               "content:strings_string5; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    if (PacketPatternSearch(det_ctx, p) == 0)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

end:
    UTHFreePackets(&p, 1);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks that a the SigInit chooses the fast_pattern with a better
 *       strength from the available patterns, when we don't specify a
 *       fast_pattern.  We also check that we get a match from the mpm
 *       phase.
 */
int DetectFastPatternTest13(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  strings5_imp now here "
        "comes our dark knight strings_string5.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        printf("de_ctx init: ");
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:string1; "
                               "content:string2; content:strings3; "
                               "content:strings4_imp; "
                               "content:strings_string5; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* start the search phase */
    det_ctx->sgh = SigMatchSignaturesGetSgh(de_ctx, det_ctx, p);
    uint32_t r = PacketPatternSearch(det_ctx, p);
    if (r != 1) {
        printf("expected 1 result, got %"PRIu32": ", r);
        goto end;
    }

    result = 1;
end:
    UTHFreePackets(&p, 1);
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks to make sure that other sigs work that should when fast_pattern is inspecting on the same payload
 *
 */
int DetectFastPatternTest14(void)
{
    uint8_t *buf = (uint8_t *) "Dummy is our name.  Oh yes.  From right here "
        "right now, all the way to hangover.  right.  strings5_imp now here "
        "comes our dark knight strings_string5.  Yes here is our dark knight";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    int alertcnt = 0;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    p = UTHBuildPacket(buf,buflen,IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    FlowInitConfig(FLOW_QUIET);

    de_ctx->mpm_matcher = MPM_B3G;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"fast_pattern test\"; content:\"strings_string5\"; content:\"knight\"; fast_pattern; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    de_ctx->sig_list->next = SigInit(de_ctx, "alert tcp any any -> any any "
                                     "(msg:\"test different content\"; content:\"Dummy is our name\"; sid:2;)");
    if (de_ctx->sig_list->next == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)){
        alertcnt++;
    }else{
        SCLogInfo("could not match on sig 1 with when fast_pattern is inspecting payload");
        goto end;
    }
    if (PacketAlertCheck(p, 2)){
        result = 1;
    }else{
        SCLogInfo("match on sig 1 fast_pattern no match sig 2 inspecting same payload");
    }
end:
    UTHFreePackets(&p, 1);
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

    DetectEngineCtxFree(de_ctx);
    FlowShutdown();
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature
 */
int DetectFastPatternTest15(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_CONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature
 */
int DetectFastPatternTest16(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_CONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest17(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    DetectContentData *cd = sm->ctx;
    if (sm != NULL && sm->type == DETECT_CONTENT) {
        if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
            cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            cd->fp_chop_offset == 0 &&
            cd->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest18(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH];
    DetectContentData *cd = sm->ctx;
    if (sm != NULL && sm->type == DETECT_CONTENT) {
        if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            cd->fp_chop_offset == 3 &&
            cd->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest19(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; fast_pattern:only; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest20(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; distance:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest21(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; fast_pattern:only; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest22(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; within:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest23(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; fast_pattern:only; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest24(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; offset:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest25(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; fast_pattern:only; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest26(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; depth:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest27(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:!two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest28(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content: one; content:two; distance:30; content:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        cd->fp_chop_offset == 0 &&
        cd->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest29(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; within:30; content:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        cd->fp_chop_offset == 0 &&
        cd->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest30(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; offset:30; content:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        cd->fp_chop_offset == 0 &&
        cd->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest31(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; depth:30; content:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        cd->fp_chop_offset == 0 &&
        cd->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest32(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; content:two; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        cd->flags & DETECT_CONTENT_NEGATED &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        cd->fp_chop_offset == 0 &&
        cd->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest33(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; fast_pattern; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest34(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; fast_pattern; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest35(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; fast_pattern; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest36(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; fast_pattern; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest37(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; content:oneonetwo; fast_pattern:3,4; content:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest38(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:twotwotwo; fast_pattern:3,4; content:three; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest39(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:twotwotwo; fast_pattern:3,4; content:three; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest40(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:twotwotwo; fast_pattern:3,4; content:three; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest41(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:twotwotwo; fast_pattern:3,4; content:three; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest42(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; distance:10; content:threethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest43(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; within:10; content:threethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest44(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; offset:10; content:threethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest45(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; depth:10; content:threethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest46(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; fast_pattern:65977,4; content:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest47(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:twooneone; fast_pattern:3,65977; content:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest48(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:two; fast_pattern:65534,4; content:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest49(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:!twooneone; fast_pattern:3,4; content:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    if (cd->flags & DETECT_CONTENT_FAST_PATTERN &&
        cd->flags & DETECT_CONTENT_NEGATED &&
        !(cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        cd->flags & cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        cd->fp_chop_offset == 3 &&
        cd->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest50(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:!twooneone; fast_pattern:3,4; distance:10; content:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest51(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:!twooneone; fast_pattern:3,4; within:10; content:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest52(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:!twooneone; fast_pattern:3,4; offset:10; content:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest53(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; content:!twooneone; fast_pattern:3,4; depth:10; content:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest54(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:\"/one/\"; fast_pattern:only; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_URICONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest55(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:oneoneone; fast_pattern:3,4; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_URICONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest56(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL && sm->type == DETECT_URICONTENT) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest57(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:oneoneone; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL && sm->type == DETECT_URICONTENT) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest58(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; fast_pattern:only; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest59(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; distance:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest60(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; fast_pattern:only; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest61(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; within:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest62(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; fast_pattern:only; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest63(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; offset:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest64(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; fast_pattern:only; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest65(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; depth:10; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest66(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:!two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest67(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent: one; uricontent:two; distance:30; uricontent:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest68(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; within:30; uricontent:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest69(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; offset:30; uricontent:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest70(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; depth:30; uricontent:two; fast_pattern:only; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest71(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:!one; fast_pattern; uricontent:two; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest72(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; uricontent:!one; fast_pattern; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest73(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; uricontent:!one; fast_pattern; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest74(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; uricontent:!one; fast_pattern; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest75(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; uricontent:!one; fast_pattern; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest76(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:oneonetwo; fast_pattern:3,4; uricontent:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest77(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:oneonetwo; fast_pattern:3,4; uricontent:three; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest78(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:oneonetwo; fast_pattern:3,4; uricontent:three; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest79(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:oneonetwo; fast_pattern:3,4; uricontent:three; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest80(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:oneonetwo; fast_pattern:3,4; uricontent:three; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest81(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; distance:10; uricontent:oneonethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest82(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; within:10; uricontent:oneonethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest83(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; offset:10; uricontent:oneonethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest84(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; depth:10; uricontent:oneonethree; fast_pattern:3,4; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest85(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; fast_pattern:65977,4; uricontent:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest86(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:oneonetwo; fast_pattern:3,65977; uricontent:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest87(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; fast_pattern:65534,4; uricontent:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest88(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:!oneonetwo; fast_pattern:3,4; uricontent:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest89(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:!oneonetwo; fast_pattern:3,4; distance:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest90(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:!oneonetwo; fast_pattern:3,4; within:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest91(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:!oneonetwo; fast_pattern:3,4; offset:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest92(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:!oneonetwo; fast_pattern:3,4; depth:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest93(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest94(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; http_uri; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_URICONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest95(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_uri; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    while (sm != NULL) {
        if (sm->type == DETECT_URICONTENT) {
            if ( ((DetectContentData *)sm->ctx)->flags &
                 DETECT_CONTENT_FAST_PATTERN) {
                result = 1;
                break;
            } else {
                result = 0;
                break;
            }
        }
        sm = sm->next;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest96(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL && sm->type == DETECT_URICONTENT) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest97(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL && sm->type == DETECT_URICONTENT) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest98(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; fast_pattern:only; http_uri; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest99(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; distance:10; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest100(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; fast_pattern:only; http_uri; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest101(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; within:10; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest102(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; fast_pattern:only; http_uri; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest103(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; offset:10; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest104(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; fast_pattern:only; http_uri; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest105(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; depth:10; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest106(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!two; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest107(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent: one; uricontent:two; distance:30; content:two; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest108(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; within:30; content:two; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest109(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; offset:30; content:two; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest110(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; depth:30; content:two; fast_pattern:only; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest111(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; http_uri; uricontent:two; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest112(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; content:!one; fast_pattern; http_uri; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest113(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; content:!one; fast_pattern; http_uri; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest114(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; content:!one; fast_pattern; http_uri; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest115(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:two; content:!one; fast_pattern; http_uri; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest116(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest117(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest118(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest119(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest120(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest121(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; distance:10; content:oneonethree; fast_pattern:3,4; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest122(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; within:10; content:oneonethree; fast_pattern:3,4; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest123(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; offset:10; content:oneonethree; fast_pattern:3,4; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest124(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; uricontent:two; depth:10; content:oneonethree; fast_pattern:3,4; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest125(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; fast_pattern:65977,4; http_uri; uricontent:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest126(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:oneonetwo; fast_pattern:3,65977; http_uri; uricontent:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest127(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:two; fast_pattern:65534,4; http_uri; uricontent:three; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest128(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest129(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!oneonetwo; fast_pattern:3,4; http_uri; distance:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest130(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!oneonetwo; fast_pattern:3,4; http_uri; within:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest131(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!twooneone; fast_pattern:3,4; http_uri; offset:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest132(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!oneonetwo; fast_pattern:3,4; http_uri; depth:10; uricontent:three; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest133(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:one; content:!oneonetwo; fast_pattern:3,4; http_uri; uricontent:three; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest134(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest135(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; http_client_body; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCBDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }


 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest136(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_client_body; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCBDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest137(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCBDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest138(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCBDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest139(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; fast_pattern:only; http_client_body; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest140(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; distance:10; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest141(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; fast_pattern:only; http_client_body; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest142(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; within:10; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest143(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; fast_pattern:only; http_client_body; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest144(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; offset:10; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest145(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; fast_pattern:only; http_client_body; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest146(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; depth:10; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest147(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!two; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest148(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content: one; http_client_body; content:two; http_client_body; distance:30; content:two; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest149(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; within:30; content:two; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest150(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; offset:30; content:two; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest151(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; depth:30; content:two; fast_pattern:only; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest152(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; http_client_body; content:two; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest153(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_client_body; content:!one; fast_pattern; http_client_body; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest154(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_client_body; content:!one; fast_pattern; http_client_body; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest155(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_client_body; content:!one; fast_pattern; http_client_body; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest156(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_client_body; content:!one; fast_pattern; http_client_body; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest157(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest158(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest159(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest160(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest161(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest162(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; distance:10; content:oneonethree; fast_pattern:3,4; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest163(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; within:10; content:oneonethree; fast_pattern:3,4; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest164(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; offset:10; content:oneonethree; fast_pattern:3,4; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest165(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; http_client_body; depth:10; content:oneonethree; fast_pattern:3,4; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest166(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; fast_pattern:65977,4; http_client_body; content:three; http_client_body; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest167(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one;  http_client_body; content:oneonetwo; fast_pattern:3,65977; http_client_body; content:three; distance:10; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest168(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:two; fast_pattern:65534,4; http_client_body; content:three; http_client_body; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest169(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest170(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!oneonetwo; fast_pattern:3,4; http_client_body; distance:10; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest171(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!oneonetwo; fast_pattern:3,4; http_client_body; within:10; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest172(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!twooneone; fast_pattern:3,4; http_client_body; offset:10; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest173(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!oneonetwo; fast_pattern:3,4; http_client_body; depth:10; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest174(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_client_body; content:!oneonetwo; fast_pattern:3,4; http_client_body; content:three; http_client_body; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest175(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; distance:20; fast_pattern; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest176(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; within:20; fast_pattern; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest177(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; offset:20; fast_pattern; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest178(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; content:!one; depth:20; fast_pattern; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/*****http_header tests v ******/

int DetectFastPatternTest179(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; "
                               "content:!oneonetwo; fast_pattern:3,4; http_header; "
                               "content:three; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest180(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; http_header; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HHDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }


 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest181(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_header; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HHDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest182(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HHDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest183(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HHDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest184(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; fast_pattern:only; http_header; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest185(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; distance:10; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest186(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; fast_pattern:only; http_header; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest187(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; within:10; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest188(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; fast_pattern:only; http_header; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest189(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; offset:10; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest190(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; fast_pattern:only; http_header; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest191(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; depth:10; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest192(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!two; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest193(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content: one; http_header; content:two; http_header; distance:30; content:two; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest194(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; within:30; content:two; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest195(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; offset:30; content:two; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest196(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; depth:30; content:two; fast_pattern:only; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest197(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; http_header; content:two; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest198(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_header; content:!one; fast_pattern; http_header; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest199(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_header; content:!one; fast_pattern; http_header; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest200(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_header; content:!one; fast_pattern; http_header; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest201(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_header; content:!one; fast_pattern; http_header; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest202(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest203(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest204(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest205(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest206(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest207(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; distance:10; content:oneonethree; fast_pattern:3,4; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest208(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; within:10; content:oneonethree; fast_pattern:3,4; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest209(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; offset:10; content:oneonethree; fast_pattern:3,4; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest210(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; http_header; depth:10; content:oneonethree; fast_pattern:3,4; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest211(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; fast_pattern:65977,4; http_header; content:three; http_header; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest212(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one;  http_header; content:oneonetwo; fast_pattern:3,65977; http_header; content:three; distance:10; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest213(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:two; fast_pattern:65534,4; http_header; content:three; http_header; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest214(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest215(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!oneonetwo; fast_pattern:3,4; http_header; distance:10; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest216(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!oneonetwo; fast_pattern:3,4; http_header; within:10; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest217(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!oneonetwo; fast_pattern:3,4; http_header; offset:10; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest218(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!oneonetwo; fast_pattern:3,4; http_header; depth:10; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest219(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_header; content:!oneonetwo; fast_pattern:3,4; http_header; content:three; http_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}













/********* http_raw_header tests v *****/

int DetectFastPatternTest220(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; "
                               "content:!oneonetwo; fast_pattern:3,4; http_raw_header; "
                               "content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest221(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; http_raw_header; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HRHDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }


 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest222(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_raw_header; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HRHDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest223(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HRHDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest224(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HRHDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest225(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; fast_pattern:only; http_raw_header; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest226(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; distance:10; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest227(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; fast_pattern:only; http_raw_header; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest228(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; within:10; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest229(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; fast_pattern:only; http_raw_header; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest230(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; offset:10; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest231(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; fast_pattern:only; http_raw_header; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest232(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; depth:10; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest233(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!two; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest234(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content: one; http_raw_header; content:two; http_raw_header; distance:30; content:two; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest235(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; within:30; content:two; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest236(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; offset:30; content:two; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest237(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; depth:30; content:two; fast_pattern:only; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest238(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; http_raw_header; content:two; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest239(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_raw_header; content:!one; fast_pattern; http_raw_header; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest240(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_raw_header; content:!one; fast_pattern; http_raw_header; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest241(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_raw_header; content:!one; fast_pattern; http_raw_header; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest242(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_raw_header; content:!one; fast_pattern; http_raw_header; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest243(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest244(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest245(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest246(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest247(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest248(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; distance:10; content:oneonethree; fast_pattern:3,4; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest249(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; within:10; content:oneonethree; fast_pattern:3,4; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest250(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; offset:10; content:oneonethree; fast_pattern:3,4; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest251(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; http_raw_header; depth:10; content:oneonethree; fast_pattern:3,4; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest252(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; fast_pattern:65977,4; http_raw_header; content:three; http_raw_header; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest253(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one;  http_raw_header; content:oneonetwo; fast_pattern:3,65977; http_raw_header; content:three; distance:10; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest254(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:two; fast_pattern:65534,4; http_raw_header; content:three; http_raw_header; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest255(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest256(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!oneonetwo; fast_pattern:3,4; http_raw_header; distance:10; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest257(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!oneonetwo; fast_pattern:3,4; http_raw_header; within:10; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest258(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!oneonetwo; fast_pattern:3,4; http_raw_header; offset:10; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest259(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!oneonetwo; fast_pattern:3,4; http_raw_header; depth:10; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest260(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_raw_header; content:!oneonetwo; fast_pattern:3,4; http_raw_header; content:three; http_raw_header; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}



















int DetectFastPatternTest261(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; "
                               "content:!oneonetwo; fast_pattern:3,4; http_method; "
                               "content:three; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest262(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; http_method; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HMDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }


 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest263(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_method; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HMDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest264(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HMDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest265(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HMDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest266(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; fast_pattern:only; http_method; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest267(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; distance:10; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest268(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; fast_pattern:only; http_method; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest269(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; within:10; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest270(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; fast_pattern:only; http_method; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest271(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; offset:10; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest272(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; fast_pattern:only; http_method; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest273(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; depth:10; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest274(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!two; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest275(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content: one; http_method; content:two; http_method; distance:30; content:two; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest276(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; within:30; content:two; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest277(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; offset:30; content:two; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest278(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; depth:30; content:two; fast_pattern:only; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest279(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; http_method; content:two; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest280(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_method; content:!one; fast_pattern; http_method; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest281(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_method; content:!one; fast_pattern; http_method; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest282(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_method; content:!one; fast_pattern; http_method; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest283(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_method; content:!one; fast_pattern; http_method; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest284(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest285(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest286(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest287(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest288(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest289(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; distance:10; content:oneonethree; fast_pattern:3,4; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest290(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; within:10; content:oneonethree; fast_pattern:3,4; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest291(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; offset:10; content:oneonethree; fast_pattern:3,4; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest292(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; http_method; depth:10; content:oneonethree; fast_pattern:3,4; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest293(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; fast_pattern:65977,4; http_method; content:three; http_method; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest294(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one;  http_method; content:oneonetwo; fast_pattern:3,65977; http_method; content:three; distance:10; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest295(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:two; fast_pattern:65534,4; http_method; content:three; http_method; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest296(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest297(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!oneonetwo; fast_pattern:3,4; http_method; distance:10; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest298(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!oneonetwo; fast_pattern:3,4; http_method; within:10; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest299(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!oneonetwo; fast_pattern:3,4; http_method; offset:10; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest300(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!oneonetwo; fast_pattern:3,4; http_method; depth:10; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest301(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_method; content:!oneonetwo; fast_pattern:3,4; http_method; content:three; http_method; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HMDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}














int DetectFastPatternTest302(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; "
                               "content:!oneonetwo; fast_pattern:3,4; http_cookie; "
                               "content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest303(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"/one/\"; fast_pattern:only; http_cookie; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }


 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Checks if a fast_pattern is registered in a Signature for uricontent.
 */
int DetectFastPatternTest304(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_cookie; "
                               "msg:\"Testing fast_pattern\"; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCDMATCH];
    if (sm != NULL) {
        if ( ((DetectContentData *)sm->ctx)->flags &
             DETECT_CONTENT_FAST_PATTERN) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest305(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
            ud->fp_chop_offset == 0 &&
            ud->fp_chop_len == 0) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest306(void)
{
    SigMatch *sm = NULL;
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:oneoneone; fast_pattern:3,4; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    result = 0;
    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_HCDMATCH];
    DetectContentData *ud = sm->ctx;
    if (sm != NULL) {
        if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
            !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
            ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
            ud->fp_chop_offset == 3 &&
            ud->fp_chop_len == 4) {
            result = 1;
        } else {
            result = 0;
        }
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest307(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; fast_pattern:only; http_cookie; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest308(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; distance:10; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest309(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; fast_pattern:only; http_cookie; within:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest310(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; within:10; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest311(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; fast_pattern:only; http_cookie; offset:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest312(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; offset:10; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest313(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; fast_pattern:only; http_cookie; depth:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest314(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; depth:10; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest315(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!two; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest316(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content: one; http_cookie; content:two; http_cookie; distance:30; content:two; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;

    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest317(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; within:30; content:two; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest318(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; offset:30; content:two; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest319(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; depth:30; content:two; fast_pattern:only; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest320(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:!one; fast_pattern; http_cookie; content:two; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) &&
        ud->fp_chop_offset == 0 &&
        ud->fp_chop_len == 0) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest321(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_cookie; content:!one; fast_pattern; http_cookie; distance:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest322(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_cookie; content:!one; fast_pattern; http_cookie; within:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest323(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_cookie; content:!one; fast_pattern; http_cookie; offset:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest324(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:two; http_cookie; content:!one; fast_pattern; http_cookie; depth:20; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest325(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest326(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; distance:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest327(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; within:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest328(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; offset:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest329(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; depth:30; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest330(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; distance:10; content:oneonethree; fast_pattern:3,4; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest331(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; within:10; content:oneonethree; fast_pattern:3,4; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest332(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; offset:10; content:oneonethree; fast_pattern:3,4; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest333(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; http_cookie; depth:10; content:oneonethree; fast_pattern:3,4; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }


    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest334(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; fast_pattern:65977,4; http_cookie; content:three; http_cookie; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest335(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one;  http_cookie; content:oneonetwo; fast_pattern:3,65977; http_cookie; content:three; distance:10; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest336(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:two; fast_pattern:65534,4; http_cookie; content:three; http_cookie; distance:10; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest337(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest338(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!oneonetwo; fast_pattern:3,4; http_cookie; distance:10; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest339(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!oneonetwo; fast_pattern:3,4; http_cookie; within:10; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest340(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!oneonetwo; fast_pattern:3,4; http_cookie; offset:10; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest341(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!oneonetwo; fast_pattern:3,4; http_cookie; depth:10; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list != NULL)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectFastPatternTest342(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:one; http_cookie; content:!oneonetwo; fast_pattern:3,4; http_cookie; content:three; http_cookie; sid:1;)");
    if (de_ctx->sig_list == NULL)
        goto end;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_HCDMATCH]->prev->ctx;
    if (ud->flags & DETECT_CONTENT_FAST_PATTERN &&
        ud->flags & DETECT_CONTENT_NEGATED &&
        !(ud->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) &&
        ud->flags & DETECT_CONTENT_FAST_PATTERN_CHOP &&
        ud->fp_chop_offset == 3 &&
        ud->fp_chop_len == 4) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}


#endif

void DetectFastPatternRegisterTests(void)
{

#ifdef UNITTESTS
    UtRegisterTest("DetectFastPatternTest01", DetectFastPatternTest01, 1);
    UtRegisterTest("DetectFastPatternTest02", DetectFastPatternTest02, 1);
    UtRegisterTest("DetectFastPatternTest03", DetectFastPatternTest03, 1);
    UtRegisterTest("DetectFastPatternTest04", DetectFastPatternTest04, 1);
    UtRegisterTest("DetectFastPatternTest05", DetectFastPatternTest05, 1);
    UtRegisterTest("DetectFastPatternTest06", DetectFastPatternTest06, 1);
    UtRegisterTest("DetectFastPatternTest07", DetectFastPatternTest07, 1);
    UtRegisterTest("DetectFastPatternTest08", DetectFastPatternTest08, 1);
    UtRegisterTest("DetectFastPatternTest09", DetectFastPatternTest09, 1);
    UtRegisterTest("DetectFastPatternTest10", DetectFastPatternTest10, 1);
    UtRegisterTest("DetectFastPatternTest11", DetectFastPatternTest11, 1);
    UtRegisterTest("DetectFastPatternTest12", DetectFastPatternTest12, 1);
    UtRegisterTest("DetectFastPatternTest13", DetectFastPatternTest13, 1);
    UtRegisterTest("DetectFastPatternTest14", DetectFastPatternTest14, 1);
    UtRegisterTest("DetectFastPatternTest15", DetectFastPatternTest15, 1);
    UtRegisterTest("DetectFastPatternTest16", DetectFastPatternTest16, 1);
    UtRegisterTest("DetectFastPatternTest17", DetectFastPatternTest17, 1);
    UtRegisterTest("DetectFastPatternTest18", DetectFastPatternTest18, 1);
    UtRegisterTest("DetectFastPatternTest19", DetectFastPatternTest19, 1);
    UtRegisterTest("DetectFastPatternTest20", DetectFastPatternTest20, 1);
    UtRegisterTest("DetectFastPatternTest21", DetectFastPatternTest21, 1);
    UtRegisterTest("DetectFastPatternTest22", DetectFastPatternTest22, 1);
    UtRegisterTest("DetectFastPatternTest23", DetectFastPatternTest23, 1);
    UtRegisterTest("DetectFastPatternTest24", DetectFastPatternTest24, 1);
    UtRegisterTest("DetectFastPatternTest25", DetectFastPatternTest25, 1);
    UtRegisterTest("DetectFastPatternTest26", DetectFastPatternTest26, 1);
    UtRegisterTest("DetectFastPatternTest27", DetectFastPatternTest27, 1);
    UtRegisterTest("DetectFastPatternTest28", DetectFastPatternTest28, 1);
    UtRegisterTest("DetectFastPatternTest29", DetectFastPatternTest29, 1);
    UtRegisterTest("DetectFastPatternTest30", DetectFastPatternTest30, 1);
    UtRegisterTest("DetectFastPatternTest31", DetectFastPatternTest31, 1);
    UtRegisterTest("DetectFastPatternTest32", DetectFastPatternTest32, 1);
    UtRegisterTest("DetectFastPatternTest33", DetectFastPatternTest33, 1);
    UtRegisterTest("DetectFastPatternTest34", DetectFastPatternTest34, 1);
    UtRegisterTest("DetectFastPatternTest35", DetectFastPatternTest35, 1);
    UtRegisterTest("DetectFastPatternTest36", DetectFastPatternTest36, 1);
    UtRegisterTest("DetectFastPatternTest37", DetectFastPatternTest37, 1);
    UtRegisterTest("DetectFastPatternTest38", DetectFastPatternTest38, 1);
    UtRegisterTest("DetectFastPatternTest39", DetectFastPatternTest39, 1);
    UtRegisterTest("DetectFastPatternTest40", DetectFastPatternTest40, 1);
    UtRegisterTest("DetectFastPatternTest41", DetectFastPatternTest41, 1);
    UtRegisterTest("DetectFastPatternTest42", DetectFastPatternTest42, 1);
    UtRegisterTest("DetectFastPatternTest43", DetectFastPatternTest43, 1);
    UtRegisterTest("DetectFastPatternTest44", DetectFastPatternTest44, 1);
    UtRegisterTest("DetectFastPatternTest45", DetectFastPatternTest45, 1);
    UtRegisterTest("DetectFastPatternTest46", DetectFastPatternTest46, 1);
    UtRegisterTest("DetectFastPatternTest47", DetectFastPatternTest47, 1);
    UtRegisterTest("DetectFastPatternTest48", DetectFastPatternTest48, 1);
    UtRegisterTest("DetectFastPatternTest49", DetectFastPatternTest49, 1);
    UtRegisterTest("DetectFastPatternTest50", DetectFastPatternTest50, 1);
    UtRegisterTest("DetectFastPatternTest51", DetectFastPatternTest51, 1);
    UtRegisterTest("DetectFastPatternTest52", DetectFastPatternTest52, 1);
    UtRegisterTest("DetectFastPatternTest53", DetectFastPatternTest53, 1);
    /*    content fast_pattern tests ^ */
    /* uricontent fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest54", DetectFastPatternTest54, 1);
    UtRegisterTest("DetectFastPatternTest55", DetectFastPatternTest55, 1);
    UtRegisterTest("DetectFastPatternTest56", DetectFastPatternTest56, 1);
    UtRegisterTest("DetectFastPatternTest57", DetectFastPatternTest57, 1);
    UtRegisterTest("DetectFastPatternTest58", DetectFastPatternTest58, 1);
    UtRegisterTest("DetectFastPatternTest59", DetectFastPatternTest59, 1);
    UtRegisterTest("DetectFastPatternTest60", DetectFastPatternTest60, 1);
    UtRegisterTest("DetectFastPatternTest61", DetectFastPatternTest61, 1);
    UtRegisterTest("DetectFastPatternTest62", DetectFastPatternTest62, 1);
    UtRegisterTest("DetectFastPatternTest63", DetectFastPatternTest63, 1);
    UtRegisterTest("DetectFastPatternTest64", DetectFastPatternTest64, 1);
    UtRegisterTest("DetectFastPatternTest65", DetectFastPatternTest65, 1);
    UtRegisterTest("DetectFastPatternTest66", DetectFastPatternTest66, 1);
    UtRegisterTest("DetectFastPatternTest67", DetectFastPatternTest67, 1);
    UtRegisterTest("DetectFastPatternTest68", DetectFastPatternTest68, 1);
    UtRegisterTest("DetectFastPatternTest69", DetectFastPatternTest69, 1);
    UtRegisterTest("DetectFastPatternTest70", DetectFastPatternTest70, 1);
    UtRegisterTest("DetectFastPatternTest71", DetectFastPatternTest71, 1);
    UtRegisterTest("DetectFastPatternTest72", DetectFastPatternTest72, 1);
    UtRegisterTest("DetectFastPatternTest73", DetectFastPatternTest73, 1);
    UtRegisterTest("DetectFastPatternTest74", DetectFastPatternTest74, 1);
    UtRegisterTest("DetectFastPatternTest75", DetectFastPatternTest75, 1);
    UtRegisterTest("DetectFastPatternTest76", DetectFastPatternTest76, 1);
    UtRegisterTest("DetectFastPatternTest77", DetectFastPatternTest77, 1);
    UtRegisterTest("DetectFastPatternTest78", DetectFastPatternTest78, 1);
    UtRegisterTest("DetectFastPatternTest79", DetectFastPatternTest79, 1);
    UtRegisterTest("DetectFastPatternTest80", DetectFastPatternTest80, 1);
    UtRegisterTest("DetectFastPatternTest81", DetectFastPatternTest81, 1);
    UtRegisterTest("DetectFastPatternTest82", DetectFastPatternTest82, 1);
    UtRegisterTest("DetectFastPatternTest83", DetectFastPatternTest83, 1);
    UtRegisterTest("DetectFastPatternTest84", DetectFastPatternTest84, 1);
    UtRegisterTest("DetectFastPatternTest85", DetectFastPatternTest85, 1);
    UtRegisterTest("DetectFastPatternTest86", DetectFastPatternTest86, 1);
    UtRegisterTest("DetectFastPatternTest87", DetectFastPatternTest87, 1);
    UtRegisterTest("DetectFastPatternTest88", DetectFastPatternTest88, 1);
    UtRegisterTest("DetectFastPatternTest89", DetectFastPatternTest89, 1);
    UtRegisterTest("DetectFastPatternTest90", DetectFastPatternTest90, 1);
    UtRegisterTest("DetectFastPatternTest91", DetectFastPatternTest91, 1);
    UtRegisterTest("DetectFastPatternTest92", DetectFastPatternTest92, 1);
    /* uricontent fast_pattern tests ^ */
    /*   http_uri fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest93", DetectFastPatternTest93, 1);
    UtRegisterTest("DetectFastPatternTest94", DetectFastPatternTest94, 1);
    UtRegisterTest("DetectFastPatternTest95", DetectFastPatternTest95, 1);
    UtRegisterTest("DetectFastPatternTest96", DetectFastPatternTest96, 1);
    UtRegisterTest("DetectFastPatternTest97", DetectFastPatternTest97, 1);
    UtRegisterTest("DetectFastPatternTest98", DetectFastPatternTest98, 1);
    UtRegisterTest("DetectFastPatternTest99", DetectFastPatternTest99, 1);
    UtRegisterTest("DetectFastPatternTest100", DetectFastPatternTest100, 1);
    UtRegisterTest("DetectFastPatternTest101", DetectFastPatternTest101, 1);
    UtRegisterTest("DetectFastPatternTest102", DetectFastPatternTest102, 1);
    UtRegisterTest("DetectFastPatternTest103", DetectFastPatternTest103, 1);
    UtRegisterTest("DetectFastPatternTest104", DetectFastPatternTest104, 1);
    UtRegisterTest("DetectFastPatternTest105", DetectFastPatternTest105, 1);
    UtRegisterTest("DetectFastPatternTest106", DetectFastPatternTest106, 1);
    UtRegisterTest("DetectFastPatternTest107", DetectFastPatternTest107, 1);
    UtRegisterTest("DetectFastPatternTest108", DetectFastPatternTest108, 1);
    UtRegisterTest("DetectFastPatternTest109", DetectFastPatternTest109, 1);
    UtRegisterTest("DetectFastPatternTest110", DetectFastPatternTest110, 1);
    UtRegisterTest("DetectFastPatternTest111", DetectFastPatternTest111, 1);
    UtRegisterTest("DetectFastPatternTest112", DetectFastPatternTest112, 1);
    UtRegisterTest("DetectFastPatternTest113", DetectFastPatternTest113, 1);
    UtRegisterTest("DetectFastPatternTest114", DetectFastPatternTest114, 1);
    UtRegisterTest("DetectFastPatternTest115", DetectFastPatternTest115, 1);
    UtRegisterTest("DetectFastPatternTest116", DetectFastPatternTest116, 1);
    UtRegisterTest("DetectFastPatternTest117", DetectFastPatternTest117, 1);
    UtRegisterTest("DetectFastPatternTest118", DetectFastPatternTest118, 1);
    UtRegisterTest("DetectFastPatternTest119", DetectFastPatternTest119, 1);
    UtRegisterTest("DetectFastPatternTest120", DetectFastPatternTest120, 1);
    UtRegisterTest("DetectFastPatternTest121", DetectFastPatternTest121, 1);
    UtRegisterTest("DetectFastPatternTest122", DetectFastPatternTest122, 1);
    UtRegisterTest("DetectFastPatternTest123", DetectFastPatternTest123, 1);
    UtRegisterTest("DetectFastPatternTest124", DetectFastPatternTest124, 1);
    UtRegisterTest("DetectFastPatternTest125", DetectFastPatternTest125, 1);
    UtRegisterTest("DetectFastPatternTest126", DetectFastPatternTest126, 1);
    UtRegisterTest("DetectFastPatternTest127", DetectFastPatternTest127, 1);
    UtRegisterTest("DetectFastPatternTest128", DetectFastPatternTest128, 1);
    UtRegisterTest("DetectFastPatternTest129", DetectFastPatternTest129, 1);
    UtRegisterTest("DetectFastPatternTest130", DetectFastPatternTest130, 1);
    UtRegisterTest("DetectFastPatternTest131", DetectFastPatternTest131, 1);
    UtRegisterTest("DetectFastPatternTest132", DetectFastPatternTest132, 1);
    UtRegisterTest("DetectFastPatternTest133", DetectFastPatternTest133, 1);
    /*         http_uri fast_pattern tests ^ */
    /* http_client_body fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest134", DetectFastPatternTest134, 1);
    UtRegisterTest("DetectFastPatternTest135", DetectFastPatternTest135, 1);
    UtRegisterTest("DetectFastPatternTest136", DetectFastPatternTest136, 1);
    UtRegisterTest("DetectFastPatternTest137", DetectFastPatternTest137, 1);
    UtRegisterTest("DetectFastPatternTest138", DetectFastPatternTest138, 1);
    UtRegisterTest("DetectFastPatternTest139", DetectFastPatternTest139, 1);
    UtRegisterTest("DetectFastPatternTest140", DetectFastPatternTest140, 1);
    UtRegisterTest("DetectFastPatternTest141", DetectFastPatternTest141, 1);
    UtRegisterTest("DetectFastPatternTest142", DetectFastPatternTest142, 1);
    UtRegisterTest("DetectFastPatternTest143", DetectFastPatternTest143, 1);
    UtRegisterTest("DetectFastPatternTest144", DetectFastPatternTest144, 1);
    UtRegisterTest("DetectFastPatternTest145", DetectFastPatternTest145, 1);
    UtRegisterTest("DetectFastPatternTest146", DetectFastPatternTest146, 1);
    UtRegisterTest("DetectFastPatternTest147", DetectFastPatternTest147, 1);
    UtRegisterTest("DetectFastPatternTest148", DetectFastPatternTest148, 1);
    UtRegisterTest("DetectFastPatternTest149", DetectFastPatternTest149, 1);
    UtRegisterTest("DetectFastPatternTest150", DetectFastPatternTest150, 1);
    UtRegisterTest("DetectFastPatternTest151", DetectFastPatternTest151, 1);
    UtRegisterTest("DetectFastPatternTest152", DetectFastPatternTest152, 1);
    UtRegisterTest("DetectFastPatternTest153", DetectFastPatternTest153, 1);
    UtRegisterTest("DetectFastPatternTest154", DetectFastPatternTest154, 1);
    UtRegisterTest("DetectFastPatternTest155", DetectFastPatternTest155, 1);
    UtRegisterTest("DetectFastPatternTest156", DetectFastPatternTest156, 1);
    UtRegisterTest("DetectFastPatternTest157", DetectFastPatternTest157, 1);
    UtRegisterTest("DetectFastPatternTest158", DetectFastPatternTest158, 1);
    UtRegisterTest("DetectFastPatternTest159", DetectFastPatternTest159, 1);
    UtRegisterTest("DetectFastPatternTest160", DetectFastPatternTest160, 1);
    UtRegisterTest("DetectFastPatternTest161", DetectFastPatternTest161, 1);
    UtRegisterTest("DetectFastPatternTest162", DetectFastPatternTest162, 1);
    UtRegisterTest("DetectFastPatternTest163", DetectFastPatternTest163, 1);
    UtRegisterTest("DetectFastPatternTest164", DetectFastPatternTest164, 1);
    UtRegisterTest("DetectFastPatternTest165", DetectFastPatternTest165, 1);
    UtRegisterTest("DetectFastPatternTest166", DetectFastPatternTest166, 1);
    UtRegisterTest("DetectFastPatternTest167", DetectFastPatternTest167, 1);
    UtRegisterTest("DetectFastPatternTest168", DetectFastPatternTest168, 1);
    UtRegisterTest("DetectFastPatternTest169", DetectFastPatternTest169, 1);
    UtRegisterTest("DetectFastPatternTest170", DetectFastPatternTest170, 1);
    UtRegisterTest("DetectFastPatternTest171", DetectFastPatternTest171, 1);
    UtRegisterTest("DetectFastPatternTest172", DetectFastPatternTest172, 1);
    UtRegisterTest("DetectFastPatternTest173", DetectFastPatternTest173, 1);
    UtRegisterTest("DetectFastPatternTest174", DetectFastPatternTest174, 1);
    /* http_client_body fast_pattern tests ^ */
    /*          content fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest175", DetectFastPatternTest175, 1);
    UtRegisterTest("DetectFastPatternTest176", DetectFastPatternTest176, 1);
    UtRegisterTest("DetectFastPatternTest177", DetectFastPatternTest177, 1);
    UtRegisterTest("DetectFastPatternTest178", DetectFastPatternTest178, 1);
    /*     content fast_pattern tests ^ */
    /* http_header fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest179", DetectFastPatternTest179, 1);
    UtRegisterTest("DetectFastPatternTest180", DetectFastPatternTest180, 1);
    UtRegisterTest("DetectFastPatternTest181", DetectFastPatternTest181, 1);
    UtRegisterTest("DetectFastPatternTest182", DetectFastPatternTest182, 1);
    UtRegisterTest("DetectFastPatternTest183", DetectFastPatternTest183, 1);
    UtRegisterTest("DetectFastPatternTest184", DetectFastPatternTest184, 1);
    UtRegisterTest("DetectFastPatternTest185", DetectFastPatternTest185, 1);
    UtRegisterTest("DetectFastPatternTest186", DetectFastPatternTest186, 1);
    UtRegisterTest("DetectFastPatternTest187", DetectFastPatternTest187, 1);
    UtRegisterTest("DetectFastPatternTest188", DetectFastPatternTest188, 1);
    UtRegisterTest("DetectFastPatternTest189", DetectFastPatternTest189, 1);
    UtRegisterTest("DetectFastPatternTest190", DetectFastPatternTest190, 1);
    UtRegisterTest("DetectFastPatternTest191", DetectFastPatternTest191, 1);
    UtRegisterTest("DetectFastPatternTest192", DetectFastPatternTest192, 1);
    UtRegisterTest("DetectFastPatternTest193", DetectFastPatternTest193, 1);
    UtRegisterTest("DetectFastPatternTest194", DetectFastPatternTest194, 1);
    UtRegisterTest("DetectFastPatternTest195", DetectFastPatternTest195, 1);
    UtRegisterTest("DetectFastPatternTest196", DetectFastPatternTest196, 1);
    UtRegisterTest("DetectFastPatternTest197", DetectFastPatternTest197, 1);
    UtRegisterTest("DetectFastPatternTest198", DetectFastPatternTest198, 1);
    UtRegisterTest("DetectFastPatternTest199", DetectFastPatternTest199, 1);
    UtRegisterTest("DetectFastPatternTest200", DetectFastPatternTest200, 1);
    UtRegisterTest("DetectFastPatternTest201", DetectFastPatternTest201, 1);
    UtRegisterTest("DetectFastPatternTest202", DetectFastPatternTest202, 1);
    UtRegisterTest("DetectFastPatternTest203", DetectFastPatternTest203, 1);
    UtRegisterTest("DetectFastPatternTest204", DetectFastPatternTest204, 1);
    UtRegisterTest("DetectFastPatternTest205", DetectFastPatternTest205, 1);
    UtRegisterTest("DetectFastPatternTest206", DetectFastPatternTest206, 1);
    UtRegisterTest("DetectFastPatternTest207", DetectFastPatternTest207, 1);
    UtRegisterTest("DetectFastPatternTest208", DetectFastPatternTest208, 1);
    UtRegisterTest("DetectFastPatternTest209", DetectFastPatternTest209, 1);
    UtRegisterTest("DetectFastPatternTest210", DetectFastPatternTest210, 1);
    UtRegisterTest("DetectFastPatternTest211", DetectFastPatternTest211, 1);
    UtRegisterTest("DetectFastPatternTest212", DetectFastPatternTest212, 1);
    UtRegisterTest("DetectFastPatternTest213", DetectFastPatternTest213, 1);
    UtRegisterTest("DetectFastPatternTest214", DetectFastPatternTest214, 1);
    UtRegisterTest("DetectFastPatternTest215", DetectFastPatternTest215, 1);
    UtRegisterTest("DetectFastPatternTest216", DetectFastPatternTest216, 1);
    UtRegisterTest("DetectFastPatternTest217", DetectFastPatternTest217, 1);
    UtRegisterTest("DetectFastPatternTest218", DetectFastPatternTest218, 1);
    UtRegisterTest("DetectFastPatternTest219", DetectFastPatternTest219, 1);
    /*     http_header fast_pattern tests ^ */
    /* http_raw_header fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest220", DetectFastPatternTest220, 1);
    UtRegisterTest("DetectFastPatternTest221", DetectFastPatternTest221, 1);
    UtRegisterTest("DetectFastPatternTest222", DetectFastPatternTest222, 1);
    UtRegisterTest("DetectFastPatternTest223", DetectFastPatternTest223, 1);
    UtRegisterTest("DetectFastPatternTest224", DetectFastPatternTest224, 1);
    UtRegisterTest("DetectFastPatternTest225", DetectFastPatternTest225, 1);
    UtRegisterTest("DetectFastPatternTest226", DetectFastPatternTest226, 1);
    UtRegisterTest("DetectFastPatternTest227", DetectFastPatternTest227, 1);
    UtRegisterTest("DetectFastPatternTest228", DetectFastPatternTest228, 1);
    UtRegisterTest("DetectFastPatternTest229", DetectFastPatternTest229, 1);
    UtRegisterTest("DetectFastPatternTest230", DetectFastPatternTest230, 1);
    UtRegisterTest("DetectFastPatternTest231", DetectFastPatternTest231, 1);
    UtRegisterTest("DetectFastPatternTest232", DetectFastPatternTest232, 1);
    UtRegisterTest("DetectFastPatternTest233", DetectFastPatternTest233, 1);
    UtRegisterTest("DetectFastPatternTest234", DetectFastPatternTest234, 1);
    UtRegisterTest("DetectFastPatternTest235", DetectFastPatternTest235, 1);
    UtRegisterTest("DetectFastPatternTest236", DetectFastPatternTest236, 1);
    UtRegisterTest("DetectFastPatternTest237", DetectFastPatternTest237, 1);
    UtRegisterTest("DetectFastPatternTest238", DetectFastPatternTest238, 1);
    UtRegisterTest("DetectFastPatternTest239", DetectFastPatternTest239, 1);
    UtRegisterTest("DetectFastPatternTest240", DetectFastPatternTest240, 1);
    UtRegisterTest("DetectFastPatternTest241", DetectFastPatternTest241, 1);
    UtRegisterTest("DetectFastPatternTest242", DetectFastPatternTest242, 1);
    UtRegisterTest("DetectFastPatternTest243", DetectFastPatternTest243, 1);
    UtRegisterTest("DetectFastPatternTest244", DetectFastPatternTest244, 1);
    UtRegisterTest("DetectFastPatternTest245", DetectFastPatternTest245, 1);
    UtRegisterTest("DetectFastPatternTest246", DetectFastPatternTest246, 1);
    UtRegisterTest("DetectFastPatternTest247", DetectFastPatternTest247, 1);
    UtRegisterTest("DetectFastPatternTest248", DetectFastPatternTest248, 1);
    UtRegisterTest("DetectFastPatternTest249", DetectFastPatternTest249, 1);
    UtRegisterTest("DetectFastPatternTest250", DetectFastPatternTest250, 1);
    UtRegisterTest("DetectFastPatternTest251", DetectFastPatternTest251, 1);
    UtRegisterTest("DetectFastPatternTest252", DetectFastPatternTest252, 1);
    UtRegisterTest("DetectFastPatternTest253", DetectFastPatternTest253, 1);
    UtRegisterTest("DetectFastPatternTest254", DetectFastPatternTest254, 1);
    UtRegisterTest("DetectFastPatternTest255", DetectFastPatternTest255, 1);
    UtRegisterTest("DetectFastPatternTest256", DetectFastPatternTest256, 1);
    UtRegisterTest("DetectFastPatternTest257", DetectFastPatternTest257, 1);
    UtRegisterTest("DetectFastPatternTest258", DetectFastPatternTest258, 1);
    UtRegisterTest("DetectFastPatternTest259", DetectFastPatternTest259, 1);
    UtRegisterTest("DetectFastPatternTest260", DetectFastPatternTest260, 1);
    /* http_raw_header fast_pattern tests ^ */
    /*     http_method fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest261", DetectFastPatternTest261, 1);
    UtRegisterTest("DetectFastPatternTest262", DetectFastPatternTest262, 1);
    UtRegisterTest("DetectFastPatternTest263", DetectFastPatternTest263, 1);
    UtRegisterTest("DetectFastPatternTest264", DetectFastPatternTest264, 1);
    UtRegisterTest("DetectFastPatternTest265", DetectFastPatternTest265, 1);
    UtRegisterTest("DetectFastPatternTest266", DetectFastPatternTest266, 1);
    UtRegisterTest("DetectFastPatternTest267", DetectFastPatternTest267, 1);
    UtRegisterTest("DetectFastPatternTest268", DetectFastPatternTest268, 1);
    UtRegisterTest("DetectFastPatternTest269", DetectFastPatternTest269, 1);
    UtRegisterTest("DetectFastPatternTest270", DetectFastPatternTest270, 1);
    UtRegisterTest("DetectFastPatternTest271", DetectFastPatternTest271, 1);
    UtRegisterTest("DetectFastPatternTest272", DetectFastPatternTest272, 1);
    UtRegisterTest("DetectFastPatternTest273", DetectFastPatternTest273, 1);
    UtRegisterTest("DetectFastPatternTest274", DetectFastPatternTest274, 1);
    UtRegisterTest("DetectFastPatternTest275", DetectFastPatternTest275, 1);
    UtRegisterTest("DetectFastPatternTest276", DetectFastPatternTest276, 1);
    UtRegisterTest("DetectFastPatternTest277", DetectFastPatternTest277, 1);
    UtRegisterTest("DetectFastPatternTest278", DetectFastPatternTest278, 1);
    UtRegisterTest("DetectFastPatternTest279", DetectFastPatternTest279, 1);
    UtRegisterTest("DetectFastPatternTest280", DetectFastPatternTest280, 1);
    UtRegisterTest("DetectFastPatternTest281", DetectFastPatternTest281, 1);
    UtRegisterTest("DetectFastPatternTest282", DetectFastPatternTest282, 1);
    UtRegisterTest("DetectFastPatternTest283", DetectFastPatternTest283, 1);
    UtRegisterTest("DetectFastPatternTest284", DetectFastPatternTest284, 1);
    UtRegisterTest("DetectFastPatternTest285", DetectFastPatternTest285, 1);
    UtRegisterTest("DetectFastPatternTest286", DetectFastPatternTest286, 1);
    UtRegisterTest("DetectFastPatternTest287", DetectFastPatternTest287, 1);
    UtRegisterTest("DetectFastPatternTest288", DetectFastPatternTest288, 1);
    UtRegisterTest("DetectFastPatternTest289", DetectFastPatternTest289, 1);
    UtRegisterTest("DetectFastPatternTest290", DetectFastPatternTest290, 1);
    UtRegisterTest("DetectFastPatternTest291", DetectFastPatternTest291, 1);
    UtRegisterTest("DetectFastPatternTest292", DetectFastPatternTest292, 1);
    UtRegisterTest("DetectFastPatternTest293", DetectFastPatternTest293, 1);
    UtRegisterTest("DetectFastPatternTest294", DetectFastPatternTest294, 1);
    UtRegisterTest("DetectFastPatternTest295", DetectFastPatternTest295, 1);
    UtRegisterTest("DetectFastPatternTest296", DetectFastPatternTest296, 1);
    UtRegisterTest("DetectFastPatternTest297", DetectFastPatternTest297, 1);
    UtRegisterTest("DetectFastPatternTest298", DetectFastPatternTest298, 1);
    UtRegisterTest("DetectFastPatternTest299", DetectFastPatternTest299, 1);
    UtRegisterTest("DetectFastPatternTest300", DetectFastPatternTest300, 1);
    UtRegisterTest("DetectFastPatternTest301", DetectFastPatternTest301, 1);
    /* http_method fast_pattern tests ^ */
    /* http_cookie fast_pattern tests v */
    UtRegisterTest("DetectFastPatternTest302", DetectFastPatternTest302, 1);
    UtRegisterTest("DetectFastPatternTest303", DetectFastPatternTest303, 1);
    UtRegisterTest("DetectFastPatternTest304", DetectFastPatternTest304, 1);
    UtRegisterTest("DetectFastPatternTest305", DetectFastPatternTest305, 1);
    UtRegisterTest("DetectFastPatternTest306", DetectFastPatternTest306, 1);
    UtRegisterTest("DetectFastPatternTest307", DetectFastPatternTest307, 1);
    UtRegisterTest("DetectFastPatternTest308", DetectFastPatternTest308, 1);
    UtRegisterTest("DetectFastPatternTest309", DetectFastPatternTest309, 1);
    UtRegisterTest("DetectFastPatternTest310", DetectFastPatternTest310, 1);
    UtRegisterTest("DetectFastPatternTest311", DetectFastPatternTest311, 1);
    UtRegisterTest("DetectFastPatternTest312", DetectFastPatternTest312, 1);
    UtRegisterTest("DetectFastPatternTest313", DetectFastPatternTest313, 1);
    UtRegisterTest("DetectFastPatternTest314", DetectFastPatternTest314, 1);
    UtRegisterTest("DetectFastPatternTest315", DetectFastPatternTest315, 1);
    UtRegisterTest("DetectFastPatternTest316", DetectFastPatternTest316, 1);
    UtRegisterTest("DetectFastPatternTest317", DetectFastPatternTest317, 1);
    UtRegisterTest("DetectFastPatternTest318", DetectFastPatternTest318, 1);
    UtRegisterTest("DetectFastPatternTest319", DetectFastPatternTest319, 1);
    UtRegisterTest("DetectFastPatternTest320", DetectFastPatternTest320, 1);
    UtRegisterTest("DetectFastPatternTest321", DetectFastPatternTest321, 1);
    UtRegisterTest("DetectFastPatternTest322", DetectFastPatternTest322, 1);
    UtRegisterTest("DetectFastPatternTest323", DetectFastPatternTest323, 1);
    UtRegisterTest("DetectFastPatternTest324", DetectFastPatternTest324, 1);
    UtRegisterTest("DetectFastPatternTest325", DetectFastPatternTest325, 1);
    UtRegisterTest("DetectFastPatternTest326", DetectFastPatternTest326, 1);
    UtRegisterTest("DetectFastPatternTest327", DetectFastPatternTest327, 1);
    UtRegisterTest("DetectFastPatternTest328", DetectFastPatternTest328, 1);
    UtRegisterTest("DetectFastPatternTest329", DetectFastPatternTest329, 1);
    UtRegisterTest("DetectFastPatternTest330", DetectFastPatternTest330, 1);
    UtRegisterTest("DetectFastPatternTest331", DetectFastPatternTest331, 1);
    UtRegisterTest("DetectFastPatternTest332", DetectFastPatternTest332, 1);
    UtRegisterTest("DetectFastPatternTest333", DetectFastPatternTest333, 1);
    UtRegisterTest("DetectFastPatternTest334", DetectFastPatternTest334, 1);
    UtRegisterTest("DetectFastPatternTest335", DetectFastPatternTest335, 1);
    UtRegisterTest("DetectFastPatternTest336", DetectFastPatternTest336, 1);
    UtRegisterTest("DetectFastPatternTest337", DetectFastPatternTest337, 1);
    UtRegisterTest("DetectFastPatternTest338", DetectFastPatternTest338, 1);
    UtRegisterTest("DetectFastPatternTest339", DetectFastPatternTest339, 1);
    UtRegisterTest("DetectFastPatternTest340", DetectFastPatternTest340, 1);
    UtRegisterTest("DetectFastPatternTest341", DetectFastPatternTest341, 1);
    UtRegisterTest("DetectFastPatternTest342", DetectFastPatternTest342, 1);
#endif

    return;
}
