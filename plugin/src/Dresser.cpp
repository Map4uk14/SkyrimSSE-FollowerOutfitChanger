#include "Dresser.h"
#include "PrismaUI_API.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    // --- Loadout mirror (feeds the anti-auto-equip hook) --------------------
    //
    // Papyrus (StorageUtil) owns the durable loadout, but the hook below runs deep
    // inside the engine's equip path and cannot call into the VM. So Papyrus pushes
    // a read-only copy down here via DYF_Native.SetLoadout, and the hook consults
    // only this.
    //
    // Presence of an actor key means "managed by us". An entry with an empty set is
    // meaningful and must be kept: it is a follower we deliberately stripped, so
    // every armor equip on them should be refused.
    //
    // Written from the Papyrus VM thread, read from the game thread -> guarded.
    //
    // LIFETIME: this rides in the SKSE co-save (OnSave/OnLoad below), so it is
    // already populated when a save finishes loading. Papyrus re-pushes it from
    // ResumeManagement as well, but only as a late confirmation - do NOT treat the
    // Papyrus push as the source of truth and clear this on load, or the hook is
    // disarmed for the seconds before Papyrus starts, which is precisely when the
    // engine dresses everyone.
    std::shared_mutex g_loadoutLock;
    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_loadouts;

    // Which outfit-preset slots hold a saved outfit, per follower (bit 0 = slot 1).
    // UI-only data for the overlay's preset buttons: Papyrus owns the preset
    // CONTENTS (StorageUtil) and re-pushes this mask on load and on every save, so
    // it does not ride in the co-save. Guarded by g_loadoutLock like the mirror.
    std::unordered_map<RE::FormID, std::int32_t> g_presetMasks;

    // Because the hook gates OUR equips too (see EquipObjectHook), anything we are
    // about to equip must already be in the mirror or we refuse ourselves. Papyrus
    // pushes the authoritative loadout a moment later; these only cover that gap and
    // are no-ops for an actor we do not manage (the hook lets those through anyway).
    void MirrorAllow(RE::Actor* a_actor, RE::FormID a_item) {
        if (!a_actor) {
            return;
        }
        std::unique_lock lock(g_loadoutLock);
        auto it = g_loadouts.find(a_actor->GetFormID());
        if (it != g_loadouts.end()) {
            it->second.insert(a_item);
        }
    }

    // Drop a piece from the mirror BEFORE unequipping it, so the engine cannot slip
    // it straight back on in the gap before Papyrus pushes the real loadout.
    void MirrorDeny(RE::Actor* a_actor, RE::FormID a_item) {
        if (!a_actor) {
            return;
        }
        std::unique_lock lock(g_loadoutLock);
        auto it = g_loadouts.find(a_actor->GetFormID());
        if (it != g_loadouts.end()) {
            it->second.erase(a_item);
        }
    }

    // Empty the mirror but KEEP the entry: still managed, now wears nothing.
    void MirrorDenyAll(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        std::unique_lock lock(g_loadoutLock);
        auto it = g_loadouts.find(a_actor->GetFormID());
        if (it != g_loadouts.end()) {
            it->second.clear();
        }
    }

    // Does this weapon claim both hands? Same engine enum the overlay draws icons
    // from, and the same rule Papyrus applies to the durable loadout.
    bool IsTwoHandedWeapon(RE::TESObjectWEAP* a_weap) {
        switch (a_weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kTwoHandSword:
            case RE::WEAPON_TYPE::kTwoHandAxe:
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:
                return true;
            default:
                return false;  // sword/dagger/axe/mace/staff take one hand
        }
    }

    // Take off the weapons the newcomer cannot share hands with: a two-hander/bow
    // claims both, a one-hander leaves room for one companion. Papyrus does the same
    // arithmetic for the durable loadout.
    //
    // Best-effort by necessity - the hook does not gate weapons (see EngineMayEquip),
    // so the engine may pick one back up.
    void EvictConflictingWeapons(RE::Actor* a_actor, RE::TESObjectWEAP* a_new) {
        if (!a_actor || !a_new) {
            return;
        }
        const bool newTwoHanded = IsTwoHandedWeapon(a_new);
        std::vector<RE::TESObjectWEAP*> victims;
        {
            std::shared_lock lock(g_loadoutLock);
            auto it = g_loadouts.find(a_actor->GetFormID());
            if (it == g_loadouts.end()) {
                return;  // not ours - leave the hands to the engine
            }
            int handsUsed = 1;  // the newcomer takes one
            for (auto id : it->second) {
                if (id == a_new->GetFormID()) {
                    continue;
                }
                auto* w = RE::TESForm::LookupByID<RE::TESObjectWEAP>(id);
                if (!w) {
                    continue;  // armor: biped slots are arbitrated elsewhere
                }
                if (newTwoHanded || IsTwoHandedWeapon(w)) {
                    victims.push_back(w);
                } else if (handsUsed < 2) {
                    handsUsed += 1;
                } else {
                    victims.push_back(w);
                }
            }
        }
        auto eqm = RE::ActorEquipManager::GetSingleton();
        for (auto* w : victims) {
            if (eqm) {
                eqm->UnequipObject(a_actor, w, nullptr, 1, w->GetEquipSlot(), false, true, false,
                                   true, nullptr);
            }
        }
    }

    // --- Co-save persistence for the mirror --------------------------------
    //
    // Papyrus re-pushes the mirror on load (ResumeManagement -> SyncLoadouts), but
    // Papyrus starts LATE: the engine has already auto-equipped everyone by the time
    // it runs, so the hook waves that first pass through and the follower wears
    // whatever they own until the poll strips it. That is exactly the "wears
    // everything on load, then snaps back" report.
    //
    // So the mirror rides in the SKSE co-save instead. SKSE restores it during the
    // load itself, ahead of the engine dressing actors, and the hook is armed from
    // the first frame. Papyrus's push then just confirms what is already there.
    constexpr std::uint32_t kSerUniqueID = 'DYFM';
    constexpr std::uint32_t kSerLoadoutRecord = 'LOAD';
    constexpr std::uint32_t kSerVersion = 1;

    void OnSave(SKSE::SerializationInterface* a_intf) {
        std::shared_lock lock(g_loadoutLock);
        if (!a_intf->OpenRecord(kSerLoadoutRecord, kSerVersion)) {
            logger::error("Could not open co-save record; loadout mirror not saved");
            return;
        }
        auto actorCount = static_cast<std::uint32_t>(g_loadouts.size());
        a_intf->WriteRecordData(actorCount);
        for (const auto& [actorId, items] : g_loadouts) {
            a_intf->WriteRecordData(actorId);
            auto itemCount = static_cast<std::uint32_t>(items.size());
            a_intf->WriteRecordData(itemCount);
            for (auto id : items) {
                a_intf->WriteRecordData(id);
            }
        }
    }

    void OnLoad(SKSE::SerializationInterface* a_intf) {
        std::unique_lock lock(g_loadoutLock);
        g_loadouts.clear();
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (a_intf->GetNextRecordInfo(type, version, length)) {
            if (type != kSerLoadoutRecord) {
                continue;
            }
            if (version != kSerVersion) {
                // Say so: silently dropping these looks identical in the log to a
                // save that never had any, and the symptom (flicker on load until
                // Papyrus re-pushes) would send someone hunting the hook instead.
                logger::warn("Ignoring loadout record v{} (expected v{}); "
                             "Papyrus will re-push shortly", version, kSerVersion);
                continue;
            }
            std::uint32_t actorCount = 0;
            a_intf->ReadRecordData(actorCount);
            for (std::uint32_t i = 0; i < actorCount; ++i) {
                RE::FormID rawActor = 0;
                a_intf->ReadRecordData(rawActor);
                std::uint32_t itemCount = 0;
                a_intf->ReadRecordData(itemCount);
                std::unordered_set<RE::FormID> items;
                for (std::uint32_t j = 0; j < itemCount; ++j) {
                    RE::FormID rawItem = 0;
                    a_intf->ReadRecordData(rawItem);
                    RE::FormID item = 0;
                    // Always resolve through the save's load order: a plugin that
                    // moved would otherwise leave us holding a stranger's FormID.
                    if (a_intf->ResolveFormID(rawItem, item)) {
                        items.insert(item);
                    } else {
                        // Same reasoning as the actor case: a dropped piece means
                        // the hook will refuse gear the follower should be wearing.
                        logger::warn("Loadout mirror: item {:08X} did not resolve, dropped",
                                     rawItem);
                    }
                }
                RE::FormID actor = 0;
                if (a_intf->ResolveFormID(rawActor, actor)) {
                    g_loadouts[actor] = std::move(items);
                } else {
                    // Worth keeping loud: a follower silently missing from the
                    // mirror is invisible in game until they refuse to stay dressed.
                    logger::warn("Loadout mirror: actor {:08X} did not resolve, {} item(s) dropped",
                                 rawActor, itemCount);
                }
            }
        }
        logger::info("Restored {} loadout mirror(s) from the co-save", g_loadouts.size());
    }

    // SKSE calls this before loading a save and on a new game.
    void OnRevert(SKSE::SerializationInterface*) {
        std::unique_lock lock(g_loadoutLock);
        g_loadouts.clear();
        g_presetMasks.clear();  // Papyrus re-pushes these after the load
    }

    // Should the engine be allowed to put this armor on this actor? Refuse only for
    // a follower we manage, and only for armor outside their saved loadout.
    //
    // ARMOR ONLY - do NOT extend to weapons. Refusing a weapon here freezes the game
    // on load: the engine's arm-the-NPC routine will not accept no and keeps asking.
    // Proven three ways (see HANDOFF.md); the mature Outfit System NG gates
    // IsArmor() only for the same reason. Cost: the engine still auto-equips a
    // follower's best weapon, and that stays.
    //
    // Fails open - unknown actor/form, not ours, nothing pushed yet -> vanilla. The
    // worst case for a bug here is the old flicker, never an undressable follower.
    bool EngineMayEquip(RE::Actor* a_actor, RE::TESBoundObject* a_object) {
        if (!a_actor || !a_object || !a_object->IsArmor()) {
            return true;
        }
        if (a_actor == RE::PlayerCharacter::GetSingleton()) {
            return true;  // we only ever manage followers
        }
        std::shared_lock lock(g_loadoutLock);
        auto it = g_loadouts.find(a_actor->GetFormID());
        if (it == g_loadouts.end()) {
            return true;  // not ours
        }
        return it->second.contains(a_object->GetFormID());
    }

    // --- Immediate re-dress ------------------------------------------------
    //
    // A managed follower arrives from a save load wearing NOTHING: EnsureManaged
    // gave them an empty outfit (deliberately - it is what stops the engine using
    // their default gear), so the game dresses them from that empty outfit. The
    // engine then tries to fill the gap with their "best" owned armor, which we
    // refuse. Nothing else puts their real outfit on until Papyrus's poll, which
    // measured ~10s after the co-save had already told us exactly what they should
    // wear. That gap IS the naked flash.
    //
    // So dress them here instead of waiting for Papyrus. A refusal is the perfect
    // trigger: it only happens while the actor is loaded and being processed.
    //
    // This is not "fighting the engine frame-by-frame" (the known dead end, which
    // was re-UNequipping in response to engine equips and oscillated forever): our
    // equips are allowed and the engine's are refused, so the state converges. A
    // second pass finds everything already worn and does nothing.
    std::mutex g_pendingLock;
    std::unordered_set<RE::FormID> g_pendingReassert;

    void ReassertFromMirror(RE::FormID a_actorId) {
        {
            std::scoped_lock lock(g_pendingLock);
            g_pendingReassert.erase(a_actorId);
        }
        auto actor = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
        if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
            return;
        }
        std::vector<RE::FormID> want;
        {
            std::shared_lock lock(g_loadoutLock);
            auto it = g_loadouts.find(a_actorId);
            if (it == g_loadouts.end() || it->second.empty()) {
                return;  // released, or deliberately stripped - nothing to put on
            }
            want.assign(it->second.begin(), it->second.end());
        }
        auto eqm = RE::ActorEquipManager::GetSingleton();
        if (!eqm) {
            return;
        }
        // One inventory snapshot for the whole pass; there is no per-item count API.
        auto carried = actor->GetInventoryCounts([](RE::TESBoundObject& o) { return o.IsArmor(); });
        for (auto id : want) {
            auto* armo = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
            if (!armo) {
                continue;  // weapons stay Papyrus's job (hand-slot arbitration)
            }
            if (actor->GetWornArmor(armo->GetFormID())) {
                continue;  // already on
            }
            auto found = carried.find(armo);
            if (found == carried.end() || found->second <= 0) {
                continue;  // they don't have it
            }
            // force=false to match the toggle's equip: same-slot pieces swap out.
            eqm->EquipObject(actor, armo, nullptr, 1, armo->GetEquipSlot(), false, false, false, true);
        }
    }

    // Debounced: a burst of refusals (19 on one load) queues exactly one pass.
    void ScheduleReassert(RE::FormID a_actorId) {
        {
            std::scoped_lock lock(g_pendingLock);
            if (!g_pendingReassert.insert(a_actorId).second) {
                return;  // already queued
            }
        }
        // Next frame, not here: this runs inside the engine's own equip call.
        SKSE::GetTaskInterface()->AddTask([a_actorId]() { ReassertFromMirror(a_actorId); });
    }

    // THE anti-auto-equip hook, and the only hook in this plugin.
    //
    // Why it exists: the engine auto-equips an NPC's "best" owned gear whenever
    // their inventory changes, so anything reactive (strip it back afterwards) can
    // only ever shrink the wrong-outfit flash, never remove it - and reacting to
    // TESEquipEvent instead produced an unbounded equip/unequip loop. This refuses
    // the equip at source, so there is nothing to undo. It covers armor AND weapons
    // (handing over a better sword otherwise made them draw it on the spot), with
    // the hands left alone until the loadout names a weapon - see EngineMayEquip.
    //
    // IMPORTANT, learned the hard way: RELOCATION_ID(37938, 38894) IS
    // ActorEquipManager::EquipObject (see Offset::ActorEquipManager::EquipObject in
    // CommonLibSSE), and we patch a call site INSIDE it. So EVERY equip flows
    // through here - the engine's auto-equip, our own overlay fast path, and
    // Papyrus EquipItemEx alike. This is NOT an engine-only interception.
    //
    // That is why the check is loadout membership rather than "is this actor
    // managed": a blanket block would make a managed follower impossible to dress.
    // It also means our own equips must be in the mirror BEFORE we call EquipObject,
    // or we refuse ourselves - see MirrorAllow at the toggle site. (Symptom of
    // getting this wrong: clicking an item does nothing until the 3s poll fires.)
    //
    // Address + offset are the ones the mature Skyrim Outfit Equipment System NG
    // uses for the same interception; RELOCATION_ID keeps them valid across SE/AE
    // via Address Library.
    struct EquipObjectHook {
        static void thunk(RE::ActorEquipManager* a_manager, RE::Actor* a_actor,
                          RE::TESBoundObject* a_object, RE::ExtraDataList* a_list) {
            if (!EngineMayEquip(a_actor, a_object)) {
                // Refusing leaves a hole: on a load the follower is naked (empty
                // outfit) and the engine's pick was their only candidate. Put their
                // real outfit on next frame rather than waiting ~10s for Papyrus.
                //
                ScheduleReassert(a_actor->GetFormID());
                return;  // swallow it: the engine never equips, so nothing flickers
            }
            func(a_manager, a_actor, a_object, a_list);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Verified present in this install's Address Library: id 38894 -> 0x6C9820 on
    // 1.6.1170.
    //
    // Be clear about the failure mode: if a future runtime has no entry for the id,
    // CommonLibSSE's report_and_fail shows a message box and TERMINATES - it is
    // [[noreturn]] and does not throw, so the catch below cannot rescue that. That
    // is the intended, loud failure (the game refuses to start; nothing is
    // corrupted, and removing the DLL or updating Address Library fixes it). The
    // catch only covers failures we can genuinely degrade from, such as trampoline
    // allocation, where running poll-only beats taking the game down.
    void InstallEquipHook() {
        try {
            SKSE::AllocTrampoline(14);
            REL::Relocation<std::uintptr_t> target{RELOCATION_ID(37938, 38894),
                                                   REL::Relocate(0xe5, 0x170)};
            EquipObjectHook::func =
                SKSE::GetTrampoline().write_call<5>(target.address(), EquipObjectHook::thunk);
            logger::info("Installed anti-auto-equip hook at {:X}", target.address());
        } catch (const std::exception& e) {
            logger::error(
                "Anti-auto-equip hook NOT installed ({}). "
                "Running poll-only (outfit flicker will return).", e.what());
        } catch (...) {
            logger::error(
                "Anti-auto-equip hook NOT installed (unknown error). "
                "Running poll-only (outfit flicker will return).");
        }
    }

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

    // Build the "{name, presets?, items[]}" wardrobe JSON for one actor's wearable
    // inventory (armor + weapons). "worn" is computed against a_source, so it is
    // meaningful for the follower's own list; the player-items ("Yours") tab
    // ignores it. a_presetMask >= 0 adds the occupied-preset-slots bitmask; the
    // player list passes -1 (presets are follower-scoped). MUST run on the main
    // thread (reads live inventory).
    std::string BuildInventoryJson(RE::Actor* a_source, const char* a_label,
                                   std::int32_t a_presetMask = -1) {
        std::string json = "{\"name\":\"";
        json += JsonEscape(a_label);
        json += '"';  // close the name string before optional fields
        if (a_presetMask >= 0) {
            json += ",\"presets\":";
            json += std::to_string(a_presetMask);
        }
        json += ",\"items\":[";
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
        // Occupied preset slots for this follower (0 if none pushed yet).
        std::int32_t presetMask = 0;
        {
            std::shared_lock lock(g_loadoutLock);
            auto it = g_presetMasks.find(actor->GetFormID());
            if (it != g_presetMasks.end()) {
                presetMask = it->second;
            }
        }
        // Call the JS receiver via Invoke (raw JS eval) rather than InteropCall
        // (InteropCall routes through a separate interop registry this view never
        // joins, so it silently no-ops). The JSON is base64-encoded so the JS
        // snippet is always parseable regardless of item/enchant name bytes.
        std::string call = "dyfRender(\"";
        call += Base64Encode(BuildInventoryJson(actor, SafeActorName(actor), presetMask));
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

        // Refresh the Yours tab on every open. The view is a page that outlives the
        // panel, so it keeps whatever it was last sent: reopening while Yours was
        // the active tab showed an inventory from before anything was picked up.
        // The tab only re-requests on a SWITCH, and switching to the tab you are
        // already on is not a switch.
        PushPlayerListSoon();

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
                        // Clear the hands before filling them: a weapon the newcomer
                        // cannot share with has to be denied and taken off first, or
                        // the engine re-equips it out of the still-permissive mirror.
                        if (auto weap = bound->As<RE::TESObjectWEAP>()) {
                            EvictConflictingWeapons(actor, weap);
                        }
                        // Authorise it in the mirror FIRST: our own EquipObject call
                        // runs through the anti-auto-equip hook, which would refuse a
                        // piece that is not in the loadout yet (Papyrus only adds it
                        // once the event below lands).
                        MirrorAllow(actor, bound->GetFormID());
                        // force=FALSE so the engine swaps out whatever occupies the
                        // same slot (force-equip would leave the old piece on, so it
                        // kept reading as worn). queue=false, applyNow=true.
                        eqm->EquipObject(actor, bound, nullptr, 1, slot, false, false, false, true);
                    } else {
                        // Deny first so the hook refuses any engine attempt to put it
                        // back before Papyrus pushes the updated loadout.
                        MirrorDeny(actor, bound->GetFormID());
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

    // "Give" (Yours tab): move one copy of the player's item into the follower's
    // inventory. argument = "0xFORMID".
    //
    // It is deliberately NOT equipped and NOT added to the saved loadout: handing
    // gear over and deciding what they wear are separate steps, so you can pass a
    // pile of items now and tick the ones you want later. Ticking it in the
    // Follower tab is what puts it on and routes it into the loadout. For a managed
    // follower the loadout is the source of truth, so if the engine auto-equips the
    // new item the re-assert poll takes it straight back off.
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
            if (auto armo = form->As<RE::TESObjectARMO>()) {
                bound = armo;
            } else if (auto weap = form->As<RE::TESObjectWEAP>()) {
                bound = weap;
            }
            if (!bound) {
                return;
            }
            auto actor = ResolveTarget();
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!actor || !player) {
                return;
            }
            player->RemoveItem(bound, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, actor);
            PushListSoon();        // the piece now shows in the follower's tab, unticked
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
            MirrorDeny(actor, bound->GetFormID());
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
            // R toggles: strip, press again to put the outfit back, again to strip.
            // "Already stripped" = mirror entry present but EMPTY (that is what
            // MirrorDenyAll/an empty loadout push leave behind), and a snapshot is
            // waiting (bit 3 of the preset mask). In that state forward to Papyrus's
            // undo instead of stripping a second time.
            bool stripped = false;
            bool hasUndo = false;
            {
                std::shared_lock lock(g_loadoutLock);
                auto it = g_loadouts.find(actor->GetFormID());
                stripped = it != g_loadouts.end() && it->second.empty();
                auto pm = g_presetMasks.find(actor->GetFormID());
                hasUndo = pm != g_presetMasks.end() && (pm->second & 8) != 0;
            }
            if (stripped && hasUndo) {
                if (auto source = SKSE::GetModCallbackEventSource()) {
                    SKSE::ModCallbackEvent ev{};
                    ev.eventName = "DYF_Undo";
                    ev.strArg = "";
                    ev.numArg = 0.0f;
                    ev.sender = nullptr;
                    source->SendEvent(&ev);
                }
                return;
            }
            auto eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) {
                return;
            }
            // Empty the mirror before stripping: while we walk the inventory the
            // engine would otherwise be free to re-equip pieces we have just taken
            // off. With an empty loadout the hook refuses every one of them.
            MirrorDenyAll(actor);
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

    // Outfit preset slot ("1".."3") from the overlay. No fast path here: Papyrus
    // owns the preset contents (StorageUtil) and the apply IS a bulk re-dress -
    // exactly what its ReassertOutfit already does - so both just forward the slot
    // as a mod event and let the panel repaint via DYF_PanelRefresh as usual.
    void SendPresetEvent(const char* a_event, const char* a_slot) {
        if (!a_slot || a_slot[0] < '1' || a_slot[0] > '3' || a_slot[1] != '\0') {
            return;
        }
        std::string name(a_event);
        std::string slot(a_slot);
        SKSE::GetTaskInterface()->AddTask([name, slot]() {
            if (auto source = SKSE::GetModCallbackEventSource()) {
                SKSE::ModCallbackEvent ev{};
                ev.eventName = name.c_str();
                ev.strArg = slot.c_str();
                ev.numArg = 0.0f;
                ev.sender = nullptr;
                source->SendEvent(&ev);
            }
        });
    }
    void OnJsPresetSave(const char* a_arg) { SendPresetEvent("DYF_PresetSave", a_arg); }
    void OnJsPresetApply(const char* a_arg) { SendPresetEvent("DYF_PresetApply", a_arg); }

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

    // Papyrus native: DYF_Native.SetLoadout(Actor, Form[]) - mirror one managed
    // follower's saved loadout down to the plugin so the anti-auto-equip hook can
    // consult it. Papyrus calls this whenever the loadout changes, and for every
    // managed follower on load as a late confirmation of what the co-save already
    // restored. An empty array is meaningful: managed, and should wear nothing.
    //
    // Known, accepted race: this replaces the whole set, so if you click two items
    // fast, the VM-thread push for the first can briefly drop the second's optimistic
    // MirrorAllow. It converges on the next push, the poll reads StorageUtil rather
    // than this mirror, and the worst case is one refused equip - not worth sequence
    // numbers to close.
    void SetLoadoutImpl(RE::StaticFunctionTag*, RE::Actor* a_actor,
                        std::vector<RE::TESForm*> a_items) {
        if (!a_actor) {
            return;
        }
        std::unordered_set<RE::FormID> ids;
        for (auto* form : a_items) {
            if (form) {
                ids.insert(form->GetFormID());
            }
        }
        std::unique_lock lock(g_loadoutLock);
        g_loadouts[a_actor->GetFormID()] = std::move(ids);
    }

    // Papyrus native: DYF_Native.SyncWornArmor(Actor) - make the worn ARMOR match
    // the mirror right now, on the game thread: strip worn armor outside the
    // loadout, then equip loadout armor they carry (ReassertFromMirror). Weapons
    // stay Papyrus's job (hand-slot arbitration). Papyrus calls this after a bulk
    // loadout change (preset apply / undo) so the outfit swaps instantly and the
    // panel repaint that follows reads the FINISHED state - EquipItemEx from the
    // VM lands too late and the overlay repainted a half-applied outfit.
    void SyncWornArmorImpl(RE::StaticFunctionTag*, RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        RE::FormID id = a_actor->GetFormID();
        SKSE::GetTaskInterface()->AddTask([id]() {
            auto actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
                return;
            }
            std::unordered_set<RE::FormID> want;
            {
                std::shared_lock lock(g_loadoutLock);
                auto it = g_loadouts.find(id);
                if (it == g_loadouts.end()) {
                    return;  // not managed - nothing to sync against
                }
                want = it->second;
            }
            auto eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) {
                return;
            }
            // Strip pass: worn armor that is not in the (already-updated) mirror.
            // force=TRUE on unequip, same as the undress-all path.
            int stripped = 0;
            auto inventory = actor->GetInventory([](RE::TESBoundObject& o) { return o.IsArmor(); });
            for (auto& [obj, data] : inventory) {
                auto armo = obj ? obj->As<RE::TESObjectARMO>() : nullptr;
                if (!armo || want.contains(armo->GetFormID())) {
                    continue;
                }
                if (actor->GetWornArmor(armo->GetFormID())) {
                    eqm->UnequipObject(actor, armo, nullptr, 1, armo->GetEquipSlot(),
                                       false, true, false, true, nullptr);
                    ++stripped;
                }
            }
            // Equip pass: loadout armor they carry (skips what is already on).
            ReassertFromMirror(id);
            logger::info("SyncWornArmor: mirror={} stripped={}", want.size(), stripped);
            // Repaint from right here, like the undress-all fast path does: the
            // armor state is final on this thread, and the panel must not depend
            // on the rest of the Papyrus handler surviving to SendPanelRefresh.
            PushList();
        });
    }

    // Papyrus native: DYF_Native.SetPresets(Actor, int) - which preset slots hold
    // a saved outfit (bit 0 = slot 1), for the overlay's preset buttons. Pushed on
    // load and after every save; the contents themselves stay in StorageUtil.
    void SetPresetsImpl(RE::StaticFunctionTag*, RE::Actor* a_actor, std::int32_t a_mask) {
        if (!a_actor) {
            return;
        }
        std::unique_lock lock(g_loadoutLock);
        g_presetMasks[a_actor->GetFormID()] = a_mask;
    }

    // Papyrus native: DYF_Native.ClearAllLoadouts() - release everyone.
    void ClearAllLoadoutsImpl(RE::StaticFunctionTag*) {
        std::unique_lock lock(g_loadoutLock);
        g_loadouts.clear();
        g_presetMasks.clear();
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
    g_prisma->RegisterJSListener(g_view, "dyf_presetsave", OnJsPresetSave);
    g_prisma->RegisterJSListener(g_view, "dyf_presetapply", OnJsPresetApply);
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

void Dresser::InstallHooks() {
    InstallEquipHook();
}

void Dresser::InitSerialization() {
    auto* ser = SKSE::GetSerializationInterface();
    if (!ser) {
        logger::error("No serialization interface; loadout mirror will not persist");
        return;
    }
    ser->SetUniqueID(kSerUniqueID);
    ser->SetSaveCallback(OnSave);
    ser->SetLoadCallback(OnLoad);
    ser->SetRevertCallback(OnRevert);
}

void Dresser::OnGameLoaded() {
    // NOTE: do NOT clear the mirror here. This runs at kPostLoadGame, i.e. AFTER
    // SKSE has already restored it from the co-save, so clearing would throw away
    // the very data that arms the hook before the engine dresses anyone. Stale
    // entries are handled by OnRevert, which SKSE calls before each load.
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
    a_vm->RegisterFunction("SetLoadout", "DYF_Native", SetLoadoutImpl);
    a_vm->RegisterFunction("SetPresets", "DYF_Native", SetPresetsImpl);
    a_vm->RegisterFunction("SyncWornArmor", "DYF_Native", SyncWornArmorImpl);
    a_vm->RegisterFunction("ClearAllLoadouts", "DYF_Native", ClearAllLoadoutsImpl);
    logger::info("DYF_Native registered");
    return true;
}
