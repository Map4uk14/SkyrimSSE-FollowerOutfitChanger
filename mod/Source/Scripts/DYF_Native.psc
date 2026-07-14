ScriptName DYF_Native Hidden
{Native bridge to the Dress Your Followers SKSE plugin (PrismaUI overlay).}

; The follower the PrismaUI dressing panel currently has open. The plugin sets
; this when the panel opens, so the controller acts on exactly that actor
; instead of guessing from the crosshair while a menu/overlay has focus.
Actor Function GetPanelTarget() global native
