# UI design: the island notice board

Every menu ACNHRecomp draws follows one design: the launcher's main menu and setup pages, the in-game settings screen
(F1), their popups and overlays. A new menu, page, popup or control uses the parts below as they are, with the values
given here, so it looks and behaves like the screens already there. When something new is truly needed, it is built
from these parts and added to this document.

The look is an island notice board: warm sand underneath, a paper board on it with a stitched edge, bark-brown ink,
round friendly type, and a few bright accents that each mean one thing.

## Principles

1. **Ink is bark, never black.** All text and line icons are bark or soft bark; white only on a strong colour.
2. **Every colour means something.** Leaf green: done, on, chosen. Lagoon teal: focus, water, progress. Sun yellow:
   the one action that matters on the screen. Bell orange: warnings. Coral: errors, quitting, deleting.
3. **One sun-yellow action per screen** (Play, Back to the game, Next, Start, Download). Everything else is quiet.
4. **Everything clickable answers.** The pointing hand on hover, an eased hover change, a press that gives under the
   pointer, a teal edge under keyboard or controller focus.
5. **Nothing spills.** Content never draws past a stitched edge; a page never scrolls by a sliver; a page that fits
   shows no scrollbar.
6. **Same sizes everywhere.** Sizes are design pixels for a 1280x720 window times the screen's scale, so a screen reads
   the same on a laptop, a 4K monitor or a phone.

## Colour

Defined once in `runtime/src/ui/theme.h` (`kTheme*`). Use the names, not new values.

| Token | RGB | Hex | Use |
|---|---|---|---|
| `kThemeInk` (bark) | 107, 84, 68 | `#6B5444` | Text, line icons on paper |
| `kThemeInkSoft` (soft bark) | 154, 133, 113 | `#9A8571` | Descriptions, hints, empty marks |
| `kThemeSand` | 251, 244, 220 | `#FBF4DC` | Background, panels on the board, hover on paper |
| `kThemeSandDeep` | 241, 230, 194 | `#F1E6C2` | Stitches, dashes, edges, quiet lips |
| `kThemeBoard` (paper) | 255, 252, 240 | `#FFFCF0` | The board, quiet buttons, fields, chips |
| `kThemeLeaf` | 120, 195, 106 | `#78C36A` | Done, on, chosen edges, the leaf |
| `kThemeLeafDeep` | 78, 154, 69 | `#4E9A45` | Check marks, leaf veins, success text |
| `kThemeLeafPale` | 227, 243, 220 | `#E3F3DC` | The fill of anything on or chosen |
| `kThemeLagoon` | 78, 195, 179 | `#4EC3B3` | Hover edges, slider grabs, progress |
| `kThemeLagoonDeep` | 46, 156, 144 | `#2E9C90` | Focus edges |
| `kThemeSun` | 246, 211, 74 | `#F6D34A` | The one action that matters, the chosen tab |
| `kThemeSunDeep` | 217, 174, 34 | `#D9AE22` | Its lip |
| `kThemeBell` | 240, 164, 58 | `#F0A43A` | Warnings, "applies after a restart" (darkened to 78 %) |
| `kThemeCoral` | 236, 122, 107 | `#EC7A6B` | Errors, Quit, Delete |

Derived colours, never new ones:

- **A lip** is its face darkened: `theme_shade(face, 0.8)` (sun and quiet faces use `kThemeSunDeep` / `kThemeSandDeep`).
- **A hovered face** is brightened: `theme_shade(face, 1.15)`.
- **A tinted badge** (a page's or action's colour): fill `theme_shade(c, 1.62)`, ring `theme_shade(c, 1.25)` at 2 px,
  icon `theme_shade(c, 0.68 to 0.72)`.
- **Ink on a face**: `theme_ink_on(face)`: bark on light faces, paper on dark ones.
- **Disabled**: face `kThemeSandDeep`, lip `rgb(224, 211, 172)`, ink `kThemeInkSoft`.
- **Behind the in-game screen**, the game is dimmed with `rgb(60, 45, 30, 130)`; behind popups, bark at 40 %.

Category colours (badges only): video `rgb(92, 172, 222)`, controls `rgb(166, 138, 214)`, amiibo `rgb(246, 190, 80)`,
Discord `rgb(110, 122, 240)`, about `rgb(176, 140, 100)`; the others use the tokens above.

## Type

| Role | Font | Size (design px) |
|---|---|---|
| Titles, headings, big buttons, row titles, step numbers | Fredoka SemiBold (600) | 30; headings ×0.78, row titles ×0.72, popup titles ×0.8 |
| Text, buttons, fields | Nunito Bold (700) | 20 |
| Descriptions, hints, chips | Nunito Bold (700) | 20 × 0.82 to 0.87 |

Both fonts are compiled in (`assets/fonts/`, SIL OFL). The system's fonts are merged under them for Cyrillic and CJK;
a player's own `fonts/ui.ttf` replaces the text font in the game. Text is left-aligned in rows and centred in buttons,
chips and the hero panel. Every string is an i18n key (`runtime/src/ui/i18n*.inc`), never literal text, except
proper names.

## Icons

Lucide (ISC), outline style, drawn from the icon font through `theme_icon(dl, icon, centre, r, colour)`; the glyph is
`r × 1.7` px. Only the glyphs the menus use are compiled in (`assets/fonts/Lucide-launcher.ttf`); their codepoints are
named by IconFontCppHeaders' `runtime/src/ui/IconsLucide.h`.

- **Play is always the filled triangle**, never Lucide's outline (row arrows, the Play button).
- Icons sit on a tinted badge (icon in the deep tint), a solid disc (white icon) or plain paper (bark icon).
- One icon per meaning: settings `settings`, online `globe`, saves `house`, add-ons `package`, mods `puzzle`,
  folder `folder-open`, rebuild `hammer`, desktop shortcut `monitor-down`, prepare again `refresh-cw`, credits `heart`,
  download `download`, warning `triangle-alert`, info `info`.
- **Adding a glyph**: add its codepoint to the subset and regenerate `Lucide-launcher.ttf` from lucide-static's
  `lucide.ttf` (the version `IconsLucide.h` was generated for) with fontTools, removing the `GSUB`, `GPOS` and `GDEF`
  tables first (their ligatures break the subsetter); then map it in `theme_icon`.

## Surfaces

**Background.** Sand with a sparse, fixed scatter of dots: one per 46 px cell, radius 1.6 to 3.4 px, `kThemeSandDeep`,
the same seed every frame. In the game, the dimmed game is the background.

**Board.** Paper, rounding 30, at most 1180 × 700 (landscape) or the window less 16 px (portrait), centred. A flat drop
under it: the same shape 6 px lower in `rgb(138, 106, 58)` at about 12 to 24 % alpha. Its **stitched edge**: a dashed
rounded rectangle 8 px inside, dashes and gaps of 6 px, 2 px thick, `kThemeSandDeep`, rounding 7 px less than the
board's (`theme_stitch`).

**Panel.** Sand on the board, rounding 22 to 28, its own stitch 6 to 8 px inside. A panel that scrolls keeps its stitch
still and scrolls its content in an inner area that ends inside the stitch (a child window's padding is not a clip).

**Divider.** A row of dashes 7 px long, 7 px apart, 2.5 px tall, `kThemeSandDeep` (`theme_dashes`): under the header,
between the rows of a list or a settings page, under a section heading. Never a solid line.

**Header.** About 56 to 64 px: the leaf (42 to 46 px, turned −0.5 rad) at the left, the title in Fredoka bark beside
it, a soft-bark tagline under the title when it fits; actions (language, Back to the game, a notice) at the right; a
divider under it.

## Controls

**Button** (`theme_button(label, size, face)`): a pill the frame height tall with a lip under it, 0.1 × its height
(3.5 px; 5 px for the big button).

| Kind | Face | Lip | Ink |
|---|---|---|---|
| Quiet (Back, Cancel, Open, Close) | `kThemeBoard` | `kThemeSandDeep` | bark |
| The one action that matters | `kThemeSun` | `kThemeSunDeep` | bark |
| Danger (Quit, Delete, Yes on a quit question) | `kThemeCoral` | its shade 0.8 | paper |
| Other (Discord, Import) | its colour | its shade 0.8 | `theme_ink_on` |

States: hovered, it lifts off its lip by 60 % of the lip and brightens (a quiet one's edge turns lagoon); pressed, it
sinks onto the lip; keyboard/controller focus, a 3 px `kThemeLagoonDeep` edge; disabled, the disabled colours. Its
width is the label's plus padding, at least 120 px in a footer.

**Big button** (Play): the sun button 64 to 80 px tall, label in Fredoka ×0.9 with the filled play triangle; during an
auto-start countdown it stays lifted with the countdown on a second, soft line.

**Toggle chip** (every on/off): a pill, the frame height (or the text + 14 px) tall.

- On: `kThemeLeafPale` fill, 1.5 px `kThemeLeaf` edge, a filled `kThemeLeafDeep` circle (radius ~8.5 px) with a white
  check.
- Off: paper fill, `kThemeSandDeep` edge, an empty 1.8 px `kThemeInkSoft` ring; the edge turns lagoon on hover.
- The label after the mark is the setting (in a list of options) or On / Off (in a settings row).
- The change fades over 0.16 s and the check grows in. Left/right set it from the keyboard or a controller.
- Several options side by side wrap as a row of chips, 24 px apart.

**Check mark** (on/off inside a card or tile): the toggle chip's mark alone, a square the frame height wide.

**Dropdown, slider, text field.** Paper, 1.5 px `kThemeSandDeep` edge, pill rounding; the dropdown's arrow box the
same colour as its frame. Hover: the pointing hand and a lagoon edge fading in; focus: a `kThemeLagoonDeep` edge. A
slider's grab is `kThemeLagoon` (`kThemeLagoonDeep` held). A value wider than the box ends in "…".

**List row** (a menu's actions, credits): the full width, padding 12 px.

- At the left, a tinted badge (radius 22 px) with the action's icon.
- The title in Fredoka ×0.72, its one-line description in Nunito ×0.82 soft bark.
- At the right, a 13 px arrow circle with the filled triangle: paper (or sand), turning sun yellow and leaning 3 px
  right on hover.
- Hovered, the row's back fades to sand (or to paper on a sand panel); focused, a lagoon edge. Rounding 16 to 18.
- Rows are divided by dashes inset 12 px. When a list must fit beside something, rows keep their size and only the
  space around the dashes gives way.

**Settings row**: the setting's name (bark) and a description (soft bark, ×0.86) at the left, the control at the right
in 45 % of the width (at most 360 px), stacked under the text when the page is narrower than 560 px; "applies after a
restart" under both in bell darkened to 78 %; a divider under the row.

**Section heading**: a small leaf (the text's height), then the heading in Fredoka ×0.78 bark, a divider under it.

**Chip** (a fact that may open something: version, add-ons, save file, mods): 38 px tall, rounding 12, paper with a
1.5 px sand edge, a coloured disc (radius 12 px) with a white icon, the text in Nunito ×0.86; a small leaf-green dot
on the disc when it is live. A clickable chip takes a lagoon edge on hover and gives 1.5 px under the pointer. Chips in
a group form two columns meeting at the middle, or one centred column when they do not fit.

**Card** (a large choice: set up, choose a folder): sand with a 4 px `kThemeSandDeep` lip, rounding 22 to 26, a
coloured disc (radius 34 px) with a white icon, title in Fredoka ×0.85, description; paper when hovered, lagoon edge
when focused, an arrow circle at the right.

**Tile** (an item in a popup: a save file, a mod): the chip's colours. The chosen or enabled one is `kThemeLeafPale`
with a 1.5 px `kThemeLeaf` edge and a filled check; the others paper with a sand edge and an empty ring. Hover: a
lagoon outline.

**Tabs** (pages of a screen): a column of rows 52 px tall at the left (a row along the top when narrow), each a tinted
badge and the page's name in bark. The open page's row is sun yellow on its lip; hovered rows turn sand; focus a
lagoon edge. Shoulder buttons above them are quiet pills with a drawn arrow, turning sun yellow while held.

**Steps** (a setup's progress): circles of radius 15 px joined by 3 px lines. To come: a number on sand with a 2 px
sand-deep ring. Current: on sun yellow with a sun-deep ring. Done: a small leaf on `kThemeLeafPale`; the line before it
leaf green.

**Status**: a small disc in leaf (ok), bell (warning), coral (error) or `rgb(218, 204, 168)` (empty) with a white
check, warning or cross; the text beside it in the matching deep colour.

## Popups and notices

**Popup**: a paper card (window rounding × 1.4) over bark dimmed to 40 %. At the top, a coloured disc (radius 18 px)
with the subject's icon and the title in Fredoka ×0.8; then the text in soft bark, the content (tiles in a sand panel),
and the buttons at the bottom: actions at the left, Close (quiet) at the right. It fades in over 0.18 s; its items
settle 6 px into place one after the other, 30 to 40 ms apart. Escape or the controller's B closes it.

**Notice pill** (a short message over the game, the first-start hint, a new release): paper with a 1.5 px sand edge, a
coloured disc with an icon, the text in bark; a release notice carries its sun button. In the launcher it sits in the
header between the title and the language and falls back to the page when there is no room.

## Motion

| What | How |
|---|---|
| Hover | Eased toward 1 or 0 at 18 per second (about 0.1 s), per item (`theme_hover`) |
| Toggle on/off | 0.16 s fade of the fill, edge and mark |
| Press | At once: the face sinks onto its lip |
| Popup | 0.18 s fade in, decelerating; items settle 6 px with a 30 to 40 ms stagger |
| A screen opening in the game | Slides 18 px up into place, decelerating |

While anything eases, the screen keeps drawing at full rate (the launcher's `pages_busy`).

## Interaction

- **The pointing hand** over everything clickable; never over plain text.
- **Focus** shows only for the keyboard or a controller (`theme_nav_focused`), not after a mouse click.
- **Controllers**: the d-pad moves between items in reading order; A activates; B goes back or closes; L/R change tabs.
  A list's top row leads up to the main action, and its rows to the left lead to it too.
- **Escape / B** closes the screen or popup on top.

## Layout

- Sizes are design pixels × `theme_scale` (1280 × 720 base; the shorter side decides; a portrait window is laid out on
  its width).
- Below an aspect of 1.2 (portrait), two-column parts stack: panels above lists, tabs along the top.
- Spacing: 12 px between items; 22 to 26 px between columns; 14 to 26 px padding inside panels.
- A footer reserves its buttons' lip; content never draws past a stitch; a page that overflows by under 12 px does not
  scroll.

## In code

`runtime/src/ui/theme.h` holds the shared parts for every menu (the launcher and the game):

| | |
|---|---|
| `theme_apply_style` | ImGui's colours and roundings for this design |
| `theme_build_fonts`, `theme_set_embedded_fonts`, `theme_set_embedded_icons` | The fonts and the icon font |
| `kTheme*`, `theme_shade`, `theme_mix`, `theme_ink_on` | Colours |
| `theme_leaf`, `theme_stitch`, `theme_dashes`, `theme_lip` | Drawing the surfaces |
| `theme_button`, `theme_hover`, `theme_nav_focused` | Buttons and states |
| `theme_icon` | Icons |

The settings screen's rows, toggle chips and tabs are in `runtime/src/ui/settings.cpp`; the launcher's cards, list
rows, chips and popups in `launcher/pages.cpp`. A new control used in more than one place moves to `theme.h`.

## Checklist for a new screen or control

- [ ] Only `kTheme*` colours and their derived shades; bark ink; one sun-yellow action.
- [ ] Fredoka for titles, Nunito for text, the sizes above; every string translated.
- [ ] Lucide icons through `theme_icon`, on a tinted badge, a solid disc or paper; Play filled.
- [ ] Paper board with its stitch; panels in sand; dashes, never solid lines.
- [ ] Buttons with a lip, toggle chips for on/off, check marks in tiles; dropdowns and fields paper with an edge.
- [ ] Hover eases with the pointing hand; focus is a lagoon edge for keyboard and controller only; press sinks.
- [ ] Works with a controller (reading order, A, B, L/R) and closes with Escape / B.
- [ ] Fits at 1280 × 720 and in portrait; nothing draws past a stitch; no sliver of scrolling.
