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
 * \author Victor Julien <victor@inliniac.net>
 */

#ifndef __DETECT_PORT_H__
#define __DETECT_PORT_H__

/* prototypes */
void DetectPortRegister (void);

int DetectPortParse(DetectPort **head, char *str);

DetectPort *DetectPortCopy(DetectEngineCtx *, DetectPort *);
DetectPort *DetectPortCopySingle(DetectEngineCtx *, DetectPort *);
int DetectPortInsertCopy(DetectEngineCtx *,DetectPort **, DetectPort *);
int DetectPortInsert(DetectEngineCtx *,DetectPort **, DetectPort *);
void DetectPortCleanupList (DetectPort *head);

DetectPort *DetectPortLookup(DetectPort *head, DetectPort *dp);
int DetectPortAdd(DetectPort **head, DetectPort *dp);

DetectPort *DetectPortLookupGroup(DetectPort *dp, uint16_t port);

void DetectPortPrintMemory(void);

DetectPort *DetectPortDpHashLookup(DetectEngineCtx *, DetectPort *);
DetectPort *DetectPortDpHashGetListPtr(DetectEngineCtx *);
int DetectPortDpHashInit(DetectEngineCtx *);
void DetectPortDpHashFree(DetectEngineCtx *);
int DetectPortDpHashAdd(DetectEngineCtx *, DetectPort *);
void DetectPortDpHashReset(DetectEngineCtx *);

DetectPort *DetectPortSpHashLookup(DetectEngineCtx *, DetectPort *);
int DetectPortSpHashInit(DetectEngineCtx *);
void DetectPortSpHashFree(DetectEngineCtx *);
int DetectPortSpHashAdd(DetectEngineCtx *, DetectPort *);
void DetectPortSpHashReset(DetectEngineCtx *);

int DetectPortJoin(DetectEngineCtx *,DetectPort *target, DetectPort *source);

void DetectPortPrint(DetectPort *);
void DetectPortPrintList(DetectPort *head);
int DetectPortCmp(DetectPort *, DetectPort *);
void DetectPortFree(DetectPort *);

#endif /* __DETECT_PORT_H__ */

