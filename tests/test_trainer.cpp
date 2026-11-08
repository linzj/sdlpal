#include <gtest/gtest.h>
#include <vector>
extern "C"{
    #include "common.h"
    #include "palcommon.h"
    #include "global.h"
    #include "trainer.h"
}

// The scripts below have the shape of those in the game data.
class TrainerScript : public ::testing::Test {
protected:
    std::vector<SCRIPTENTRY> script;
    LPSCRIPTENTRY savedEntries;
    int savedCount;

    void SetUp() override {
        savedEntries = gpGlobals->g.lprgScriptEntry;
        savedCount = gpGlobals->g.nScriptEntry;
        script.resize(4096);
        for (auto &e : script) e = SCRIPTENTRY{ 0, { 0, 0, 0 } };
        gpGlobals->g.lprgScriptEntry = script.data();
        gpGlobals->g.nScriptEntry = (int)script.size();
    }

    void TearDown() override {
        gpGlobals->g.lprgScriptEntry = savedEntries;
        gpGlobals->g.nScriptEntry = savedCount;
    }

    void line(int at, WORD op, WORD a = 0, WORD b = 0, WORD c = 0) {
        script[at] = SCRIPTENTRY{ op, { a, b, c } };
    }

    TRAINERSCRIPTINFO analyze(WORD entry) {
        TRAINERSCRIPTINFO info;
        TRAINER_AnalyzeScript(entry, &info);
        return info;
    }
};

TEST_F(TrainerScript, PickUpOfOneItem) {
    line(1, 0x0047, 0x4E);           // sound
    line(2, 0x003E);                 // text in a small window
    line(3, 0xFFFF, 0x04F1);
    line(4, 0x001F, 0x63);           // add the item
    line(5, 0x0049, 0xFFFF, 0);      // hide the event
    line(6, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(kTrainerScriptItem, info.wFlags);
    EXPECT_EQ(0x63, info.wItem);
    EXPECT_EQ(1, info.nLoot);
}

TEST_F(TrainerScript, ChestGivesItsItemsOnlyTheFirstTime) {
    line(1, 0x0004, 10);             // call what opens the chest
    line(2, 0x003E);
    line(3, 0xFFFF, 0x09AB);
    line(4, 0x001F, 0x43);
    line(5, 0x001F, 0x44);
    line(6, 0x0002, 20);             // next time, go on from line 20

    line(10, 0x0014, 1);
    line(11, 0x0005);
    line(12, 0x0047, 0x4E);
    line(13, 0x0000);

    line(20, 0x003B);                // "it is empty"
    line(21, 0xFFFF, 5);
    line(22, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(kTrainerScriptItem, info.wFlags);
    EXPECT_EQ(0x43, info.wItem);
    EXPECT_EQ(2, info.nLoot);

    info = analyze(20);
    EXPECT_EQ(0, info.nLoot);
    EXPECT_NE(0, info.wFlags & kTrainerScriptOther);
}

TEST_F(TrainerScript, Money) {
    line(1, 0x001E, 50);
    line(2, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(kTrainerScriptCash, info.wFlags);
    EXPECT_EQ(50, info.wCash);
}

TEST_F(TrainerScript, ShopIsNoTreasure) {
    line(1, 0x000A, 10);             // jump if the player says no
    line(2, 0x001E, (WORD)-50, 10);  // pay, or jump if there is not enough money
    line(3, 0x001F, 0x70);
    line(4, 0x0000);
    line(10, 0xFFFF, 6);
    line(11, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_NE(0, info.wFlags & kTrainerScriptItem);
    EXPECT_NE(0, info.wFlags & kTrainerScriptOther);
    EXPECT_EQ(0, info.wCash);
}

TEST_F(TrainerScript, MonsterIsNoStoryEvenWithTextWhenLosing) {
    line(1, 0x0007, 1, 10, 20);      // battle, jump to 10 if lost and to 20 if fled
    line(2, 0x0000);
    line(10, 0x003B);
    line(11, 0xFFFF, 0x313D);
    line(12, 0x0000);
    line(20, 0x004B);
    line(21, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_NE(0, info.wFlags & kTrainerScriptBattle);
    EXPECT_EQ(0, info.wFlags & kTrainerScriptStory);
}

TEST_F(TrainerScript, PeopleTalkingIsStory) {
    line(1, 0x003C, 3);
    line(2, 0xFFFF, 1);
    line(3, 0x0007, 1);
    line(4, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_NE(0, info.wFlags & kTrainerScriptBattle);
    EXPECT_NE(0, info.wFlags & kTrainerScriptStory);
}

TEST_F(TrainerScript, TextThatWaitsForAKeyIsStory) {
    line(1, 0x003B);                 // text in the middle of the screen
    line(2, 0xFFFF, 5);
    line(3, 0x0000);

    EXPECT_NE(0, analyze(1).wFlags & kTrainerScriptStory);
}

TEST_F(TrainerScript, Exit) {
    line(1, 0x0046, 45, 96, 0);      // the place in the next scene
    line(2, 0x0059, 3);              // the next scene
    line(3, 0x0050);
    line(4, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_NE(0, info.wFlags & kTrainerScriptScene);
    EXPECT_NE(0, info.wFlags & kTrainerScriptMove);
    EXPECT_EQ(3, info.wScene);
    EXPECT_EQ(45 * 32, info.xMove);
    EXPECT_EQ(96 * 16, info.yMove);
}

TEST_F(TrainerScript, TeleporterWithinAScene) {
    line(1, 0x0059, 225);            // the same scene, which does nothing
    line(2, 0x0046, 18, 75, 1);      // the half a tile counts
    line(3, 0x0050);
    line(4, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(225, info.wScene);
    EXPECT_EQ(18 * 32 + 16, info.xMove);
    EXPECT_EQ(75 * 16 + 8, info.yMove);
}

TEST_F(TrainerScript, RaftCarriesThePartyToTheLastPlace) {
    line(1, 0x0044, 40, 50, 0);      // ride to the other side
    line(2, 0x0044, 41, 52, 0);
    line(3, 0x007A, 42, 53, 1);      // and step off
    line(4, 0x0001);
    line(5, 0x0044, 1, 1, 0);        // the way back is for the next time

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_NE(0, info.wFlags & kTrainerScriptCarry);
    EXPECT_NE(0, info.wFlags & kTrainerScriptCarryTo);
    EXPECT_EQ(42 * 32 + 16, info.xCarry);
    EXPECT_EQ(53 * 16 + 8, info.yCarry);
    EXPECT_NE(0, info.wFlags & kTrainerScriptRide);
    EXPECT_EQ(41 * 32, info.xRide);
    EXPECT_EQ(52 * 16, info.yRide);
}

TEST_F(TrainerScript, StepFromWhereverThePartyIsEndsSomewhereUnknown) {
    line(1, 0x007A, 42, 53, 1);
    line(2, 0x006E, 16, 8, 0);
    line(3, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_NE(0, info.wFlags & kTrainerScriptCarry);
    EXPECT_EQ(0, info.wFlags & kTrainerScriptCarryTo);
}

TEST_F(TrainerScript, FollowsConditionalJumps) {
    line(1, 0x0058, 0x70, 1, 10);    // jump if the item is missing
    line(2, 0x0000);
    line(10, 0x0059, 7);
    line(11, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(7, info.wScene);
}

TEST_F(TrainerScript, StopsWhereTheScriptStops) {
    line(1, 0x0001);                 // the rest is for the next time
    line(2, 0x001F, 0x70);
    line(3, 0x0000);

    EXPECT_EQ(0, analyze(1).nLoot);
}

TEST_F(TrainerScript, GoesOnAfterAStopThatCounts) {
    line(1, 0x0002, 10, 2);          // only stops the first time
    line(2, 0x001F, 0x70);
    line(3, 0x0000);

    EXPECT_EQ(1, analyze(1).nLoot);
}

TEST_F(TrainerScript, EndlessLoopEnds) {
    line(1, 0x001F, 0x70);
    line(2, 0x0003, 1);

    EXPECT_EQ(kTrainerScriptItem, analyze(1).wFlags);
}

TEST_F(TrainerScript, TooLongToReadCountsAsStory) {
    for (int i = 1; i < 4000; i++) line(i, 0x001F, 0x70);

    EXPECT_NE(0, analyze(1).wFlags & kTrainerScriptStory);
}

TEST_F(TrainerScript, NoScript) {
    EXPECT_EQ(0, analyze(0).wFlags);
}

// The way on from the deepest floor of the tomb: only with the jade beads on.
TEST_F(TrainerScript, DoorThatOpensWithSomethingOnLeadsOn) {
    line(1, 0x0086, 0x112, 0, 10);   // jump if nobody wears the item
    line(2, 0x0049, 0x4E4, 0);       // the barrier goes away
    line(3, 0x0073);
    line(4, 0x0002, 20);             // and the next time goes through
    line(10, 0x006E, (WORD)-10, 10); // pushed back
    line(11, 0x003B);
    line(12, 0xFFFF, 0x133F);
    line(13, 0x0000);

    line(20, 0x0004, 30);            // walk in
    line(21, 0x0059, 60);
    line(22, 0x0046, 42, 95, 1);
    line(23, 0x0050);
    line(24, 0x0000);
    line(30, 0x006E, (WORD)-10, 10);
    line(31, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(0x112, info.wWear);
    EXPECT_NE(0, info.wFlags & kTrainerScriptScene);
    EXPECT_NE(0, info.wFlags & kTrainerScriptMove);
    EXPECT_EQ(0, info.wFlags & kTrainerScriptStory);
    EXPECT_EQ(60, info.wScene);
    EXPECT_EQ(42 * 32 + 16, info.xMove);
    EXPECT_EQ(95 * 16 + 8, info.yMove);
}

TEST_F(TrainerScript, WhatTheNextTimeSaysIsNotFollowed) {
    line(1, 0x0049, 0x4E4, 0);
    line(2, 0x0002, 20);
    line(20, 0x003B);
    line(21, 0xFFFF, 5);
    line(22, 0x0000);

    EXPECT_EQ(0, analyze(1).wFlags & kTrainerScriptStory);
}

TEST_F(TrainerScript, NextTimeDoesNotCountAfterTheStory) {
    line(1, 0x003C, 3);              // someone talks first
    line(2, 0xFFFF, 1);
    line(3, 0x0002, 20);
    line(20, 0x0059, 60);
    line(21, 0x0000);

    TRAINERSCRIPTINFO info = analyze(1);
    EXPECT_EQ(0, info.wFlags & kTrainerScriptScene);
    EXPECT_EQ(0, info.wScene);
}
