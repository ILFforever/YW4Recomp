# Fonts of the in-game overlay

The overlay (`runtime/src/gamemenu.c`, drawn by `runtime/src/ui.c`) uses the fonts of its design: M PLUS
Rounded 1c for titles and buttons, Zen Kaku Gothic New for body text. Both are under the SIL Open Font License
1.1 (`OFL-*.txt`), which allows shipping them with the program; `recomp.build` builds them into `yw4.exe`.

Source: github.com/google/fonts at commit `bbda7253fc363238b05d97515ca71f8ae49c4fa9`
(`ofl/mplusrounded1c/`, `ofl/zenkakugothicnew/`), the weights the design uses. They were cut down to the
characters the overlay draws (Latin, Latin-1, general punctuation, arrows, the check mark), without hinting:

```
python -m fontTools.subset <font>.ttf --unicodes="U+0020-007E,U+00A0-00FF,U+2010-2027,U+2030-203A,U+2122,U+2190-2193,U+2713" \
    --layout-features='kern,liga' --no-hinting --desubroutinize --output-file=<font>.ttf
```

The OFL lets a modified version keep the font's name unless the name is a Reserved Font Name; neither font
declares one.
