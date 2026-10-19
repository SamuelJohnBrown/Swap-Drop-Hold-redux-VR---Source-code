// Console tests for McmSelfRepair.h, the in-game menu self-repair rules ([mcmfix]).
//
// Ported unchanged from TrueWieldVR's tests/mcm_selfrepair_test.cpp: McmSelfRepair.h is the same file in
// every mod that carries the repair, so its tests are too. Only this comment and the include differ.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_mcm_selfrepair_tests.cpp /Fe:mcm_selfrepair_tests.exe
//   mcm_selfrepair_tests.exe
// Exit code 0 means every check passed.
#include "McmSelfRepair.h"

#include <cstdio>
#include <string>

static int g_passed = 0, g_failed = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_passed;                                                        \
        } else {                                                               \
            ++g_failed;                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                      \
    } while (0)

using namespace mcm_selfrepair;

// A menu SkyUI has listed, with MCM Helper's init done: the state every working menu is in.
static MenuState Listed(int pages, const std::string& title, const std::string& listName) {
    MenuState s;
    s.questFound = true;
    s.scriptFound = true;
    s.managerFound = true;
    s.standardLayout = true;
    s.configId = 12;
    s.listedAtId = true;
    s.pageCount = pages;
    s.title = title;
    s.listName = listName;
    s.haveListName = true;
    return s;
}

static void JudgeReadsEachStage() {
    MenuState s;
    CHECK(Judge(s) == Verdict::kNoMenu);  // the ini-only install: no quest at all
    s.questFound = true;
    CHECK(Judge(s) == Verdict::kNotBound);
    s.scriptFound = true;
    CHECK(Judge(s) == Verdict::kNoSkyUI);
    s.managerFound = true;
    CHECK(Judge(s) == Verdict::kUnknownSkyUI);  // a manager without _modConfigs/_modNames: not ours to touch
    s.standardLayout = true;
    CHECK(Judge(s) == Verdict::kNotRegistered);  // _configID -1: SkyUI has not listed it yet
    s.configId = -2;
    CHECK(Judge(s) == Verdict::kNotRegistered);  // SkyUI was busy; its own retry comes within 30 s
    s.configId = 12;
    CHECK(Judge(s) == Verdict::kNotRegistered);  // an id, but the slot holds another menu
}

static void JudgeMgoBlankMenu() {
    // What the MGO authors saw, 2026-09-16: listed under the plugin's internal name, no pages.
    CHECK(Judge(Listed(0, "PhysicalCollisionVR", "PhysicalCollisionVR")) == Verdict::kEmptyPages);
    // Blank pages are blank whatever the title says.
    CHECK(Judge(Listed(0, "Physical Collision VR", "Physical Collision VR")) == Verdict::kEmptyPages);
}

static void JudgeHealthyAndStaleName() {
    CHECK(Judge(Listed(13, "Physical Collision VR", "Physical Collision VR")) == Verdict::kHealthy);
    // TrueWieldVR's display name IS its internal name: a run-together title alone is not a fault.
    CHECK(Judge(Listed(6, "TrueWieldVR", "TrueWieldVR")) == Verdict::kHealthy);
    // Pages back, but SkyUI's mod list still holds the name it cached at registration.
    CHECK(Judge(Listed(13, "Physical Collision VR", "PhysicalCollisionVR")) == Verdict::kStaleListName);
    // A list name that could not be read is not evidence of anything.
    MenuState unknownName = Listed(13, "Physical Collision VR", "");
    unknownName.haveListName = false;
    CHECK(Judge(unknownName) == Verdict::kHealthy);
}

static void RefillOncePerCooldownAndGiveUpAfterThree() {
    Repairer r;
    CHECK(r.Next(Verdict::kEmptyPages, 0.0, true) == Action::kRefill);
    CHECK(r.Next(Verdict::kEmptyPages, 2.0, true) == Action::kNone);  // the last refill is still landing
    CHECK(r.Next(Verdict::kNotRegistered, 4.0, true) == Action::kNone);  // mid-refill: SkyUI re-listing it
    CHECK(r.Next(Verdict::kEmptyPages, 9.9, true) == Action::kNone);
    CHECK(r.Next(Verdict::kEmptyPages, 10.0, true) == Action::kRefill);
    CHECK(!r.GaveUp());
    CHECK(r.Next(Verdict::kEmptyPages, 20.0, true) == Action::kRefill);
    CHECK(r.Next(Verdict::kEmptyPages, 30.0, true) == Action::kNone);  // three refills without a healthy menu
    CHECK(r.GaveUp());
    CHECK(r.Next(Verdict::kEmptyPages, 300.0, true) == Action::kNone);  // and it stays given up
}

static void HealthyStartsANewEpisode() {
    Repairer r;
    CHECK(r.Next(Verdict::kEmptyPages, 0.0, true) == Action::kRefill);
    CHECK(r.Next(Verdict::kEmptyPages, 10.0, true) == Action::kRefill);
    CHECK(r.Next(Verdict::kEmptyPages, 20.0, true) == Action::kRefill);
    CHECK(r.Next(Verdict::kHealthy, 22.0, true) == Action::kNone);
    CHECK(!r.GaveUp());
    // Blanked again later in the session: repaired again, without waiting out a cooldown from long ago.
    CHECK(r.Next(Verdict::kEmptyPages, 25.0, true) == Action::kRefill);
    CHECK(r.refills == 1);
}

static void SessionCapStopsAnEndlessLoop() {
    Repairer r;
    double t = 0.0;
    int refills = 0;
    for (int episode = 0; episode < 20; ++episode) {
        if (r.Next(Verdict::kEmptyPages, t, true) == Action::kRefill) {
            ++refills;
        }
        t += 11.0;
        r.Next(Verdict::kHealthy, t, true);
        t += 11.0;
    }
    CHECK(refills == Repairer::kMaxRefillsPerSession);
    CHECK(r.Next(Verdict::kEmptyPages, t + 100.0, true) == Action::kNone);
    CHECK(r.GaveUp());
}

static void SwitchOffOnlyReports() {
    Repairer r;
    CHECK(r.Next(Verdict::kEmptyPages, 0.0, false) == Action::kNone);
    CHECK(r.Next(Verdict::kStaleListName, 1.0, false) == Action::kNone);
    CHECK(r.refills == 0 && r.renames == 0);
    CHECK(!r.GaveUp());  // it never tried, so it has not given up
}

static void RenameIsCappedAndResetByHealth() {
    Repairer r;
    CHECK(r.Next(Verdict::kStaleListName, 0.0, true) == Action::kRename);
    CHECK(r.Next(Verdict::kStaleListName, 2.0, true) == Action::kRename);
    CHECK(r.Next(Verdict::kStaleListName, 4.0, true) == Action::kRename);
    CHECK(r.Next(Verdict::kStaleListName, 6.0, true) == Action::kNone);
    CHECK(r.Next(Verdict::kHealthy, 8.0, true) == Action::kNone);
    CHECK(r.Next(Verdict::kStaleListName, 10.0, true) == Action::kRename);
}

static void NothingElseEverActs() {
    Repairer r;
    for (auto v : { Verdict::kNoMenu, Verdict::kNotBound, Verdict::kNoSkyUI, Verdict::kUnknownSkyUI,
                    Verdict::kNotRegistered, Verdict::kHealthy }) {
        CHECK(r.Next(v, 0.0, true) == Action::kNone);
    }
}

static void SelfTestBlanksOnceAHealthyMenu() {
    CHECK(!ShouldBlankForSelfTest(false, false, Verdict::kHealthy));  // switch off
    CHECK(ShouldBlankForSelfTest(true, false, Verdict::kHealthy));
    CHECK(!ShouldBlankForSelfTest(true, true, Verdict::kHealthy));  // once per load
    CHECK(!ShouldBlankForSelfTest(true, false, Verdict::kNotRegistered));  // only a menu that already works
    CHECK(!ShouldBlankForSelfTest(true, false, Verdict::kEmptyPages));
}

static void DescribeSaysWhatWasRead() {
    const std::string mgo = Describe(Listed(0, "PhysicalCollisionVR", "PhysicalCollisionVR"));
    CHECK(mgo.find("slot 12") != std::string::npos);
    CHECK(mgo.find("\"PhysicalCollisionVR\"") != std::string::npos);
    CHECK(mgo.find("0 pages") != std::string::npos);
    CHECK(Describe(Listed(1, "A", "A")).find("1 page;") == std::string::npos);  // no stray separator
    CHECK(Describe(Listed(1, "A", "A")).find("1 page") != std::string::npos);
    MenuState waiting = Listed(0, "PhysicalCollisionVR", "");
    waiting.configId = -1;
    waiting.listedAtId = false;
    CHECK(Describe(waiting).find("not listed") != std::string::npos);
    CHECK(Describe(MenuState{}).find("not loaded") != std::string::npos);
    CHECK(std::string(VerdictText(Verdict::kEmptyPages)).find("BLANK") != std::string::npos);
    CHECK(std::string(VerdictText(Verdict::kHealthy)) == "healthy");
}

int main() {
    JudgeReadsEachStage();
    JudgeMgoBlankMenu();
    JudgeHealthyAndStaleName();
    RefillOncePerCooldownAndGiveUpAfterThree();
    HealthyStartsANewEpisode();
    SessionCapStopsAnEndlessLoop();
    SwitchOffOnlyReports();
    RenameIsCappedAndResetByHealth();
    NothingElseEverActs();
    SelfTestBlanksOnceAHealthyMenu();
    DescribeSaysWhatWasRead();
    std::printf("mcm_selfrepair_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
