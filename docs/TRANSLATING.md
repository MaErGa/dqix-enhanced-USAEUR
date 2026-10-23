# Translating the game's interface

The menus, messages and dialogs of the PC game can be shown in several
languages (**Language / Idioma** menu). Right now: English and Spanish.

This is separate from the game's own text, which comes from the ROM. The
Language menu has a second part, *Game text*, that sets the DS language the
game starts with; it only does something with a ROM that has that language
(the European Dragon Quest IX has English, French, German, Italian and
Spanish; the USA one is English only).

## How it works

Every text in the code is written in English inside `tr("...")` (or `N_("...")`
where it is translated later, or `item(...)`, `sub(...)`, `W("...")` in the
Windows menus). `tr()` returns the translation if the current language has one
and the English text if not, so a new or changed English text never breaks
anything: it just shows in English until it is translated.

Translations are plain gettext files, `lang/<code>.po`, with the English text as
`msgid`. Open them with Poedit or any text editor.

## Adding a language

1. Copy `lang/es.po` to `lang/<code>.po` (`fr`, `de`, ...), change the header
   (`Language:` and `X-Native-Name:`) and translate each `msgstr`.
2. Run `python3 tools/lang_to_c.py`: it writes `runtime/lang_tables.c`.
3. Run `python3 tools/lang_to_c.py --check` to see what is missing, what is
   no longer used, and mistakes that would crash the game.

Rules the check enforces: keep the `%s`, `%d`... in the same order as the English,
the same number of tab characters (`\t`: what follows is the key shown in the
menu), and the same number of `&` (the underlined letter of a menu entry; pick
one that no other entry of the same menu uses).

The language of the operating system is used the first time; the choice is saved
in `settings.ini` (`language = es`). `game_language = auto` follows the system.
