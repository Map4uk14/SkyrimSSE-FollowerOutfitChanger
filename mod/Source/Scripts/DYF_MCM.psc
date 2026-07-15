ScriptName DYF_MCM extends MCM_ConfigBase
{Dress Your Followers - MCM anchor.
 MCM Helper requires a script extending MCM_ConfigBase on a quest to
 register the mod's menu. Page content is declarative, in
 MCM\Config\DressYourFollowers\config.json; the only thing implemented here is
 populating the read-only "Dressed followers" list from the live registry.}

; The dressed-followers menu is a browsable read-only list: its options are the
; managed follower names, refreshed from DYF_Main whenever the menu is opened or
; the Maintenance page is shown. The bound int just holds the dropdown's cursor;
; we reset it to 0 each open so the collapsed row always shows the count summary.
string Property MANAGED_LIST_ID = "iManagedList:Maintenance" AutoReadOnly

DYF_Main Function Main()
    return Game.GetFormFromFile(0xD62, "DressYourFollowers.esp") as DYF_Main
EndFunction

Function RefreshManagedList()
    DYF_Main main = Main()
    if main
        SetMenuOptions(MANAGED_LIST_ID, main.GetManagedNames())
        SetModSettingInt(MANAGED_LIST_ID, 0)
    endif
EndFunction

Event OnConfigOpen()
    RefreshManagedList()
EndEvent

Event OnPageSelect(string a_page)
    RefreshManagedList()
EndEvent

; Push the overlay hotkey / accent colour to the plugin the moment they change,
; so a rebind or colour pick takes effect without needing a reload.
Event OnSettingChange(string a_ID)
    if a_ID == "iOpenKey:General" || a_ID == "iAccent:General"
        DYF_Main main = Main()
        if main
            main.SyncSettings()
        endif
    endif
EndEvent
