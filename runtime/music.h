/* Music packs (music.c): the game's songs replaced by OGG / FLAC recordings. */
#ifndef MUSIC_H
#define MUSIC_H
int music_enable(int on);                  /* on/off (off in replays of bug reports); returns the songs the pack has */
int music_reload(void);                    /* look at the music folder again; returns the songs the pack has */
void music_set_folder(const char* dir);    /* default "music" */
void music_set_volume(int percent);        /* the recordings' level, 0-200 (default 70) */
int music_song_count(void);                /* the game's songs */
int music_now(char* out, int n);           /* the song that started last; 1 once per new song */
int music_write_list(const char* path);    /* songs.txt */
int music_export(const char* dir, void (*progress)(int done, int total));   /* the songs as WAV files (music_export.c) */
#endif
