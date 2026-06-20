#pragma once
#include "imgui.h"

#include <cstdint>

namespace nxtdbg::theme
{

// Palette — the single source of truth. One edit re-themes the whole app.
// Industrial dark with one amber accent (the signature). Status colors stay
// reserved for live data states (good/warn/bad/info), not chrome.
constexpr ImU32 kBg          = IM_COL32(0x0A, 0x0B, 0x0E, 255);
constexpr ImU32 kPanel       = IM_COL32(0x12, 0x14, 0x1A, 255);
constexpr ImU32 kPanelDeep   = IM_COL32(0x0E, 0x10, 0x16, 255);
constexpr ImU32 kBorder      = IM_COL32(0x1E, 0x22, 0x2B, 255);
constexpr ImU32 kBorderHi    = IM_COL32(0x33, 0x3A, 0x48, 255);
constexpr ImU32 kTextHi      = IM_COL32(0xE6, 0xE8, 0xEE, 255);
constexpr ImU32 kText        = IM_COL32(0xB6, 0xBC, 0xC8, 255);
constexpr ImU32 kTextDim     = IM_COL32(0x6B, 0x72, 0x80, 255);
constexpr ImU32 kAccent      = IM_COL32(0xFF, 0xAB, 0x47, 255);
constexpr ImU32 kAccentSoft  = IM_COL32(0xFF, 0xAB, 0x47,  44);
constexpr ImU32 kAccentGlow  = IM_COL32(0xFF, 0xAB, 0x47, 120);
constexpr ImU32 kGood        = IM_COL32(0x4A, 0xD1, 0x91, 255);
constexpr ImU32 kGoodSoft    = IM_COL32(0x4A, 0xD1, 0x91,  60);
constexpr ImU32 kWarn        = IM_COL32(0xFF, 0xC1, 0x07, 255);
constexpr ImU32 kBad         = IM_COL32(0xE3, 0x4A, 0x4A, 255);
constexpr ImU32 kInfo        = IM_COL32(0x5C, 0x9F, 0xE0, 255);
constexpr ImU32 kRowAlt      = IM_COL32(0xFF, 0xFF, 0xFF,   6);
constexpr ImU32 kRowHover    = IM_COL32(0xFF, 0xAB, 0x47,  18);

// Apply the project ImGui style. Call once after ImGui::CreateContext().
void Apply();

// --- Section card ----------------------------------------------------------
//
// Padded child region with a left accent stripe + bold title at the top.
// Pair every BeginCard with EndCard. Returns false if the child is clipped
// (caller MUST still call EndCard for stack discipline).
//
// `fillHeight` controls vertical sizing: false (default) auto-resizes the card
// to its content — right for stacked content cards (KeyLine dumps, stat blocks).
// true makes the card fill the parent's available height — required when the
// card hosts a scrolling child (a list / tree) that itself fills remaining
// space; an auto-resizing parent would collapse that child to a sliver.
bool BeginCard(const char *id, const char *title, ImU32 stripeCol = kAccent,
               bool fillHeight = false);
void EndCard();

// --- Inline primitives -----------------------------------------------------

// Pill: rounded capsule with text. Width sizes to text + padding.
void Pill(const char *text, ImU32 fillCol, ImU32 textCol = kTextHi);

// Status dot: filled circle, optionally pulsing. `phase` is the panel-owned
// animation accumulator (seconds since the last "ping"). When `alive` is true
// the dot breathes at ~2 Hz; otherwise it stays steady-dim.
void StatusDot(ImU32 col, bool alive, float phase);

// Heading text rendered slightly dim + small (section subheaders inside cards).
void Subheading(const char *text);

// Horizontal value strip: left-aligned label in dim, right-aligned value
// in primary. Single line at GetFrameHeight().
void KeyLine(const char *label, const char *value);

// Hero number: big text + small label beneath. Width = autosize.
// Use for top-of-panel statistics (tickId, npcCount, etc.).
void HeroStat(const char *label, const char *value, ImU32 valueCol = kTextHi);

// Progress strip: gradient fill `frac` of width, with optional right-aligned
// caption. Height = GetFrameHeight() * 0.35.
void ProgressStrip(float frac, ImU32 fillCol, const char *caption);

// Toggle: pill-shaped switch widget. Persistent state via the bool ref.
bool Toggle(const char *id, bool *value);

// Section separator: a thin amber bar 18% of the width centred, with
// breathing room above/below.
void AccentRule();

// Hover-row helper for table rows. Returns true when the current row is
// hovered; caller can use it to display extras inline.
bool RowHover();

}
