// MENU SELF-REPAIR RULES -- the menu fills itself in during its first session ([mcmfix]).
//
// The same file ships in Physical Collision VR (src/mcm_selfrepair.h) and TrueWieldVR (src/McmSelfRepair.h), byte
// for byte. Change one, copy it to the other. No game types, so tests/ can run it on the host.
//
// WHY THIS EXISTS. Reported by the MGO list authors on 2026-09-16: on the first session with this mod the in-game
// menu listed it, but every page was blank and the title was the plugin's internal name. One save and reload fixed
// it. MCM Helper fills a menu's title (the script's ModName) and its page list (Pages) in exactly one place: its
// native LoadConfig, which the script calls just before SkyUI registers the menu. Once SkyUI lists the menu,
// LoadConfig returns early for the rest of the session -- so if those two values are lost after that moment,
// nothing in MCM Helper or SkyUI puts them back. The plugin reads the menu's state through the Papyrus VM and,
// when SkyUI lists it with no pages, has MCM Helper read config.json again. These rules decide WHEN; the plugin
// does the reading and the writing.
#pragma once

#include <string>

namespace mcm_selfrepair {

    // What the plugin read about its own menu at one moment.
    struct MenuState {
        bool questFound = false;      // the menu quest exists, i.e. the plugin is loaded
        bool scriptFound = false;     // MCM_ConfigBase is bound to that quest
        bool managerFound = false;    // SkyUI's SKI_ConfigManager script was found
        bool standardLayout = false;  // the manager has _modConfigs and _modNames arrays (menu-sorting mods may not)
        int configId = -1;            // the menu script's _configID: -1 until SkyUI registers it, -2 if SkyUI was busy
        bool listedAtId = false;      // SkyUI's _modConfigs[configId] is this menu's script
        int pageCount = 0;            // entries in the script's Pages (0 when there is no array)
        std::string title;            // the script's ModName: config.json's displayName once MCM Helper has run
        std::string listName;         // SkyUI's _modNames[configId]: the name its mod list shows
        bool haveListName = false;    // listName was actually read
    };

    enum class Verdict {
        kNoMenu,          // no quest: the plugin is not loaded (an ini-only install) -- nothing to watch
        kNotBound,        // the quest has no script object yet: the first moments of a load
        kNoSkyUI,         // SkyUI's manager is missing
        kUnknownSkyUI,    // a manager without the standard arrays -- not ours to write into
        kNotRegistered,   // SkyUI has not listed the menu yet; its own retries (every 5-30 s) do that
        kEmptyPages,      // listed, with no page list: the blank menu
        kStaleListName,   // pages are fine, but SkyUI's mod list still shows another name than the title
        kHealthy,
    };

    inline Verdict Judge(const MenuState& s) {
        if (!s.questFound) {
            return Verdict::kNoMenu;
        }
        if (!s.scriptFound) {
            return Verdict::kNotBound;
        }
        if (!s.managerFound) {
            return Verdict::kNoSkyUI;
        }
        if (!s.standardLayout) {
            return Verdict::kUnknownSkyUI;
        }
        if (s.configId < 0 || !s.listedAtId) {
            return Verdict::kNotRegistered;
        }
        // The title is deliberately NOT a fault on its own: TrueWieldVR's display name is its internal name. MCM
        // Helper sets the title and the pages together, so an empty page list is the one reliable sign.
        if (s.pageCount <= 0) {
            return Verdict::kEmptyPages;
        }
        if (s.haveListName && s.listName != s.title) {
            return Verdict::kStaleListName;
        }
        return Verdict::kHealthy;
    }

    enum class Action { kNone, kRefill, kRename };

    // Attempt bookkeeping. An EPISODE runs from the menu going wrong until it is next seen healthy.
    struct Repairer {
        static constexpr int kMaxRefillsPerEpisode = 3;
        static constexpr int kMaxRefillsPerSession = 10;
        static constexpr int kMaxRenamesPerEpisode = 3;
        // A refill is two queued Papyrus calls plus a config.json read; this long without the pages coming back
        // means it did not take. SkyUI's own registration retry is at most 30 s away if the menu is left unlisted.
        static constexpr double kRefillCooldownSeconds = 10.0;

        int refills = 0;        // this episode
        int renames = 0;        // this episode
        int totalRefills = 0;   // this session
        double lastRefillAt = -1.0e9;

        // a_now is seconds on any steady clock; a_repairOn is the plugin's switch (off = report only).
        Action Next(Verdict a_verdict, double a_now, bool a_repairOn) {
            switch (a_verdict) {
                case Verdict::kHealthy:
                    refills = 0;
                    renames = 0;
                    lastRefillAt = -1.0e9;
                    return Action::kNone;
                case Verdict::kEmptyPages:
                    if (!a_repairOn || GaveUp() || a_now - lastRefillAt < kRefillCooldownSeconds) {
                        return Action::kNone;
                    }
                    ++refills;
                    ++totalRefills;
                    lastRefillAt = a_now;
                    return Action::kRefill;
                case Verdict::kStaleListName:
                    if (!a_repairOn || renames >= kMaxRenamesPerEpisode) {
                        return Action::kNone;
                    }
                    ++renames;
                    return Action::kRename;
                default:
                    return Action::kNone;
            }
        }

        [[nodiscard]] bool GaveUp() const {
            return refills >= kMaxRefillsPerEpisode || totalRefills >= kMaxRefillsPerSession;
        }
    };

    // bMcmRepairSelfTest: once per load, blank a menu that already works, so the repair can be watched on any save.
    inline bool ShouldBlankForSelfTest(bool a_selfTestOn, bool a_alreadyBlanked, Verdict a_verdict) {
        return a_selfTestOn && !a_alreadyBlanked && a_verdict == Verdict::kHealthy;
    }

    inline std::string Describe(const MenuState& s) {
        if (!s.questFound) {
            return "no menu quest (the plugin is not loaded)";
        }
        if (!s.scriptFound) {
            return "the menu quest has no MCM_ConfigBase script bound yet";
        }
        if (!s.managerFound) {
            return "SkyUI's config manager was not found";
        }
        if (!s.standardLayout) {
            return "SkyUI's config manager has no _modConfigs/_modNames lists (a menu-sorting mod?)";
        }
        std::string out = s.configId >= 0 && s.listedAtId
                              ? "SkyUI lists it in slot " + std::to_string(s.configId) + " as \"" + s.listName + "\""
                              : "not listed by SkyUI yet (_configID " + std::to_string(s.configId) + ")";
        out += "; title \"" + s.title + "\"; " + std::to_string(s.pageCount) + (s.pageCount == 1 ? " page" : " pages");
        return out;
    }

    inline const char* VerdictText(Verdict v) {
        switch (v) {
            case Verdict::kNoMenu:
                return "no menu";
            case Verdict::kNotBound:
                return "waiting for the menu script";
            case Verdict::kNoSkyUI:
                return "no SkyUI";
            case Verdict::kUnknownSkyUI:
                return "unrecognised SkyUI layout, not touching it";
            case Verdict::kNotRegistered:
                return "waiting for SkyUI to list it";
            case Verdict::kEmptyPages:
                return "BLANK -- the page list is empty";
            case Verdict::kStaleListName:
                return "the MCM list shows an old name";
            case Verdict::kHealthy:
                return "healthy";
        }
        return "?";
    }
}
