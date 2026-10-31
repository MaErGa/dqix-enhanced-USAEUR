# Roadmap

What is planned for DQIX Enhanced (USA and EUR). Nothing here is promised; the order is a guess.

## Next

- **More interface languages** (French, German, Italian). The system is ready (`lang/es.po` is the model,
  docs/TRANSLATING.md says how); each one needs a native speaker to review it.
- **Check the Windows installer, updater and online sessions on real PCs** and fix what shows up.
- **More window options**: remember the window's position, borderless fullscreen, a frame limiter.
- **Download data for the European version** (DQVC and the downloadable quests): the files in `wfc/` are
  the USA ones.

## Maybe

- **The Japanese version.** Its code is a different build: the function list the recompiler needs would have
  to be made for it.
- **Save states.** Not possible as things are: the game's threads run on native stacks that can't be saved
  safely. Exporting the save (*Game > Save file*) is the safe copy.
- **Controls**: stick sensitivity and profiles (the keys and buttons are already changeable in
  *Game > Controls*).
