========================================================================
 DRESS YOUR FOLLOWERS
 Redress your followers with a hotkey - and it sticks.
 Skyrim Special Edition / Anniversary Edition (all 1.6.x)
========================================================================

WHAT IT DOES
------------
Look at your follower, press a hotkey, and the familiar trade screen opens
- no dialogue. Anything wearable you hand over is put on immediately when
you close the menu, even a plain dress over heavy armor. The outfit then
STICKS: it survives cell changes, fast travel, waiting, saves, and even
dismissal, until you change it the same way.

Inside the trade screen you can also flip through outfits live: highlight
any wearable (theirs or yours) and press the try-on key (Left Shift by
default) to put it on them instantly. Great for cycling between several
outfits without the take-one, give-one dance.

v1.0 handles armor, clothing and jewelry. Weapons and ammo are not
force-equipped (planned for a later version).


REQUIREMENTS
------------
- SKSE64
- SkyUI
- Address Library for SKSE Plugins
- MCM Helper
- PapyrusUtil SE

All of these are already present in the vast majority of modded 1.6.x
setups.


HOW TO USE
----------
1. Open the MCM ("Dress Your Followers") and bind the "Open wardrobe
   hotkey" (it ships unbound). The try-on key defaults to Left Shift.
2. In the world, look at one of your followers and press the hotkey.
3. Give them clothing/armor and close the menu - they put it on.
4. Or, with the menu open, highlight a wearable and tap the try-on key to
   equip it on the spot.


NOTES & KNOWN QUIRKS
--------------------
- FOLLOWER MANAGERS (NFF / EFF / AFT): if you use their outfit-management
  feature on a follower, turn it OFF for that follower before dressing them
  here, or the two systems will fight over the wardrobe.
- After a live try-on, the little "equipped" marker in the menu list may
  lag until you reopen the menu. This is cosmetic - the follower's actual
  outfit is always correct.
- The mod only manages followers you have explicitly dressed. It edits no
  vanilla records, so its load order position does not matter.


UNINSTALLING
------------
Open the MCM -> Maintenance -> "Release all followers" first, then remove
the mod. Managed followers keep whatever they are currently wearing.


CREDITS
-------
Built with SKSE, SkyUI, MCM Helper (Exit-9B) and PapyrusUtil (Ryan-rsm).
