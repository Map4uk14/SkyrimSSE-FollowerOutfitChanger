# Changelog

Release history for Follower Outfit Changer. Versions 0.1 and 0.2 predate this
file — see the git history for those.

## 0.3 — unreleased

### Added

- **Automatic outfit switching by situation.** Each follower can hold a saved
  outfit for Home (any player house), Travel and Combat, and changes into it by
  itself when the situation changes. Dress them however you like, then press
  "Save this outfit as Home / Travel / Combat" in the overlay; a ✕ next to the
  row forgets that outfit again. Situations with nothing saved never switch.
  Off by default — enable "Automatic outfit switching" in the MCM.
- **Apply button / manual-look hold.** Changing a follower's look by hand
  (ticking items, applying a numbered preset, Undo, Unequip all) pauses
  automatic switching for that follower, so the look you just made survives
  entering a house, combat, and reloading a save. The Apply button under the
  situation rows turns amber while switching is paused; pressing it puts the
  current situation's outfit back on and resumes switching. Saving a situation
  outfit also resumes switching.
- **Side panel** in the overlay holding the saved outfits and the situation
  rows, so the item list keeps its full height.

### Changed

- The three numbered outfit presets are unchanged and stay fully independent of
  the situation outfits — the situation outfits keep their own storage, so
  saving one never overwrites slot 1, 2 or 3.

### Fixed

- Outfit changes could silently stop working after certain save/load sequences,
  because Skyrim can leave stale copies of the mod's script running. Every
  handler now ignores itself if it is not the live instance.
- More diagnostic detail in the SKSE log to make future outfit-not-applying
  reports diagnosable.
