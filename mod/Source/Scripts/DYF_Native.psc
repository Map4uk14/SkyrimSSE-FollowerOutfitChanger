ScriptName DYF_Native Hidden
{Native bridge to the Dress Your Followers SKSE plugin (PrismaUI overlay).}

; The follower the PrismaUI dressing panel currently has open. The plugin sets
; this when the panel opens, so the controller acts on exactly that actor
; instead of guessing from the crosshair while a menu/overlay has focus.
Actor Function GetPanelTarget() global native

; Push the MCM-bound overlay open/close key (DirectX scancode; -1 = unbound) to
; the plugin. The plugin defaults to F4 each launch, so this is re-sent on load.
Function SetToggleKey(int aKeyCode) global native

; Push the MCM overlay accent colour (0xRRGGBB) to the plugin, which forwards it
; to the overlay CSS the next time the panel opens.
Function SetAccentColor(int aRgb) global native

; Mirror a managed follower's saved loadout down to the plugin. The plugin's
; anti-auto-equip hook runs inside the engine's equip path and cannot call into
; the VM, so it needs its own copy: it refuses any armor equip on a managed
; follower unless the piece is in this list. An EMPTY list is meaningful - the
; follower is managed and should wear nothing. Call this after every change to the
; loadout, and for every managed follower on load (the plugin's copy is rebuilt
; from scratch each launch, it never persists).
Function SetLoadout(Actor akActor, Form[] akItems) global native

; Stop managing this follower in the plugin: hands them back to vanilla auto-equip.
; Note this is NOT the same as SetLoadout with an empty array (= wear nothing).
Function ClearLoadout(Actor akActor) global native

; Release everyone (used by the MCM's "Release all followers").
Function ClearAllLoadouts() global native
