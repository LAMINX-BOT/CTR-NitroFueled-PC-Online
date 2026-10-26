# Launcher artwork

The launcher reads these files at start; replace them with your own to change its look.

- `background.jpg`: the Home page background (any size; cropped to fill). Without it, the
  launcher uses the background from your own game dump (`sce_sys/pic1.png`).
- `logo.png`: the header logo (transparent PNG).
- `LuckiestGuy-Regular.ttf`: the display font of titles and tabs, by Astigmatic, under the
  Apache License 2.0 (`LuckiestGuy-LICENSE.txt`), from Google Fonts.
- `LemonadaCTR-Bold.ttf`: the Arabic display font, Lemonada Bold by the Lemonada Project
  Authors under the SIL Open Font License 1.1 (`Lemonada-OFL.txt`), renamed "Lemonada CTR"
  because it is modified: `tools/arabic_presentation_forms.py` adds cmap entries for the Arabic
  presentation forms (the glyphs themselves are unchanged).

`background.jpg` and `logo.png` are official *Crash Team Racing Nitro-Fueled* artwork
(© Activision Publishing, Inc.), as published on gamesatlas.com. They are not covered by this
project's license; they are here only to identify the game the launcher starts.
