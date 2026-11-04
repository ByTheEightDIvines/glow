# Master Sword Glow

Native Dusklight mod: a soft BotW-style glow on the Master Sword while it is drawn
(bright core over the blade + wide soft halo, additive, camera-facing), and a coloured
light cast into the world.

- Shows only while the Master Sword is in Link's hand and visible.
- Fades out while the vanilla Light Sword (Sol) glow is active; Sol is never touched.
- Colour comes from **Hero of Legend > Blade glow** (Off / Gold / Cyan). Hero of Legend
  publishes `/res/hol_swordglow_gold.bin` or `/res/hol_swordglow_cyan.bin` on the virtual
  disc; this mod checks for them a few times a second.
- Gold casts the same light colour vanilla uses for the glowing sword; Cyan casts blue.
- Fine tuning (halo size, halo brightness, blade brightness, light strength) is in this
  mod's own settings panel.

## Build

Same pipeline as the ocarina mod: GitHub Actions builds all eight Dusklight platforms
(Windows x64/ARM64, Linux x64/ARM64, macOS Apple Silicon/Intel, iOS, Android) against
Dusklight `v2.0.3`, merges them into one `.dusk` with `tools/merge_mod.py` and symgen,
publishes it to the `dist` branch, and attaches it to a release when a tag is pushed.

Local build: `cmake -B build -G Ninja && cmake --build build`.
