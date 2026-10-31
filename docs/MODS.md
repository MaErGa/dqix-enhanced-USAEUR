# Mods: texture packs and music packs

Both live in folders next to the game (`textures` and `music`) and are
turned on and off in the **Mods** menu. Nothing in the game's files is
changed: take the folder away and the game is as it was.

## Texture packs (Enhanced mode)

PNG pictures anywhere in the `textures` folder (outside `textures\dump`)
are drawn instead of the game's textures. Each is named after the texture
it replaces.

To make one:
1. Turn on **Mods > Dump textures** and play. Every texture shown is saved
   in `textures\dump`.
2. Redraw or upscale those PNGs. Keep their names and their see-through
   parts. A bigger picture than the original is fine.
3. Put them in `textures` (any subfolder except `dump`) and choose
   **Mods > Reload the texture packs**.

Hold **`** (the key left of 1) to see the game's own textures and compare.
**Upscale the 3D textures** enlarges the game's own textures 4x with xBR;
a pack's pictures come first.

## Music packs

OGG (Vorbis), FLAC or WAV recordings in the `music` folder play instead of the
game's songs. Each is named after the song it replaces: `BG_012.ogg`,
`ME_003.flac`. A title after the name is fine (`BG_012 - Town.flac`), and
so is `BG_012.ogg.flac` (what renaming gives when Windows hides
extensions): the file's contents say whether it is OGG or FLAC.

- **Mods > Open the music folder** opens it and writes `songs.txt`, the
  list of every song (`BG_` songs, `ME_` jingles).
- **Mods > Show each song's name as it starts** puts the name in the
  window's title, so you can tell which one is playing.
- **Mods > Reload the music** after adding files: each takes over the next
  time its song starts.
- **Mods > Export the game's songs** writes every song as a WAV file to
  `music\export` (about 650 MB, a minute or two), made from the ROM's own
  sequences and instruments: close to the game's sound, not exact. Each
  file holds the intro and one pass of the loop, and `loops.txt` lists the
  loop points (as `LOOPSTART` / `LOOPLENGTH` in samples) to line a
  recording up with. A WAV with its loop (the "smpl" chunk, which sound
  editors and samplers keep) also works in the `music` folder, so an
  exported song can be edited and put back.

Looping: songs loop between the `LOOPSTART` and `LOOPSTART + LOOPLENGTH`
tags (or `LOOPEND`), in samples, the usual tags of game music rips.
Without them, songs loop the whole file and jingles play once.

The game's own song keeps playing silently (every note at volume 0), so
its timing, loops and anything the game waits for stay exactly the same;
the recording follows the game's fades, pauses and stops.
`music_pack_volume` in `settings.ini` (0 to 200, default 70) sets how loud
the recordings are next to the game's sound effects.

While playing together (Wireless), the game's own music plays: the
players' games must stay identical in memory, and a silenced song is a
change in memory. Replays of bug reports also leave music packs off.

Packs you share should contain only your own recordings or ones you have
the right to share.
