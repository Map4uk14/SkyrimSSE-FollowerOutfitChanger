# PrismaUI API notes (read before touching C++ ↔ JS)

The authoritative contract is `plugin/include/PrismaUI_API.h` (the vendored interface)
and the official example plugin: <https://github.com/PrismaUI-SKSE/example-skse-plugin>
(`src/main.cpp` + `view/index.html`). When something in the overlay "doesn't render"
or a call "does nothing", re-check those two sources FIRST — the API has sharp edges
that fail silently (no crash, no log).

## C++ → JS: use `Invoke`, NOT `InteropCall`

- **`Invoke(view, "funcName(<args>)")`** — evaluates a raw JS snippet in the view.
  This is what the official example uses for *every* C++→JS call, and what this
  project uses. The target is a plain `window.funcName` global defined in the HTML.
- **`InteropCall(view, "funcName", argument)`** — routes through PrismaUI's separate
  JS-interop registry. Our views do NOT register into that registry, so InteropCall
  **silently no-ops** — the payload never reaches the page. This cost us a long
  debugging session (2026-07-14): the panel chrome (static HTML) rendered but every
  C++-pushed update — follower list, empty-state, wardrobe — was invisible.
- `Invoke` evaluates its argument as **JS source**, so anything you splice in must be
  valid JS *source*, not just valid JSON. Embedding a raw JSON payload
  (`Invoke(view, "dyfRender(" + json + ")")`) works for small/clean data but fails
  SILENTLY when a value contains bytes that are legal JSON yet illegal in a JS literal
  — e.g. U+2028/U+2029 or stray bytes in mod item/enchant names. The whole snippet
  then fails to parse and the function never runs (2026-07-14: the follower picker
  rendered but clicking a follower never showed the wardrobe for exactly this reason).
- FIX / rule: **base64-encode the payload in C++ and decode it in JS.** Base64 is pure
  ASCII with no quotes/backslashes/newlines, so `Invoke(view, "dyfRender(\"" + b64 +
  "\")")` is always parseable. JS side: `JSON.parse(decodeURIComponent(escape(atob(b64))))`
  (the `escape`+`decodeURIComponent` pair rebuilds UTF-8 and works in Ultralight's
  older WebKit; `TextDecoder` may not be available). See `Base64Encode` in Dresser.cpp
  and `dyfDecode` in index.html.

## JS → C++: `RegisterJSListener`

- C++: `RegisterJSListener(view, "funcName", callback)`. PrismaUI injects a global
  `window.funcName(arg)` into the page; calling it in JS fires the C++ callback with
  the string `arg`. One string argument only — pack structured data as JSON/`|`-joined.

## View lifecycle / timing

- `CreateView(htmlPath, onDomReady)` resolves `htmlPath` relative to
  `Data/PrismaUI/views/`. It returns a `PrismaView` handle synchronously; the DOM is
  only usable once `onDomReady` fires. Do not `Invoke` before DOM-ready.
- The WebKit view **outlives save loads** (it is created once at `kDataLoaded`). Any
  panel/target state it holds must be reset on `kPostLoadGame`/`kNewGame`.
- `Show`/`Hide`/`Focus`/`Unfocus` affect only visibility/input — they do NOT reload
  the page or re-run the script, so `window.*` globals persist across open/close.
- `Focus(view, pauseGame=false, disableFocusMenu=false)`. While the overlay has
  focus the game crosshair pick is unreliable — don't rely on it mid-overlay.

## Deploy

- CMake POST_BUILD copies `view/index.html` to
  `Data/PrismaUI/views/DressYourFollowers/index.html` and the DLL to
  `Data/SKSE/Plugins/`. Skyrim must be fully restarted to pick up either.

## Hand-built JSON is a trap — validate the raw string first

Assembling JSON by string concatenation in C++ is error-prone. A single missing
delimiter produces a payload that is invalid JSON *and* invalid JS, so the JS receiver
silently fails and the overlay renders nothing. Real bug (2026-07-14): the wardrobe
builder emitted `"name":"Boots,"kind":0` — the name value's closing `"` was missing
because the following `statbuf` started with `,` and nothing closed the string. Every
wardrobe payload was malformed at the first item; the overlay never rendered, for days.
- When you build JSON by hand, close every string value explicitly and eyeball a real
  payload. Prefer keeping the escaping/closing in ONE place per value.
- The follower-picker builder worked only because its name field *was* closed
  (`"\",\"name\":\"...\"}"`) — copy that discipline everywhere.

## Debugging when the overlay shows nothing — do this IN ORDER

The failure is almost always silent (no crash, no log), so build visibility FIRST
instead of theorising. This exact sequence is what finally cracked it; earlier guesses
about transport/encoding wasted several rebuilds.

1. **Log on the C++ side right before `Invoke`** — confirm it fires and the payload is
   non-empty. (Already in place: `PushList`/`PushFollowers` log lines.)
2. **Install a JS→C++ error bridge** so JS exceptions reach the SKSE log. Register a
   `dyf_log` JS listener in C++ that just `logger::info`s its arg, and in the page add
   `window.onerror = function(m,s,l,c){ window.dyf_log("onerror: "+m+" @"+l+":"+c); }`
   plus a `try/catch` around each receiver that logs `e` (and `e.stack`). Without this
   you are blind — it is worth adding the moment "C++ fires but the page doesn't react".
3. **When a `JSON.parse` fails, DUMP THE RAW STRING immediately** (`logger::info` the
   built JSON for small payloads; `{`/`}` inside the *argument* are safe, only the
   format string's `{}` are placeholders). One look at the literal string shows the
   defect (missing quote/comma, `NaN`/`inf` value, truncated field). Do NOT keep
   theorising about parsers/encodings before you have looked at the bytes.
4. `IVPrismaUI2::RegisterConsoleCallback` also pipes the page's `console.*` to C++, but
   the `dyf_log` bridge above is V1-safe (no interface-version risk to the install).
