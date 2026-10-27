**The newest work, less tested.** Use the stable release for your main
adventure; try this one in its own folder with a copy of your save.

**New in nightly2**
- **The menu bar in fullscreen works**: it was greyed out and wouldn't open.
- **Outlines only on models' edges**: nightly1's smooth outlines also traced
  fire, people drawn as flat pictures and speech bubbles. Not any more.
- **No flash of misplaced scenery in widescreen**: something coming into
  view at the sides (the railing by Coffinwell's well) could be drawn in the
  wrong place for one frame.
- **The turnstile in Gortress's prison no longer pops in** across the
  middle of the picture in widescreen: very large things are now drawn a
  good way before they can come into view (the game only checks a small box
  around their centre). Thanks to celvrc for the report.
- **No flicker of sleeping people as the camera turns**: Enhanced drew each
  frame a moment too early and showed the old angle's picture for a frame.

Still being looked into: the light from the window in Upover's inn
flickers (#19). It isn't widescreen: it does it with the sides off too.

**New since stable v31**

Installing
- **The installer fixes broken or old MSYS2 setups by itself.** If you saw
  `target not found: mingw-w64-ucrt-x86_64-gcc` and then
  `No module named 'yaml'`, your MSYS2 was older than the packages the build
  needs: it now gets them added. It also uses an MSYS2 you already have on
  any drive and installs only what's missing, clears the lock an
  interrupted update leaves behind ("unable to lock database"), installs
  MSYS2 without winget when needed, and never deletes an old MSYS2 (it asks
  first and keeps it renamed). Thanks to everyone on the Discord who
  tracked this down.
- **A ROM inside a .zip works**: pick the zip, no unzipping needed.

Fixes
- **Game > Controls works**: changing a key or controller button now
  finishes (it didn't at all in v31).
- **Leaving fullscreen** gives the window back its title bar and borders.
- **Fullscreen**: the menu bar shows over the top of the picture instead of
  shrinking it.
- **Quitting asks first** (Esc, the window's X, Alt+F4, File > Exit), so a
  stray key can't lose your progress. "Don't ask again" turns it off.
- **Walking with the stick** no longer goes the wrong way after a menu, a
  battle or a scene change while the stick is held.
- **Widescreen**:
  - Places made only for the DS's width (the starry sky at the start, the
    way up to the Observatory) get black sides instead of showing past the
    edge of the scene. Found another one? Modern > "Widescreen: black sides
    in this place".
  - The background behind menus and the title screen continues into the
    sides instead of being smeared into stripes.

Enhanced graphics
- **Character outlines** are an even, smooth line instead of jagged steps
  at the models' corners. Graphics > Character outlines: Thin (the
  default), Medium, or Like the DS.
- **Soft shadows** stay a smooth gradient instead of rings of flat steps.

New
- **Tab turns fast forward on and off** (three arrows in the bottom left
  while it's on). Prefer holding it? Set `fast_forward = hold` in
  settings.ini.
- **Game > Save file**: export your save, or import one (also DeSmuME's
  .dsv and melonDS saves). Your current save is backed up first.
- **The controller's Share button** takes a screenshot.
- **Bug reports attach themselves**: after Send, the report is on your
  clipboard, so on the GitHub page it's just **Ctrl+V**, then **Create**
  (or paste it on the Discord). Reports now include a picture of the window
  as you saw it and your display settings.

**How to install**: download *Source code (zip)* below, unzip it and
double-click `Install Dragon Quest IX.bat`. Already playing? Use Help >
Check for updates instead.
