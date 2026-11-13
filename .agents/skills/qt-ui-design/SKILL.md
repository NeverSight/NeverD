---
name: qt-ui-design
description: >-
  Design or audit UI for Qt/QML, Qt projects, web, or embedded MPU or MCU targets. Use when creating screens, layouts, navigation, or auditing UX.
license: LicenseRef-Qt-Commercial OR BSD-3-Clause
metadata:
  author: qt-ai-skills
  version: "1.5"
  qt-version: "6.x"
  category: conceptual
  changelog: "Added auditing every window: inventory from the code, measured widget trees, a baseline, what the numbers usually find, keeping the fixes and their checks true on other platforms, and one accent per window as a look apart from the Return key"
---
# Qt UI Design

## NeverD desktop profile

For NeverD screens, read [the NeverD desktop profile](references/neverd-desktop.md)
before designing or auditing. It records the product's established visual
direction, workbench chrome, content hierarchy, resizing behavior, and Qt
rendering checks. Apply its desktop density and layout guidance in place of
the generic sizing defaults below; preserve accessibility requirements and the
user's current instructions.
For other products, use the general guidance without importing NeverD's palette
or screen dimensions.

For Windows custom-frame, dialog, transparency, or shadow work, also read
[the Windows desktop notes](references/windows.md). They cover existing native
handles, alpha-format ordering, and checks against the real Windows desktop.

For macOS Qt work, also read [macOS rendering and input](references/macos-qt.md)
for platform font sizes, menu defaults, precision scrolling, Retina captures
and native test limits.

Before producing UI output, confirm you know: target platform, screen geometry, design system, content priority, viewing distance, locale, and input methods. Run the seven items below as a check against the conversation and the project state; ask only the items that are genuinely missing. When the user cannot answer an item, choose a sensible Qt default and name it in your response so the user can correct it.

Small edits to an existing design — for example *"move the OK button to the right"*, *"change this label"*, *"make this red"* — do not trigger the checklist. Apply section 1 silently and verify section 2 (contrast, hit-target) where relevant.

## 0. Context check (before designing)

Use the seven items below to decide what is already known and what to ask. If the conversation or repository has already answered an item, do not re-ask.

1. **Target platform** — Desktop, web browser, mobile, or specific hardware (MCU, Raspberry Pi, other embedded board)?
   - If a specific board: ask whether a board-specific skill exists for it and load it if so.
2. **Screen shape** — Rectangle (default), Square, or Circle?
3. **Resolution and DPI** — Do you know the screen resolution and DPI? (Approximate is fine.)
4. **Design system** — Check whether the project already uses a design system or Qt Quick Controls style. If so, follow it and reuse its tokens. If not, recommend a Qt Quick Controls style: Basic, Fusion, Imagine, Material, Universal, iOS, or FluentWinUI3 (the iOS and FluentWinUI3 styles require Qt 6.7 or later). Where the project follows a third-party design language (Material Design 3, Apple Human Interface Guidelines, Fluent 2), map its tokens to the corresponding Qt Quick Controls style rather than introducing a parallel token vocabulary.
5. **Content priority** — What information is most important (primary), secondary, and tertiary on this screen?
6. **Viewing distance** — How far will users be from the screen? (e.g. handheld ~30 cm, desk ~60 cm, panel ~1.5 m, wall ~3 m)
7. **Locale and input** — What is the primary locale/language? Is RTL (Arabic, Hebrew, Farsi, Urdu) support required? What input methods must be supported (touch, keyboard, mouse/pointer, hardware buttons, voice)?
If the target is an embedded or MCU device, also read **section 4** in full before any design decisions — it overrides several desktop defaults.

If the user is requesting an **audit of an existing design**, skip to section 5 (Audit).

---

## 1. Design principles to apply (all targets)

Apply these while designing. Do not ask about each one — use them to inform decisions silently.

**Content and layout:**
- **Golden Ratio + Rule of Thirds:** Place primary elements at visual intersections.
- **Progressive Disclosure:** Show only what is needed at the current step.
- **Inverted Pyramid:** Critical information first, elaboration after.
- **Modularity:** Divide complex flows into smaller, self-contained screens.
- **Ockham's Razor:** When two designs are equivalent, choose the simpler one.
- **Performance Load:** Fewer steps = higher task completion.
- **Five Hat Racks:** Organise by category, time, location, alphabet, or continuum.
**Perception and interaction:**
- **Jakob's Law:** Match patterns users already know.
- **Affordance:** Controls should look like what they do.
- **Hick's Law:** More choices = slower decisions. Limit options per screen.
- **Miller's Law:** Working memory holds ~7 items. Chunk accordingly.
- **Recognition Over Recall:** Show options; don't require memorisation.
- **Proximity + Similarity:** Group related elements visually.
- **Uniform Connectedness:** Shared border or color = same group.
- **von Restorff Effect:** One visually distinct element draws attention — use sparingly.
- **Peak-End Rule:** Users remember the peak moment and the ending. Design completion states (e.g. installer finish screens) to feel rewarding, not abrupt.
- **Doherty Threshold:** System feedback within 400 ms, or show a progress indicator.
- **Aesthetic-Usability Effect:** Polished design is perceived as more usable.
- **Wayfinding:** Users must always know where they are, where they've been, where they can go.
**Reading patterns (use to guide information placement):**
- **F-shaped:** Text-heavy content — top bar, shorter secondary bar, left-edge scan.
- **Z-shaped:** Sparse content — top-left → top-right → diagonal → bottom-right.
- **Layer-cake:** Users scan headings and skip body text.
- **Spotted:** Users jump to landmarks — links, capitals, list items.
**Buttons and CTAs:**
- Limit CTA buttons per group. OK + Cancel = one group; additional actions must be visually secondary.
- Use Proximity and Similarity to distinguish primary, secondary, and tertiary controls.
**Error prevention:**
- Design affordances that guide correct use.
- Allow undo wherever technically possible.
- Confirm before destructive or irreversible actions.
- Add alarms or prompts for danger states.
**Responsiveness (desktop/web only — embedded: see section 4):**
- Design to your primary target's resolution first — desktop with chrome, embedded fixed-resolution, or a window-resize range typical for the application. Stack, collapse, or hide secondary content for narrower widths.
- Minimum layout width: 240 px. Stack or collapse elements below this.
- Hide secondary features behind menus or dropdowns when space is tight.

### 1.1 Motion and animation (desktop/web — embedded: see section 4)

Motion communicates state, relationship, and causality. Every animation must be functional, not decorative.

- **Enter animations:** Use deceleration easing (fast start, slow end). Elements should appear to arrive, not just pop.
- **Exit animations:** Use acceleration easing (slow start, fast end). Elements should appear to leave, not vanish.
- **Direct manipulation feedback:** Respond within 100 ms. Operations taking > 1 s must show a progress indicator; > 10 s must show estimated time.
- **Duration budgets:** Small elements (icons, badges): 100–150 ms. Medium elements (cards, panels): 200–300 ms. Full-screen transitions: 300–400 ms. Never exceed 500 ms for any UI animation — slower feels broken.
- **Limit simultaneous animations** to one or two elements. Animating the whole screen at once disorients.
- **Animate only `transform` and `opacity`** in QML/CSS — these are GPU-composited. Animating geometry (width, height, anchors) triggers layout recalculation and causes jank.
- **Honour user preference for reduced motion.** On the web, gate non-essential animation behind the `prefers-reduced-motion` CSS media query. Qt 6.x has no built-in equivalent: expose a project-level setting (for example a singleton property bound to `QSettings` or to a runtime accessibility option) and gate animations on it. When the user opts out, replace animation with instant transitions — do not simply slow them down.
- **Spatial consistency:** Elements that move between screens should animate in the direction that matches their destination (forward = slide left, back = slide right for LTR layouts).
### 1.2 Typography scale (desktop/web)

For embedded typography, see section 4.4. This subsection governs desktop and web targets only.

**Use a modular scale to derive all type sizes.** A modular scale is a sequence of numbers related by a fixed ratio — every size is mathematically proportional to every other, producing a scale that feels harmonious rather than arbitrary. The ratios are listed below; see also https://www.modularscale.com/ for an interactive generator.

#### How to build the scale

1. **Choose a base** — the size at which your body text looks best at the target viewing distance. For desktop at ~60 cm, 16 px is a reliable starting point. The base is your `ms(0)`.
2. **Choose a ratio** — multiply the base by this ratio to get the next step up, divide to get the next step down. Pick based on the character of the product:
| Ratio | Name | Factor | Character |
|---|---|---|---|
| 8:9 | Major second | 1.125 | Compact, dense — good for data-heavy UIs, installer flows |
| 5:6 | Minor third | 1.200 | Moderate — good for general desktop apps |
| 4:5 | Major third | 1.250 | Open, comfortable — good for marketing, onboarding |
| 3:4 | Perfect fourth | 1.333 | Strong contrast — good for dashboards, bold hierarchy |
| 1:1.618 | Golden section | 1.618 | High contrast — use sparingly; large gaps between steps |

3. **Map scale steps to roles** — assign a step number to each typographic role. Never add roles not on the scale; if a size is needed, pick the nearest step.
#### Worked example (base 16 px, Perfect Fourth 1.333)

| ms() | Value (px, rounded) | Role |
|---|---|---|
| ms(3) | 37.9 → 38 px | Display / hero text |
| ms(2) | 28.4 → 28 px | Page title / H1 |
| ms(1) | 21.3 → 21 px | Section heading / H2 |
| ms(0) | 16 px | Body — **base** |
| ms(-1) | 12.0 → 12 px | Caption / label / metadata |

Use a maximum of **three to four of these steps per screen**. More than four active sizes creates visual noise.

#### Rules that apply to all modular scales

- **Body (ms(0)) minimum is 16 px** at desktop viewing distance (~60 cm). Never use ms(-1) or smaller for primary reading content.
- **Line height:** 1.4–1.6× for body. 1.1–1.2× for headings (they need less leading).
- **Line length:** 45–75 characters per line for comfortable reading. Use `max-width` on text containers — do not let prose span the full viewport.
- **Weight pairs:** Use Regular (400) for body and captions; Medium (500) for headings. Never use Bold (700) inside body text.
- **System font first.** Use the OS/platform system font by default (Segoe UI Variable on Windows, SF Pro on macOS, Roboto on Android/Linux). Only introduce a custom font when there is a brand requirement and a Figma token exists for it.
- **Verify at Large system font size.** Do not hardcode pixel values that ignore the OS font scale. In Qt, prefer `font.pointSize` (which respects the platform DPI scale) over `font.pixelSize` for body text, or derive sizes from a singleton driven by `Screen.pixelDensity`. On the web, use relative units (`rem`).
#### Qt/QML implementation

In QML, define the scale as a singleton (e.g. `TypeScale.qml`) so sizes are referenced by role name, not hardcoded values:

```qml
// TypeScale.qml — generated from modular scale, base 16, ratio 1.333
pragma Singleton
import QtQuick

QtObject {
    readonly property real base:    16   // ms(0)
    readonly property real h2:      21   // ms(1)
    readonly property real h1:      28   // ms(2)
    readonly property real display: 38   // ms(3)
    readonly property real caption: 12   // ms(-1)
}
```

Reference via `TypeScale.h1` in components. Recalculate the whole singleton when the base or ratio changes — never patch individual values.

Register the singleton in your QML module so it can be imported. In a `qmldir` file:

```
module MyApp.Theme
singleton TypeScale 1.0 TypeScale.qml
```

In CMake with `qt_add_qml_module`:

```cmake
qt_add_qml_module(myapp_theme
    URI MyApp.Theme
    VERSION 1.0
    QML_FILES TypeScale.qml
)
set_source_files_properties(TypeScale.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
```

### 1.3 Multi-input and keyboard navigation (desktop/web)

Every desktop and web interface must support multiple input methods. Do not design for pointer alone.

- **Full keyboard navigability is required.** Tab order must follow the visual reading order (top-left to bottom-right for LTR). Always render a visible focus indicator — in QML, drive it from `activeFocus` (for example a `Rectangle` border or scale bound to `activeFocus`) or use the style-specific focus visuals on `Control`. On the web, do not remove the focus ring without a styled replacement.
- **No keyboard traps.** Users navigating by keyboard must always be able to exit any modal, popover, or overlay using Escape or a keyboard-accessible close control.
- **Every interactive element** must be reachable by pointer, keyboard, and — where Qt's accessibility APIs expose it — screen reader. This is a requirement, not a recommendation.
- **Hover states are an enhancement, not the primary disclosure mechanism.** Any information shown on hover must also be accessible without a pointer (e.g. a visible label, a dedicated info button, or keyboard-triggered tooltip).
- **Touch and pointer coexist.** On touch devices that also support a stylus or pointer, do not remove touch targets when a pointer is detected.
### 1.4 Semantic colour

Colour should communicate meaning consistently across the entire interface. Avoid arbitrary colour choices.

- **Use role-based tokens, not raw hex values.** Token names should describe the role a colour plays (interactive, surface, on-surface, error, outline), not its appearance (blue, dark-blue, grey-2). The role vocabulary above mirrors Material Design 3; when the project's design language is Fluent 2 or Apple Human Interface Guidelines, map to the equivalent role names from those systems rather than mixing vocabularies. If the project ships with a Figma token set, copy the tokens manually into a singleton until tooling is available to extract them.
- **Distinguish interactive colour from decorative colour.** A colour used on a button to signal "this is tappable" must not also be used as a background accent that doesn't invite interaction. Reusing interactive colour decoratively trains users to tap things that aren't tappable.
- **Colour is never the sole carrier of state.** Already covered in section 2 (WCAG), but applies equally to non-disabled states: success, warning, and error must always pair colour with an icon or text label.
- **Dark mode:** Design for both light and dark themes from the start. Token-based colour systems handle this automatically — if hardcoded colours are used, a dark variant must be explicitly defined.
- **High contrast:** Where the platform has a high contrast mode (Windows High Contrast themes, macOS Increase Contrast), ship high contrast themes and follow the setting. They mark hover, selection and the current item with an outline rather than a fill, draw every edge in one contrast border colour, and keep text at 7:1. Give each outline its own token, transparent in the other themes, so those paint exactly as before.
- **Culturally variable colour meanings** (see also §1.5): Red = danger in Western contexts but good fortune in some East Asian contexts. White = purity in Western contexts but mourning in some East Asian contexts. For safety-critical or internationally shipped products, do not rely on colour alone to convey critical meaning — always pair with text or universal iconography.
### 1.5 Localisation and RTL layout

Ask question 7 in the intake. Act on the answer here.

- **Date, time, and number formats** must be locale-aware. Never hardcode format strings like `DD/MM/YYYY`. Use Qt's `QLocale` or the platform locale API.
- **RTL layout mirroring:** For Arabic, Hebrew, Farsi, and Urdu, the entire layout mirrors horizontally. Navigation moves from right to left. Back buttons point right. Icons that imply direction (arrows, chevrons, playback controls) must flip. Enable `LayoutMirroring.enabled` with `LayoutMirroring.childrenInherit: true` near the root of your scene; it handles anchors and Qt Quick Layouts. The following items still need explicit attention even when `LayoutMirroring` is enabled: `Text.horizontalAlignment` defaults, `Image` content (use `mirror: true` on directional icons), custom `Canvas` painting, and any manual `x` positions.
- **Text expansion:** Translated strings are typically 30–40% longer than English source. Design containers and buttons to accommodate expansion — avoid fixed-width containers for any user-visible string.
- **Icons:** Avoid icons that are culturally specific or that carry variable meaning across regions (e.g. a thumbs-up, certain hand gestures). Prefer universal symbols (checkmark for success, X for close, magnifying glass for search).
- **Avoid embedding text in images or icons.** Localisation requires all text to be in string resources — text baked into assets cannot be translated.
### 1.6 Crisp strokes on the pixel grid (desktop/web)

Thin lines show their full colour only where they cover whole pixels. A 1 px line centred on a pixel boundary renders as two pixels at half strength, and a 1.4 px stroke as two pixels at about 70%. The line looks blurred, and darker and duller than its colour token. Users report this as a colour problem ("the blue looks heavy"), not as blur.

- **Snap straight strokes at the size the asset is drawn.** Centre strokes at least 1 px and under 2 px wide on pixel centres (x.5), centre 2 px strokes on pixel boundaries, and put the edges of unstroked fills on whole pixels. Curves and diagonals always antialias. An icon drawn on a 16 px grid stays crisp at 16 px and at whole multiples (32, 48), but not at 20 or 24.
- **In paint code**, fill hairlines as integer rectangles or offset 1 px pens by 0.5. Round a computed position before drawing a thin line with antialiasing on. In QML, place thin items and icons at whole-pixel positions (for example with `Math.round`), and draw icons at their native size or a whole multiple of it.
- **Choose thin-stroke colours by how they render.** On dark surfaces, a thin icon needs a lighter tone than text of the same hue. Visual Studio Code, for example, uses `#75BEFF` for coloured icons and `#4DAAFC` for links.
- **Verify at 100% scale** in a capture of the real UI, because 100% is common on desktop monitors. Then check 125%, 150% and 200%. At a fractional scale a toolkit can place a widget at a fraction of a device pixel, so a 1 px border drawn at that position spreads over two pixels. Paint chrome borders and hairlines in device pixels instead: round each side to the nearest device pixel and give a 1 px line one device pixel, as browsers snap border widths. Icons and text cannot be snapped everywhere; keep them no worse.
### 1.7 Quiet states, command rows and gutters (desktop/web)

A screen looks calm when each part is only as loud as its job. It looks wrong, and users say "ugly" without naming a cause, when parts that cannot act or that only divide are as loud as the parts that matter. Measure loudness instead of judging it by eye: take the WCAG contrast ratio of each part (fill, edge, label) against what lies directly behind it, after compositing any transparency. 1.00:1 is invisible; an enabled button's face usually stands at 1.1–1.4:1 on its surface.

**A state is the same control turned up or down, never another drawing.**
- A control that cannot act keeps the shape of the enabled one and recedes. Its fill fades and its label dims to about 2–3:1. Visual Studio Code fades the whole button to 40%; Windows 11 keeps a fill of about 4% white (30% of the fill in its light theme) inside an edge of 6–7%.
- Nothing about a disabled control should stand out from the surface by more than about 1.25:1, and all of it must stay quieter than the enabled control beside it.
- Two failures bracket the target. A full-strength fill behind a gray label reads as a dead block. An outline alone at the enabled border's strength reads as an empty frame, and a row of them as wire: the outline is as loud as the fill it replaced, around nothing.
- Keep the fill where the enabled control has one. A transparent box with an outline is what several systems draw for an *enabled* secondary button, so it cannot also mean "cannot act".
- Add an edge to a receded fill only where the fill alone would be lost, which is usually the light theme (a 40% fill can sit within 1.05:1 of a light surface). On a dark surface an edge that faint glows more than it helps.
- A primary (accent) control that cannot act keeps a faint accent, so the screen's next step stays recognizable.

**Measure a design system; do not quote it.**
- Before writing "as Windows 11 draws it" or "as Visual Studio Code does", read the source of the values (theme resource files, CSS, token files), composite them over that system's own surface, and compare the numbers with yours, per theme. A claim can hold in the light theme and fail in the dark one.
- Note what each system composites an edge over: the surface, the fill, or half of each. The same alpha gives a different edge.
- Record the numbers beside the rule, so the next change is checked against them and not against memory.

**Count before restyling.**
- When a window shows many controls that cannot act at once, no disabled style makes it look right; the count is the problem. Hide by mode, disable by selection: commands that cannot apply in the window's current mode (nothing loaded, nothing compared yet) stay away as a group and appear with the content they act on. From then on single commands disable in place, so positions stay stable.
- A reveal that coincides with the content arriving does not read as a jumping layout.

**One accent, on the main action; the key is a separate decision.**
- Give a window one filled accent button: the action the window is for. In a dialog that asks one thing it is the accept. In a window of tools it is the command that brings the content in or produces it (import, connect, trace), or the accept of a form inside it. In a window that only shows things it is the way on (Close, OK). A window with buttons and no accent looks unfinished beside every window that has one; users say it has "only one color".
- At most one. While the main action cannot act yet, keep its accent, faint, so the way forward stays recognizable. Where pressing it again would only redo finished work (a comparison that is already shown), take the Return key away from it and keep the accent: the window still needs its one. Do not move the accent from button to button with a window's state unless the window is built as a sequence of steps and each step is tested. One case is not a move but a fallback: a main action must be on show, so where it sits on a page of tabs, a command that stays visible takes the accent while that page is hidden.
- Choose the button from evidence, not taste: the window's own line of help names it, Return in a field already runs it, it is the window's only command, or the window does nothing but show. Where nothing names it, take the command that acts on what is selected, as OK does in a chooser, or the first step of a sequence. Never leave a window without one: shown renders of candidates for four windows left bare, the user answered that every window keeps at least one, on its key action.
- The accent is a look and the default is a key: decide them separately. A toolkit ties them together and then chooses for you. Qt makes a dialog's first push button its default and then each button that takes focus, which puts the accent on an arbitrary button and binds Return to it. Where that key does harm, take the key away and keep the look: give the style a state it draws exactly like the default (a property on the button) without making the button the default.
- Decide what Return does in each kind of place, and test each. On a focused button it presses that button. In an input that feeds the main action it runs that action, but only where the input changed what would run, so the key cannot throw finished work away, and again after a run that failed. In a filter, a table or a toggle it starts nothing.
- A window that owns child windows needs its own Return handling once it has no default: Qt's dialog looks for a default button among all its descendants and presses one in a child window.

**Command rows.**
- Group commands by what they act on, in the order of use. Keep a group close and leave a wider gap between groups. Prefer the gap to a separator line.
- In a row of many commands, let each take its natural width and let the row wrap, a group whole. A grid that stretches a row of commands to its columns scatters them and hides the grouping; a row that cannot wrap is clipped by a long translation or a narrow window.
- One command, or a small block of them, at the foot of a list may span the list. It reads as the list's foot, as a footer button does; cut down to its label it leaves the pane looking empty. Leave such a layout as its window has it.
- Keyboard order follows the visual order.
- Flat (borderless) commands suit a row that has icons or stands on a band of its own. Text-only flat commands read as labels; keep their faces until they have icons.

**Gutters, dividers and alignment.**
- No control touches a divider or the next pane. Leave at least the layout's spacing on either side of a dividing line.
- A divider is a hairline in the middle of its room. Its grab area may be wider than the line, but is never painted whole: a band beside a field, in the field's own gray, merges with the field.
- Neighbors of one color merge. Check every seam for two shapes of the same color meeting.
- Line up what adjacent panes show first: their first rows share a text line, and the rows below start level. Rows that miss each other by a few pixels read as clutter even when every control is drawn well.

**Judge on the user's screen.**
- Render the real UI with the user's font size, scale and window size before judging spacing or wrapping. A headless platform's default font can be two points smaller than the desktop's, and everything then looks roomier than it is.
- Compare variants in place: same size, same position, switched back and forth. Side by side, the eye compares two layouts; switched in place, it sees the one thing that changed.
- Check both themes, 125%, the minimum window size, the longest bundled language and a right-to-left one before settling a layout.
- Turn each rule into a test with a number in it (a contrast bound, a count of pixels across a seam, two tops that must be equal), and plant the old behavior once to see the test fail.
- Find out which build a screenshot came from before reasoning from its details. A window left open for days shows an older binary.

### 1.8 Auditing every window (desktop/web)

A polished main screen says little about the rest. Defects collect in the windows that were added later and are opened rarely, and each of them breaks a rule the main screen already keeps. When asked to look at everything, work from the code, measure before judging, and keep a baseline.

**Take the inventory from the code.**
- List every dialog class, every dialog built inline in a function, every pane, every menu and context menu, and every prompt that only a file or a state can raise. A walk through the menus misses the windows a button inside another window opens, those a selection enables, and those shown while a file loads.
- Keep one table of window against capture. An empty cell is a window nobody looked at; the report says so.
- Capture each window as it first opens and again holding content. Complaints are about first sight as often as about a busy state, and some commands only appear with content.
- Run the commands from each kind of place they act on (a listing line, a source line that calls, a function with a frame), and record what a command said when it opened nothing, so "nothing to show here" stays apart from "missed".

**Capture what the user sees.**
- Match the user's interface font (family and size), code font, scale, theme and screen before the first capture. Find out where the application takes its font from: one that bundles a family and reads only the size from the desktop shows neither the desktop's family nor a headless platform's size.
- Put that font in place before the first widget exists. A font set later leaves widgets built at startup with sizes they computed under the old one, and those stale sizes then look like defects of the product.
- Make captures repeatable: stop the caret blinking, keep input paths the same from run to run, wait for lists to fill. Then capture the untouched build twice and compare. What still differs (a clock, free disk space, a list of open windows) is the noise to ignore later.
- Read captures at their own size. A sheet scaled to fit loses hairlines and offsets of a few pixels, which are what the audit is for.

**Measure, then look.**
- Save each window's widget tree beside its picture: class, name, rectangle, text, enabled, default, font, alignment, layout margins. A question about every window is then a query: which button is the default as each window opens, which buttons are wider than their labels need, which headings are centered, which windows set a font of their own, which controls on one row differ in height, whose margins are the outliers.
- Numbers find candidates and the picture decides. A measured outlier can be deliberate, so read the code, its comments and the tests that pin a layout before changing it.
- Group findings by cause. Where one cause explains many windows (a toolkit default, a style rule), fix it once in the shared layer and name the windows it changes.

**What the numbers usually find.**
- *A default nobody chose.* The first push button of a window: Refresh, Reload, Close, Reset or a file chooser's opener draws the accent and takes Return from every field and list. List each window's default at first show and ask two things of it: is this the window's main action, and should Return press it from a field or a list? A window of tools leaves Return with the focused control; a command that discards work (Reset) never takes the key.
- *An accident that did two jobs.* Before removing what a toolkit put there by accident, ask what it was doing. The accidental default bound a key, which did harm, and gave the window its one colored button, which did not. An audit that removed both left fourteen windows in one color, and the user asked for the color back. Fix the job that fails and keep the other on purpose (section 1.7).
- *One thing drawn two ways.* Headings centered in one list and left in the next, or bold only while a row is selected; a list inset in one chooser and edge to edge in another. Toolkits give each widget class its own defaults, so two classes that show the same kind of table disagree until the style sets one.
- *An outlier that is a design.* A measurement that stands out is a question, not a finding. A lone button as wide as the list above it measured 835 pixels and read, on a sheet of numbers, as a band; cut down to its label it left the pane empty, and the user wanted it back: "the earlier design was fine". Before changing a layout that is consistent within its window, look at the whole window with and without the change, and prefer what was there unless something in it fails.
- *A window with a font of its own.* A size set in one constructor makes that window larger than every other.
- *A frame around nothing.* A group whose fields show only while it is checked draws an empty box when it is not. A flat group, a rule beside its title, frames nothing.
- *Forms that miss each other.* Two forms stacked in one column each size their own label column, so their fields start on two edges; give them one. A label beside a group of boxes belongs level with the first box. A box without a title keeps no room for one above its frame.
- *Panes on a hairline.* A second pane whose title or tabs start on the dividing line (section 1.7).
- *Text that fits only at the test font.* A one-line label written at a headless platform's size ends mid-word at the desktop's. Wrap what can outgrow its window.

**Change against a baseline.**
- Save every capture before the first edit, in every theme. After the edits compare all of them again: every window that changed must be one you meant to change, and the shared-layer fixes must have changed the windows you named and no others.
- For anything that changes behavior, write the test first and watch it fail on the old code with the harm in the failure (the request sent twice, the chooser that opened, the setting that flipped), not only a property that differs.
- A layout that looks arbitrary may be holding something up: a margin that keeps a control inside a right-to-left pane, a grid that keeps two rows of commands inside the narrowest window. Run the window's own tests, at its minimum size and in the longest and the right-to-left languages, before and after; when one fails, find what the old layout was protecting and keep that.
- Land what applies a rule the user has already confirmed. Show without landing, as renders, what needs a new judgment: commands renamed or removed, new colors, a size that would move every window.

**Keep it true on the other platforms.**
- A fix proven on one desktop is a claim about one desktop. Change shared code, not a platform's branch, and ask of each change what differs elsewhere: the interface font and its size, the display scale, which keys press a focused button, native dialogs in place of the toolkit's own, window frames that take room inside the client area.
- A size measured before a window shows is an estimate on another platform. Let the window correct itself once it shows (grow to what its layout needs) instead of trusting the number.
- Write each check so it holds on a native screen at 150% and 200%: draw into an image at a scale the check names, count device pixels in proportion to that scale, and never compare two pictures taken on either side of the moment a real window becomes active.
- A check that expects no dialog to open must not hang where the dialog would be a native one; make the toolkit use its own for the length of the check.
- A check that presses a key in a window waits until that window is the active one. A window is visible, and the modal one, before it is active; until then the key belongs to the window behind it and can run that window's shortcut instead. Such a check passes on a quiet machine and fails on a busy one.
- Run the whole suite, not the one new test by name, before saying it passes, and get the other platforms' verdict from their own machines before the change reaches the shared branch.
---

## 2. Accessibility (WCAG 2.2)

Check every design against these before delivering:

- **Perceivable:** All information has a non-visual equivalent (alt text, labels). Contrast ≥ 4.5:1 for text, ≥ 3:1 for large text and UI components.
- **Operable:** Full keyboard navigation, no focus traps. All interactive targets reachable without a pointer.
- **Understandable:** Labels, instructions, and error messages are plain language.
- **Robust:** Markup and structure work with screen readers and assistive tools.
- Never rely on color alone to communicate state — always pair with shape, icon, or text.
- **Test with OS font size set to Large** — verify that layout tolerates text scaling without truncation or overflow.
- **Test with a colour-blindness simulation** (Deuteranopia is the most common) — confirm that all state changes are distinguishable without colour.
- **Validate focus order** — tab through every interactive element and confirm the sequence is logical. Focus must be visible at all times — see §1.3 for the Qt-side focus indicator pattern.
---

## 3. AI-specific UX

When designing screens that involve AI features:

- **User Control:** Users must be able to start, stop, and modify AI actions. Always include Undo or Regenerate.
- **Transparency:** Show why the AI made a decision when it affects the user.
- **Graceful Failure:** When AI fails, provide a clear manual fallback path.
- **Value over Novelty:** AI features must solve a real problem — not exist for demonstration.
- **Uncertainty Communication:** AI output is probabilistic. When confidence is low or the result may be incorrect, surface that — use hedging language ("suggested", "approximately", "review before using"), visual confidence indicators, or explicit labelling. Never present probabilistic output as authoritative fact. For Qt AI Assistant and similar features, provide a "show source" or "why this?" affordance wherever the answer affects consequential decisions.
- **Latency patterns:** AI operations typically take 1–15 seconds. Do not show a generic spinner — users cannot estimate duration from a spinner and become anxious after ~3 s.
  - **Streaming output** (text generation): begin rendering partial results immediately. Show a blinking cursor or pulsing indicator at the insertion point while generation continues.
  - **Long operations** (code generation, image processing): show a skeleton screen or placeholder that matches the shape of the expected result. This anchors attention and sets expectations.
  - **Background operations** (indexing, analysis): surface as a subtle persistent status indicator (progress bar in a status bar, animated icon in a toolbar), not a blocking modal.
  - Never blank the entire screen while waiting for an AI response.
- **Consent before action:** If an AI feature will read, send, or modify user data (files, code, settings), make this explicit before the action executes. A one-line confirmation ("This will read your project files to generate a suggestion") is sufficient — it does not need to be a modal dialog.
---

## 4. Embedded and MCU targets

This section overrides desktop defaults. Read it fully before designing for any embedded, MCU, or hardware-constrained target.

Hardware facts that drive every decision (collect these during the intake step above if not already answered):
- Physical pixel dimensions and DPI
- GPU present? If unknown, assume none.
- Available RAM and flash for UI assets
- Input method: touchscreen, hardware buttons, rotary encoder, pointer
- Whether a board-specific skill exists
### 4.1 Rendering without a GPU

Software rasterisation means every visual effect costs CPU time.

| Allowed | Not allowed |
|---|---|
| Flat solid fills | Gradients |
| 1 px solid strokes | Drop shadows |
| Bitmap (PNG) icons and fonts | Blur or transparency layers |
| Opacity-only transitions | Simultaneous move + fade animations |
| Fixed pixel layout | Flexbox or fluid layout |

Prefer bitmap icons over vector paths — raster cost is paid at compile time, not runtime. Relax these constraints only when a GPU is confirmed.
**Exceptions.** The constraints above apply to runtime software rasterisation. Qt Quick Ultralite supports compile-time-baked gradients via its static rendering pipeline. Some MCUs ship with a 2D blitter or a small GPU that relaxes the no-blur and no-transparency constraints. Verify the hardware data sheet and the Qt toolchain in use before treating the table as a hard prohibition.

### 4.2 Layout

- Fixed pixel layout. No fluid grids, no breakpoints. Design to the exact screen dimensions.
- One task per screen. No overlapping panels.
- Flat navigation: move between screens, not within them. Max 2 levels deep.
- System state (connected, error, active mode) must be permanently visible — no tooltips or hover states.
### 4.3 Interaction

- No hover states. Replace hover-triggered disclosure with explicit buttons or dedicated screens.
- Touch minimum: 48 px is the default. Some controlled environments (fixed-grip medical instruments, secured industrial panels) may justify smaller targets after explicit safety review. Gloved-hand environments (industrial, outdoor): 60–72 px.
- Every action must have a hardware button fallback. No touch-only actions.
- No drag, scroll inertia, or multi-touch unless hardware confirms support.
- Confirm before any action that controls a physical actuator.
### 4.4 Typography

Embedded fonts are bitmaps — size is fixed at build time. Get it right from the start.

| Viewing distance | Minimum size |
|---|---|
| ~50 cm (handheld, appliance) | 14 px min, 16 px preferred |
| ~1.0–1.5 m (HMI, instrument cluster) | 20 px min |
| ~2–3 m (wall panel, public display) | 28 px min |

Contrast ≥ 4.5:1 for all text. Embedded screens often have poor viewing angles and bright ambient light — higher contrast is always better.

### 4.5 Error and safety

- Confirm before irreversible physical actuator commands — this is a safety requirement.
- Error state indicators must be persistent: driven by application state, not transient UI state. They must survive a screen repaint.
- Alarms require three independent cues: color + shape + text.
- Define a safe default screen the UI returns to if the application crashes or loses communication.
- No silent failures — unacknowledged hardware commands must be surfaced visibly.
### 4.6 Desktop → MCU quick reference

| Rule | Desktop / web | MCU / embedded |
|---|---|---|
| Responsiveness | Flexbox, fluid | Fixed pixel layout |
| Animation | Easing curves, ≤ 400 ms | Opacity-only, < 200 ms |
| Progressive disclosure | Tooltips, hover | Explicit button only |
| Touch targets | 44 px recommended | 48 px default (relax only with explicit safety review) |
| Font rendering | Vector, OS-scalable | Bitmap, fixed at build time |
| Status communication | Color + token-based | Color + shape + text |
| Error recovery | Undo | Confirm before action |
| State visibility | Status bar / tooltip | Always on screen |
| Navigation depth | Unlimited | Max 2 levels |
| Localisation | QLocale, RTL mirroring | QLocale only (RTL rarely applicable) |

---

## 5. Audit instructions

When auditing an existing design, categorize every finding:

1. **Critical** — Violates a core UX law or WCAG accessibility rule. Must fix.
2. **Warning** — Potential friction or elevated cognitive load. Should fix.
3. **Opportunity** — Enhancement or AI-driven improvement. Consider.
Apply section 4 constraints first if the target is embedded.

**Extended audit checklist for desktop/web targets:**
- [ ] Motion: are animations ≤ 400 ms, functional, and does a reduced-motion path exist?
- [ ] Typography: are body text ≥ 16 px and no more than three type sizes used per screen?
- [ ] Keyboard: is every interactive element reachable and operable by keyboard alone?
- [ ] Colour: are all colours token-based or semantically named? Is colour paired with a second cue for all state changes?
- [ ] Crispness: at 100% scale, do straight icon strokes and 1 px lines cover whole pixels, so thin elements show their real colour? At 125%, does each border and hairline still take one whole device pixel?
- [ ] States: does every control read differently at rest, under the pointer, pressed, with keyboard focus, checked and disabled, in every theme, and does a disabled control stay legible rather than vanish or look enabled?
- [ ] Disabled look: is a control that cannot act its enabled self turned down (same shape, its fill kept and faded, nothing louder than about 1.25:1 on the surface), not a dead block and not an empty outline?
- [ ] Command load: does the window show only the commands that can apply in its current mode, one accent on its main action, and its commands in groups by what they act on, a group wrapping whole?
- [ ] Return key: does Return press the focused button, run the main action only from that action's own inputs, and start nothing from a filter, a table or a child window's default?
- [ ] Seams: does any control touch a divider or the next pane, is any divider wider than a hairline painted whole, and do adjacent panes' first rows share a line?
- [ ] Every window: is each window, pane, menu and prompt of the application in an inventory taken from its code, captured at the user's font and theme, and looked at; which were not?
- [ ] Default at open: for each window, which button is the default as it first shows, and should Return press it from a field or a list? Does Return in every field and list of a window without a default start nothing?
- [ ] One accent per window: does every window with buttons show exactly one in the accent, chosen where the window is built, and is each exception on a list with its reason?
- [ ] One drawing per thing: do all lists align and weight their headings alike, all windows take the application's font, and no frame stand around nothing?
- [ ] What worked stays: for each layout changed, is there a failure it fixes, beyond a number that looked unusual? Does the window look as full and as finished as before?
- [ ] High contrast: in the high contrast themes, does each state still read by its outline, is text at least 7:1 and every line at least 3:1, and does selected or highlighted text keep a colour of its own?
- [ ] Localisation: do all user-visible strings use locale-aware formatting? Are containers able to expand 30–40% for translated text?
- [ ] Languages: does every window fit its texts in every bundled language, does a live language switch reword every pane, and do layouts placed by hand mirror right to left?
- [ ] AI features: are latency patterns, uncertainty, and consent handled per section 3?
---

## 6. References

- Nielsen Norman Group — 10 Usability Heuristics: https://www.nngroup.com/articles/ten-usability-heuristics/
- Laws of UX: https://lawsofux.com/
- WCAG 2.2: https://www.w3.org/TR/WCAG22/
- Modular Scale calculator: https://www.modularscale.com/
- Apple Human Interface Guidelines: https://developer.apple.com/design/human-interface-guidelines
- Microsoft Fluent 2 Design Principles: https://fluent2.microsoft.design/design-principles
- Google Material Design 3 Foundations: https://m3.material.io/foundations
- Windows UX Guide — disabling or removing controls: https://learn.microsoft.com/en-us/windows/win32/uxguide/win-dialog-box#disabling-or-removing-controls-vs-giving-error-messages
- WinUI theme resources (the values behind each control state): https://github.com/microsoft/microsoft-ui-xaml/tree/main/controls/dev/CommonStyles
- Visual Studio Code button states: https://github.com/microsoft/vscode/blob/main/src/vs/base/browser/ui/button/button.css
- Visual Studio Code UX Guidelines: https://code.visualstudio.com/api/ux-guidelines/overview
- Qt QLocale (localisation API): https://doc.qt.io/qt-6/qlocale.html
- Qt LayoutMirroring (RTL): https://doc.qt.io/qt-6/qml-qtquick-layoutmirroring.html
