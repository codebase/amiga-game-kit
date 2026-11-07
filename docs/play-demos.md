# Play the demos

Download a ZIP, extract it, and put its `.adf` file in your emulator's first
floppy drive (DF0). These are bootable game disks; you don't need the kit,
Docker or a compiler to play.

| Game | Download | Machine setting |
|---|---|---|
| **IRON WRAITH**, mech combat | [AGA demo ZIP](https://github.com/codebase/amiga-game-kit/releases/download/v0.1.0/ironwraith-aga-1.0.zip) | **A1200, AGA, 68020, 2 MB chip RAM, PAL, Kickstart 3.1 for A1200** |
| **VOIDRUNNER**, space shooter | [Demo ZIP](https://github.com/codebase/amiga-game-kit/releases/download/v0.1.0/voidrunner-1.0.zip) | A500, OCS, 512 KB chip + 512 KB slow RAM, PAL, Kickstart 1.3 |
| **REDLINE**, road racer | [Demo ZIP](https://github.com/codebase/amiga-game-kit/releases/download/v0.1.0/racer-1.0.zip) | Same A500 settings |
| **MUSHROOM RUN**, platformer | [Demo ZIP](https://github.com/codebase/amiga-game-kit/releases/download/v0.1.0/sidescroller-1.0.zip) | Same A500 settings |

Use [FS-UAE](https://fs-uae.net/) or [WinUAE](https://www.winuae.net/), select the
machine above, mount the disk in DF0 and boot. Configure a keyboard or gamepad
as the joystick in **port 2**. Use your own licensed Kickstart ROM; the downloads
contain no ROMs. The A500 games also pass the kit's free AROS profile tests.

## Controls

| Game | Controls |
|---|---|
| IRON WRAITH | Joystick to move; fire to deploy/shoot; X or second fire button for EMP; P to pause; M to toggle music |
| VOIDRUNNER | Fire to start; joystick to move; hold fire to shoot |
| REDLINE | Fire to start; up or fire to accelerate; down to brake; left/right to steer |
| MUSHROOM RUN | Left/right to run; up or fire to jump; land on mushrooms to stomp them |

Esc exits each game. With the kit's `agk play`, movement is on the arrow keys
and fire is Space. Other emulator keyboard mappings depend on your settings.

IRON WRAITH requires **AGA** and will not run on an A500. All four are tested
in emulation; physical Amiga validation is still outstanding.

Already have the kit? Launch directly:

```sh
tools/agk play examples/ironwraith
tools/agk play examples/voidrunner
tools/agk play examples/racer
tools/agk play examples/sidescroller
```

[Release notes and all downloads](https://github.com/codebase/amiga-game-kit/releases/tag/v0.1.0)
· [Build your own game](first-game.md)
