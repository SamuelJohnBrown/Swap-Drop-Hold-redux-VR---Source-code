"""Build the FOMOD installer for Swap Drop & Hold redux.

Stage the mod first, then:  python tools_build_fomod.py

Two install steps: the weapon mode to start in (Simple is preselected) together with how a
weapon you pick up is held (test15: off / where you grab / reverse grip / both, both
preselected), and whether to install the MCM menu. The mode and the grip switches live in
BOTH the mod's ini and the menu's two settings files, and the menu's copy overrides the
ini - so a Grip hold install whose menu files still said Simple would quietly run Simple.
Every mode x grip pair therefore gets its own ini and its own pair of menu settings files,
generated from the staged (Simple, both-on) copies by changing those lines and nothing
else. The ini goes in through a conditional install on the two choices; the menu's pair
only when the menu is installed too.
"""

import io, os, re, shutil, sys, xml.etree.ElementTree as ET

SRC = r"E:\SwapDropAndHoldRedux\zip_staging"
OUT = r"E:\SwapDropAndHoldRedux\fomod_staging"
VERSION = "3.0.0"

# Spelled out rather than escaped: these keep getting mangled in transit.
CRLF = chr(13) + chr(10)
LF = chr(10)
BOM = b"\xef\xbb\xbf"

DLL_REL = os.path.join("SKSE", "Plugins", "SwapDropAndHoldRedux.dll")
INI_REL = os.path.join("SKSE", "Plugins", "SwapDropAndHoldRedux.ini")
MENU_DEFAULTS_REL = [
    os.path.join("MCM", "Config", "SwapDropAndHoldRedux", "settings.ini"),
    os.path.join("MCM", "Settings", "SwapDropAndHoldRedux.ini"),
]


def write_fomod_xml(path, root):
    """Write a FOMOD xml the way installers actually accept one.

    NO XML DECLARATION. Vortex validates by handing the file's CONTENT to .NET
    as a string, and a declaration carrying an encoding attribute makes that
    reader throw. It reported an invalid character at line 1 char 39, which is
    the first byte after the declaration. Every working FOMOD on this machine
    begins straight at its root element, and so does this one now.

    No byte-order mark, and CRLF endings, matching those same files.
    """
    body = ET.tostring(root, encoding="unicode")
    body = body.replace(CRLF, LF).replace(LF, CRLF)
    if not body.endswith(CRLF):
        body += CRLF
    with io.open(path, "wb") as f:
        f.write(body.encode("utf-8"))

    # Refuse to leave behind the exact thing that just failed validation.
    raw = io.open(path, "rb").read()
    assert not raw.startswith(BOM), path + " has a byte-order mark"
    assert not raw.lstrip().startswith(b"<?xml"), path + " has an xml declaration"
    ET.parse(path)


SIMPLE_DESC = (
    "Grip a weapon to pick it up and equip it, and it stays in your hand when you let "
    "go. Double-tap grip to drop it: with your other hand holding it, it moves to that "
    "hand; buried in someone, it stays in the wound; otherwise it drops with your "
    "hand's momentum. No trigger chords to learn. The mode can be changed later on the "
    "menu's Weapon mode and controls page or in the ini."
)
GRIPHOLD_DESC = (
    "A weapon stays in your hand only while you hold grip: open your hand and it drops, "
    "moves to your other hand, or stays in a wound. Tap the trigger three times to "
    "switch the mod off and on, five times to blacklist the item in that hand (hold "
    "grip while tapping if that hand is empty). This is how version 2.0 worked."
)
LEGACY_DESC = (
    "The original Swap Drop & Hold controls: tap then hold the trigger to drop a "
    "weapon, and pull a weapon across your body to swap it to the other hand. None of "
    "the grip-held features."
)

# (option name, flag value, WeaponMode number, ini folder, menu settings folder, type, description)
MODES = [
    ("Simple (recommended)", "Simple", 2, "10 Weapon mode - Simple", "20 Menu settings - Simple",
     "Recommended", SIMPLE_DESC),
    ("Grip hold", "GripHold", 1, "11 Weapon mode - Grip hold", "21 Menu settings - Grip hold",
     "Optional", GRIPHOLD_DESC),
    ("Legacy", "Legacy", 0, "12 Weapon mode - Legacy", "22 Menu settings - Legacy",
     "Optional", LEGACY_DESC),
]


def with_weapon_mode(data, pattern, replacement, what):
    """The staged file with its one WeaponMode line set, or refuse to build."""
    text = data.decode("utf-8")
    changed, count = re.subn(pattern, replacement, text, flags=re.M)
    if count != 1:
        print("REFUSING TO BUILD - expected exactly one Simple weapon mode line in %s, found %d"
              % (what, count))
        sys.exit(1)
    return changed.encode("utf-8")


# 3.0.0 test 15: how a weapon you pick up is held, chosen on the same page as the weapon mode. The two switches
# the menu shows on the Simple and Grip Hold pages ("Hold where you grab", "Reverse grip") are set for every
# mode at once - Legacy's ini-only pair too - and the reversed two-handers and "hold anywhere" stay off.
# (option name, flag value, folder label, hold where you grab, reverse grip, type, description)
GRIPS = [
    ("Off", "Off", "Grip off", 0, 0, "Optional",
     "A weapon you pick up is always held the normal way: by its grip, and never reversed. Both switches can "
     "be turned on later on the menu's Simple mode and Grip Hold mode pages (Holding a weapon you pick up), "
     "or in the ini."),
    ("Hold where you grab only", "Where", "Where you grab", 1, 0, "Optional",
     "A weapon is held where your hand took hold of it - by the hilt, half way up, near the tip - but never "
     "reversed. Needs TrueWield VR and Physical Collision VR; without them nothing changes."),
    ("Reverse grip only", "Reverse", "Reverse grip", 0, 1, "Optional",
     "A weapon you take hold of backwards is held reversed - always at its normal grip, wherever you took hold "
     "of it. Needs TrueWield VR and Physical Collision VR; without them nothing changes."),
    ("Both (recommended)", "Both", "Where and reverse", 1, 1, "Recommended",
     "Held where your hand took hold of it, and reversed when you take hold of it backwards. Needs TrueWield "
     "VR and Physical Collision VR; without them nothing changes. Every switch can be changed later in the "
     "menu or the ini."),
]

# The staged files carry both switches ON (the "Both" option); every line is set explicitly all the same.
GRIP_INI_LINES = [
    ("bGrabGripSimple", "position"), ("bGrabGripGripHold", "position"), ("bGrabGrip", "position"),
    ("bGrabGripReversedSimple", "reversed"), ("bGrabGripReversedGripHold", "reversed"),
    ("bGrabGripReversed", "reversed"),
]
GRIP_MENU_LINES = [
    ("bGrabGripSimple", "position"), ("bGrabGripGripHold", "position"),
    ("bGrabGripReversedSimple", "reversed"), ("bGrabGripReversedGripHold", "reversed"),
]


def with_grip(data, lines, separator, position, reversed_, what):
    """The file with each grip switch line set, or refuse to build: each key must appear exactly once, ON."""
    text = data.decode("utf-8")
    for key, which in lines:
        value = position if which == "position" else reversed_
        pattern = r"^%s%s1(?=\r?$)" % (re.escape(key), re.escape(separator))
        text, count = re.subn(pattern, "%s%s%d" % (key, separator, value), text, flags=re.M)
        if count != 1:
            print("REFUSING TO BUILD - expected exactly one '%s%s1' line in %s, found %d"
                  % (key, separator, what, count))
            sys.exit(1)
    return text.encode("utf-8")


def ini_folder_for(mode_folder, grip_label):
    return "%s - %s" % (mode_folder, grip_label)


if os.path.isdir(OUT):
    shutil.rmtree(OUT)

CORE = os.path.join(OUT, "00 Core")
MENU = os.path.join(OUT, "01 MCM Menu")
FOMOD = os.path.join(OUT, "fomod")
for d in (CORE, MENU, FOMOD):
    os.makedirs(d)


def copy(rel, dest_root):
    src = os.path.join(SRC, rel)
    dst = os.path.join(dest_root, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def write_bytes(dest_root, rel, data):
    dst = os.path.join(dest_root, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    io.open(dst, "wb").write(data)


# Core: the mod itself. Its ini comes from the weapon mode step.
copy(DLL_REL, CORE)

ini_data = io.open(os.path.join(SRC, INI_REL), "rb").read()
menu_defaults_data = [io.open(os.path.join(SRC, rel), "rb").read() for rel in MENU_DEFAULTS_REL]
for name, flag, number, ini_folder, menu_folder, kind, desc in MODES:
    mode_ini = with_weapon_mode(ini_data, r"^iWeaponMode = 2(?=\r?$)", "iWeaponMode = %d" % number, INI_REL)
    mode_menus = [with_weapon_mode(data, r"^iWeaponMode=2(?=\r?$)", "iWeaponMode=%d" % number, rel)
                  for rel, data in zip(MENU_DEFAULTS_REL, menu_defaults_data)]
    for _gname, _gflag, glabel, position, reversed_, _gkind, _gdesc in GRIPS:
        write_bytes(os.path.join(OUT, ini_folder_for(ini_folder, glabel)), INI_REL,
                    with_grip(mode_ini, GRIP_INI_LINES, " = ", position, reversed_, INI_REL))
        for rel, data in zip(MENU_DEFAULTS_REL, mode_menus):
            write_bytes(os.path.join(OUT, ini_folder_for(menu_folder, glabel)), rel,
                        with_grip(data, GRIP_MENU_LINES, "=", position, reversed_, rel))

# Menu option: the plugin, its seq and the MCM folders are ONE unit. A
# config.json with no plugin behind it is never looked at - no error, no
# warning, just no menu - so the plugin lives in this option and nowhere else.
# Declining the menu then installs no plugin at all, which is what that choice
# promises. Its two settings files come from the weapon mode's own folder.
MENU_FILES = [
    # The buttons on the Excluded weapons page call a global function in this script,
    # which sends a mod event the dll listens for. It is attached to NOTHING, so if
    # it ever fails to load the buttons simply do nothing and the rest of the menu is
    # untouched - a subclass on the MCM quest, by contrast, can empty every page.
    # It ships with the MENU because without the menu there are no buttons to press.
    r"Scripts\SDHR_Actions.pex",
    r"SwapDropAndHoldRedux.esp",
    r"SEQ\SwapDropAndHoldRedux.seq",
    r"MCM\Config\SwapDropAndHoldRedux\config.json",
]
for rel in MENU_FILES:
    copy(rel, MENU)

missing = [f for f in MENU_FILES if not os.path.isfile(os.path.join(MENU, f))]
if missing:
    print("REFUSING TO BUILD - the menu option is missing:", missing)
    sys.exit(1)

MENU_DESC = (
    "Adds an in-game settings menu (Mod Configuration Menu) with eight pages: Weapon mode "
    "and controls, Simple mode, Grip Hold mode, Legacy controls, Throwing weapons, VRIK "
    "holsters, Stab and Stick, and Excluded weapons. "
    "Requires MCM Helper. The menu starts with the weapon mode and the grip choice you picked "
    "on the previous page. "
    "AFTER INSTALLING YOU MUST TICK SwapDropAndHoldRedux.esp IN YOUR MOD MANAGER'S "
    "PLUGINS LIST. Enabling the mod and enabling its plugin are two different actions, "
    "and if the plugin is not in your load order the menu will not appear at all. The "
    "mod's own log says which of those went wrong if it does. "
    "The plugin contains one start-enabled quest and nothing else - no records, no "
    "edits, no conflicts - and that quest is the only reason a menu can register at "
    "all. It is a normal (non-light) plugin, because a light-flagged one does not "
    "register here, so it does occupy one regular load order slot. "
    "Note that installing or updating this mod RESETS THE MENU to the shipped defaults. "
    "That is deliberate, because MCM Helper stores its settings outside the mod, so "
    "without the reset a value set in an old build would keep winning in a new one "
    "while the menu displayed something else. Note down any customisations first."
)

NOMENU_DESC = (
    "No menu, and no plugin at all, so nothing is added to your load order. "
    "Every setting is still available by editing "
    "Data/SKSE/Plugins/SwapDropAndHoldRedux.ini, which documents each one with a "
    "comment explaining what it does and why the default is what it is, including a "
    "number of advanced settings the menu does not offer. Choose this if you do not use "
    "MCM Helper, or if you would rather keep your settings in a file that survives "
    "updates."
)

config = ET.Element(
    "config",
    {
        "xmlns:xsi": "http://www.w3.org/2001/XMLSchema-instance",
        "xsi:noNamespaceSchemaLocation": "http://qconsulting.ca/fo3/ModConfig5.0.xsd",
    },
)
ET.SubElement(config, "moduleName").text = "Swap Drop & Hold redux"

req = ET.SubElement(config, "requiredInstallFiles")
ET.SubElement(req, "folder", {"source": "00 Core", "destination": "", "priority": "0"})

steps = ET.SubElement(config, "installSteps", {"order": "Explicit"})

# Step 1: the weapon mode. Each option installs its ini and says which it was.
mode_step = ET.SubElement(steps, "installStep", {"name": "Weapon mode"})
mode_groups = ET.SubElement(mode_step, "optionalFileGroups", {"order": "Explicit"})
mode_group = ET.SubElement(
    mode_groups, "group", {"name": "How weapons stay in your hand", "type": "SelectExactlyOne"}
)
mode_plugins = ET.SubElement(mode_group, "plugins", {"order": "Explicit"})
for name, flag, number, ini_folder, menu_folder, kind, desc in MODES:
    plugin = ET.SubElement(mode_plugins, "plugin", {"name": name})
    ET.SubElement(plugin, "description").text = desc
    # No files here: the ini depends on this choice AND the grip choice below, so it is a conditional install.
    ET.SubElement(plugin, "files")
    flags = ET.SubElement(plugin, "conditionFlags")
    ET.SubElement(flags, "flag", {"name": "WeaponMode"}).text = flag
    ET.SubElement(ET.SubElement(plugin, "typeDescriptor"), "type", {"name": kind})

# Same page (test15): how a weapon you pick up is held.
grip_group = ET.SubElement(
    mode_groups, "group", {"name": "Holding a weapon you pick up", "type": "SelectExactlyOne"}
)
grip_plugins = ET.SubElement(grip_group, "plugins", {"order": "Explicit"})
for gname, gflag, glabel, position, reversed_, gkind, gdesc in GRIPS:
    plugin = ET.SubElement(grip_plugins, "plugin", {"name": gname})
    ET.SubElement(plugin, "description").text = gdesc
    ET.SubElement(plugin, "files")
    flags = ET.SubElement(plugin, "conditionFlags")
    ET.SubElement(flags, "flag", {"name": "Grip"}).text = gflag
    ET.SubElement(ET.SubElement(plugin, "typeDescriptor"), "type", {"name": gkind})

# Step 2: the menu.
menu_step = ET.SubElement(steps, "installStep", {"name": "Settings menu"})
menu_groups = ET.SubElement(menu_step, "optionalFileGroups", {"order": "Explicit"})
menu_group = ET.SubElement(
    menu_groups, "group", {"name": "In-game settings menu", "type": "SelectExactlyOne"}
)
menu_plugins = ET.SubElement(menu_group, "plugins", {"order": "Explicit"})

p1 = ET.SubElement(menu_plugins, "plugin", {"name": "Install the MCM menu (recommended)"})
ET.SubElement(p1, "description").text = MENU_DESC
files1 = ET.SubElement(p1, "files")
ET.SubElement(files1, "folder", {"source": "01 MCM Menu", "destination": "", "priority": "1"})
ET.SubElement(ET.SubElement(p1, "conditionFlags"), "flag", {"name": "Menu"}).text = "On"
ET.SubElement(ET.SubElement(p1, "typeDescriptor"), "type", {"name": "Recommended"})

p2 = ET.SubElement(menu_plugins, "plugin", {"name": "No menu - ini only"})
ET.SubElement(p2, "description").text = NOMENU_DESC
ET.SubElement(p2, "files")
ET.SubElement(ET.SubElement(p2, "conditionFlags"), "flag", {"name": "Menu"}).text = "Off"
ET.SubElement(ET.SubElement(p2, "typeDescriptor"), "type", {"name": "Optional"})

# The ini for the chosen mode and grip; and the menu's settings files for the same pair, only with the menu.
conditional = ET.SubElement(config, "conditionalFileInstalls")
patterns = ET.SubElement(conditional, "patterns")
for name, flag, number, ini_folder, menu_folder, kind, desc in MODES:
    for _gname, gflag, glabel, _position, _reversed, _gkind, _gdesc in GRIPS:
        pattern = ET.SubElement(patterns, "pattern")
        dependencies = ET.SubElement(pattern, "dependencies", {"operator": "And"})
        ET.SubElement(dependencies, "flagDependency", {"flag": "WeaponMode", "value": flag})
        ET.SubElement(dependencies, "flagDependency", {"flag": "Grip", "value": gflag})
        files = ET.SubElement(pattern, "files")
        ET.SubElement(files, "folder",
                      {"source": ini_folder_for(ini_folder, glabel), "destination": "", "priority": "0"})
for name, flag, number, ini_folder, menu_folder, kind, desc in MODES:
    for _gname, gflag, glabel, _position, _reversed, _gkind, _gdesc in GRIPS:
        pattern = ET.SubElement(patterns, "pattern")
        dependencies = ET.SubElement(pattern, "dependencies", {"operator": "And"})
        ET.SubElement(dependencies, "flagDependency", {"flag": "Menu", "value": "On"})
        ET.SubElement(dependencies, "flagDependency", {"flag": "WeaponMode", "value": flag})
        ET.SubElement(dependencies, "flagDependency", {"flag": "Grip", "value": gflag})
        files = ET.SubElement(pattern, "files")
        ET.SubElement(files, "folder",
                      {"source": ini_folder_for(menu_folder, glabel), "destination": "", "priority": "2"})

ET.indent(config, space="  ")
write_fomod_xml(os.path.join(FOMOD, "ModuleConfig.xml"), config)

info = ET.Element("fomod")
ET.SubElement(info, "Name").text = "Swap Drop & Hold redux"
ET.SubElement(info, "Author").text = "SamuelJohnBrown"
ET.SubElement(info, "Version").text = VERSION
ET.SubElement(info, "Description").text = (
    "Take weapons with your hand. Simple mode (the default): grip to take a weapon, "
    "double-tap grip to drop it. Grip hold mode: it stays only while you hold it. "
    "Letting go throws with your real swing, a blade left in a body stays there, and "
    "you drag it back out along its own axis. Requires SKSE VR and HIGGS."
)
ET.indent(info, space="  ")
write_fomod_xml(os.path.join(FOMOD, "info.xml"), info)

print("FOMOD staged at", OUT)
for root, _, files in os.walk(OUT):
    for f in sorted(files):
        p = os.path.join(root, f)
        print("  %-58s %d bytes" % (os.path.relpath(p, OUT), os.path.getsize(p)))
