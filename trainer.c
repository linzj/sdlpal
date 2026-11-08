/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// Copyright (c) 2011-2026, SDLPAL development team.
// All rights reserved.
//
// This file is part of SDLPAL.
//
// SDLPAL is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// The in-game trainer, opened with the T key while walking around a scene, or from the
// system menu of the game:
//
//  - Walking: goes to one treasure or exit of the current scene, or collects every treasure,
//    or goes through as many scenes as it takes to the deepest place of a dungeon.
//    It only steers the party the way the arrow keys do, so events and battles still happen
//    as they would for a player, and any key takes back control.
//    Whoever stands in the way of an exit gets talked to, as that is what moves the story on,
//    and a lever that opens the way gets pulled. A way that only lets the party through
//    with something on gets it put on first.
//  - Puzzles: goes to whatever in the scene an item in the inventory is meant to be used on,
//    and uses it, the way the hammer breaks the statues around the lake.
//  - Monsters: walks up to the monsters of the scene and fights them by itself, to level up.
//  - Battles: fights by itself whenever one of the above is going on, and turns on the
//    game's own auto battle for all battles if asked to.
//  - Levels: keeps everyone in the party at a chosen level, including whoever joins later.
//  - Money, items and magics: sets the amount of money, adds any item to the inventory,
//    or teaches or removes any magic.
//
// The game has no list of treasures. Which events are treasures is worked out by reading
// their trigger scripts: see TRAINER_AnalyzeScript() and TRAINER_IsTreasure().
//

#include "main.h"

// The game data keeps both spellings: BIG5 data gets the first, GBK data the second.
#define TR(tw, cn)              (PAL_GetCodePage() == CP_GBK ? L##cn : L##tw)

// Object numbers of the items and magics in the game data.
#define OBJECT_ITEM_FIRST       0x003D
#define OBJECT_ITEM_LAST        0x0126
#define OBJECT_MAGIC_FIRST      0x0127
#define OBJECT_MAGIC_LAST       0x018D

// The party moves 16 pixels across and 8 down at a time on a map of 2048x2048 pixels.
#define GRID_WIDTH              (2048 / 16)
#define GRID_HEIGHT             (2048 / 8)
#define GRID_UNVISITED          0
#define GRID_START              0xFF
#define GRID_BLOCKED            0xFE

#define MAX_ZONES               1024
#define MAX_TARGETS             256
#define MAX_SCRIPT_WALK         2048
#define MAX_SCRIPT_TODO         64
#define MAX_PUZZLES             32

// Planning a route across scenes
#define MAX_PLAN_NODES          512
#define MAX_PLAN_EDGES          128
#define MAX_PLAN_MAPS           48
#define MAX_DUNGEON_SCENES      32  // more than any dungeon has: this must be the world
#define MIN_DUNGEON_FLOORS      3   // fewer than this is no dungeon, and has no deepest place

// How a route may be found: first without touching any event, and if that fails through
// monsters and other events that do not move the party away or start a story. The last one
// is not for walking: it only tells what has to be got past, see TRAINER_FindObstacle().
#define TIER_SAFE               1
#define TIER_THROUGH_MONSTERS   2
#define TIER_THROUGH_OBSTACLES  3

#define START_GRACE_FRAMES      3   // the key that confirmed the menu may still be down
#define STUCK_FRAMES            30  // something the route did not expect is in the way
#define TOAST_FRAMES            30
#define HUNT_IDLE_FRAMES        600 // give up when no battle has come out of this much walking

#define LIST_ROWS               8   // and one more if there is no text below the list
#define MAX_CASH                999999   // the game shows six digits of it
#define LIST_X                  15
#define LIST_Y                  12
#define LIST_LINE_HEIGHT        18
#define LIST_WIDTH              288

#define EVENT(id)               (&gpGlobals->g.lprgEventObject[(id) - 1])
#define FIRST_EVENT_OF(scene)   (gpGlobals->g.rgScene[(scene) - 1].wEventObjectIndex + 1)
#define LAST_EVENT_OF(scene)    (gpGlobals->g.rgScene[(scene)].wEventObjectIndex)
#define SCENE_FIRST_EVENT()     FIRST_EVENT_OF(gpGlobals->wNumScene)
#define SCENE_LAST_EVENT()      LAST_EVENT_OF(gpGlobals->wNumScene)
#define EVENT_IS_ACTIVE(p)      ((p)->sState > 0 && (p)->sVanishTime == 0)
#define TOUCH_RADIUS(p)         (((int)(p)->wTriggerMode - kTriggerTouchNear) * 32 + 16)

typedef enum tagTRAINERJOB
{
   kTrainerJobNone,
   kTrainerJobWalk,      // go to one event
   kTrainerJobCollect,   // go to every treasure, nearest first
   kTrainerJobRoute,     // go to another scene through as many as it takes
   kTrainerJobHunt,      // fight the monsters of the scene, nearest first
   kTrainerJobPuzzle,    // use items on what they are meant to be used on
} TRAINERJOB;

typedef struct tagTRAINERROUTE
{
   INT             iDist;     // number of steps
   PALDIRECTION    dirStep;   // where to go now, unknown if already there
   PALDIRECTION    dirFace;   // where to face when there, to search for the event
   WORD            wTarget;   // the event reached
} TRAINERROUTE, *LPTRAINERROUTE;

typedef struct tagTRAINERZONE
{
   INT             x, y, iRadius;
} TRAINERZONE;

// Something in the way that going up to it, or to a lever, may get out of the way.
typedef struct tagTRAINEROBSTACLE
{
   WORD            wEvent;    // what to go up to
   INT             x, y, iRadius;
} TRAINEROBSTACLE;

// A way out of a place reachable on foot, and where it leads.
typedef struct tagTRAINEREDGE
{
   WORD            wEvent;
   WORD            wScene;
   INT             x, y;      // -1 if the script does not tell
   INT             iDist;     // -1 if it cannot be reached
   INT             iRide;     // how far it carries the party, as that takes time too
} TRAINEREDGE;

// A place the party can get to: a scene, and where it arrives there.
typedef struct tagTRAINERNODE
{
   WORD            wScene;
   INT             x, y;
   INT             iSteps;
   INT             nFloors;   // how many times the scene changes on the way
   INT             iFrom;     // the node this is reached from, -1 for where it all starts
   WORD            wEvent;    // the event in that node that leads here
   BOOL            fExpanded;
} TRAINERNODE;

// Something an item in the inventory is meant to be used on.
typedef struct tagTRAINERPUZZLE
{
   WORD            wEvent;
   WORD            wItem;
   INT             iRadius;   // how close the party has to be
} TRAINERPUZZLE;

typedef struct tagTRAINERTARGET
{
   WORD            wEvent;
   INT             iDist;     // -1 if there is no way to get there
   WORD            wObstacle; // what has to be got past first, and iDist is how far that is
} TRAINERTARGET;

static struct
{
   TRAINERJOB      job;
   WORD            wScene;
   WORD            wTarget;
   PALDIRECTION    dirStep;
   INT             nFrames;
   INT             nStuckFrames;
   INT             nStepsLeft;
   PAL_POS         posLast;
   INT             nCollected;
   INT             nUnreachable;
   WORD            rgwTried[MAX_TARGETS];
   INT             nTried;
   WORD            wGoalScene;
   WORD            wHuntLevel;   // 0 = until stopped
   INT             nFights;
   INT             nIdleFrames;
   WORD            wHop;         // the raft, teleporter or obstacle on the way to the target, if any
   BOOL            fObstacles;   // go up to what stands in the way, which may start a story
} g_Walk = { kTrainerJobNone, 0, 0, kDirUnknown };

static WCHAR            g_szToast[64];
static INT              g_nToastFrames = 0;
static WORD             g_wAutoLevel = 0;     // 0 = off
static BOOL             g_fAutoBattle = FALSE;
static WORD             g_wCurrentScene = 0;
static WORD             g_wPreviousScene = 0;

static TRAINERZONE      g_rgZone[MAX_ZONES];
static INT              g_nZone;
static TRAINEROBSTACLE  g_rgObstacle[MAX_TARGETS];
static INT              g_nObstacle;

static TRAINERTARGET    g_rgTarget[MAX_TARGETS];
static INT              g_nTarget;

static TRAINERPUZZLE    g_rgPuzzle[MAX_PUZZLES];
static INT              g_nPuzzle;

// Events to look for while planning, in the current scene.
static struct
{
   WORD            wEvent;
   INT             iSteps;
   INT             iNode;     // the place they can be walked to from, -1 if none was found
} g_rgWant[MAX_TARGETS];
static INT              g_nWant = 0;
static BOOL             g_fWantNearest = FALSE;

static TRAINEREDGE      g_rgEdge[MAX_PLAN_EDGES];
static INT              g_iFloodNode = -1;   // the place being planned from, if any
static INT              g_nEdge;
static TRAINERNODE      g_rgNode[MAX_PLAN_NODES];
static INT              g_nNode;
static BYTE             g_rgbFloor[MAX_SCENES + 1];           // how deep each scene of the dungeon is, 0 if not in it
static BYTE             g_rgbOutside[(MAX_SCENES + 8) / 8];   // the scenes the dungeon leads out to

static WORD             g_rgwObject[OBJECT_MAGIC_LAST - OBJECT_ITEM_FIRST + 1];
static INT              g_nObject;
static WORD             g_wMagicRole;

static WCHAR            g_szLabel[80];

static BYTE
TRAINER_JumpOperands(
   WORD           wOperation
)
/*++
  Purpose:

    Tell which operands of a script operation may be the address of a jump.

  Parameters:

    [IN]  wOperation - the operation code.

  Return value:

    A bit for each operand: bit 0 for the first one and so on.

--*/
{
   switch (wOperation)
   {
   case 0x0003: case 0x0004: case 0x000A: case 0x0033: case 0x0034: case 0x0038:
   case 0x003A: case 0x0061: case 0x0068: case 0x0074: case 0x0091:
      return 1;

   case 0x0006: case 0x001E: case 0x005D: case 0x005E: case 0x0064: case 0x0079:
   case 0x0095: case 0x009C:
      return 2;

   case 0x0020: case 0x002E: case 0x0058: case 0x0081: case 0x0083: case 0x0084:
   case 0x0086: case 0x0094: case 0x009E:
      return 4;

   case 0x0007:
      return 2 | 4;

   default:
      return 0;
   }
}

static VOID
TRAINER_PushNextLines(
   const SCRIPTENTRY   *p,
   WORD                 wCurrent,
   WORD                *rgwTodo,
   INT                 *pnTodo
)
/*++
  Purpose:

    Add the lines of a script that can come after a line to the ones still to be looked at.

    The last one added is the next one to be looked at, so that the first item found
    is the first one in the script.

  Parameters:

    [IN]      p - the line.

    [IN]      wCurrent - the address of the line.

    [OUT]     rgwTodo - the lines to be looked at.

    [IN/OUT]  pnTodo - how many of them there are.

  Return value:

    None.

--*/
{
   const BYTE   bJumps = TRAINER_JumpOperands(p->wOperation);
   BOOL         fNext = TRUE;
   INT          i;

   if (p->wOperation == 0x0000 || p->wOperation == 0x0001)
   {
      fNext = FALSE;
   }
   else if (p->wOperation == 0x0002 || p->wOperation == 0x0003)
   {
      //
      // Goes on to the next line only after having been run the given number of times
      //
      fNext = (p->rgwOperand[1] != 0);
   }

   for (i = 0; i < 3; i++)
   {
      if ((bJumps & (1 << i)) && p->rgwOperand[i] != 0 && *pnTodo < MAX_SCRIPT_TODO)
      {
         rgwTodo[(*pnTodo)++] = p->rgwOperand[i];
      }
   }

   if (fNext && *pnTodo < MAX_SCRIPT_TODO)
   {
      rgwTodo[(*pnTodo)++] = wCurrent + 1;
   }
}

static VOID
TRAINER_ReadScript(
   WORD                  wEntry,
   LPTRAINERSCRIPTINFO   pInfo,
   WORD                 *pwNext
)
/*++
  Purpose:

    Find out what a trigger script does when it runs once, by following every branch and
    call that can be reached from its entry.

  Parameters:

    [IN]  wEntry - the entry of the script.

    [OUT] pInfo - what the script can do.

    [OUT] pwNext - where it goes on from the next time, if it stops to wait for that.

  Return value:

    None.

--*/
{
   static BYTE    rgbSeen[65536 / 8];
   WORD           rgwTodo[MAX_SCRIPT_TODO];
   INT            nTodo = 0, nWalked = 0;
   BOOL           fWindow = FALSE, fText = FALSE, fRelative = FALSE;

   memset(pInfo, 0, sizeof(TRAINERSCRIPTINFO));
   memset(rgbSeen, 0, sizeof(rgbSeen));

   *pwNext = 0;
   pInfo->wEntry = wEntry;
   rgwTodo[nTodo++] = wEntry;

   while (nTodo > 0)
   {
      const WORD            wCurrent = rgwTodo[--nTodo];
      const SCRIPTENTRY    *p;

      if (wCurrent == 0 || wCurrent >= gpGlobals->g.nScriptEntry ||
         (rgbSeen[wCurrent >> 3] & (1 << (wCurrent & 7))))
      {
         continue;
      }

      rgbSeen[wCurrent >> 3] |= (1 << (wCurrent & 7));
      p = &gpGlobals->g.lprgScriptEntry[wCurrent];

      if (++nWalked > MAX_SCRIPT_WALK)
      {
         pInfo->wFlags |= kTrainerScriptStory | kTrainerScriptOther;
         return;
      }

      switch (p->wOperation)
      {
      case 0x0000:
      case 0x0003:
         break;

      case 0x0001:   // stops, and goes on from the next line the next time
         if (*pwNext == 0)
         {
            *pwNext = wCurrent + 1;
         }
         break;

      case 0x0002:   // stops, and goes on from another line the next time
         if (*pwNext == 0 && p->rgwOperand[1] == 0)
         {
            *pwNext = p->rgwOperand[0];
         }
         break;

      case 0x0086:
         //
         // Goes on only if someone in the party wears an item. The trainer puts it on before
         // going there, see TRAINER_PutOn(), so what happens without it is left out.
         //
         if (pInfo->wWear == 0)
         {
            pInfo->wWear = p->rgwOperand[0];
         }
         if (nTodo < MAX_SCRIPT_TODO)
         {
            rgwTodo[nTodo++] = wCurrent + 1;
         }
         continue;   // instead of following the jump as well

      case 0x003E:   // text in a small window, which goes away by itself
         fWindow = TRUE;
         break;

      case 0xFFFF:   // text
         fText = TRUE;
         break;

      case 0x0004:   // call
      case 0x0005:   // redraw the screen
      case 0x0014:   // change the picture of the event, which opens a chest
      case 0x0047:   // sound
      case 0x0049:   // hide the event
         break;

      case 0x001E:
         if ((SHORT)p->rgwOperand[0] > 0)
         {
            pInfo->wFlags |= kTrainerScriptCash;
            pInfo->nLoot++;
            if (pInfo->wCash == 0)
            {
               pInfo->wCash = p->rgwOperand[0];
            }
         }
         else
         {
            pInfo->wFlags |= kTrainerScriptOther;   // this one takes money
         }
         break;

      case 0x001F:
         pInfo->wFlags |= kTrainerScriptItem;
         pInfo->nLoot++;
         if (pInfo->wItem == 0)
         {
            pInfo->wItem = p->rgwOperand[0];
         }
         break;

      case 0x0007:
         pInfo->wFlags |= kTrainerScriptBattle | kTrainerScriptOther;
         break;

      case 0x0046:
         if (!(pInfo->wFlags & kTrainerScriptMove))
         {
            pInfo->xMove = p->rgwOperand[0] * 32 + p->rgwOperand[2] * 16;
            pInfo->yMove = p->rgwOperand[1] * 16 + p->rgwOperand[2] * 8;
         }
         pInfo->wFlags |= kTrainerScriptMove | kTrainerScriptOther;
         break;

      case 0x0059:
         pInfo->wFlags |= kTrainerScriptScene | kTrainerScriptOther;
         if (pInfo->wScene == 0)
         {
            pInfo->wScene = p->rgwOperand[0];
         }
         break;

      case 0x003C:
      case 0x003D:
         pInfo->wFlags |= kTrainerScriptStory | kTrainerScriptOther;
         break;

      case 0x003F:   // a raft and the like
      case 0x0044:
      case 0x0097:
         pInfo->xRide = p->rgwOperand[0] * 32 + p->rgwOperand[2] * 16;
         pInfo->yRide = p->rgwOperand[1] * 16 + p->rgwOperand[2] * 8;
         pInfo->wFlags |= kTrainerScriptRide;
         // fall through

      case 0x0070:   // walks the party
      case 0x007A:
      case 0x007B:
         //
         // The last one is where it ends.
         //
         pInfo->xCarry = p->rgwOperand[0] * 32 + p->rgwOperand[2] * 16;
         pInfo->yCarry = p->rgwOperand[1] * 16 + p->rgwOperand[2] * 8;
         pInfo->wFlags |= kTrainerScriptCarry | kTrainerScriptCarryTo | kTrainerScriptOther;
         break;

      case 0x006E:   // one big step from wherever the party is
      case 0x007F:   // moves the view
         fRelative = TRUE;
         pInfo->wFlags |= kTrainerScriptCarry | kTrainerScriptOther;
         break;

      default:
         pInfo->wFlags |= kTrainerScriptOther;
         break;
      }

      TRAINER_PushNextLines(p, wCurrent, rgwTodo, &nTodo);
   }

   if (fRelative)
   {
      pInfo->wFlags &= ~kTrainerScriptCarryTo;
   }

   //
   // Text that waits for a key is someone saying something, not to be walked into by itself.
   // Every monster has some of that too, for when the party loses.
   //
   if (fText && !fWindow && !(pInfo->wFlags & kTrainerScriptBattle))
   {
      pInfo->wFlags |= kTrainerScriptStory;
   }
}

VOID
TRAINER_AnalyzeScript(
   WORD                  wEntry,
   LPTRAINERSCRIPTINFO   pInfo
)
/*++
  Purpose:

    Find out what a trigger script can do without running it, by following every branch
    and call that can be reached from its entry.

    A script stopping with operation 0x0001 or 0x0002 continues somewhere else the next
    time it is triggered. That is mostly not followed, as it is not what triggering it now
    does: a chest gives its items once and tells that it is empty from then on.

    A door that opens is different: the first time only gets it ready, and leaves the
    party where it is, and the next time takes the party to another scene. An event that
    is touched gets triggered again in the very next frame while the party stands there,
    so going through is what touching it does.

  Parameters:

    [IN]  wEntry - the entry of the script.

    [OUT] pInfo - what the script can do.

  Return value:

    None.

--*/
{
   TRAINERSCRIPTINFO   next;
   WORD                wNext, wAfter;

   TRAINER_ReadScript(wEntry, pInfo, &wNext);

   if (wNext == 0 || wNext == wEntry ||
      (pInfo->wFlags & (kTrainerScriptScene | kTrainerScriptMove | kTrainerScriptCarry |
         kTrainerScriptStory | kTrainerScriptBattle)) != 0)
   {
      return;
   }

   TRAINER_ReadScript(wNext, &next, &wAfter);

   if ((next.wFlags & kTrainerScriptScene) == 0)
   {
      return;
   }

   //
   // What it gives, if anything, has already been counted the first time.
   //
   pInfo->wFlags |= next.wFlags & ~(kTrainerScriptItem | kTrainerScriptCash);
   pInfo->wScene = next.wScene;
   pInfo->xMove = next.xMove;
   pInfo->yMove = next.yMove;
   pInfo->xCarry = next.xCarry;
   pInfo->yCarry = next.yCarry;
   pInfo->xRide = next.xRide;
   pInfo->yRide = next.yRide;

   if (pInfo->wWear == 0)
   {
      pInfo->wWear = next.wWear;
   }
}

static const TRAINERSCRIPTINFO *
TRAINER_GetScriptInfo(
   WORD           wEntry
)
/*++
  Purpose:

    Get what a script can do. Routes are planned every frame around the events of a whole
    scene, while scripts never change, so the answers are remembered.

  Parameters:

    [IN]  wEntry - the entry of the script.

  Return value:

    What the script can do.

--*/
{
   static TRAINERSCRIPTINFO   rgCache[4096];
   LPTRAINERSCRIPTINFO        p = &rgCache[wEntry % 4096];

   if (p->wEntry != wEntry)
   {
      TRAINER_AnalyzeScript(wEntry, p);
   }

   return p;
}

static BOOL
TRAINER_IsTreasure(
   WORD           wEventObjectID
)
/*++
  Purpose:

    Tell whether an event is a treasure: searching it does nothing but give things.
    That leaves out people who give things as part of the story, shops and fights.

  Parameters:

    [IN]  wEventObjectID - the event.

  Return value:

    TRUE if the event is a treasure that has not been taken.

--*/
{
   const EVENTOBJECT         *p = EVENT(wEventObjectID);
   const TRAINERSCRIPTINFO   *pInfo;

   if (!EVENT_IS_ACTIVE(p) || p->wTriggerMode < kTriggerSearchNear || p->wTriggerMode > kTriggerSearchFar)
   {
      return FALSE;
   }

   pInfo = TRAINER_GetScriptInfo(p->wTriggerScript);

   return (pInfo->wFlags & (kTrainerScriptItem | kTrainerScriptCash)) != 0 &&
      (pInfo->wFlags & kTrainerScriptOther) == 0;
}

static BOOL
TRAINER_LeadsElsewhere(
   WORD           wEventObjectID
)
/*++
  Purpose:

    Tell whether an event takes the party to another scene, whatever else it does.

  Parameters:

    [IN]  wEventObjectID - the event.

  Return value:

    TRUE if the event leads to another scene.

--*/
{
   const EVENTOBJECT         *p = EVENT(wEventObjectID);
   const TRAINERSCRIPTINFO   *pInfo;

   if (!EVENT_IS_ACTIVE(p) || p->wTriggerMode == kTriggerNone)
   {
      return FALSE;
   }

   pInfo = TRAINER_GetScriptInfo(p->wTriggerScript);

   return (pInfo->wFlags & kTrainerScriptScene) != 0 && pInfo->wScene != gpGlobals->wNumScene;
}

static BOOL
TRAINER_HasMonsterScript(
   const EVENTOBJECT   *p
)
{
   const WORD wFlags = TRAINER_GetScriptInfo(p->wTriggerScript)->wFlags;

   return p->wTriggerMode >= kTriggerTouchNear && (wFlags & kTrainerScriptBattle) != 0 &&
      (wFlags & (kTrainerScriptScene | kTrainerScriptStory | kTrainerScriptMove | kTrainerScriptCarry)) == 0;
}

static BOOL
TRAINER_IsMonster(
   WORD           wEventObjectID
)
/*++
  Purpose:

    Tell whether an event is a monster to fight right now: touching it starts a battle and
    nothing else happens. That leaves out the bosses, who have something to say first.

  Parameters:

    [IN]  wEventObjectID - the event.

  Return value:

    TRUE if the event is such a monster.

--*/
{
   const EVENTOBJECT *p = EVENT(wEventObjectID);

   return EVENT_IS_ACTIVE(p) && TRAINER_HasMonsterScript(p);
}

static BOOL
TRAINER_IsWorn(
   WORD           wItem
)
{
   int   i, j;

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      for (j = 0; j < MAX_PLAYER_EQUIPMENTS; j++)
      {
         if (gpGlobals->g.PlayerRoles.rgwEquipment[j][gpGlobals->rgParty[i].wPlayerRole] == wItem)
         {
            return TRUE;
         }
      }
   }

   return FALSE;
}

static INT
TRAINER_FindWearer(
   WORD           wItem
)
/*++
  Purpose:

    Find who in the party can put on an item from the inventory.

  Parameters:

    [IN]  wItem - the item.

  Return value:

    The index in the party, or -1 if the item is not in the inventory or nobody can wear it.

--*/
{
   int   i;

   if (PAL_GetItemAmount(wItem) <= 0)
   {
      return -1;
   }

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      if (gpGlobals->g.rgObject[wItem].item.wFlags & (kItemFlagEquipableByPlayerRole_First << gpGlobals->rgParty[i].wPlayerRole))
      {
         return i;
      }
   }

   return -1;
}

static BOOL
TRAINER_CanWear(
   WORD           wItem
)
/*++
  Purpose:

    Tell whether someone in the party wears an item an event asks for, or can put it on.

  Parameters:

    [IN]  wItem - the item, 0 for none.

  Return value:

    TRUE if the event will let the party through.

--*/
{
   return wItem == 0 || TRAINER_IsWorn(wItem) || TRAINER_FindWearer(wItem) >= 0;
}

static BOOL
TRAINER_IsWayOut(
   const EVENTOBJECT   *p
)
/*++
  Purpose:

    Tell whether an event moves the party when used, to somewhere the script tells or to
    another scene, and nothing else stands in the way: no story, no fight, and nothing to
    wear that the party does not have.

  Parameters:

    [IN]  p - the event.

  Return value:

    TRUE if the event is a way out.

--*/
{
   const TRAINERSCRIPTINFO   *pInfo = TRAINER_GetScriptInfo(p->wTriggerScript);
   const WORD                 wFlags = pInfo->wFlags;

   return EVENT_IS_ACTIVE(p) && p->wTriggerMode != kTriggerNone &&
      (wFlags & (kTrainerScriptScene | kTrainerScriptMove | kTrainerScriptCarryTo)) != 0 &&
      (wFlags & (kTrainerScriptStory | kTrainerScriptBattle)) == 0 && TRAINER_CanWear(pInfo->wWear);
}

static BOOL
TRAINER_WasTried(
   WORD           wEventObjectID
)
{
   int   i;

   for (i = 0; i < g_Walk.nTried; i++)
   {
      if (g_Walk.rgwTried[i] == wEventObjectID)
      {
         return TRUE;
      }
   }

   return FALSE;
}

static BOOL
TRAINER_CanBeGotPast(
   WORD           wEventObjectID
)
/*++
  Purpose:

    Tell whether an event in the way may get out of the way by going up to it: someone to
    talk to, a boss, or a story that starts at some place. A way out does not, as it takes
    the party away, and neither does what has already been tried without moving aside.

  Parameters:

    [IN]  wEventObjectID - the event.

  Return value:

    TRUE if it is worth going up to the event.

--*/
{
   const EVENTOBJECT *p = EVENT(wEventObjectID);

   //
   // Nothing can stand close enough to touch a blocker of the nearest kind.
   //
   if (!EVENT_IS_ACTIVE(p) || p->wTriggerMode == kTriggerNone || p->wTriggerScript == 0 ||
      (p->wTriggerMode == kTriggerTouchNear && p->sState >= kObjStateBlocker))
   {
      return FALSE;
   }

   return !TRAINER_IsWayOut(p) && !(g_Walk.job != kTrainerJobNone && TRAINER_WasTried(wEventObjectID));
}

static WORD
TRAINER_FindLever(
   WORD           wBlocker
)
/*++
  Purpose:

    Find a lever in the current scene that gets something out of the way, as the ones that
    open the gates of the tomb: triggering it does nothing else a player has to be there for.

  Parameters:

    [IN]  wBlocker - what is in the way.

  Return value:

    The lever, or 0 if there is none.

--*/
{
   static BYTE   rgbSeen[65536 / 8];
   int           id;

   for (id = SCENE_FIRST_EVENT(); id <= SCENE_LAST_EVENT(); id++)
   {
      const EVENTOBJECT   *p = EVENT(id);
      WORD                 rgwTodo[MAX_SCRIPT_TODO];
      INT                  nTodo = 0, nWalked = 0;

      if (id == wBlocker || !EVENT_IS_ACTIVE(p) || p->wTriggerMode == kTriggerNone || p->wTriggerScript == 0 ||
         (TRAINER_GetScriptInfo(p->wTriggerScript)->wFlags & (kTrainerScriptScene | kTrainerScriptStory |
            kTrainerScriptBattle | kTrainerScriptMove | kTrainerScriptCarry)) != 0 ||
         (g_Walk.job != kTrainerJobNone && TRAINER_WasTried((WORD)id)))
      {
         continue;
      }

      memset(rgbSeen, 0, sizeof(rgbSeen));
      rgwTodo[nTodo++] = p->wTriggerScript;

      while (nTodo > 0 && ++nWalked < MAX_SCRIPT_WALK)
      {
         const WORD          wCurrent = rgwTodo[--nTodo];
         const SCRIPTENTRY  *q;

         if (wCurrent == 0 || wCurrent >= gpGlobals->g.nScriptEntry ||
            (rgbSeen[wCurrent >> 3] & (1 << (wCurrent & 7))))
         {
            continue;
         }

         rgbSeen[wCurrent >> 3] |= (1 << (wCurrent & 7));
         q = &gpGlobals->g.lprgScriptEntry[wCurrent];

         if (q->wOperation == 0x0049 && q->rgwOperand[0] == wBlocker && (SHORT)q->rgwOperand[1] < kObjStateBlocker)
         {
            return (WORD)id;
         }

         TRAINER_PushNextLines(q, wCurrent, rgwTodo, &nTodo);
      }
   }

   return 0;
}

static VOID
TRAINER_BuildZones(
   const WORD    *rgwTarget,
   INT            nTarget,
   INT            iTier
)
/*++
  Purpose:

    List the places around the events of the current scene that a route has to stay out of.
    With TIER_THROUGH_OBSTACLES those it may be possible to get past go to g_rgObstacle,
    along with what to go up to for that.

  Parameters:

    [IN]  rgwTarget - the events the route is going to. Touching them is the point.

    [IN]  nTarget - the number of those events.

    [IN]  iTier - one of the TIER_ values.

  Return value:

    None.

--*/
{
   int   id, i;

   g_nZone = 0;
   g_nObstacle = 0;

   for (id = SCENE_FIRST_EVENT(); id <= SCENE_LAST_EVENT() && g_nZone < MAX_ZONES; id++)
   {
      const EVENTOBJECT         *p = EVENT(id);
      const TRAINERSCRIPTINFO   *pInfo = TRAINER_GetScriptInfo(p->wTriggerScript);
      const BOOL                 fDangerous = (pInfo->wFlags & (kTrainerScriptScene | kTrainerScriptStory | kTrainerScriptMove | kTrainerScriptCarry)) != 0;
      const BOOL                 fMonster = (pInfo->wFlags & kTrainerScriptBattle) != 0 && !fDangerous;
      BOOL                       fIsTarget = FALSE;
      int                        iRadius = 0;
      WORD                       wWho = 0;

      for (i = 0; i < nTarget; i++)
      {
         if (rgwTarget[i] == id)
         {
            fIsTarget = TRUE;
            break;
         }
      }

      //
      // This has to match PAL_CheckObstacle(), except that a monster in the way is no
      // obstacle when going through monsters: the battle starts before bumping into it.
      //
      if (p->sState >= kObjStateBlocker && !(iTier >= TIER_THROUGH_MONSTERS && fMonster))
      {
         iRadius = 16;
      }

      //
      // This has to match the touch check in PAL_GameUpdate().
      //
      if (!fIsTarget && EVENT_IS_ACTIVE(p) && p->wTriggerMode >= kTriggerTouchNear &&
         p->wTriggerScript != 0 && (iTier < TIER_THROUGH_MONSTERS || fDangerous) &&
         TOUCH_RADIUS(p) > iRadius)
      {
         iRadius = TOUCH_RADIUS(p);
      }

      if (iRadius > 0 && iTier >= TIER_THROUGH_OBSTACLES && !fIsTarget && g_nObstacle < MAX_TARGETS)
      {
         //
         // What to go up to: the event itself, or a lever somewhere else that clears it.
         //
         wWho = TRAINER_CanBeGotPast((WORD)id) ? (WORD)id : 0;

         if (wWho == 0 && p->sState >= kObjStateBlocker)
         {
            wWho = TRAINER_FindLever((WORD)id);
         }
      }

      if (wWho != 0)
      {
         g_rgObstacle[g_nObstacle].wEvent = wWho;
         g_rgObstacle[g_nObstacle].x = p->x;
         g_rgObstacle[g_nObstacle].y = p->y;
         g_rgObstacle[g_nObstacle].iRadius = iRadius;
         g_nObstacle++;
      }
      else if (iRadius > 0)
      {
         g_rgZone[g_nZone].x = p->x;
         g_rgZone[g_nZone].y = p->y;
         g_rgZone[g_nZone].iRadius = iRadius;
         g_nZone++;
      }
   }
}

static BOOL
TRAINER_IsFree(
   int            x,
   int            y
)
/*++
  Purpose:

    Tell whether the party can stand at a position, given the zones listed.

  Parameters:

    [IN]  x - the X coordinate on the map.

    [IN]  y - the Y coordinate on the map.

  Return value:

    TRUE if the party can stand there.

--*/
{
   int   i;

   if (PAL_CheckObstacleWithRange(PAL_XY(x, y), FALSE, 0, TRUE))
   {
      return FALSE;
   }

   for (i = 0; i < g_nZone; i++)
   {
      if (abs(g_rgZone[i].x - x) + abs(g_rgZone[i].y - y) * 2 < g_rgZone[i].iRadius)
      {
         return FALSE;
      }
   }

   return TRUE;
}

static const TRAINERPUZZLE *
TRAINER_FindPuzzle(
   const EVENTOBJECT   *p
)
{
   const WORD   wEvent = (WORD)(p - gpGlobals->g.lprgEventObject) + 1;
   int          i;

   if (g_Walk.job == kTrainerJobPuzzle)
   {
      for (i = 0; i < g_nPuzzle; i++)
      {
         if (g_rgPuzzle[i].wEvent == wEvent)
         {
            return &g_rgPuzzle[i];
         }
      }
   }

   return NULL;
}

static BOOL
TRAINER_CanReachEvent(
   int                  x,
   int                  y,
   const EVENTOBJECT   *p,
   PALDIRECTION        *pdirFace
)
/*++
  Purpose:

    Tell whether an event can be triggered from a position.

  Parameters:

    [IN]  x - the X coordinate on the map.

    [IN]  y - the Y coordinate on the map.

    [IN]  p - the event.

    [OUT] pdirFace - where the party has to face to search for the event.

  Return value:

    TRUE if the event can be triggered from there.

--*/
{
   const TRAINERPUZZLE   *pPuzzle = TRAINER_FindPuzzle(p);
   const int              dx = abs((int)p->x - x), dy = abs((int)p->y - y);
   int                    dir, i;

   *pdirFace = kDirUnknown;

   if (pPuzzle != NULL)
   {
      //
      // This has to match operation 0x0081 in the script of the item.
      //
      for (dir = kDirSouth; dir < kDirUnknown; dir++)
      {
         const int ex = (int)p->x + ((dir == kDirWest || dir == kDirSouth) ? 16 : -16) - x;
         const int ey = (int)p->y + ((dir == kDirWest || dir == kDirNorth) ? 8 : -8) - y;

         if (abs(ex) + abs(ey * 2) < pPuzzle->iRadius)
         {
            *pdirFace = (PALDIRECTION)dir;
            return TRUE;
         }
      }

      return FALSE;
   }

   if (p->wTriggerMode >= kTriggerTouchNear)
   {
      return dx + dy * 2 < TOUCH_RADIUS(p);
   }

   //
   // Most positions are nowhere near the event and do not need the check below.
   //
   if (dx > 96 || dy > 64)
   {
      return FALSE;
   }

   //
   // This has to match PAL_Search().
   //
   for (dir = kDirSouth; dir < kDirUnknown; dir++)
   {
      const TRIGGERRANGE range = PAL_GetSearchTriggerRangeAt(x, y, (WORD)dir);

      for (i = 0; i < 13 && i < (int)p->wTriggerMode * 6 - 4; i++)
      {
         const int rx = PAL_X(range.rgPos[i]), ry = PAL_Y(range.rgPos[i]);

         if (rx / 32 == p->x / 32 && ry / 16 == p->y / 16 && (rx % 32 != 0) == (p->x % 32 != 0))
         {
            *pdirFace = (PALDIRECTION)dir;
            return TRUE;
         }
      }
   }

   return FALSE;
}

static BOOL
TRAINER_FindRouteInTier(
   const WORD       *rgwTarget,
   INT               nTarget,
   INT               iTier,
   LPTRAINERROUTE    pRoute
)
/*++
  Purpose:

    Find the shortest route from the party to the nearest one of some events, with a
    breadth-first search over everywhere the party can walk to.

    The party keeps to a grid of 16x8 pixels that goes through wherever it stands.
    The map tiles are diamonds and the grid covers the odd and even rows of them.

  Parameters:

    [IN]  rgwTarget - the events to go to.

    [IN]  nTarget - the number of those events.

    [IN]  iTier - TIER_SAFE or TIER_THROUGH_MONSTERS.

    [OUT] pRoute - the route found.

  Return value:

    TRUE if there is a route.

--*/
{
   static BYTE    rgbFirstStep[GRID_WIDTH * GRID_HEIGHT];   // 1 + direction of the first step
   static WORD    rgwQueue[GRID_WIDTH * GRID_HEIGHT];
   static WORD    rgwDist[GRID_WIDTH * GRID_HEIGHT];

   const int      xStart = PAL_X(gpGlobals->viewport) + PAL_X(gpGlobals->partyoffset);
   const int      yStart = PAL_Y(gpGlobals->viewport) + PAL_Y(gpGlobals->partyoffset);
   const int      xRest = xStart & 15, yRest = yStart & 7;
   const int      iStart = (yStart >> 3) * GRID_WIDTH + (xStart >> 4);
   int            iHead = 0, iTail = 0, i;

   if (xStart < 0 || yStart < 0 || iStart >= GRID_WIDTH * GRID_HEIGHT)
   {
      return FALSE;
   }

   TRAINER_BuildZones(rgwTarget, nTarget, iTier);

   memset(rgbFirstStep, GRID_UNVISITED, sizeof(rgbFirstStep));
   rgbFirstStep[iStart] = GRID_START;
   rgwDist[iStart] = 0;
   rgwQueue[iTail++] = (WORD)iStart;

   while (iHead < iTail)
   {
      const int      iNode = rgwQueue[iHead++];
      const int      gx = iNode % GRID_WIDTH, gy = iNode / GRID_WIDTH;
      const int      x = gx * 16 + xRest, y = gy * 8 + yRest;
      int            dir;

      for (i = 0; i < nTarget; i++)
      {
         if (TRAINER_CanReachEvent(x, y, EVENT(rgwTarget[i]), &pRoute->dirFace))
         {
            pRoute->iDist = rgwDist[iNode];
            pRoute->dirStep = (iNode == iStart) ? kDirUnknown : (PALDIRECTION)(rgbFirstStep[iNode] - 1);
            pRoute->wTarget = rgwTarget[i];
            return TRUE;
         }
      }

      for (dir = kDirSouth; dir < kDirUnknown; dir++)
      {
         const int   gxNext = gx + ((dir == kDirWest || dir == kDirSouth) ? -1 : 1);
         const int   gyNext = gy + ((dir == kDirWest || dir == kDirNorth) ? -1 : 1);
         int         iNext;

         if (gxNext < 0 || gxNext >= GRID_WIDTH || gyNext < 0 || gyNext >= GRID_HEIGHT)
         {
            continue;
         }

         iNext = gyNext * GRID_WIDTH + gxNext;

         if (rgbFirstStep[iNext] != GRID_UNVISITED)
         {
            continue;
         }

         if (!TRAINER_IsFree(gxNext * 16 + xRest, gyNext * 8 + yRest))
         {
            rgbFirstStep[iNext] = GRID_BLOCKED;
            continue;
         }

         rgbFirstStep[iNext] = (iNode == iStart) ? (BYTE)(dir + 1) : rgbFirstStep[iNode];
         rgwDist[iNext] = rgwDist[iNode] + 1;
         rgwQueue[iTail++] = (WORD)iNext;
      }
   }

   return FALSE;
}

static BOOL
TRAINER_FindRoute(
   const WORD       *rgwTarget,
   INT               nTarget,
   LPTRAINERROUTE    pRoute
)
/*++
  Purpose:

    Find a route to the nearest one of some events, staying away from everything if possible.

  Parameters:

    [IN]  rgwTarget - the events to go to.

    [IN]  nTarget - the number of those events.

    [OUT] pRoute - the route found.

  Return value:

    TRUE if there is a route.

--*/
{
   return TRAINER_FindRouteInTier(rgwTarget, nTarget, TIER_SAFE, pRoute) ||
      TRAINER_FindRouteInTier(rgwTarget, nTarget, TIER_THROUGH_MONSTERS, pRoute);
}

static WORD
TRAINER_ObstacleAt(
   int            x,
   int            y
)
{
   int   i;

   for (i = 0; i < g_nObstacle; i++)
   {
      if (abs(g_rgObstacle[i].x - x) + abs(g_rgObstacle[i].y - y) * 2 < g_rgObstacle[i].iRadius)
      {
         return g_rgObstacle[i].wEvent;
      }
   }

   return 0;
}

static BOOL
TRAINER_FindObstacle(
   WORD           wTarget,
   WORD          *pwObstacle
)
/*++
  Purpose:

    Find what to go up to for getting to an event there is no route to: the first thing on
    the way that may let the party through once it has had its say.

    The way is the one past as few of those as possible, with a breadth-first search that
    only goes past one more when there is nowhere else left to go. People in a crowd can
    be walked around, so the shortest way through them may not be what is really in the way.

  Parameters:

    [IN]  wTarget - the event to get to.

    [OUT] pwObstacle - what to go up to.

  Return value:

    TRUE if there is something to go up to.

--*/
{
   static BYTE    rgbState[GRID_WIDTH * GRID_HEIGHT];   // one of the values below
   static WORD    rgwQueue[GRID_WIDTH * GRID_HEIGHT];
   static WORD    rgwPast[GRID_WIDTH * GRID_HEIGHT];    // where going past one more leads to
   static WORD    rgwFirst[GRID_WIDTH * GRID_HEIGHT];   // the first obstacle on the way to here

   enum { kUnvisited, kVisited, kPast };

   const int      xStart = PAL_X(gpGlobals->viewport) + PAL_X(gpGlobals->partyoffset);
   const int      yStart = PAL_Y(gpGlobals->viewport) + PAL_Y(gpGlobals->partyoffset);
   const int      xRest = xStart & 15, yRest = yStart & 7;
   const int      iStart = (yStart >> 3) * GRID_WIDTH + (xStart >> 4);
   int            iHead = 0, iTail = 0, nPast = 0, i;

   if (xStart < 0 || yStart < 0 || iStart >= GRID_WIDTH * GRID_HEIGHT)
   {
      return FALSE;
   }

   TRAINER_BuildZones(&wTarget, 1, TIER_THROUGH_OBSTACLES);

   memset(rgbState, kUnvisited, sizeof(rgbState));
   rgbState[iStart] = kVisited;
   rgwFirst[iStart] = 0;
   rgwQueue[iTail++] = (WORD)iStart;

   while (iHead < iTail)
   {
      const int      iNode = rgwQueue[iHead++];
      const int      gx = iNode % GRID_WIDTH, gy = iNode / GRID_WIDTH;
      const int      x = gx * 16 + xRest, y = gy * 8 + yRest;
      const WORD     wHere = TRAINER_ObstacleAt(x, y);
      PALDIRECTION   dirFace;
      int            dir;

      if (TRAINER_CanReachEvent(x, y, EVENT(wTarget), &dirFace))
      {
         *pwObstacle = rgwFirst[iNode];
         return *pwObstacle != 0;
      }

      for (dir = kDirSouth; dir < kDirUnknown; dir++)
      {
         const int   gxNext = gx + ((dir == kDirWest || dir == kDirSouth) ? -1 : 1);
         const int   gyNext = gy + ((dir == kDirWest || dir == kDirNorth) ? -1 : 1);
         const int   xNext = gxNext * 16 + xRest, yNext = gyNext * 8 + yRest;
         int         iNext;
         WORD        wThere;

         if (gxNext < 0 || gxNext >= GRID_WIDTH || gyNext < 0 || gyNext >= GRID_HEIGHT)
         {
            continue;
         }

         iNext = gyNext * GRID_WIDTH + gxNext;

         if (rgbState[iNext] == kVisited)
         {
            continue;
         }

         if (rgbState[iNext] == kUnvisited && !TRAINER_IsFree(xNext, yNext))
         {
            rgbState[iNext] = kVisited;
            continue;
         }

         wThere = TRAINER_ObstacleAt(xNext, yNext);

         if (wThere == 0 || wThere == wHere)
         {
            rgbState[iNext] = kVisited;
            rgwFirst[iNext] = rgwFirst[iNode];
            rgwQueue[iTail++] = (WORD)iNext;
         }
         else if (rgbState[iNext] == kUnvisited)
         {
            rgbState[iNext] = kPast;
            rgwFirst[iNext] = (rgwFirst[iNode] != 0) ? rgwFirst[iNode] : wThere;
            rgwPast[nPast++] = (WORD)iNext;
         }
      }

      if (iHead == iTail)
      {
         //
         // Nowhere left to go without getting past one more.
         //
         for (i = 0; i < nPast; i++)
         {
            if (rgbState[rgwPast[i]] == kPast)
            {
               rgbState[rgwPast[i]] = kVisited;
               rgwQueue[iTail++] = rgwPast[i];
            }
         }

         nPast = 0;
      }
   }

   return FALSE;
}

#define SCENE_IN_SET(set, s)    (((set)[(s) >> 3] & (1 << ((s) & 7))) != 0)
#define ADD_SCENE_TO_SET(set, s) ((set)[(s) >> 3] |= (BYTE)(1 << ((s) & 7)))

static BOOL
TRAINER_IsWanted(
   WORD           wEventObjectID
)
{
   int   i;

   for (i = 0; i < g_nWant; i++)
   {
      if (g_rgWant[i].wEvent == wEventObjectID)
      {
         return TRUE;
      }
   }

   return FALSE;
}

static VOID
TRAINER_FloodScene(
   WORD           wScene,
   LPCPALMAP      lpMap,
   INT            xStart,
   INT            yStart
)
/*++
  Purpose:

    Find all the ways out that the party can walk to from a position in any scene, not only
    the current one, and how far they are. The same for the events wanted by the planning.

    Monsters do not count as obstacles, as there will be a battle and then they are gone.
    People who start a story do, the same as when walking in a scene.

    A raft taken on the way to here is no longer where it was but where it ended up, and
    stepping on it there would only carry the party back. So it is in the way over there.

  Parameters:

    [IN]  wScene - the scene.

    [IN]  lpMap - the map of that scene.

    [IN]  xStart - the X coordinate to start from.

    [IN]  yStart - the Y coordinate to start from.

  Return value:

    None. The ways out are in g_rgEdge.

--*/
{
   static BYTE    rgbSeen[GRID_WIDTH * GRID_HEIGHT];
   static WORD    rgwQueue[GRID_WIDTH * GRID_HEIGHT];
   static WORD    rgwDist[GRID_WIDTH * GRID_HEIGHT];

   const int      xRest = xStart & 15, yRest = yStart & 7;
   const int      iStart = (yStart >> 3) * GRID_WIDTH + (xStart >> 4);
   int            iHead = 0, iTail = 0, id, i;

   g_nEdge = 0;
   g_nZone = 0;

   for (id = FIRST_EVENT_OF(wScene); id <= LAST_EVENT_OF(wScene); id++)
   {
      const EVENTOBJECT         *p = EVENT(id);
      const TRAINERSCRIPTINFO   *pInfo = TRAINER_GetScriptInfo(p->wTriggerScript);
      const BOOL                 fMoves = (pInfo->wFlags & (kTrainerScriptScene | kTrainerScriptMove | kTrainerScriptCarry)) != 0;
      const BOOL                 fStory = (pInfo->wFlags & kTrainerScriptStory) != 0;
      const BOOL                 fWanted = TRAINER_IsWanted((WORD)id);
      int                        iRadius = 0, n;
      BOOL                       fRidden = FALSE;

      if (pInfo->wFlags & kTrainerScriptRide)
      {
         for (n = g_iFloodNode; n > 0 && !fRidden; n = g_rgNode[n].iFrom)
         {
            fRidden = (g_rgNode[n].wEvent == id);
         }
      }

      if (fRidden)
      {
         if (g_nZone < MAX_ZONES)
         {
            g_rgZone[g_nZone].x = pInfo->xRide;
            g_rgZone[g_nZone].y = pInfo->yRide;
            g_rgZone[g_nZone].iRadius = (p->wTriggerMode >= kTriggerTouchNear) ? TOUCH_RADIUS(p) : 16;
            g_nZone++;
         }

         continue;
      }

      if (TRAINER_IsWayOut(p))
      {
         BOOL fTried = FALSE;

         if (g_Walk.job != kTrainerJobNone)
         {
            for (i = 0; i < g_Walk.nTried; i++)
            {
               fTried = fTried || (g_Walk.rgwTried[i] == id);
            }
         }

         if (!fTried && g_nEdge < MAX_PLAN_EDGES)
         {
            g_rgEdge[g_nEdge].wEvent = (WORD)id;
            g_rgEdge[g_nEdge].wScene = (pInfo->wFlags & kTrainerScriptScene) ? pInfo->wScene : wScene;
            g_rgEdge[g_nEdge].x = -1;
            g_rgEdge[g_nEdge].y = -1;
            g_rgEdge[g_nEdge].iDist = -1;
            g_rgEdge[g_nEdge].iRide = 0;

            if (pInfo->wFlags & kTrainerScriptMove)
            {
               g_rgEdge[g_nEdge].x = pInfo->xMove;
               g_rgEdge[g_nEdge].y = pInfo->yMove;
            }
            else if (pInfo->wFlags & kTrainerScriptCarryTo)
            {
               g_rgEdge[g_nEdge].x = pInfo->xCarry;
               g_rgEdge[g_nEdge].y = pInfo->yCarry;
               g_rgEdge[g_nEdge].iRide = abs((int)pInfo->xCarry - (int)p->x) / 16;
            }

            g_nEdge++;
         }

         continue;
      }

      if (fWanted && g_nEdge < MAX_PLAN_EDGES)
      {
         //
         // Nowhere to go from here: this only tells how far it is.
         //
         g_rgEdge[g_nEdge].wEvent = (WORD)id;
         g_rgEdge[g_nEdge].wScene = 0;
         g_rgEdge[g_nEdge].x = -1;
         g_rgEdge[g_nEdge].y = -1;
         g_rgEdge[g_nEdge].iDist = -1;
         g_rgEdge[g_nEdge].iRide = 0;
         g_nEdge++;
      }

      if (p->sState >= kObjStateBlocker && !TRAINER_HasMonsterScript(p))
      {
         iRadius = 16;
      }

      if (EVENT_IS_ACTIVE(p) && p->wTriggerMode >= kTriggerTouchNear && p->wTriggerScript != 0 &&
         (fStory || fMoves) && !fWanted && TOUCH_RADIUS(p) > iRadius)
      {
         iRadius = TOUCH_RADIUS(p);
      }

      if (iRadius > 0 && g_nZone < MAX_ZONES)
      {
         g_rgZone[g_nZone].x = p->x;
         g_rgZone[g_nZone].y = p->y;
         g_rgZone[g_nZone].iRadius = iRadius;
         g_nZone++;
      }
   }

   if (xStart < 0 || yStart < 0 || iStart >= GRID_WIDTH * GRID_HEIGHT)
   {
      return;
   }

   memset(rgbSeen, 0, sizeof(rgbSeen));
   rgbSeen[iStart] = 1;
   rgwDist[iStart] = 0;
   rgwQueue[iTail++] = (WORD)iStart;

   while (iHead < iTail)
   {
      const int      iNode = rgwQueue[iHead++];
      const int      gx = iNode % GRID_WIDTH, gy = iNode / GRID_WIDTH;
      const int      x = gx * 16 + xRest, y = gy * 8 + yRest;
      PALDIRECTION   dirFace;
      int            dir;

      //
      // Ways out that are searched for can be used from where the party stands.
      //
      for (i = 0; i < g_nEdge; i++)
      {
         const EVENTOBJECT *p = EVENT(g_rgEdge[i].wEvent);

         if (g_rgEdge[i].iDist < 0 && (p->wTriggerMode < kTriggerTouchNear || TRAINER_FindPuzzle(p) != NULL) &&
            TRAINER_CanReachEvent(x, y, p, &dirFace))
         {
            g_rgEdge[i].iDist = rgwDist[iNode];
         }
      }

      for (dir = kDirSouth; dir < kDirUnknown; dir++)
      {
         const int   gxNext = gx + ((dir == kDirWest || dir == kDirSouth) ? -1 : 1);
         const int   gyNext = gy + ((dir == kDirWest || dir == kDirNorth) ? -1 : 1);
         const int   xNext = gxNext * 16 + xRest, yNext = gyNext * 8 + yRest;
         BOOL        fFree = TRUE;
         int         iNext;

         if (gxNext < 0 || gxNext >= GRID_WIDTH || gyNext < 0 || gyNext >= GRID_HEIGHT)
         {
            continue;
         }

         iNext = gyNext * GRID_WIDTH + gxNext;

         if (rgbSeen[iNext])
         {
            continue;
         }

         rgbSeen[iNext] = 1;

         if (PAL_MapPosIsBlocked(PAL_XY(xNext, yNext), TRUE, lpMap))
         {
            continue;
         }

         for (i = 0; i < g_nZone && fFree; i++)
         {
            fFree = abs(g_rgZone[i].x - xNext) + abs(g_rgZone[i].y - yNext) * 2 >= g_rgZone[i].iRadius;
         }

         if (!fFree)
         {
            continue;
         }

         //
         // Ways out that are touched take the party away from here at once.
         //
         for (i = 0; i < g_nEdge && fFree; i++)
         {
            const EVENTOBJECT *p = EVENT(g_rgEdge[i].wEvent);

            if (p->wTriggerMode >= kTriggerTouchNear && TRAINER_FindPuzzle(p) == NULL &&
               TRAINER_CanReachEvent(xNext, yNext, p, &dirFace))
            {
               fFree = FALSE;

               if (g_rgEdge[i].iDist < 0)
               {
                  g_rgEdge[i].iDist = rgwDist[iNode] + 1;
               }
            }
         }

         if (fFree)
         {
            rgwDist[iNext] = rgwDist[iNode] + 1;
            rgwQueue[iTail++] = (WORD)iNext;
         }
      }
   }
}

static BOOL
TRAINER_SceneIsFloor(
   WORD           wScene
)
/*++
  Purpose:

    Tell whether a scene looks like part of a dungeon: it has monsters, whether they are
    around right now or not, or it is a maze of teleporters.

  Parameters:

    [IN]  wScene - the scene.

  Return value:

    TRUE if the scene looks like part of a dungeon.

--*/
{
   int   id;

   for (id = FIRST_EVENT_OF(wScene); id <= LAST_EVENT_OF(wScene); id++)
   {
      const EVENTOBJECT *p = EVENT(id);

      if (TRAINER_HasMonsterScript(p) ||
         (TRAINER_IsWayOut(p) && TRAINER_GetScriptInfo(p->wTriggerScript)->wScene == wScene))
      {
         return TRUE;
      }
   }

   return FALSE;
}

static INT
TRAINER_FindDungeon(
   VOID
)
/*++
  Purpose:

    Find the dungeon the party is in: the scenes joined to the current one that look like
    part of a dungeon, and the side rooms and the place at the bottom that lead nowhere else.

    How deep each scene is goes into g_rgbFloor, counted from the scenes outside. This looks
    at how the scenes are joined and not at what can be walked right now, so the answer
    does not depend on where in the dungeon the party is, or on how far the story has got.

  Parameters:

    None.

  Return value:

    The number of floors, 0 if this is no dungeon at all.

--*/
{
   static BYTE   rgbLink[MAX_SCENES + 1][(MAX_SCENES + 8) / 8];   // scene -> the scenes it leads to
   WORD          rgwTodo[MAX_DUNGEON_SCENES];
   INT           nTodo = 0, nFloors = 0, i, id, d;

   memset(rgbLink, 0, sizeof(rgbLink));
   memset(g_rgbFloor, 0, sizeof(g_rgbFloor));
   memset(g_rgbOutside, 0, sizeof(g_rgbOutside));

   for (i = 1; i < MAX_SCENES; i++)
   {
      for (id = FIRST_EVENT_OF(i); id <= LAST_EVENT_OF(i); id++)
      {
         const WORD wDest = TRAINER_GetScriptInfo(EVENT(id)->wTriggerScript)->wScene;

         if (TRAINER_IsWayOut(EVENT(id)) && wDest != 0 && wDest < MAX_SCENES && wDest != i)
         {
            ADD_SCENE_TO_SET(rgbLink[i], wDest);
         }
      }
   }

   //
   // Which scenes? One-way holes count too, so this goes both ways along what joins them.
   // The value 255 stands for being in the dungeon, how deep will be worked out next.
   //
   g_rgbFloor[gpGlobals->wNumScene] = 255;
   rgwTodo[nTodo++] = gpGlobals->wNumScene;

   for (i = 0; i < nTodo; i++)
   {
      nFloors += TRAINER_SceneIsFloor(rgwTodo[i]) ? 1 : 0;

      for (d = 1; d < MAX_SCENES; d++)
      {
         INT   nNeighbours = 0, n;

         if ((!SCENE_IN_SET(rgbLink[rgwTodo[i]], d) && !SCENE_IN_SET(rgbLink[d], rgwTodo[i])) ||
            g_rgbFloor[d] != 0 || SCENE_IN_SET(g_rgbOutside, d))
         {
            continue;
         }

         for (n = 1; n < MAX_SCENES; n++)
         {
            nNeighbours += (SCENE_IN_SET(rgbLink[d], n) || SCENE_IN_SET(rgbLink[n], d)) ? 1 : 0;
         }

         if (!TRAINER_SceneIsFloor((WORD)d) && nNeighbours > 1)
         {
            ADD_SCENE_TO_SET(g_rgbOutside, d);
         }
         else if (nTodo >= MAX_DUNGEON_SCENES)
         {
            memset(g_rgbFloor, 0, sizeof(g_rgbFloor));
            return 0;
         }
         else
         {
            g_rgbFloor[d] = 255;
            rgwTodo[nTodo++] = (WORD)d;
         }
      }
   }

   //
   // How deep? Follow the way the scenes lead, starting from the ones entered from outside.
   //
   {
      INT       iDone;

      nTodo = 0;

      for (i = 1; i < MAX_SCENES; i++)
      {
         if (g_rgbFloor[i] == 255)
         {
            for (d = 1; d < MAX_SCENES; d++)
            {
               if (SCENE_IN_SET(g_rgbOutside, d) && SCENE_IN_SET(rgbLink[d], i))
               {
                  g_rgbFloor[i] = 1;
                  rgwTodo[nTodo++] = (WORD)i;
                  break;
               }
            }
         }
      }

      if (nTodo == 0)
      {
         g_rgbFloor[gpGlobals->wNumScene] = 1;
         rgwTodo[nTodo++] = gpGlobals->wNumScene;
      }

      for (iDone = 0; iDone < nTodo; iDone++)
      {
         for (d = 1; d < MAX_SCENES; d++)
         {
            if (g_rgbFloor[d] == 255 && SCENE_IN_SET(rgbLink[rgwTodo[iDone]], d))
            {
               g_rgbFloor[d] = g_rgbFloor[rgwTodo[iDone]] + 1;
               rgwTodo[nTodo++] = (WORD)d;
            }
         }
      }

      //
      // Whatever cannot be got to from an entrance is not to be counted as deep.
      //
      for (i = 1; i < MAX_SCENES; i++)
      {
         if (g_rgbFloor[i] == 255)
         {
            g_rgbFloor[i] = 1;
         }
      }
   }

   return nFloors;
}

static INT
TRAINER_Explore(
   WORD           wGoalScene,
   BOOL           fDungeon
)
/*++
  Purpose:

    Find out where the party can get to through the ways out of the scenes, shortest way
    first. The places are in g_rgNode.

    A scene can have several parts that only connect through other scenes, or through
    teleporters within the scene, so what matters is not only the scene but where the
    party arrives in it. The right way can even be to climb back up and take another hole.

    With events wanted in g_rgWant this stays within the current scene, and tells how far
    each of them is. With g_fWantNearest it stops at the first one found.

  Parameters:

    [IN]  wGoalScene - the scene to stop at, or 0 to look everywhere.

    [IN]  fDungeon - TRUE to stay within the dungeon found by TRAINER_FindDungeon().

  Return value:

    The node of the goal, or -1 if it cannot be reached.

--*/
{
   struct { WORD wMap; LPPALMAP lpMap; } rgMap[MAX_PLAN_MAPS];
   FILE        *fpMap = UTIL_OpenRequiredFile("map.mkf");
   FILE        *fpGop = UTIL_OpenRequiredFile("gop.mkf");
   INT          nMap = 0, iGoal = -1, i, j;

   g_nNode = 1;
   g_rgNode[0].wScene = gpGlobals->wNumScene;
   g_rgNode[0].x = PAL_X(gpGlobals->viewport) + PAL_X(gpGlobals->partyoffset);
   g_rgNode[0].y = PAL_Y(gpGlobals->viewport) + PAL_Y(gpGlobals->partyoffset);
   g_rgNode[0].iSteps = 0;
   g_rgNode[0].nFloors = 0;
   g_rgNode[0].iFrom = -1;
   g_rgNode[0].wEvent = 0;
   g_rgNode[0].fExpanded = FALSE;

   while (TRUE)
   {
      TRAINERNODE    *pNode = NULL;
      LPCPALMAP       lpMap = NULL;
      WORD            wMapNum;
      INT             iNode = -1;

      for (i = 0; i < g_nNode; i++)
      {
         if (!g_rgNode[i].fExpanded && (iNode < 0 || g_rgNode[i].iSteps < g_rgNode[iNode].iSteps))
         {
            iNode = i;
         }
      }

      if (iNode < 0)
      {
         break;
      }

      pNode = &g_rgNode[iNode];
      pNode->fExpanded = TRUE;

      if (wGoalScene != 0 && pNode->wScene == wGoalScene)
      {
         iGoal = iNode;
         break;
      }

      if (pNode->x < 0 || pNode->wScene == 0 || pNode->wScene >= MAX_SCENES)
      {
         continue;   // the script does not tell where this arrives, so it is as far as we can see
      }

      wMapNum = gpGlobals->g.rgScene[pNode->wScene - 1].wMapNum;

      for (i = 0; i < nMap; i++)
      {
         if (rgMap[i].wMap == wMapNum)
         {
            lpMap = rgMap[i].lpMap;
         }
      }

      if (lpMap == NULL && nMap < MAX_PLAN_MAPS)
      {
         rgMap[nMap].wMap = wMapNum;
         rgMap[nMap].lpMap = PAL_LoadMap(wMapNum, fpMap, fpGop);
         lpMap = rgMap[nMap].lpMap;
         nMap++;
      }

      if (lpMap == NULL)
      {
         continue;
      }

      g_iFloodNode = iNode;
      TRAINER_FloodScene(pNode->wScene, lpMap, pNode->x, pNode->y);
      g_iFloodNode = -1;

      {
         BOOL fFound = FALSE;

         for (i = 0; i < g_nEdge; i++)
         {
            for (j = 0; j < g_nWant && g_rgEdge[i].iDist >= 0; j++)
            {
               if (g_rgWant[j].wEvent == g_rgEdge[i].wEvent &&
                  (g_rgWant[j].iNode < 0 || pNode->iSteps + g_rgEdge[i].iDist < g_rgWant[j].iSteps))
               {
                  g_rgWant[j].iNode = iNode;
                  g_rgWant[j].iSteps = pNode->iSteps + g_rgEdge[i].iDist;
                  fFound = TRUE;
               }
            }
         }

         if (fFound && g_fWantNearest)
         {
            break;
         }
      }

      for (i = 0; i < g_nEdge; i++)
      {
         const TRAINEREDGE   *pEdge = &g_rgEdge[i];
         const INT            iSteps = pNode->iSteps + pEdge->iDist + 1 + pEdge->iRide;
         const INT            nFloors = pNode->nFloors + (pEdge->wScene != pNode->wScene ? 1 : 0);
         TRAINERNODE         *pNext = NULL;

         if (pEdge->iDist < 0 || pEdge->wScene == 0 || pEdge->wScene >= MAX_SCENES ||
            (fDungeon && g_rgbFloor[pEdge->wScene] == 0) ||
            (g_nWant > 0 && pEdge->wScene != gpGlobals->wNumScene))
         {
            continue;
         }

         for (j = 0; j < g_nNode; j++)
         {
            if (g_rgNode[j].wScene == pEdge->wScene && g_rgNode[j].x == pEdge->x && g_rgNode[j].y == pEdge->y)
            {
               pNext = &g_rgNode[j];
               break;
            }
         }

         if (pNext == NULL)
         {
            if (g_nNode >= MAX_PLAN_NODES)
            {
               continue;
            }

            pNext = &g_rgNode[g_nNode++];
            pNext->wScene = pEdge->wScene;
            pNext->x = pEdge->x;
            pNext->y = pEdge->y;
            pNext->fExpanded = FALSE;
         }
         else if (pNext->fExpanded || pNext->iSteps <= iSteps)
         {
            continue;
         }

         pNext->iSteps = iSteps;
         pNext->nFloors = nFloors;
         pNext->iFrom = iNode;
         pNext->wEvent = pEdge->wEvent;
      }
   }

   for (i = 0; i < nMap; i++)
   {
      PAL_FreeMap(rgMap[i].lpMap);
   }

   fclose(fpMap);
   fclose(fpGop);

   return iGoal;
}

static BOOL
TRAINER_NextEventToScene(
   WORD           wGoalScene,
   WORD          *pwEvent
)
/*++
  Purpose:

    Find the way out of the current place to take for getting to another scene.

  Parameters:

    [IN]  wGoalScene - the scene to get to.

    [OUT] pwEvent - the way out to take.

  Return value:

    TRUE if there is a way.

--*/
{
   INT   i = TRAINER_Explore(wGoalScene, FALSE);

   if (i <= 0)
   {
      return FALSE;
   }

   while (g_rgNode[i].iFrom != 0)
   {
      i = g_rgNode[i].iFrom;
   }

   *pwEvent = g_rgNode[i].wEvent;
   return TRUE;
}

static BOOL
TRAINER_PlanHop(
   const WORD    *rgwTarget,
   INT            nTarget,
   WORD          *pwHop,
   WORD          *pwChosen
)
/*++
  Purpose:

    Find the way to the nearest one of some events of the current scene that cannot be
    walked to from here, through the rafts and teleporters of the scene.

  Parameters:

    [IN]  rgwTarget - the events to go to.

    [IN]  nTarget - the number of those events.

    [OUT] pwHop - the raft or teleporter to go to now.

    [OUT] pwChosen - the event that leads to.

  Return value:

    TRUE if there is a way.

--*/
{
   INT   i, iBest = -1;

   g_nWant = 0;

   for (i = 0; i < nTarget && i < MAX_TARGETS; i++)
   {
      g_rgWant[g_nWant].wEvent = rgwTarget[i];
      g_rgWant[g_nWant].iNode = -1;
      g_rgWant[g_nWant].iSteps = 0;
      g_nWant++;
   }

   g_fWantNearest = TRUE;
   TRAINER_Explore(0, FALSE);
   g_fWantNearest = FALSE;

   for (i = 0; i < g_nWant; i++)
   {
      if (g_rgWant[i].iNode > 0 && (iBest < 0 || g_rgWant[i].iSteps < g_rgWant[iBest].iSteps))
      {
         iBest = i;
      }
   }

   g_nWant = 0;

   if (iBest < 0)
   {
      return FALSE;
   }

   *pwChosen = g_rgWant[iBest].wEvent;

   for (i = g_rgWant[iBest].iNode; g_rgNode[i].iFrom != 0; i = g_rgNode[i].iFrom);

   *pwHop = g_rgNode[i].wEvent;
   return TRUE;
}

static WORD
TRAINER_FindDeepestScene(
   VOID
)
/*++
  Purpose:

    Find the deepest scene of the dungeon that the party can get to from where it is.

  Parameters:

    None.

  Return value:

    The scene, or 0 if there is no dungeon or nowhere deeper to go.

--*/
{
   const TRAINERNODE   *pBest = NULL;
   int                  i;

   if (TRAINER_FindDungeon() < MIN_DUNGEON_FLOORS)
   {
      memset(g_rgbFloor, 0, sizeof(g_rgbFloor));
      memset(g_rgbOutside, 0, sizeof(g_rgbOutside));
      return 0;
   }

   TRAINER_Explore(0, TRUE);

   for (i = 1; i < g_nNode; i++)
   {
      const TRAINERNODE *p = &g_rgNode[i];

      if (pBest == NULL || g_rgbFloor[p->wScene] > g_rgbFloor[pBest->wScene] ||
         (g_rgbFloor[p->wScene] == g_rgbFloor[pBest->wScene] && p->iSteps > pBest->iSteps))
      {
         pBest = p;
      }
   }

   if (pBest == NULL || pBest->wScene == gpGlobals->wNumScene ||
      g_rgbFloor[pBest->wScene] <= g_rgbFloor[gpGlobals->wNumScene])
   {
      return 0;
   }

   return pBest->wScene;
}

static VOID
TRAINER_SetToast(
   LPCWSTR        lpszText
)
{
   wcsncpy(g_szToast, lpszText, sizeof(g_szToast) / sizeof(WCHAR) - 1);
   g_szToast[sizeof(g_szToast) / sizeof(WCHAR) - 1] = L'\0';
   g_nToastFrames = TOAST_FRAMES;
}

static VOID
TRAINER_EndJob(
   LPCWSTR        lpszMessage
)
/*++
  Purpose:

    Stop walking and tell why.

  Parameters:

    [IN]  lpszMessage - what to tell, or NULL for nothing.

  Return value:

    None.

--*/
{
   if (g_Walk.job != kTrainerJobNone && !g_fAutoBattle)
   {
      gpGlobals->fAutoBattle = FALSE;
   }

   g_Walk.job = kTrainerJobNone;
   g_Walk.dirStep = kDirUnknown;

   if (lpszMessage != NULL)
   {
      TRAINER_SetToast(lpszMessage);
   }
}

VOID
TRAINER_Stop(
   VOID
)
/*++
  Purpose:

    Stop walking without a word, as when a saved game gets loaded.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   TRAINER_EndJob(NULL);
}

static VOID
TRAINER_StartJob(
   TRAINERJOB     job,
   WORD           wTarget
)
/*++
  Purpose:

    Start walking.

  Parameters:

    [IN]  job - what to do.

    [IN]  wTarget - the event to go to, if there is only one.

  Return value:

    None.

--*/
{
   memset(&g_Walk, 0, sizeof(g_Walk));

   g_Walk.job = job;
   g_Walk.wScene = gpGlobals->wNumScene;
   g_Walk.wTarget = wTarget;
   g_Walk.dirStep = kDirUnknown;
   g_Walk.posLast = PAL_XY(-1, -1);
   g_Walk.nStepsLeft = -1;

   TRAINER_ApplyAutoBattle();
}

static VOID
TRAINER_MarkTried(
   WORD           wEventObjectID
)
{
   if (g_Walk.nTried < MAX_TARGETS)
   {
      g_Walk.rgwTried[g_Walk.nTried++] = wEventObjectID;
   }
}

static VOID
TRAINER_EndCollect(
   VOID
)
{
   WCHAR   szText[64];

   if (g_Walk.nUnreachable > 0)
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("收集完成 %d 處，%d 處無法到達", "收集完成 %d 处，%d 处无法到达"), g_Walk.nCollected, g_Walk.nUnreachable);
   }
   else
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("收集完成 %d 處", "收集完成 %d 处"), g_Walk.nCollected);
   }

   TRAINER_EndJob(szText);
}

static VOID
TRAINER_ChooseNextTreasure(
   VOID
)
/*++
  Purpose:

    Go on to the nearest treasure not tried yet, or finish when there is no such thing.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   WORD           rgwTarget[MAX_TARGETS];
   INT            nTarget = 0;
   int            id;
   TRAINERROUTE   route;
   WORD           wHop, wChosen;

   for (id = SCENE_FIRST_EVENT(); id <= SCENE_LAST_EVENT() && nTarget < MAX_TARGETS; id++)
   {
      if (TRAINER_IsTreasure((WORD)id) && !TRAINER_WasTried((WORD)id))
      {
         rgwTarget[nTarget++] = (WORD)id;
      }
   }

   if (nTarget == 0)
   {
      TRAINER_EndCollect();
   }
   else if (TRAINER_FindRoute(rgwTarget, nTarget, &route))
   {
      g_Walk.wTarget = route.wTarget;
      g_Walk.nStepsLeft = -1;
   }
   else if (TRAINER_PlanHop(rgwTarget, nTarget, &wHop, &wChosen))
   {
      g_Walk.wTarget = wChosen;
      g_Walk.wHop = wHop;
      g_Walk.nStepsLeft = -1;
   }
   else
   {
      g_Walk.nUnreachable += nTarget;
      TRAINER_EndCollect();
   }
}

static VOID
TRAINER_HealParty(
   VOID
)
{
   int   i;

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      const WORD w = gpGlobals->rgParty[i].wPlayerRole;

      gpGlobals->g.PlayerRoles.rgwHP[w] = gpGlobals->g.PlayerRoles.rgwMaxHP[w];
      gpGlobals->g.PlayerRoles.rgwMP[w] = gpGlobals->g.PlayerRoles.rgwMaxMP[w];
   }
}

static BOOL
TRAINER_PrepareHunt(
   VOID
)
/*++
  Purpose:

    Decide which monster to go for.

    Monsters that have been beaten only come back after more than a minute, and only with
    the party out of sight. There is no point in waiting for that when levelling up, so
    they come right back once the scene has run out of them.

    The party gets healed before each battle. It fights by itself and uses no items, and
    losing would mean going back to the last saved game.

  Parameters:

    None.

  Return value:

    TRUE to go on, FALSE if there is nothing more to do.

--*/
{
   WORD           rgwTarget[MAX_TARGETS];
   INT            nTarget = 0;
   int            id, i;
   TRAINERROUTE   route;
   BOOL           fDone = (g_Walk.wHuntLevel != 0);

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      fDone = fDone && gpGlobals->g.PlayerRoles.rgwLevel[gpGlobals->rgParty[i].wPlayerRole] >= g_Walk.wHuntLevel;
   }

   if (fDone)
   {
      TRAINER_EndJob(TR("已練到目標等級", "已练到目标等级"));
      return FALSE;
   }

   if (g_Walk.wTarget != 0)
   {
      if (TRAINER_IsMonster(g_Walk.wTarget))
      {
         if (++g_Walk.nIdleFrames > HUNT_IDLE_FRAMES)
         {
            TRAINER_EndJob(TR("打不到怪物", "打不到怪物"));
            return FALSE;
         }

         return TRUE;
      }

      //
      // It is gone, so there has been a battle.
      //
      g_Walk.nFights++;
      g_Walk.nIdleFrames = 0;
      g_Walk.wTarget = 0;
   }

   TRAINER_HealParty();

   for (i = 0; i < 2 && nTarget == 0; i++)
   {
      for (id = SCENE_FIRST_EVENT(); id <= SCENE_LAST_EVENT() && nTarget < MAX_TARGETS; id++)
      {
         if (i == 0)
         {
            if (TRAINER_IsMonster((WORD)id) && !TRAINER_WasTried((WORD)id))
            {
               rgwTarget[nTarget++] = (WORD)id;
            }
         }
         else if (EVENT(id)->sState != 0 && TRAINER_HasMonsterScript(EVENT(id)))
         {
            EVENT(id)->sVanishTime = 0;
            EVENT(id)->sState = abs(EVENT(id)->sState);
            EVENT(id)->wCurrentFrameNum = 0;
            rgwTarget[nTarget++] = (WORD)id;
         }
      }

      g_Walk.nTried = (i == 0 && nTarget == 0) ? 0 : g_Walk.nTried;
   }

   if (nTarget == 0)
   {
      TRAINER_EndJob(TR("這裡沒有怪物", "这里没有怪物"));
      return FALSE;
   }

   if (!TRAINER_FindRoute(rgwTarget, nTarget, &route))
   {
      TRAINER_EndJob(TR("走不到怪物那裡", "走不到怪物那里"));
      return FALSE;
   }

   g_Walk.wTarget = route.wTarget;
   g_Walk.nStepsLeft = -1;
   return TRUE;
}

static BOOL
TRAINER_PrepareRoute(
   VOID
)
/*++
  Purpose:

    Decide which way out of the current scene to go for. It is decided again whenever the
    party has been moved, so that a route through teleporters always starts from where
    the party really is.

  Parameters:

    None.

  Return value:

    TRUE to go on, FALSE if the party has arrived or there is no way.

--*/
{
   const PAL_POS   posNow = gpGlobals->viewport;

   if (gpGlobals->wNumScene == g_Walk.wGoalScene)
   {
      WCHAR szText[64];

      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR), TR("已到達場景 %d", "已到达场景 %d"), g_Walk.wGoalScene);
      TRAINER_EndJob(szText);
      return FALSE;
   }

   if (g_Walk.posLast != PAL_XY(-1, -1) &&
      (abs(PAL_X(posNow) - PAL_X(g_Walk.posLast)) > 16 || abs(PAL_Y(posNow) - PAL_Y(g_Walk.posLast)) > 8))
   {
      g_Walk.wTarget = 0;
      g_Walk.nTried = 0;
      g_Walk.posLast = PAL_XY(-1, -1);
   }

   if (g_Walk.wTarget == 0)
   {
      WORD wEvent;

      if (!TRAINER_NextEventToScene(g_Walk.wGoalScene, &wEvent))
      {
         TRAINER_EndJob(TR("無法到達", "无法到达"));
         return FALSE;
      }

      g_Walk.wTarget = wEvent;
      g_Walk.nStepsLeft = -1;
   }

   return TRUE;
}

static VOID
TRAINER_ListPuzzles(
   WORD          *pwMissing
)
/*++
  Purpose:

    List what the items of the inventory are meant to be used on in the current scene.
    Their scripts tell: operation 0x0081 goes on only if the party is facing an event.

  Parameters:

    [OUT] pwMissing - an item that would be needed here but is not in the inventory, or 0.

  Return value:

    None. The puzzles are in g_rgPuzzle.

--*/
{
   static BYTE   rgbSeen[65536 / 8];
   int           w;

   g_nPuzzle = 0;
   *pwMissing = 0;

   for (w = OBJECT_ITEM_FIRST; w <= OBJECT_ITEM_LAST; w++)
   {
      const OBJECT   *pObject = &gpGlobals->g.rgObject[w];
      WORD            rgwTodo[MAX_SCRIPT_TODO];
      INT             nTodo = 0, nWalked = 0;

      if (!(pObject->item.wFlags & kItemFlagUsable) || pObject->item.wScriptOnUse == 0)
      {
         continue;
      }

      memset(rgbSeen, 0, sizeof(rgbSeen));
      rgwTodo[nTodo++] = pObject->item.wScriptOnUse;

      while (nTodo > 0 && ++nWalked < MAX_SCRIPT_WALK)
      {
         const WORD          wCurrent = rgwTodo[--nTodo];
         const SCRIPTENTRY  *p = &gpGlobals->g.lprgScriptEntry[wCurrent];

         if (wCurrent == 0 || wCurrent >= gpGlobals->g.nScriptEntry ||
            (rgbSeen[wCurrent >> 3] & (1 << (wCurrent & 7))))
         {
            continue;
         }

         rgbSeen[wCurrent >> 3] |= (1 << (wCurrent & 7));

         if (p->wOperation == 0x0081 && p->rgwOperand[0] >= SCENE_FIRST_EVENT() && p->rgwOperand[0] <= SCENE_LAST_EVENT() &&
            EVENT_IS_ACTIVE(EVENT(p->rgwOperand[0])))
         {
            if (PAL_GetItemAmount((WORD)w) <= 0)
            {
               *pwMissing = (WORD)w;
            }
            else if (g_nPuzzle < MAX_PUZZLES)
            {
               g_rgPuzzle[g_nPuzzle].wEvent = p->rgwOperand[0];
               g_rgPuzzle[g_nPuzzle].wItem = (WORD)w;
               g_rgPuzzle[g_nPuzzle].iRadius = p->rgwOperand[1] * 32 + 16;
               g_nPuzzle++;
            }
         }

         TRAINER_PushNextLines(p, wCurrent, rgwTodo, &nTodo);
      }
   }
}

static BOOL
TRAINER_PreparePuzzle(
   VOID
)
/*++
  Purpose:

    Decide what to go and use an item on next, or finish when there is nothing left.

  Parameters:

    None.

  Return value:

    TRUE to go on, FALSE if there is nothing more to do.

--*/
{
   WORD           rgwTarget[MAX_PUZZLES];
   INT            nTarget = 0, i;
   TRAINERROUTE   route;
   WORD           wMissing, wHop, wChosen;

   if (g_Walk.wTarget != 0)
   {
      return TRUE;
   }

   TRAINER_ListPuzzles(&wMissing);

   for (i = 0; i < g_nPuzzle; i++)
   {
      if (!TRAINER_WasTried(g_rgPuzzle[i].wEvent))
      {
         rgwTarget[nTarget++] = g_rgPuzzle[i].wEvent;
      }
   }

   if (nTarget > 0)
   {
      if (TRAINER_FindRoute(rgwTarget, nTarget, &route))
      {
         g_Walk.wTarget = route.wTarget;
         g_Walk.nStepsLeft = -1;
         return TRUE;
      }

      if (TRAINER_PlanHop(rgwTarget, nTarget, &wHop, &wChosen))
      {
         g_Walk.wTarget = wChosen;
         g_Walk.wHop = wHop;
         g_Walk.nStepsLeft = -1;
         return TRUE;
      }

      g_Walk.nUnreachable += nTarget;
   }

   if (g_Walk.nUnreachable > 0)
   {
      WCHAR szText[64];

      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR), TR("解開 %d 處，%d 處走不到", "解开 %d 处，%d 处走不到"), g_Walk.nCollected, g_Walk.nUnreachable);
      TRAINER_EndJob(szText);
   }
   else
   {
      TRAINER_EndJob(TR("謎題已解開", "谜题已解开"));
   }

   return FALSE;
}

static VOID
TRAINER_UseItemOnTarget(
   PALDIRECTION   dirFace
)
/*++
  Purpose:

    Use an item on the event the party is in front of, the same way PAL_GameUseItem() does.
    If it is the right place, the script of the item makes the event trigger in the next frame.

  Parameters:

    [IN]  dirFace - where the party has to face.

  Return value:

    None.

--*/
{
   const TRAINERPUZZLE   *pPuzzle = TRAINER_FindPuzzle(EVENT(g_Walk.wTarget));
   const WORD             wTarget = g_Walk.wTarget;

   gpGlobals->wPartyDirection = dirFace;
   PAL_UpdatePartyGestures(FALSE);

   TRAINER_MarkTried(wTarget);
   g_Walk.nCollected++;
   g_Walk.wTarget = 0;

   if (pPuzzle != NULL)
   {
      const WORD   wItem = pPuzzle->wItem;
      OBJECT      *pObject = &gpGlobals->g.rgObject[wItem];

      pObject->item.wScriptOnUse = PAL_RunTriggerScript(pObject->item.wScriptOnUse,
         (pObject->item.wFlags & kItemFlagApplyToAll) ? 0xFFFF : gpGlobals->rgParty[0].wPlayerRole);

      if ((pObject->item.wFlags & kItemFlagConsuming) && g_fScriptSuccess)
      {
         PAL_AddItemToInventory(wItem, -1);
      }
   }

   PAL_ClearKeyState();
}

static VOID
TRAINER_FailTarget(
   VOID
)
/*++
  Purpose:

    Give up on the event being walked to.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   if (g_Walk.job == kTrainerJobCollect || g_Walk.job == kTrainerJobPuzzle)
   {
      TRAINER_MarkTried(g_Walk.wTarget);
      g_Walk.nUnreachable++;
      g_Walk.wTarget = 0;
   }
   else if (g_Walk.job == kTrainerJobRoute || g_Walk.job == kTrainerJobHunt)
   {
      //
      // Look for another one.
      //
      TRAINER_MarkTried(g_Walk.wTarget);
      g_Walk.wTarget = 0;
   }
   else if (g_Walk.fObstacles && g_Walk.nTried > 0)
   {
      TRAINER_EndJob(TR("阻礙還在，暫時過不去", "阻碍还在，暂时过不去"));
   }
   else
   {
      TRAINER_EndJob(TR("無法到達", "无法到达"));
   }
}

static VOID
TRAINER_PutOn(
   WORD           wEventObjectID
)
/*++
  Purpose:

    Put on what an event asks someone in the party to wear, the same way the equipment menu
    does, and tell who wears it now. It stays on, as the player may want it so.

  Parameters:

    [IN]  wEventObjectID - the event.

  Return value:

    None.

--*/
{
   WORD    wItem, wRole;
   INT     i;
   WCHAR   szText[64];

   if (wEventObjectID == 0)
   {
      return;
   }

   wItem = TRAINER_GetScriptInfo(EVENT(wEventObjectID)->wTriggerScript)->wWear;

   if (wItem == 0 || TRAINER_IsWorn(wItem) || (i = TRAINER_FindWearer(wItem)) < 0)
   {
      return;
   }

   wRole = gpGlobals->rgParty[i].wPlayerRole;
   gpGlobals->g.rgObject[wItem].item.wScriptOnEquip =
      PAL_RunTriggerScript(gpGlobals->g.rgObject[wItem].item.wScriptOnEquip, wRole);

   PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR), TR("%ls 裝備了 %ls", "%ls 装备了 %ls"),
      PAL_GetWord(gpGlobals->g.PlayerRoles.rgwName[wRole]), PAL_GetWord(wItem));
   TRAINER_SetToast(szText);
}

static VOID
TRAINER_Walk(
   VOID
)
/*++
  Purpose:

    Take one step of the job, or trigger the event if it is in reach.

    The route is planned again for every step, as monsters and people keep moving.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   TRAINERROUTE    route;
   LPEVENTOBJECT   p;

   //
   // Riding a raft or using a teleporter takes the party somewhere else: start over from there.
   //
   if (g_Walk.job != kTrainerJobRoute && g_Walk.posLast != PAL_XY(-1, -1) &&
      (abs(PAL_X(gpGlobals->viewport) - PAL_X(g_Walk.posLast)) > 16 ||
       abs(PAL_Y(gpGlobals->viewport) - PAL_Y(g_Walk.posLast)) > 8))
   {
      g_Walk.wHop = 0;
      g_Walk.nStepsLeft = -1;
      g_Walk.posLast = PAL_XY(-1, -1);
   }

   if (g_Walk.job == kTrainerJobCollect && g_Walk.wTarget == 0)
   {
      TRAINER_ChooseNextTreasure();

      if (g_Walk.job == kTrainerJobNone)
      {
         return;
      }
   }
   else if (g_Walk.job == kTrainerJobHunt && !TRAINER_PrepareHunt())
   {
      return;
   }
   else if (g_Walk.job == kTrainerJobRoute && !TRAINER_PrepareRoute())
   {
      return;
   }
   else if (g_Walk.job == kTrainerJobPuzzle && !TRAINER_PreparePuzzle())
   {
      return;
   }

   p = EVENT(g_Walk.wTarget);

   if (g_Walk.job == kTrainerJobCollect ? !TRAINER_IsTreasure(g_Walk.wTarget) : !EVENT_IS_ACTIVE(p))
   {
      //
      // Something else took care of it on the way
      //
      if (g_Walk.job == kTrainerJobWalk)
      {
         TRAINER_EndJob(NULL);
      }
      else
      {
         g_Walk.wTarget = 0;
      }
      return;
   }

   if (g_Walk.wHop != 0 && !EVENT_IS_ACTIVE(EVENT(g_Walk.wHop)))
   {
      g_Walk.wHop = 0;
      g_Walk.nStepsLeft = -1;
   }

   if (g_Walk.wHop == 0 && !TRAINER_FindRoute(&g_Walk.wTarget, 1, &route))
   {
      WORD wHop, wChosen;

      //
      // Not on this part of the map: is there a raft or a teleporter that leads there?
      //
      if (g_Walk.job == kTrainerJobRoute || g_Walk.job == kTrainerJobHunt)
      {
         TRAINER_FailTarget();
         return;
      }

      if (!TRAINER_PlanHop(&g_Walk.wTarget, 1, &wHop, &wChosen))
      {
         //
         // Is there something in the way? Going up to it is the one thing left to try, and
         // only once: what it says may well be that the party has to come back later.
         //
         if (!g_Walk.fObstacles || !TRAINER_FindObstacle(g_Walk.wTarget, &wHop))
         {
            TRAINER_FailTarget();
            return;
         }

         TRAINER_MarkTried(wHop);
      }

      g_Walk.wHop = wHop;
      g_Walk.nStepsLeft = -1;
   }

   //
   // Now, as getting close may be all it takes to trigger the event.
   //
   TRAINER_PutOn((g_Walk.wHop != 0) ? g_Walk.wHop : g_Walk.wTarget);

   if (g_Walk.wHop != 0)
   {
      if (!TRAINER_FindRoute(&g_Walk.wHop, 1, &route))
      {
         //
         // No way to it any more: what comes next gets its own count of steps.
         //
         TRAINER_MarkTried(g_Walk.wHop);
         g_Walk.wHop = 0;
         g_Walk.nStepsLeft = -1;
         return;
      }

      if (route.iDist == 0)
      {
         const WORD      wHop = g_Walk.wHop;
         const PAL_POS   posBefore = gpGlobals->viewport;

         //
         // Touching it has already happened, without doing anything, or it has to be searched.
         //
         if (EVENT(wHop)->wTriggerMode < kTriggerTouchNear)
         {
            gpGlobals->wPartyDirection = route.dirFace;
            PAL_UpdatePartyGestures(FALSE);
            PAL_TriggerSearchEvent(wHop);
         }

         if (posBefore == gpGlobals->viewport)
         {
            TRAINER_MarkTried(wHop);
         }

         g_Walk.wHop = 0;
         g_Walk.nStepsLeft = -1;
         return;
      }
   }
   else if (route.iDist == 0 && g_Walk.job == kTrainerJobPuzzle)
   {
      TRAINER_UseItemOnTarget(route.dirFace);
      return;
   }

   if (route.iDist == 0)
   {
      //
      // Touching an event has already triggered it in PAL_GameUpdate().
      //
      if (p->wTriggerMode < kTriggerTouchNear)
      {
         WORD wTarget = g_Walk.wTarget;

         gpGlobals->wPartyDirection = route.dirFace;
         PAL_UpdatePartyGestures(FALSE);

         if (g_Walk.job == kTrainerJobCollect)
         {
            TRAINER_MarkTried(wTarget);
            g_Walk.nCollected++;
            g_Walk.wTarget = 0;
         }

         PAL_TriggerSearchEvent(wTarget);
      }

      if (g_Walk.job == kTrainerJobWalk)
      {
         TRAINER_EndJob(NULL);
      }
      else if (g_Walk.job != kTrainerJobCollect)
      {
         //
         // If that did what it should, the next frame finds the party somewhere else or
         // finds the monster gone. If not, this is no use.
         //
         TRAINER_MarkTried(g_Walk.wTarget);

         if (g_Walk.job == kTrainerJobRoute)
         {
            g_Walk.wTarget = 0;
         }
      }

      return;
   }

   //
   // Do not walk around forever: a monster may be keeping the party from getting there,
   // or a route through monsters may end up in front of one that does not start a battle.
   //
   if (g_Walk.nStepsLeft < 0)
   {
      g_Walk.nStepsLeft = route.iDist * 4 + 100;
      g_Walk.nStuckFrames = 0;
   }

   if (g_Walk.posLast == gpGlobals->viewport)
   {
      g_Walk.nStuckFrames++;
   }
   else
   {
      g_Walk.nStuckFrames = 0;
   }
   g_Walk.posLast = gpGlobals->viewport;

   if (g_Walk.nStuckFrames > STUCK_FRAMES || g_Walk.nStepsLeft-- == 0)
   {
      TRAINER_FailTarget();
      return;
   }

   g_Walk.dirStep = route.dirStep;
}

static VOID
TRAINER_RaiseLevel(
   WORD           wPlayerRole,
   WORD           wLevel
)
/*++
  Purpose:

    Raise a player to a level the way battles do, learning the magics that come with it.

  Parameters:

    [IN]  wPlayerRole - the player.

    [IN]  wLevel - the level to reach.

  Return value:

    None.

--*/
{
   int   j;

   if (gpGlobals->g.PlayerRoles.rgwLevel[wPlayerRole] >= wLevel)
   {
      return;
   }

   PAL_PlayerLevelUp(wPlayerRole, wLevel - gpGlobals->g.PlayerRoles.rgwLevel[wPlayerRole]);

   gpGlobals->g.PlayerRoles.rgwHP[wPlayerRole] = gpGlobals->g.PlayerRoles.rgwMaxHP[wPlayerRole];
   gpGlobals->g.PlayerRoles.rgwMP[wPlayerRole] = gpGlobals->g.PlayerRoles.rgwMaxMP[wPlayerRole];

   if (wPlayerRole < MAX_PLAYABLE_PLAYER_ROLES)
   {
      for (j = 0; j < gpGlobals->g.nLevelUpMagic; j++)
      {
         const LEVELUPMAGIC *m = &gpGlobals->g.lprgLevelUpMagic[j].m[wPlayerRole];

         if (m->wMagic != 0 && m->wLevel <= wLevel)
         {
            PAL_AddMagic(wPlayerRole, m->wMagic);
         }
      }
   }
}

static VOID
TRAINER_ApplyAutoLevel(
   VOID
)
{
   int   i;

   if (g_wAutoLevel != 0)
   {
      for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
      {
         TRAINER_RaiseLevel(gpGlobals->rgParty[i].wPlayerRole, g_wAutoLevel);
      }
   }
}

VOID
TRAINER_ApplyAutoBattle(
   VOID
)
/*++
  Purpose:

    Turn on auto battle if the trainer wants it: when it is switched on, and whenever the
    trainer is doing something by itself, as nobody may be there to fight the battles that
    come up. The game turns it off after each battle, and touching two monsters at once
    means another battle right after that one.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   if (g_fAutoBattle || g_Walk.job != kTrainerJobNone)
   {
      gpGlobals->fAutoBattle = TRUE;
   }
}

BOOL
TRAINER_IsFightingByItself(
   VOID
)
{
   return g_Walk.job != kTrainerJobNone && gpGlobals->fAutoBattle;
}

VOID
TRAINER_CheckBattleInput(
   VOID
)
/*++
  Purpose:

    Called in each frame of a battle. Any key gives this battle back to the player when
    the trainer has turned on auto battle, and stops whatever the trainer is doing.
    In an auto battle during a job of the trainer this also tops up the party.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   int   i;

   if (g_InputState.dwKeyPress != 0 && gpGlobals->fAutoBattle && (g_fAutoBattle || g_Walk.job != kTrainerJobNone))
   {
      TRAINER_EndJob(L"已停止");
      gpGlobals->fAutoBattle = FALSE;
   }

   //
   // Nobody may be watching while it walks and fights by itself, and losing a battle means
   // going back to the last saved game. So the party does not get close to that.
   //
   if (TRAINER_IsFightingByItself())
   {
      for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
      {
         const WORD w = gpGlobals->rgParty[i].wPlayerRole;

         if (gpGlobals->g.PlayerRoles.rgwHP[w] != 0 && gpGlobals->g.PlayerRoles.rgwHP[w] < gpGlobals->g.PlayerRoles.rgwMaxHP[w] / 2)
         {
            gpGlobals->g.PlayerRoles.rgwHP[w] = gpGlobals->g.PlayerRoles.rgwMaxHP[w];
         }
      }
   }
}

VOID
TRAINER_Update(
   VOID
)
/*++
  Purpose:

    Run the trainer for one frame.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   if (g_wCurrentScene != gpGlobals->wNumScene)
   {
      g_wPreviousScene = g_wCurrentScene;
      g_wCurrentScene = gpGlobals->wNumScene;
   }

   TRAINER_ApplyAutoLevel();
   TRAINER_ApplyAutoBattle();

   g_Walk.dirStep = kDirUnknown;

   if (g_Walk.job == kTrainerJobNone)
   {
      return;
   }

   if (gpGlobals->wNumScene != g_Walk.wScene)
   {
      if (g_Walk.job != kTrainerJobRoute)
      {
         //
         // Either the exit asked for, or something on the way did this.
         //
         TRAINER_EndJob(NULL);
         return;
      }

      g_Walk.wScene = gpGlobals->wNumScene;
      g_Walk.wTarget = 0;
      g_Walk.nTried = 0;
      g_Walk.posLast = PAL_XY(-1, -1);
   }

   if (++g_Walk.nFrames > START_GRACE_FRAMES &&
      (g_InputState.dwKeyPress != 0 || g_InputState.dir != kDirUnknown))
   {
      TRAINER_EndJob(L"已停止");
      return;
   }

   TRAINER_Walk();
}

PALDIRECTION
TRAINER_GetDirection(
   VOID
)
/*++
  Purpose:

    Get where the party should walk in this frame.

  Parameters:

    None.

  Return value:

    Where the trainer is going, or else where the player is going.

--*/
{
   return (g_Walk.dirStep != kDirUnknown) ? g_Walk.dirStep : g_InputState.dir;
}

VOID
TRAINER_DrawOverlay(
   VOID
)
/*++
  Purpose:

    Show what the trainer is doing on top of the scene.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   LPCWSTR   lpszText = NULL;
   WCHAR     szText[64];

   if (g_Walk.job == kTrainerJobCollect)
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("尋寶中，已收集 %d 處，按任意鍵停止", "寻宝中，已收集 %d 处，按任意键停止"), g_Walk.nCollected);
      lpszText = szText;
   }
   else if (g_Walk.job == kTrainerJobWalk)
   {
      //
      // Only an obstacle counts as tried before the party gets there.
      //
      lpszText = (g_Walk.wHop != 0 && TRAINER_WasTried(g_Walk.wHop)) ?
         TR("前往阻礙，按任意鍵停止", "前往阻碍，按任意键停止") :
         TR("尋路中，按任意鍵停止", "寻路中，按任意键停止");
   }
   else if (g_Walk.job == kTrainerJobRoute)
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("前往場景 %d，按任意鍵停止", "前往场景 %d，按任意键停止"), g_Walk.wGoalScene);
      lpszText = szText;
   }
   else if (g_Walk.job == kTrainerJobPuzzle)
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("解謎中，已解開 %d 處，按任意鍵停止", "解谜中，已解开 %d 处，按任意键停止"), g_Walk.nCollected);
      lpszText = szText;
   }
   else if (g_Walk.job == kTrainerJobHunt)
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("打怪中，已戰 %d 場，按任意鍵停止", "打怪中，已战 %d 场，按任意键停止"), g_Walk.nFights);
      lpszText = szText;
   }
   else if (g_nToastFrames > 0)
   {
      g_nToastFrames--;
      lpszText = g_szToast;
   }

   if (lpszText != NULL)
   {
      PAL_DrawText(lpszText, PAL_XY(8, 182), MENUITEM_COLOR_CONFIRMED, TRUE, FALSE, FALSE);
   }
}

static VOID
TRAINER_WaitForInput(
   DWORD         *pdwTime
)
/*++
  Purpose:

    Wait until the next frame or a key press.

  Parameters:

    [IN/OUT]  pdwTime - the time of the next frame.

  Return value:

    None.

--*/
{
   PAL_ClearKeyState();
   PAL_ProcessEvent();

   while (!SDL_TICKS_PASSED(SDL_GetTicks(), *pdwTime))
   {
      PAL_ProcessEvent();

      if (g_InputState.dwKeyPress != 0)
      {
         break;
      }

      SDL_Delay(5);
   }

   *pdwTime = SDL_GetTicks() + FRAME_TIME;
}

static INT
TRAINER_ListMenu(
   LPCWSTR        lpszTitle,
   INT            nItems,
   INT            nColumns,
   INT           *piCursor,
   LPCWSTR      (*lpfnLabel)(INT iItem, BYTE *pbColor),
   LPCWSTR      (*lpfnFooter)(INT iItem)
)
/*++
  Purpose:

    Let the player choose from a list that may be longer than the screen.

  Parameters:

    [IN]      lpszTitle - what the list is about.

    [IN]      nItems - the number of items.

    [IN]      nColumns - how many items go on a line.

    [IN/OUT]  piCursor - the item selected.

    [IN]      lpfnLabel - gets the text of an item. The color is set to the normal one,
                          to be changed if the item is special.

    [IN]      lpfnFooter - gets the text below the list for the item selected. May be NULL.

  Return value:

    The item chosen, or -1 if cancelled.

--*/
{
   const int   iColumnWidth = LIST_WIDTH / nColumns;
   const int   nRows = (lpfnFooter != NULL) ? LIST_ROWS : LIST_ROWS + 1;
   DWORD       dwTime = SDL_GetTicks();
   int         iRow, iColumn;

   PAL_ClearKeyState();

   while (TRUE)
   {
      int         iFirst = (*piCursor / nColumns - nRows / 2) * nColumns;
      const int   iLastPage = ((nItems + nColumns - 1) / nColumns - nRows) * nColumns;

      if (iFirst > iLastPage)
      {
         iFirst = iLastPage;
      }
      if (iFirst < 0)
      {
         iFirst = 0;
      }

      PAL_MakeScene();
      PAL_CreateBoxWithShadow(PAL_XY(2, 0), LIST_ROWS + 1, 17, 1, FALSE, 0);
      PAL_DrawText(lpszTitle, PAL_XY(LIST_X, LIST_Y), MENUITEM_COLOR_CONFIRMED, TRUE, FALSE, FALSE);

      for (iRow = 0; iRow < nRows; iRow++)
      {
         for (iColumn = 0; iColumn < nColumns; iColumn++)
         {
            const int   iItem = iFirst + iRow * nColumns + iColumn;
            BYTE        bColor = MENUITEM_COLOR;
            LPCWSTR     lpszLabel;

            if (iItem >= nItems)
            {
               break;
            }

            lpszLabel = lpfnLabel(iItem, &bColor);

            if (iItem == *piCursor)
            {
               bColor = MENUITEM_COLOR_SELECTED;
            }

            PAL_DrawText(lpszLabel, PAL_XY(LIST_X + iColumn * iColumnWidth, LIST_Y + (iRow + 1) * LIST_LINE_HEIGHT),
               bColor, TRUE, FALSE, FALSE);
         }
      }

      if (lpfnFooter != NULL && nItems > 0)
      {
         PAL_DrawText(lpfnFooter(*piCursor), PAL_XY(LIST_X, LIST_Y + (LIST_ROWS + 1) * LIST_LINE_HEIGHT),
            DESCTEXT_COLOR, TRUE, FALSE, FALSE);
      }

      VIDEO_UpdateScreen(NULL);

      TRAINER_WaitForInput(&dwTime);

      if (g_InputState.dwKeyPress & kKeyMenu)
      {
         PAL_ClearKeyState();
         return -1;
      }
      else if (g_InputState.dwKeyPress & kKeySearch)
      {
         if (nItems > 0)
         {
            PAL_ClearKeyState();
            return *piCursor;
         }
      }
      else if (nItems > 0)
      {
         int iNew = *piCursor;

         if (g_InputState.dwKeyPress & kKeyUp)        iNew -= nColumns;
         else if (g_InputState.dwKeyPress & kKeyDown) iNew += nColumns;
         else if (g_InputState.dwKeyPress & kKeyLeft) iNew -= 1;
         else if (g_InputState.dwKeyPress & kKeyRight) iNew += 1;
         else if (g_InputState.dwKeyPress & kKeyPgUp) iNew -= nRows * nColumns;
         else if (g_InputState.dwKeyPress & kKeyPgDn) iNew += nRows * nColumns;
         else if (g_InputState.dwKeyPress & kKeyHome) iNew = 0;
         else if (g_InputState.dwKeyPress & kKeyEnd)  iNew = nItems - 1;

         *piCursor = (iNew < 0) ? 0 : ((iNew >= nItems) ? nItems - 1 : iNew);
      }
   }
}

static INT
TRAINER_ReadNumberInSteps(
   LPCWSTR        lpszTitle,
   INT            iValue,
   INT            iMin,
   INT            iMax,
   LPCWSTR        lpszZero,
   INT            iStep,
   INT            iBigStep,
   LPCWSTR        lpszHint
)
/*++
  Purpose:

    Let the player choose a number.

  Parameters:

    [IN]  lpszTitle - what the number is about.

    [IN]  iValue - the number to start from.

    [IN]  iMin - the lowest number.

    [IN]  iMax - the highest number.

    [IN]  lpszZero - what to show instead of zero. May be NULL.

    [IN]  iStep - how much the up and down keys change the number.

    [IN]  iBigStep - how much the left and right keys change the number.

    [IN]  lpszHint - what to tell about those keys.

  Return value:

    The number chosen, or -1 if cancelled.

--*/
{
   DWORD   dwTime = SDL_GetTicks();
   WCHAR   szValue[16];

   PAL_ClearKeyState();

   while (TRUE)
   {
      PAL_MakeScene();
      PAL_CreateBoxWithShadow(PAL_XY(48, 56), 4, 12, 1, FALSE, 0);
      PAL_DrawText(lpszTitle, PAL_XY(60, 68), MENUITEM_COLOR_CONFIRMED, TRUE, FALSE, FALSE);

      if (iValue == 0 && lpszZero != NULL)
      {
         PAL_DrawText(lpszZero, PAL_XY(60, 88), MENUITEM_COLOR_SELECTED, TRUE, FALSE, FALSE);
      }
      else
      {
         PAL_swprintf(szValue, sizeof(szValue) / sizeof(WCHAR), L"%d", iValue);
         PAL_DrawText(szValue, PAL_XY(60, 88), MENUITEM_COLOR_SELECTED, TRUE, FALSE, FALSE);
      }

      PAL_DrawText(lpszHint, PAL_XY(60, 108), DESCTEXT_COLOR, TRUE, FALSE, FALSE);

      VIDEO_UpdateScreen(NULL);

      TRAINER_WaitForInput(&dwTime);

      if (g_InputState.dwKeyPress & kKeyMenu)
      {
         PAL_ClearKeyState();
         return -1;
      }
      else if (g_InputState.dwKeyPress & kKeySearch)
      {
         PAL_ClearKeyState();
         return iValue;
      }
      else if (g_InputState.dwKeyPress & kKeyUp)    iValue += iStep;
      else if (g_InputState.dwKeyPress & kKeyDown)  iValue -= iStep;
      else if (g_InputState.dwKeyPress & kKeyRight) iValue += iBigStep;
      else if (g_InputState.dwKeyPress & kKeyLeft)  iValue -= iBigStep;

      iValue = (iValue < iMin) ? iMin : ((iValue > iMax) ? iMax : iValue);
   }
}

static INT
TRAINER_ReadNumber(
   LPCWSTR        lpszTitle,
   INT            iValue,
   INT            iMin,
   INT            iMax,
   LPCWSTR        lpszZero
)
{
   return TRAINER_ReadNumberInSteps(lpszTitle, iValue, iMin, iMax, lpszZero, 1, 10,
      TR("上下調整 左右加減十", "上下调整 左右加减十"));
}

static LPCWSTR
TRAINER_TargetDistanceText(
   const TRAINERTARGET   *pTarget
)
{
   static WCHAR   szText[16];

   if (pTarget->iDist < 0)
   {
      return TR("無路", "无路");
   }

   PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
      (pTarget->wObstacle != 0) ? TR("阻礙 %d步", "阻碍 %d步") : L"%d步", pTarget->iDist);
   return szText;
}

// The order of the list: what can be walked to, then what is behind something, then the rest.
#define TARGET_RANK(p)          (((p)->iDist < 0) ? 2 : (((p)->wObstacle != 0) ? 1 : 0))

static VOID
TRAINER_ListTargets(
   BOOL         (*lpfnFilter)(WORD),
   BOOL           fObstacles
)
/*++
  Purpose:

    List the events of the current scene that pass a filter, nearest first and those out
    of reach last.

  Parameters:

    [IN]  lpfnFilter - tells which events to list.

    [IN]  fObstacles - TRUE to tell what stands in the way of those out of reach.

  Return value:

    None.

--*/
{
   int   id, i, j;

   g_nTarget = 0;

   for (id = SCENE_FIRST_EVENT(); id <= SCENE_LAST_EVENT() && g_nTarget < MAX_TARGETS; id++)
   {
      if (lpfnFilter((WORD)id))
      {
         TRAINERROUTE   route;
         WORD           wTarget = (WORD)id;

         g_rgTarget[g_nTarget].wEvent = wTarget;
         g_rgTarget[g_nTarget].iDist = TRAINER_FindRoute(&wTarget, 1, &route) ? route.iDist : -1;
         g_rgTarget[g_nTarget].wObstacle = 0;
         g_nTarget++;
      }
   }

   //
   // What cannot be walked to may still be reached on rafts or through teleporters.
   //
   g_nWant = 0;

   for (i = 0; i < g_nTarget; i++)
   {
      if (g_rgTarget[i].iDist < 0)
      {
         g_rgWant[g_nWant].wEvent = g_rgTarget[i].wEvent;
         g_rgWant[g_nWant].iNode = -1;
         g_rgWant[g_nWant].iSteps = 0;
         g_nWant++;
      }
   }

   if (g_nWant > 0)
   {
      TRAINER_Explore(0, FALSE);

      for (i = 0; i < g_nTarget; i++)
      {
         for (j = 0; j < g_nWant; j++)
         {
            if (g_rgWant[j].wEvent == g_rgTarget[i].wEvent && g_rgWant[j].iNode >= 0)
            {
               g_rgTarget[i].iDist = g_rgWant[j].iSteps;
            }
         }
      }

      g_nWant = 0;
   }

   for (i = 0; i < g_nTarget && fObstacles; i++)
   {
      TRAINERROUTE   route;
      WORD           wObstacle;

      if (g_rgTarget[i].iDist < 0 && TRAINER_FindObstacle(g_rgTarget[i].wEvent, &wObstacle) &&
         TRAINER_FindRoute(&wObstacle, 1, &route))
      {
         g_rgTarget[i].iDist = route.iDist;
         g_rgTarget[i].wObstacle = wObstacle;
      }
   }

   for (i = 1; i < g_nTarget; i++)
   {
      TRAINERTARGET t = g_rgTarget[i];

      for (j = i; j > 0; j--)
      {
         const TRAINERTARGET *q = &g_rgTarget[j - 1];

         if (TARGET_RANK(q) < TARGET_RANK(&t) || (TARGET_RANK(q) == TARGET_RANK(&t) && q->iDist <= t.iDist))
         {
            break;
         }

         g_rgTarget[j] = *q;
      }

      g_rgTarget[j] = t;
   }
}

static VOID
TRAINER_ListExits(
   VOID
)
/*++
  Purpose:

    List the exits of the current scene, and what leads to another scene as part of the
    story or a fight. The way on may well be that, even with exits close by: the ones at
    the bottom of a dungeon only lead back up, and the way on is its boss.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   TRAINER_ListTargets(TRAINER_LeadsElsewhere, TRUE);
}

static LPCWSTR
TRAINER_TreasureLabel(
   INT            iItem,
   BYTE          *pbColor
)
{
   const TRAINERSCRIPTINFO   *pInfo;
   INT                        iDist;

   if (iItem == 0)
   {
      PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR),
         TR("全部收集，共 %d 處", "全部收集，共 %d 处"), g_nTarget);
      return g_szLabel;
   }

   pInfo = TRAINER_GetScriptInfo(EVENT(g_rgTarget[iItem - 1].wEvent)->wTriggerScript);
   iDist = g_rgTarget[iItem - 1].iDist;

   if (iDist < 0)
   {
      *pbColor = MENUITEM_COLOR_INACTIVE;
   }

   if (pInfo->wItem != 0)
   {
      PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), L"%ls%ls  %ls", PAL_GetWord(pInfo->wItem),
         (pInfo->nLoot > 1) ? L" 等" : L"", TRAINER_TargetDistanceText(&g_rgTarget[iItem - 1]));
   }
   else
   {
      PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), L"%ls %d  %ls", PAL_GetWord(CASH_LABEL),
         pInfo->wCash, TRAINER_TargetDistanceText(&g_rgTarget[iItem - 1]));
   }

   return g_szLabel;
}

static BOOL
TRAINER_TreasureMenu(
   VOID
)
/*++
  Purpose:

    Let the player go for the treasures of the current scene.

  Parameters:

    None.

  Return value:

    TRUE if the party is going.

--*/
{
   INT   iCursor = 0, i;

   TRAINER_ListTargets(TRAINER_IsTreasure, FALSE);

   if (g_nTarget == 0)
   {
      TRAINER_SetToast(TR("這裡沒有寶物了", "这里没有宝物了"));
      return TRUE;
   }

   i = TRAINER_ListMenu(TR("尋寶", "寻宝"), g_nTarget + 1, 1, &iCursor, TRAINER_TreasureLabel, NULL);

   if (i == 0)
   {
      TRAINER_StartJob(kTrainerJobCollect, 0);
      return TRUE;
   }
   else if (i > 0)
   {
      TRAINER_StartJob(kTrainerJobWalk, g_rgTarget[i - 1].wEvent);
      return TRUE;
   }

   return FALSE;
}

static WORD   g_wDeepestScene;   // 0 if there is no dungeon or nowhere deeper to go

static LPCWSTR
TRAINER_ExitLabel(
   INT            iItem,
   BYTE          *pbColor
)
{
   const TRAINERSCRIPTINFO   *pInfo;
   INT                        iDist;
   LPCWSTR                    lpszWhere = L"", lpszWear = L"";
   WCHAR                      szWhere[24], szWear[24];

   if (g_wDeepestScene != 0)
   {
      if (iItem == 0)
      {
         PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR),
            TR("走到最深處：場景 %d，第 %d 層", "走到最深处：场景 %d，第 %d 层"), g_wDeepestScene, g_rgbFloor[g_wDeepestScene]);
         return g_szLabel;
      }

      iItem--;
   }

   pInfo = TRAINER_GetScriptInfo(EVENT(g_rgTarget[iItem].wEvent)->wTriggerScript);
   iDist = g_rgTarget[iItem].iDist;

   if (iDist < 0)
   {
      *pbColor = MENUITEM_COLOR_INACTIVE;
   }

   if (pInfo->wScene < MAX_SCENES && g_rgbFloor[pInfo->wScene] != 0)
   {
      PAL_swprintf(szWhere, sizeof(szWhere) / sizeof(WCHAR), TR("  第 %d 層", "  第 %d 层"), g_rgbFloor[pInfo->wScene]);
      lpszWhere = szWhere;
   }
   else if (pInfo->wScene < MAX_SCENES && SCENE_IN_SET(g_rgbOutside, pInfo->wScene))
   {
      lpszWhere = L"  通往外面";
   }

   if (pInfo->wWear != 0 && !TRAINER_IsWorn(pInfo->wWear))
   {
      const BOOL fMissing = (TRAINER_FindWearer(pInfo->wWear) < 0);

      if (fMissing)
      {
         *pbColor = MENUITEM_COLOR_INACTIVE;
      }

      PAL_swprintf(szWear, sizeof(szWear) / sizeof(WCHAR),
         fMissing ? L"  缺%ls" : TR("  裝備%ls", "  装备%ls"), PAL_GetWord(pInfo->wWear));
      lpszWear = szWear;
   }

   PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), TR("場景 %d  %ls%ls%ls%ls", "场景 %d  %ls%ls%ls%ls"),
      pInfo->wScene, TRAINER_TargetDistanceText(&g_rgTarget[iItem]), lpszWhere, lpszWear,
      (pInfo->wFlags & (kTrainerScriptBattle | kTrainerScriptStory)) ? TR("  劇情", "  剧情") :
      ((pInfo->wScene == g_wPreviousScene) ? TR("  來路", "  来路") : L""));

   return g_szLabel;
}

static BOOL
TRAINER_ExitMenu(
   VOID
)
/*++
  Purpose:

    Let the player go to one of the exits of the current scene, or all the way to the
    deepest place of the dungeon.

  Parameters:

    None.

  Return value:

    TRUE if the party is going.

--*/
{
   INT   iCursor = 0, i;

   TRAINER_ListExits();

   g_wDeepestScene = TRAINER_FindDeepestScene();

   if (g_nTarget == 0 && g_wDeepestScene == 0)
   {
      TRAINER_SetToast(TR("這裡沒有出口", "这里没有出口"));
      return TRUE;
   }

   i = TRAINER_ListMenu(L"前往出口", g_nTarget + (g_wDeepestScene != 0 ? 1 : 0), 1, &iCursor, TRAINER_ExitLabel, NULL);

   if (i >= 0 && g_wDeepestScene != 0)
   {
      if (i == 0)
      {
         TRAINER_StartJob(kTrainerJobRoute, 0);
         g_Walk.wGoalScene = g_wDeepestScene;
         return TRUE;
      }

      i--;
   }

   if (i >= 0)
   {
      const WORD wWear = TRAINER_GetScriptInfo(EVENT(g_rgTarget[i].wEvent)->wTriggerScript)->wWear;

      if (!TRAINER_CanWear(wWear))
      {
         WCHAR szText[64];

         PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR), TR("需要物品：%ls", "需要物品：%ls"), PAL_GetWord(wWear));
         TRAINER_SetToast(szText);
         return TRUE;
      }

      TRAINER_StartJob(kTrainerJobWalk, g_rgTarget[i].wEvent);
      g_Walk.fObstacles = TRUE;
      return TRUE;
   }

   return FALSE;
}

static VOID
TRAINER_StartPuzzle(
   VOID
)
/*++
  Purpose:

    Start solving the puzzles of the current scene, or tell why there is nothing to do.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   WORD   wMissing;
   WCHAR  szText[64];

   TRAINER_ListPuzzles(&wMissing);

   if (g_nPuzzle > 0)
   {
      TRAINER_StartJob(kTrainerJobPuzzle, 0);
   }
   else if (wMissing != 0)
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR), TR("需要物品：%ls", "需要物品：%ls"), PAL_GetWord(wMissing));
      TRAINER_SetToast(szText);
   }
   else
   {
      TRAINER_SetToast(TR("這裡沒有要解的謎題", "这里没有要解的谜题"));
   }
}

static BOOL
TRAINER_HuntMenu(
   VOID
)
/*++
  Purpose:

    Let the player start fighting the monsters of the current scene.

  Parameters:

    None.

  Return value:

    TRUE if the fighting has started.

--*/
{
   const WORD wLevel = gpGlobals->g.PlayerRoles.rgwLevel[gpGlobals->rgParty[0].wPlayerRole];
   INT        iValue = TRAINER_ReadNumber(TR("練到幾級，零為不停", "练到几级，零为不停"),
      (wLevel < MAX_LEVELS) ? wLevel + 1 : MAX_LEVELS, 0, MAX_LEVELS, L"不停");

   if (iValue < 0)
   {
      return FALSE;
   }

   TRAINER_StartJob(kTrainerJobHunt, 0);
   g_Walk.wHuntLevel = (WORD)iValue;
   return TRUE;
}

static BOOL
TRAINER_LevelMenu(
   VOID
)
/*++
  Purpose:

    Let the player choose the level to keep the party at.

  Parameters:

    None.

  Return value:

    TRUE if the menu should close to show what happened.

--*/
{
   WCHAR   szText[64];
   INT     iValue = g_wAutoLevel;

   if (iValue == 0)
   {
      iValue = gpGlobals->g.PlayerRoles.rgwLevel[gpGlobals->rgParty[0].wPlayerRole] + 1;
   }

   iValue = TRAINER_ReadNumber(TR("目標等級，零為關閉", "目标等级，零为关闭"),
      (iValue > MAX_LEVELS) ? MAX_LEVELS : iValue, 0, MAX_LEVELS, TR("關閉", "关闭"));

   if (iValue < 0)
   {
      return FALSE;
   }

   g_wAutoLevel = (WORD)iValue;
   TRAINER_ApplyAutoLevel();

   if (g_wAutoLevel == 0)
   {
      TRAINER_SetToast(TR("自動升級已關閉", "自动升级已关闭"));
   }
   else
   {
      PAL_swprintf(szText, sizeof(szText) / sizeof(WCHAR),
         TR("全隊保持在 %d 級以上", "全队保持在 %d 级以上"), g_wAutoLevel);
      TRAINER_SetToast(szText);
   }

   return TRUE;
}

static VOID
TRAINER_MoneyMenu(
   VOID
)
/*++
  Purpose:

    Let the player set the amount of money.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   const INT iValue = TRAINER_ReadNumberInSteps(TR("修改金錢", "修改金钱"),
      (gpGlobals->dwCash > MAX_CASH) ? MAX_CASH : (INT)gpGlobals->dwCash, 0, MAX_CASH, NULL, 1000, 100000,
      TR("上下加減千 左右加減十萬", "上下加减千 左右加减十万"));

   if (iValue >= 0)
   {
      gpGlobals->dwCash = (DWORD)iValue;
   }
}

static VOID
TRAINER_ListObjects(
   WORD           wFirst,
   WORD           wLast
)
/*++
  Purpose:

    List the objects in a range that have a name.

  Parameters:

    [IN]  wFirst - the first object.

    [IN]  wLast - the last object.

  Return value:

    None.

--*/
{
   int   w;

   g_nObject = 0;

   for (w = wFirst; w <= wLast; w++)
   {
      LPCWSTR lpszName = PAL_GetWord((WORD)w);

      if (lpszName != NULL && lpszName[0] != L'\0')
      {
         g_rgwObject[g_nObject++] = (WORD)w;
      }
   }
}

static LPCWSTR
TRAINER_ItemLabel(
   INT            iItem,
   BYTE          *pbColor
)
{
   if (PAL_GetItemAmount(g_rgwObject[iItem]) > 0)
   {
      *pbColor = MENUITEM_COLOR_EQUIPPEDITEM;
   }

   return PAL_GetWord(g_rgwObject[iItem]);
}

static LPCWSTR
TRAINER_ItemFooter(
   INT            iItem
)
{
   PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), TR("現有 %d", "现有 %d"), PAL_GetItemAmount(g_rgwObject[iItem]));
   return g_szLabel;
}

static VOID
TRAINER_ItemMenu(
   VOID
)
/*++
  Purpose:

    Let the player add any item to the inventory.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   static INT   iCursor = 0;
   INT          i;

   TRAINER_ListObjects(OBJECT_ITEM_FIRST, OBJECT_ITEM_LAST);

   while ((i = TRAINER_ListMenu(TR("獲得物品", "获得物品"), g_nObject, 3, &iCursor, TRAINER_ItemLabel, TRAINER_ItemFooter)) >= 0)
   {
      const WORD   wItem = g_rgwObject[i];
      const INT    iHave = PAL_GetItemAmount(wItem);

      if (iHave < 99)
      {
         const INT iAmount = TRAINER_ReadNumber(PAL_GetWord(wItem), 1, 1, 99 - iHave, NULL);

         if (iAmount > 0)
         {
            PAL_AddItemToInventory(wItem, iAmount);
         }
      }
   }
}

static BOOL
TRAINER_RoleHasMagic(
   WORD           wPlayerRole,
   WORD           wMagic
)
{
   int   i;

   for (i = 0; i < MAX_PLAYER_MAGICS; i++)
   {
      if (gpGlobals->g.PlayerRoles.rgwMagic[i][wPlayerRole] == wMagic)
      {
         return TRUE;
      }
   }

   return FALSE;
}

static LPCWSTR
TRAINER_MagicLabel(
   INT            iItem,
   BYTE          *pbColor
)
{
   if (TRAINER_RoleHasMagic(g_wMagicRole, g_rgwObject[iItem]))
   {
      *pbColor = MENUITEM_COLOR_EQUIPPEDITEM;
   }

   return PAL_GetWord(g_rgwObject[iItem]);
}

static LPCWSTR
TRAINER_MagicFooter(
   INT            iItem
)
{
   int   i, n = 0;

   for (i = 0; i < MAX_PLAYER_MAGICS; i++)
   {
      if (gpGlobals->g.PlayerRoles.rgwMagic[i][g_wMagicRole] != 0)
      {
         n++;
      }
   }

   PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), TR("已學 %d/%d，按確定學會或忘記", "已学 %d/%d，按确定学会或忘记"), n, MAX_PLAYER_MAGICS);
   return g_szLabel;
}

static LPCWSTR
TRAINER_RoleLabel(
   INT            iItem,
   BYTE          *pbColor
)
{
   return PAL_GetWord(gpGlobals->g.PlayerRoles.rgwName[gpGlobals->rgParty[iItem].wPlayerRole]);
}

static VOID
TRAINER_MagicMenu(
   VOID
)
/*++
  Purpose:

    Let the player teach any magic to someone in the party, or take it away.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   static INT   iCursor = 0;
   INT          iRoleCursor = 0, iRole, i;

   TRAINER_ListObjects(OBJECT_MAGIC_FIRST, OBJECT_MAGIC_LAST);

   while ((iRole = TRAINER_ListMenu(TR("學習法術", "学习法术"), gpGlobals->wMaxPartyMemberIndex + 1, 1, &iRoleCursor, TRAINER_RoleLabel, NULL)) >= 0)
   {
      g_wMagicRole = gpGlobals->rgParty[iRole].wPlayerRole;

      while ((i = TRAINER_ListMenu(PAL_GetWord(gpGlobals->g.PlayerRoles.rgwName[g_wMagicRole]), g_nObject, 3, &iCursor, TRAINER_MagicLabel, TRAINER_MagicFooter)) >= 0)
      {
         if (TRAINER_RoleHasMagic(g_wMagicRole, g_rgwObject[i]))
         {
            PAL_RemoveMagic(g_wMagicRole, g_rgwObject[i]);
         }
         else
         {
            PAL_AddMagic(g_wMagicRole, g_rgwObject[i]);
         }
      }
   }
}

static LPCWSTR
TRAINER_MainLabel(
   INT            iItem,
   BYTE          *pbColor
)
{
   switch (iItem)
   {
   case 0:
      return TR("尋寶", "寻宝");

   case 1:
      return L"前往出口";

   case 2:
      return TR("自動解謎", "自动解谜");

   case 3:
      return TR("自動打怪", "自动打怪");

   case 4:
      return g_fAutoBattle ? TR("自動戰鬥：開啟", "自动战斗：开启") : TR("自動戰鬥：關閉", "自动战斗：关闭");

   case 5:
      if (g_wAutoLevel == 0)
      {
         return TR("自動升級：關閉", "自动升级：关闭");
      }
      PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), TR("自動升級：%d 級", "自动升级：%d 级"), g_wAutoLevel);
      return g_szLabel;

   case 6:
      PAL_swprintf(g_szLabel, sizeof(g_szLabel) / sizeof(WCHAR), TR("修改金錢：%d", "修改金钱：%d"), (INT)gpGlobals->dwCash);
      return g_szLabel;

   case 7:
      return TR("獲得物品", "获得物品");

   default:
      return TR("學習法術", "学习法术");
   }
}

VOID
TRAINER_Menu(
   VOID
)
/*++
  Purpose:

    Show the trainer menu.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   static INT   iCursor = 0;
   BOOL         fClose = FALSE;

   while (!fClose)
   {
      switch (TRAINER_ListMenu(L"修改器", 9, 1, &iCursor, TRAINER_MainLabel, NULL))
      {
      case 0:
         fClose = TRAINER_TreasureMenu();
         break;

      case 1:
         fClose = TRAINER_ExitMenu();
         break;

      case 2:
         TRAINER_StartPuzzle();
         fClose = TRUE;
         break;

      case 3:
         fClose = TRAINER_HuntMenu();
         break;

      case 4:
         g_fAutoBattle = !g_fAutoBattle;
         gpGlobals->fAutoBattle = g_fAutoBattle;
         break;

      case 5:
         fClose = TRAINER_LevelMenu();
         break;

      case 6:
         TRAINER_MoneyMenu();
         break;

      case 7:
         TRAINER_ItemMenu();
         break;

      case 8:
         TRAINER_MagicMenu();
         break;

      default:
         fClose = TRUE;
         break;
      }
   }

   PAL_ClearKeyState();
}
