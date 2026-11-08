Star Wars: Rogue Squadron (N64 recomp) - New Nintendo 3DS, test build
====================================================================

NEW 3DS / NEW 3DS XL / NEW 2DS XL ONLY.

The game comes in two parts: rs64.3dsx (the port, from the releases
page) and game.bin (the game code, made from your own ROM on your
computer, once, by the setup). Keep game.bin to yourself.

Updates: at each start (with Wi-Fi) the game checks for a newer
release and offers to install it. If it ever says game.bin must be
rebuilt, run the setup on your computer again. An empty
no_update_check.txt in this folder turns the check off.

Controls
   Circle Pad      stick
   A               A (fire lasers)
   B               B (secondary weapon)
   R / ZR          Z
   L               R
   ZL              L
   X / Y / SELECT  C-up / C-left / C-down
   C-stick         C buttons
   D-pad, START    D-pad, START
   L + R + START + SELECT: quit

Sound: the game's audio is capped at 22050 Hz to save CPU. To try a
lower rate (less CPU, duller sound), put a file audio_rate.txt in this
folder containing just a number, e.g. 16000. Delete it to go back.

Screens: the bottom screen shows the radar, enlarged, during missions.
The 3D slider turns on stereoscopic 3D (it draws the scene twice, so
it costs some speed; slide it down for the fastest game).

Widescreen: tap the bottom screen to switch between the original 4:3
picture and widescreen, which shows more of the world to the sides
and fills the whole top screen. The choice is kept in aspect.txt.

Speed: the game builds its next frame while the 3DS draws the current
one. If you see flickering or broken graphics, put an empty file
gfx_sync.txt in this folder to turn that off (slower, but safe), and
report it.

Frame dump (for bug reports): hold L + R and tap the bottom screen;
the next frame's draw details are written to rs64_log.txt.

The bottom screen shows a log while starting; everything also goes to
/3ds/rs64/rs64_log.txt. If it crashes, send that file and the Luma
crash dump (sdmc:/luma/dumps/arm11/).
