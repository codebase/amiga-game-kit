# IRON WRAITH art provenance

The industrial background was generated with the built-in ImageGen tool. The unmodified source is `foundry.png`; the compiled 320×256 version is `../foundry.png`.

Final background prompt:

Create a stunning original pixel art BACKGROUND for IRON WRAITH, a badass side-on Amiga 1200 mech shooter. No characters, no vehicles, no text, no HUD. Wide 5:4 composition designed to reduce cleanly to 320x256 pixels. A colossal abandoned weapons foundry in a rainy cyberpunk megacity at night, layered monumental steel girders, ribbed pipes, hanging chains, distant industrial silhouettes, a massive cylindrical furnace on the right glowing fiery orange, smoky deep blue / petrol teal atmosphere, hazard stripes, glowing little red lamps, battered metal and dramatic chiaroscuro. Bottom 15 percent is a horizontal heavy steel deck platform with warm reflections and exposed mechanical understructure. Middle 65 percent must stay relatively dark and open for readable flying mechs and projectiles. Ornate dense atmospheric pixel-art details around edges, painterly pixel clusters with deliberate sharp shapes, premium 1995 hand-pixeled arcade background, Metal Slug level of craftsmanship but original dark sci-fi setting. Restrained saturated amber highlights against desaturated blue-black steel. Do not draw a border or vignette. Pixel artwork fills whole canvas. No smooth vector shapes, no blurry airbrush, no game screenshot mockup.

The selected combat art was generated with Retro Diffusion at native resolution: `rd/mech-1.png` (48×48) and `rd/boss-0.png` (80×80). Two alternatives per subject cost $0.36 per pair. `rd/mech-idle.png` is the successful eight-frame idle sheet, costing $0.14. The first animation attempt failed with HTTP 502/inference_failed and was refunded. The first art pass cost $0.86. The account is shared, so balance-after values may include other work.

Exact prompts, style, seed, dimensions, and costs are in the adjacent `rd/*.json` files. The idle compiler selects frames 0, 1, 6, and 7, which retain the full cannon length; other poses retract it away from the projectile origin. Sources and unused candidates are retained for comparison. Hardware conversion reduces the mech to 15 colors, keeps its alpha mask, and maps the boss into the game's palette.

The boss animation pass added `boss-walking.png`, `boss-attack.png`, and
`boss-destroy.png`: eight 80×80 poses each, $0.14 per sheet, $0.42 total.
Total successful RD charges for this task are $1.28. The compiler retains four
walking poses (0, 2, 4, 6), all eight attack poses, and six destruction poses
(0, 1, 2, 3, 4, 7), giving 18 distinct frames. Game logic synchronizes wind-up,
projectiles and recoil, keeps the feet at deck height, and plays the destruction
sequence before awarding victory.

The original hand-built versions are in `handcrafted/`, with their editable construction in `../tools/build_art.py`. Drones, ammunition, explosions, repairs, and the font remain native pixel art. The soundtrack and effects are original synthesized material in `../../sound/`.
