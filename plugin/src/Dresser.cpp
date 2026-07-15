#include "Dresser.h"
#include "PrismaUI_API.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PrismaUI dressing overlay. See Dresser.h for the design.
// ---------------------------------------------------------------------------

namespace {
    PRISMA_UI_API::IVPrismaUI1* g_prisma = nullptr;
    PrismaView g_view = 0;
    std::atomic<bool> g_open{false};

    // The follower the panel is currently dressing, stored as a FormID (never a
    // raw pointer - the actor can unload or die while the panel is open, and a
    // stale pointer would crash). Resolve with ResolveTarget() at every use.
    std::atomic<RE::FormID> g_targetId{0};

    // Overlay open/close key (DirectX scancode). Default F4 (0x3E); the MCM pushes
    // the user's binding here via DYF_Native.SetToggleKey. -1 = unbound (disabled).
    // The plugin resets to this default on every launch, so Papyrus re-pushes the
    // saved MCM value on each game load.
    std::atomic<std::int32_t> g_toggleKey{0x3E};

    // Overlay accent colour as 0xRRGGBB, pushed from the MCM via SetAccentColor.
    // -1 = no custom accent set yet (the view keeps its built-in theme colour).
    std::atomic<std::int32_t> g_accentRgb{-1};

    // Vanilla CurrentFollowerFaction [FACT:0005C84E].
    constexpr RE::FormID kFollowerFactionId = 0x0005C84E;

    // --- helpers -----------------------------------------------------------

    RE::Actor* GetCrosshairActor() {
        auto pick = RE::CrosshairPickData::GetSingleton();
        if (!pick) {
            return nullptr;
        }
        auto ref = pick->target.get();
        if (!ref) {
            return nullptr;
        }
        return ref->As<RE::Actor>();
    }

    // One definition of "is a follower", shared by the crosshair check, the
    // nearby-follower picker and the pick validation - and it matches what the
    // Papyrus controller accepts. Teammate flag alone is NOT enough: follower
    // frameworks (NFF/EFF/AFT) drop it and only restore it a while after a save
    // loads, which left the picker empty right after loading. The vanilla
    // follower faction persists through that window, so accept either.
    bool IsFollower(RE::Actor* a_actor) {
        if (!a_actor || a_actor->IsDead()) {
            return false;
        }
        if (a_actor->IsPlayerTeammate()) {
            return true;
        }
        static auto* followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(kFollowerFactionId);
        return followerFaction && a_actor->IsInFaction(followerFaction);
    }

    // The panel target, re-validated: null once the actor is gone, dead or
    // unloaded (e.g. after a save load), never a dangling reference.
    RE::Actor* ResolveTarget() {
        auto id = g_targetId.load();
        if (!id) {
            return nullptr;
        }
        auto actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
            return nullptr;
        }
        return actor;
    }

    std::string JsonEscape(const char* a_text) {
        std::string out;
        if (!a_text) {
            return out;
        }
        for (const char* p = a_text; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            switch (c) {
                case '\"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

    // Base64 (standard alphabet). C++ -> JS goes through Invoke, which evaluates
    // "dyfRender(<payload>)" as JS SOURCE. Splicing raw JSON in there is fragile:
    // a mod item/enchant name can contain bytes that are valid JSON but break the
    // JS parse (e.g. U+2028/U+2029, stray bytes), and then the whole call silently
    // fails to run. Base64 is pure ASCII with no quote/backslash/newline, so the
    // snippet is always parseable; JS base64-decodes it back to the JSON string.
    std::string Base64Encode(const std::string& a_in) {
        static constexpr char tbl[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((a_in.size() + 2) / 3 * 4);
        std::size_t i = 0;
        auto byte = [&](std::size_t k) { return static_cast<unsigned char>(a_in[k]); };
        while (i + 3 <= a_in.size()) {
            unsigned n = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
            out += tbl[(n >> 18) & 63];
            out += tbl[(n >> 12) & 63];
            out += tbl[(n >> 6) & 63];
            out += tbl[n & 63];
            i += 3;
        }
        if (a_in.size() - i == 1) {
            unsigned n = byte(i) << 16;
            out += tbl[(n >> 18) & 63];
            out += tbl[(n >> 12) & 63];
            out += "==";
        } else if (a_in.size() - i == 2) {
            unsigned n = (byte(i) << 16) | (byte(i + 1) << 8);
            out += tbl[(n >> 18) & 63];
            out += tbl[(n >> 12) & 63];
            out += tbl[(n >> 6) & 63];
            out += '=';
        }
        return out;
    }

    const char* SafeName(RE::TESForm* a_form) {
        const char* n = a_form ? a_form->GetName() : nullptr;
        return (n && n[0]) ? n : "<unnamed>";
    }

    // Actor display name: GetName() is empty on actor references, so use the
    // reference's display name, falling back to the base NPC name.
    const char* SafeActorName(RE::Actor* a_actor) {
        if (!a_actor) {
            return "<unnamed>";
        }
        const char* n = a_actor->GetDisplayFullName();
        if (n && n[0]) {
            return n;
        }
        if (auto base = a_actor->GetActorBase()) {
            n = base->GetName();
            if (n && n[0]) {
                return n;
            }
        }
        return "<unnamed>";
    }

    void PushFollowers();  // defined below

    // Build the "{name, items[]}" wardrobe JSON for one actor's wearable inventory
    // (armor + weapons). "worn" is computed against a_source, so it is meaningful
    // for the follower's own list; the player-items ("Yours") tab ignores it. MUST
    // run on the main thread (reads live inventory).
    std::string BuildInventoryJson(RE::Actor* a_source, const char* a_label) {
        std::string json = "{\"name\":\"";
        json += JsonEscape(a_label);
        json += "\",\"items\":[";
        auto inventory = a_source->GetInventory([](RE::TESBoundObject& o) {
            if (o.IsArmor()) {
                return true;
            }
            // Skyrim's "Unarmed" pseudo-weapon sits in most actors' inventories
            // but is not a wearable item, so keep it out of the wardrobe.
            auto weap = o.As<RE::TESObjectWEAP>();
            return weap && !weap->IsHandToHandMelee();
        });
        bool first = true;
        for (auto& [obj, data] : inventory) {
            auto& [count, entry] = data;
            if (count <= 0 || !obj) {
                continue;
            }
            auto armo = obj->As<RE::TESObjectARMO>();
            auto weap = armo ? nullptr : obj->As<RE::TESObjectWEAP>();
            if (!armo && !weap) {
                continue;
            }
            RE::TESForm* form = armo ? static_cast<RE::TESForm*>(armo)
                                     : static_cast<RE::TESForm*>(weap);

            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "0x%08X", form->GetFormID());

            // kind 0 = armor (biped slot mask, armor rating, light/heavy/cloth),
            // kind 1 = weapon (weapon type 0-9, base damage). "worn" is whether
            // a_source has it equipped - by biped slot for armor, by hand for
            // weapons.
            bool worn;
            char statbuf[128];
            const char* ench;
            if (armo) {
                worn = a_source->GetWornArmor(armo->GetFormID()) != nullptr;
                std::snprintf(statbuf, sizeof(statbuf),
                    ",\"kind\":0,\"slot\":%u,\"armor\":%d,\"atype\":%d,\"weight\":%.1f,\"value\":%d",
                    static_cast<std::uint32_t>(armo->GetSlotMask()),
                    static_cast<int>(armo->GetArmorRating() + 0.5f),
                    static_cast<int>(armo->GetArmorType()),
                    armo->weight, armo->value);
                ench = armo->formEnchanting ? SafeName(armo->formEnchanting) : "";
            } else {
                auto rh = a_source->GetEquippedObject(false);
                auto lh = a_source->GetEquippedObject(true);
                auto wid = weap->GetFormID();
                worn = (rh && rh->GetFormID() == wid) || (lh && lh->GetFormID() == wid);
                std::snprintf(statbuf, sizeof(statbuf),
                    ",\"kind\":1,\"wtype\":%d,\"damage\":%d,\"weight\":%.1f,\"value\":%d",
                    static_cast<int>(weap->GetWeaponType()),
                    static_cast<int>(weap->GetAttackDamage()),
                    weap->weight, weap->value);
                ench = weap->formEnchanting ? SafeName(weap->formEnchanting) : "";
            }

            if (!first) {
                json += ',';
            }
            first = false;
            json += "{\"id\":\"";
            json += idbuf;
            json += "\",\"name\":\"";
            json += JsonEscape(SafeName(form));
            json += '"';  // close the name string; statbuf begins with ",\"kind\"..."
            json += statbuf;
            json += ",\"ench\":\"";
            json += JsonEscape(ench);
            json += "\",\"worn\":";
            json += worn ? "true" : "false";
            json += '}';
        }
        json += "]}";
        return json;
    }

    // Build the JSON payload for the current target and push it to the view.
    // MUST run on the main thread (reads live inventory).
    void PushList() {
        if (!g_prisma || !g_view) {
            return;
        }
        auto actor = ResolveTarget();
        if (!actor) {
            // The target vanished (unloaded, died, save changed) - drop back to
            // the follower picker instead of painting an empty wardrobe.
            g_targetId.store(0);
            PushFollowers();
            return;
        }
        // Call the JS receiver via Invoke (raw JS eval) rather than InteropCall
        // (InteropCall routes through a separate interop registry this view never
        // joins, so it silently no-ops). The JSON is base64-encoded so the JS
        // snippet is always parseable regardless of item/enchant name bytes.
        std::string call = "dyfRender(\"";
        call += Base64Encode(BuildInventoryJson(actor, SafeActorName(actor)));
        call += "\")";
        g_prisma->Invoke(g_view, call.c_str());
    }

    // Push the player's wearable inventory to the overlay's "Yours" tab.
    void PushPlayerList() {
        if (!g_prisma || !g_view) {
            return;
        }
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        std::string call = "dyfPlayerItems(\"";
        call += Base64Encode(BuildInventoryJson(player, "Your items"));
        call += "\")";
        g_prisma->Invoke(g_view, call.c_str());
    }

    void PushPlayerListSoon() {
        SKSE::GetTaskInterface()->AddTask([]() { PushPlayerList(); });
    }

    void PushListSoon() {
        SKSE::GetTaskInterface()->AddTask([]() { PushList(); });
    }

    // Loaded, living teammates near the player - i.e. active followers. The
    // high-process actor list is already the nearby, loaded set, so no extra
    // range test is needed; we sort by distance so the closest are listed first.
    std::vector<RE::Actor*> GetNearbyFollowers() {
        std::vector<RE::Actor*> out;
        auto player = RE::PlayerCharacter::GetSingleton();
        auto lists = RE::ProcessLists::GetSingleton();
        if (!player || !lists) {
            return out;
        }
        auto origin = player->GetPosition();
        for (auto& handle : lists->highActorHandles) {
            auto ptr = handle.get();
            RE::Actor* a = ptr.get();
            if (!a || a == player || !IsFollower(a)) {
                continue;
            }
            out.push_back(a);
        }
        std::sort(out.begin(), out.end(), [&](RE::Actor* x, RE::Actor* y) {
            return origin.GetDistance(x->GetPosition()) < origin.GetDistance(y->GetPosition());
        });
        return out;
    }

    // Push the "choose a follower" list to the view.
    void PushFollowers() {
        if (!g_prisma || !g_view) {
            return;
        }
        auto followers = GetNearbyFollowers();
        std::string json = "{\"followers\":[";
        bool first = true;
        for (auto* a : followers) {
            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "0x%08X", a->GetFormID());
            if (!first) {
                json += ',';
            }
            first = false;
            json += "{\"id\":\"";
            json += idbuf;
            json += "\",\"name\":\"";
            json += JsonEscape(SafeActorName(a));
            json += "\"}";
        }
        json += "]}";
        // Invoke, not InteropCall - see PushList. Base64 so the snippet is safe.
        std::string call = "dyfFollowers(\"";
        call += Base64Encode(json);
        call += "\")";
        g_prisma->Invoke(g_view, call.c_str());
    }

    void PushFollowersSoon() {
        SKSE::GetTaskInterface()->AddTask([]() { PushFollowers(); });
    }

    // Push the configured accent colour to the overlay CSS. No-op until the MCM
    // has supplied one (g_accentRgb < 0), so the view keeps its default theme.
    void PushAccent() {
        if (!g_prisma || !g_view) {
            return;
        }
        std::int32_t rgb = g_accentRgb.load();
        if (rgb < 0) {
            return;
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "dyfAccent(\"#%06X\")", rgb & 0xFFFFFF);
        g_prisma->Invoke(g_view, buf);
    }

    void OpenPanel() {
        // Only in normal gameplay - not at the main menu, in a loading screen or
        // while a pausing menu (journal, console, ...) is up.
        auto ui = RE::UI::GetSingleton();
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!ui || ui->GameIsPaused() || !player || !player->Is3DLoaded()) {
            return;
        }

        g_prisma->Show(g_view);
        g_prisma->Focus(g_view, false, false);  // pauseGame=false: watch them live
        g_open.store(true);
        PushAccent();  // apply the user's accent before the list paints

        // If the crosshair is already on a follower, dress them straight away;
        // otherwise show the nearby-follower picker so the user can choose.
        auto actor = GetCrosshairActor();
        if (IsFollower(actor)) {
            g_targetId.store(actor->GetFormID());
            PushListSoon();
        } else {
            g_targetId.store(0);
            PushFollowersSoon();
        }
    }

    void ClosePanel() {
        g_prisma->Unfocus(g_view);
        g_prisma->Hide(g_view);
        g_open.store(false);
        g_targetId.store(0);
    }

    // --- JS -> C++ listeners (fire on the PrismaUI thread) ------------------

    // argument = "0x0100ABCD|1"  (armor form id | 1=equip 0=unequip)
    void OnJsToggle(const char* a_arg) {
        if (!a_arg) {
            return;
        }
        std::string s(a_arg);
        auto bar = s.find('|');
        if (bar == std::string::npos) {
            return;
        }
        RE::FormID id = static_cast<RE::FormID>(std::strtoul(s.substr(0, bar).c_str(), nullptr, 16));
        bool equip = s.substr(bar + 1) != "0";

        SKSE::GetTaskInterface()->AddTask([id, equip]() {
            auto form = RE::TESForm::LookupByID(id);
            if (!form) {
                return;
            }
            // Armor or weapon - both are TESBoundObject with an equip slot.
            RE::TESBoundObject* bound = nullptr;
            const RE::BGSEquipSlot* slot = nullptr;
            if (auto armo = form->As<RE::TESObjectARMO>()) {
                bound = armo;
                slot = armo->GetEquipSlot();
            } else if (auto weap = form->As<RE::TESObjectWEAP>()) {
                bound = weap;
                slot = weap->GetEquipSlot();
            }
            if (!bound) {
                return;
            }

            // Fast path: apply the equip/unequip right here on the game thread so
            // the follower changes instantly, instead of waiting on the Papyrus VM
            // to schedule the handler. With the empty outfit in place the engine no
            // longer auto-reverts, so this sticks.
            auto actor = ResolveTarget();
            if (actor) {
                if (auto eqm = RE::ActorEquipManager::GetSingleton()) {
                    if (equip) {
                        // force=FALSE so the engine swaps out whatever occupies the
                        // same slot (force-equip would leave the old piece on, so it
                        // kept reading as worn). queue=false, applyNow=true.
                        eqm->EquipObject(actor, bound, nullptr, 1, slot, false, false, false, true);
                    } else {
                        // force=TRUE on unequip = prevent the engine re-equipping it.
                        eqm->UnequipObject(actor, bound, nullptr, 1, slot, false, true, false, true, nullptr);
                    }
                }
            }

            // Slow path: Papyrus still owns the durable state - it edits the saved
            // loadout, runs EnsureManaged on first touch, and keeps the poll alive.
            // It edits the loadout by intent (equip/unequip), so it stays consistent
            // with the fast-path apply above regardless of exact timing.
            if (auto source = SKSE::GetModCallbackEventSource()) {
                SKSE::ModCallbackEvent ev{};
                ev.eventName = "DYF_ToggleItem";
                ev.strArg = equip ? "equip" : "unequip";
                ev.numArg = 0.0f;
                ev.sender = bound;
                source->SendEvent(&ev);
            }
        });
        // No fixed-delay refresh: Papyrus sends "DYF_PanelRefresh" once it has
        // applied the change, and RefreshSink repaints from the real worn state.
    }

    // "Give" (Yours tab): move one copy of the player's item to the follower and
    // equip it. argument = "0xFORMID". Same equip fast-path as OnJsToggle, plus the
    // player->follower inventory move; Papyrus records it in the saved loadout.
    void OnJsGive(const char* a_arg) {
        if (!a_arg) {
            return;
        }
        RE::FormID id = static_cast<RE::FormID>(std::strtoul(a_arg, nullptr, 16));
        SKSE::GetTaskInterface()->AddTask([id]() {
            auto form = RE::TESForm::LookupByID(id);
            if (!form) {
                return;
            }
            RE::TESBoundObject* bound = nullptr;
            const RE::BGSEquipSlot* slot = nullptr;
            if (auto armo = form->As<RE::TESObjectARMO>()) {
                bound = armo;
                slot = armo->GetEquipSlot();
            } else if (auto weap = form->As<RE::TESObjectWEAP>()) {
                bound = weap;
                slot = weap->GetEquipSlot();
            }
            if (!bound) {
                return;
            }
            auto actor = ResolveTarget();
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!actor || !player) {
                return;
            }
            // Move one from the player to the follower, then equip it (force=false
            // so a same-slot piece is swapped out, matching OnJsToggle's equip).
            player->RemoveItem(bound, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, actor);
            if (auto eqm = RE::ActorEquipManager::GetSingleton()) {
                eqm->EquipObject(actor, bound, nullptr, 1, slot, false, false, false, true);
            }
            // Papyrus keeps the durable state: add the piece to the saved loadout.
            if (auto source = SKSE::GetModCallbackEventSource()) {
                SKSE::ModCallbackEvent ev{};
                ev.eventName = "DYF_ToggleItem";
                ev.strArg = "equip";
                ev.numArg = 0.0f;
                ev.sender = bound;
                source->SendEvent(&ev);
            }
            PushListSoon();        // the piece now shows in the follower's tab
            PushPlayerListSoon();  // and its count dropped in the Yours tab
        });
    }

    // "Return" (follower row): unequip an item on the follower and move one copy
    // back to the player. argument = "0xFORMID".
    void OnJsReturn(const char* a_arg) {
        if (!a_arg) {
            return;
        }
        RE::FormID id = static_cast<RE::FormID>(std::strtoul(a_arg, nullptr, 16));
        SKSE::GetTaskInterface()->AddTask([id]() {
            auto form = RE::TESForm::LookupByID(id);
            if (!form) {
                return;
            }
            RE::TESBoundObject* bound = nullptr;
            const RE::BGSEquipSlot* slot = nullptr;
            if (auto armo = form->As<RE::TESObjectARMO>()) {
                bound = armo;
                slot = armo->GetEquipSlot();
            } else if (auto weap = form->As<RE::TESObjectWEAP>()) {
                bound = weap;
                slot = weap->GetEquipSlot();
            }
            if (!bound) {
                return;
            }
            auto actor = ResolveTarget();
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!actor || !player) {
                return;
            }
            if (auto eqm = RE::ActorEquipManager::GetSingleton()) {
                eqm->UnequipObject(actor, bound, nullptr, 1, slot, false, true, false, true, nullptr);
            }
            // Papyrus: drop the piece from the saved loadout (unequip intent).
            if (auto source = SKSE::GetModCallbackEventSource()) {
                SKSE::ModCallbackEvent ev{};
                ev.eventName = "DYF_ToggleItem";
                ev.strArg = "unequip";
                ev.numArg = 0.0f;
                ev.sender = bound;
                source->SendEvent(&ev);
            }
            // Move one copy back to the player (removes it from the follower).
            actor->RemoveItem(bound, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
            PushListSoon();
            PushPlayerListSoon();
        });
    }

    // "Unequip all": strip every piece the follower currently wears. Same
    // fast-path/slow-path split as OnJsToggle - we unequip here on the game thread
    // for an instant result, and Papyrus clears the saved loadout so its re-assert
    // poll keeps them undressed instead of putting the outfit straight back on.
    // Items stay in the follower's inventory; this only takes them off.
    void OnJsUndressAll(const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto actor = ResolveTarget();
            if (!actor) {
                return;
            }
            auto eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) {
                return;
            }
            // GetInventory returns a snapshot, so unequipping while we walk it is
            // safe. Same filter as the wardrobe list, Unarmed included.
            auto inventory = actor->GetInventory([](RE::TESBoundObject& o) {
                if (o.IsArmor()) {
                    return true;
                }
                auto weap = o.As<RE::TESObjectWEAP>();
                return weap && !weap->IsHandToHandMelee();
            });
            for (auto& [obj, data] : inventory) {
                auto& [count, entry] = data;
                if (count <= 0 || !obj) {
                    continue;
                }
                // force=TRUE on unequip = stop the engine re-equipping it, matching
                // OnJsToggle's unequip path.
                if (auto armo = obj->As<RE::TESObjectARMO>()) {
                    if (actor->GetWornArmor(armo->GetFormID())) {
                        eqm->UnequipObject(actor, armo, nullptr, 1, armo->GetEquipSlot(),
                                           false, true, false, true, nullptr);
                    }
                } else if (auto weap = obj->As<RE::TESObjectWEAP>()) {
                    auto rh = actor->GetEquippedObject(false);
                    auto lh = actor->GetEquippedObject(true);
                    auto wid = weap->GetFormID();
                    if ((rh && rh->GetFormID() == wid) || (lh && lh->GetFormID() == wid)) {
                        eqm->UnequipObject(actor, weap, nullptr, 1, weap->GetEquipSlot(),
                                           false, true, false, true, nullptr);
                    }
                }
            }
            // Papyrus owns the durable state: clear the loadout for this follower.
            // The actor is the sender so Papyrus knows who to clear.
            if (auto source = SKSE::GetModCallbackEventSource()) {
                SKSE::ModCallbackEvent ev{};
                ev.eventName = "DYF_UndressAll";
                ev.strArg = "";
                ev.numArg = 0.0f;
                ev.sender = actor;
                source->SendEvent(&ev);
            }
            PushListSoon();
        });
    }

    // Yours tab activated: (re)send the player's wearable inventory.
    void OnJsPlayerList(const char*) {
        SKSE::GetTaskInterface()->AddTask([]() { PushPlayerList(); });
    }

    void OnJsClose(const char*) {
        SKSE::GetTaskInterface()->AddTask([]() { ClosePanel(); });
    }

    // The user picked a follower from the nearby list (argument = "0xFORMID").
    void OnJsPick(const char* a_arg) {
        if (!a_arg) {
            return;
        }
        RE::FormID id = static_cast<RE::FormID>(std::strtoul(a_arg, nullptr, 16));
        SKSE::GetTaskInterface()->AddTask([id]() {
            auto form = RE::TESForm::LookupByID(id);
            auto actor = form ? form->As<RE::Actor>() : nullptr;
            if (IsFollower(actor)) {
                g_targetId.store(actor->GetFormID());
                PushList();  // already on the main thread here
            }
        });
    }

    // Diagnostic bridge: JS calls window.dyf_log("...") to surface errors/breadcrumbs
    // in the SKSE log (we otherwise have no view into JS exceptions).
    void OnJsLog(const char* a_msg) {
        logger::info("JS: {}", a_msg ? a_msg : "");
    }

    // "Change follower" button: drop back to the nearby-follower picker.
    void OnJsShowPicker(const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            g_targetId.store(0);
            PushFollowers();
        });
    }

    // --- input toggle ------------------------------------------------------

    // Papyrus native: DYF_Native.GetPanelTarget()
    RE::Actor* GetPanelTargetImpl(RE::StaticFunctionTag*) {
        return ResolveTarget();
    }

    // Papyrus native: DYF_Native.SetToggleKey(int) - the MCM-bound overlay key
    // (DirectX scancode; -1 = unbound). Pushed on load and on MCM change.
    void SetToggleKeyImpl(RE::StaticFunctionTag*, std::int32_t a_key) {
        g_toggleKey.store(a_key);
    }

    // Papyrus native: DYF_Native.SetAccentColor(int) - overlay accent as 0xRRGGBB.
    void SetAccentColorImpl(RE::StaticFunctionTag*, std::int32_t a_rgb) {
        g_accentRgb.store(a_rgb);
    }

    class KeySink : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static KeySink* GetSingleton() {
            static KeySink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
                                              RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!a_events) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto e = *a_events; e; e = e->next) {
                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }
                auto btn = e->AsButtonEvent();
                if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !btn->IsDown()) {
                    continue;
                }
                auto key = g_toggleKey.load();
                if (key >= 0 && btn->GetIDCode() == static_cast<std::uint32_t>(key) &&
                    g_prisma && g_view) {
                    if (g_open.load()) {
                        ClosePanel();
                    } else {
                        OpenPanel();
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        KeySink() = default;
    };

    // Papyrus -> C++ : "the follower's gear now matches the loadout, repaint".
    // This is what makes the checkboxes authoritative - they are painted from the
    // real worn state AFTER Papyrus settled, never from a guessed delay.
    class RefreshSink : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
    public:
        static RefreshSink* GetSingleton() {
            static RefreshSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event,
                                              RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
            if (a_event && g_open.load() &&
                std::strcmp(a_event->eventName.c_str(), "DYF_PanelRefresh") == 0) {
                PushListSoon();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        RefreshSink() = default;
    };
}

void Dresser::Init() {
    g_prisma = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
    if (!g_prisma) {
        logger::warn("PrismaUI not installed - dressing overlay disabled");
        return;
    }

    g_view = g_prisma->CreateView("DressYourFollowers/index.html", [](PrismaView view) {
        logger::info("Dresser view DOM ready ({})", view);
        // Start hidden; opened with the toggle key while looking at a follower.
        g_prisma->Hide(view);
    });

    g_prisma->RegisterJSListener(g_view, "dyf_toggle", OnJsToggle);
    g_prisma->RegisterJSListener(g_view, "dyf_give", OnJsGive);
    g_prisma->RegisterJSListener(g_view, "dyf_return", OnJsReturn);
    g_prisma->RegisterJSListener(g_view, "dyf_undressall", OnJsUndressAll);
    g_prisma->RegisterJSListener(g_view, "dyf_playerlist", OnJsPlayerList);
    g_prisma->RegisterJSListener(g_view, "dyf_close", OnJsClose);
    g_prisma->RegisterJSListener(g_view, "dyf_pick", OnJsPick);
    g_prisma->RegisterJSListener(g_view, "dyf_showpicker", OnJsShowPicker);
    g_prisma->RegisterJSListener(g_view, "dyf_log", OnJsLog);

    if (auto input = RE::BSInputDeviceManager::GetSingleton()) {
        input->AddEventSink(KeySink::GetSingleton());
    }
    if (auto mod = SKSE::GetModCallbackEventSource()) {
        mod->AddEventSink(RefreshSink::GetSingleton());
    }
    logger::info("Dresser initialised (toggle key F4)");
}

void Dresser::OnGameLoaded() {
    SKSE::GetTaskInterface()->AddTask([]() {
        g_targetId.store(0);
        if (g_prisma && g_view && g_open.load()) {
            ClosePanel();
        }
    });
}

bool Dresser::RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm) {
    a_vm->RegisterFunction("GetPanelTarget", "DYF_Native", GetPanelTargetImpl);
    a_vm->RegisterFunction("SetToggleKey", "DYF_Native", SetToggleKeyImpl);
    a_vm->RegisterFunction("SetAccentColor", "DYF_Native", SetAccentColorImpl);
    logger::info("DYF_Native registered");
    return true;
}
