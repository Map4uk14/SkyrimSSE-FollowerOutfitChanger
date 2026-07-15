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
