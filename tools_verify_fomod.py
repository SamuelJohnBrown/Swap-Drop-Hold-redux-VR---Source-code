"""Verify the staged FOMOD by walking every way through the installer.

For each combination of choices it works out exactly which files land in Data, then
checks what a broken installer gets wrong: the mod or its ini missing, two options
writing the same file, menu folders without the plugin and seq behind them (the
silent failure the MCM guide names), and - since 2.1.0 - a menu whose settings files
name a different weapon mode than the ini. The menu's copy wins, so that player
would silently get a mode they did not pick.

    python tools_verify_fomod.py        exit 0 = every combination is sound
"""
import itertools
import os
import re
import sys
import xml.etree.ElementTree as ET

OUT = r"E:\SwapDropAndHoldRedux\fomod_staging"
SEP = chr(92)
MODE_NUMBER = {"Simple": 2, "GripHold": 1, "Legacy": 0}
DLL = "SKSE/Plugins/SwapDropAndHoldRedux.dll"
INI = "SKSE/Plugins/SwapDropAndHoldRedux.ini"
MENU_SETTINGS = [
    "MCM/Config/SwapDropAndHoldRedux/settings.ini",
    "MCM/Settings/SwapDropAndHoldRedux.ini",
]

cfg = ET.parse(os.path.join(OUT, "fomod", "ModuleConfig.xml")).getroot()
failures = []


def fail(message):
    failures.append(message)
    print("FAIL: " + message)


def files_of(folder):
    found = {}
    base = os.path.join(OUT, folder)
    if not os.path.isdir(base):
        fail("folder '%s' is referenced but does not exist" % folder)
        return found
    for root, _, names in os.walk(base):
        for name in names:
            path = os.path.join(root, name)
            found[os.path.relpath(path, base).replace(SEP, "/")] = path
    return found


def folders(node):
    if node is None:
        return []
    return [(f.get("source"), (f.get("destination") or "").replace(SEP, "/").strip("/"))
            for f in node.findall("folder")]


def weapon_mode(path):
    text = open(path, encoding="utf-8-sig").read()
    found = re.findall(r"^iWeaponMode\s*=\s*(\d+)", text, flags=re.M)
    return int(found[0]) if len(found) == 1 else None


groups = []
for step in cfg.find("installSteps").findall("installStep"):
    for group in step.find("optionalFileGroups").findall("group"):
        options = []
        for plugin in group.find("plugins").findall("plugin"):
            flags = {}
            flag_list = plugin.find("conditionFlags")
            if flag_list is not None:
                for flag in flag_list.findall("flag"):
                    flags[flag.get("name")] = (flag.text or "").strip()
            kind = plugin.find("typeDescriptor/type")
            options.append({
                "name": plugin.get("name"),
                "folders": folders(plugin.find("files")),
                "flags": flags,
                "kind": kind.get("name") if kind is not None else "",
            })
        groups.append({"step": step.get("name"), "name": group.get("name"),
                       "type": group.get("type"), "options": options})

patterns = []
conditional = cfg.find("conditionalFileInstalls")
if conditional is not None:
    for pattern in conditional.find("patterns").findall("pattern"):
        dependencies = pattern.find("dependencies")
        conditions = [(d.get("flag"), d.get("value")) for d in dependencies.findall("flagDependency")]
        patterns.append((dependencies.get("operator", "And"), conditions, folders(pattern.find("files"))))

required = folders(cfg.find("requiredInstallFiles"))

print("STEPS:")
for group in groups:
    preselected = [o["name"] for o in group["options"] if o["kind"] == "Recommended"]
    print("  %s / %s (%s): %s | preselected: %s" % (
        group["step"], group["name"], group["type"],
        ", ".join(o["name"] for o in group["options"]), ", ".join(preselected) or "none"))
    if group["type"] != "SelectExactlyOne":
        fail("group '%s' is %s, expected SelectExactlyOne" % (group["name"], group["type"]))
    if len(preselected) != 1:
        fail("group '%s' preselects %d options, expected exactly 1" % (group["name"], len(preselected)))

mode_groups = [g for g in groups if any("WeaponMode" in o["flags"] for o in g["options"])]
if len(mode_groups) != 1:
    fail("expected exactly one weapon mode group, found %d" % len(mode_groups))
else:
    preselected = [o for o in mode_groups[0]["options"] if o["kind"] == "Recommended"]
    if not preselected or preselected[0]["flags"].get("WeaponMode") != "Simple":
        fail("the preselected weapon mode is not Simple")

print("")
print("EVERY WAY THROUGH THE INSTALLER:")
for combo in itertools.product(*[g["options"] for g in groups]):
    label = " + ".join(o["name"] for o in combo)
    flags = {}
    chosen = list(required)
    for option in combo:
        flags.update(option["flags"])
        chosen += option["folders"]
    for operator, conditions, pattern_folders in patterns:
        matches = [flags.get(flag) == value for flag, value in conditions]
        if all(matches) if operator == "And" else any(matches):
            chosen += pattern_folders

    installed = {}
    for source, destination in chosen:
        for rel, path in files_of(source).items():
            target = destination + "/" + rel if destination else rel
            if target in installed and installed[target] != path:
                fail("%s: two folders both install %s" % (label, target))
            installed[target] = path

    if DLL not in installed:
        fail("%s: the plugin DLL is not installed" % label)
    if INI not in installed:
        fail("%s: the ini is not installed" % label)
        continue

    ini_mode = weapon_mode(installed[INI])
    expected = MODE_NUMBER.get(flags.get("WeaponMode"))
    if ini_mode is None or ini_mode != expected:
        fail("%s: the ini says WeaponMode %s, the choice means %s" % (label, ini_mode, expected))

    has_mcm = any(t.startswith("MCM/") for t in installed)
    has_esp = any(t.endswith(".esp") for t in installed)
    has_seq = any(t.endswith(".seq") for t in installed)
    if has_mcm and not (has_esp and has_seq):
        fail("%s: menu folders without the plugin and seq" % label)
    if (has_esp or has_seq) and not has_mcm:
        fail("%s: the plugin without its menu" % label)

    menu_modes = []
    if has_mcm:
        for rel in MENU_SETTINGS:
            if rel not in installed:
                fail("%s: the menu is installed without %s" % (label, rel))
                continue
            menu_modes.append(weapon_mode(installed[rel]))
        if any(mode != ini_mode for mode in menu_modes):
            fail("%s: the menu's settings say WeaponMode %s but the ini says %s" % (label, menu_modes, ini_mode))

    print("  %-52s ini WeaponMode %s, %s, %d files" % (
        label, ini_mode, "menu WeaponMode %s" % menu_modes if has_mcm else "no menu", len(installed)))

print("")
if failures:
    print("%d problem(s) found" % len(failures))
    sys.exit(1)
print("CHECK: every combination installs the mod and one ini in the chosen mode, and any menu agrees with it - OK")
