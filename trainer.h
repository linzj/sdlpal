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

#ifndef TRAINER_H
#define TRAINER_H

#include "common.h"
#include "palcommon.h"

// What a script can do, worked out by reading it without running it.
typedef enum tagTRAINERSCRIPTFLAG
{
   kTrainerScriptItem    = (1 << 0),   // adds an item to the inventory
   kTrainerScriptCash    = (1 << 1),   // gives money
   kTrainerScriptScene   = (1 << 2),   // changes the scene
   kTrainerScriptBattle  = (1 << 3),   // starts a battle
   kTrainerScriptStory   = (1 << 4),   // characters talk, or too long to read through
   kTrainerScriptOther   = (1 << 5),   // does something a plain pick-up never does
   kTrainerScriptMove    = (1 << 6),   // puts the party at a position
   kTrainerScriptCarry   = (1 << 7),   // walks or carries the party somewhere
} TRAINERSCRIPTFLAG;

typedef struct tagTRAINERSCRIPTINFO
{
   WORD          wEntry;               // the script entry this is about
   WORD          wFlags;               // TRAINERSCRIPTFLAG
   WORD          wItem;                // the first item it adds
   WORD          wCash;                // the first amount of money it gives
   WORD          wScene;               // the first scene it changes to
   WORD          nLoot;                // how many items and amounts of money it gives
   WORD          xMove, yMove;         // the first position it puts the party at
} TRAINERSCRIPTINFO, *LPTRAINERSCRIPTINFO;

PAL_C_LINKAGE_BEGIN

VOID
TRAINER_AnalyzeScript(
   WORD                  wEntry,
   LPTRAINERSCRIPTINFO   pInfo
);

VOID
TRAINER_Menu(
   VOID
);

VOID
TRAINER_Update(
   VOID
);

VOID
TRAINER_Stop(
   VOID
);

VOID
TRAINER_DrawOverlay(
   VOID
);

PALDIRECTION
TRAINER_GetDirection(
   VOID
);

BOOL
TRAINER_IsFightingByItself(
   VOID
);

VOID
TRAINER_CheckBattleInput(
   VOID
);

VOID
TRAINER_ApplyAutoBattle(
   VOID
);

PAL_C_LINKAGE_END

#endif
