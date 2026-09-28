# Verification, 2026-09-27

- Game rules: passed movement/combat/damage/EMP/pause/boss/restart/determinism tests.
- Seven A1200 scenarios: passed; reviewed goldens for title, combat, boss, pause, victory and restart.
- Harness: 69 tests passed, using only the Python standard library.
- Snapshot check: two cached runs and one fresh boot produced byte-identical stride, charge, recoil, destruction and victory screenshots, serial log, and audio. Hashes are in `build/determinism/VERIFIED.txt`.
- `tools/selftest a500-aros`: completed successfully, including the template round trip.
- Full `tools/selftest`: IRON WRAITH and the other completed examples passed; stopped at the separately edited VOIDRUNNER (3 screenshot-test mismatches and 2 performance-test failures). Its files and goldens were not modified by this task.
- Playable FS-UAE A1200 launch visually checked. Real hardware remains untested.
- 32.0-second showcase recorded at 50 fps with sound, no dropped frames, peak 94% frame load. Includes deployment, two waves, EMP, MOLOCH and victory.

Scenario measurements (current soundtrack and boss animation build):

- boot: 0 dropped, peak 57%
- boss: 0 dropped, peak 91%
- boss_animation: 0 dropped, peak 92%
- combat: 0 dropped, peak 91%
- mission: 0 dropped, peak 92%
- pause: 0 dropped, peak 91%
- restart: 0 dropped, peak 91%

Goldens were reviewed and updated for the new boss poses on title, defeat and
boss screens, and for the winning route's changed score/player position.
The new boss-animation scenario captures two stride poses, wind-up, recoil,
reactor rupture, burning armor and victory. Combat and pause goldens still match.

Music: 58.2-second, 32-bar original arrangement at 132 BPM; eight synthesized
sampled instruments. Reviewed the generated spectrogram and recorded the actual
Paula mix in the showcase. Music plus effects use 74,174 bytes of chip samples.
