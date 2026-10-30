#pragma once
// ---------------------------------------------------------------------------
// Custom §-colour codes (§z / §y / §x).
//
// Bedrock resolves a formatting code in two stages (reversed from 1.26.42.01):
//
//   1) A letter-range gate inside the text processor at
//          lea edx, [rax-61h]   ; code letter - 'a'
//          cmp dl, 16h          ; 'a'..'w' only
//          ja  <formatting-code switch>
//      Anything outside 'a'..'w' (and '0'..'9') never becomes a colour at all.
//
//   2) For codes that pass, the processor builds the 3-byte std::string
//      "\xC2\xA7X" and calls
//          TextColor::getColorFromCode(std::string const&) -> mce::Color const*
//      an FNV-1a hash map over the 29 vanilla keys "§0".."§w". A null return
//      makes the caller fall back to white (xmmword_14E7D8760).
//      The colours themselves live in .data at 0x151533BB0, stride 0x10,
//      as RGBA floats — e.g. §9 = {0.333, 0.333, 1.0, 1.0}.
//
// So adding codes takes exactly two edits: widen the gate immediate from 0x16 to
// 0x19 ('a'..'z'), and hook getColorFromCode to answer for the keys we own.
// Because the caller copies RGB out of the returned pointer every time it lays
// the text out, rewriting that pointer's contents per call makes the text flow
// through a live gradient on its own.
//
// Vanilla already claims every letter through 'w', so the free letters are
// exactly 'x', 'y' and 'z' — which is why the gate stops at 'z'.
// ---------------------------------------------------------------------------

namespace ColorCodeHook {

// The codes this client adds on top of vanilla's "§0".."§w".
enum class Code {
    // §z — the live theme colour, swept so one §z run flows THROUGH the palette
    // instead of being a single flat colour.
    Theme = 0,
    // §y — the theme colour sampled at one fixed stop: a stable accent that does
    // not gradient along the text. Reads better on short labels, where a sweep
    // just looks noisy.
    Accent = 1,
    // §x — the user's own RGB from the Interface module's "Code Color" setting,
    // independent of the active theme.
    Custom = 2,
};

// Widen the letter gate + install the getColorFromCode detour. Safe to call when
// either signature is missing: the other half still applies, and the custom codes
// render white instead of coloured.
void Initialize();

// Restore the original gate immediate. MinHook disables the detour itself.
void Shutdown();

// True once the custom codes can actually produce a colour (both halves live).
bool IsAvailable();

// The literal code string for `c`, e.g. "§z". Falls back to §f (white) when the
// hook could not be installed, so callers never emit a code the game would draw
// as garbage.
const char* CodeString(Code c);

// Shorthand for CodeString(Code::Theme) — the common case, and the spelling the
// existing call sites already use.
const char* ThemeCode();
} // namespace ColorCodeHook
