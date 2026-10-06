# Vendored third-party code

| File | Project | Commit | License |
|---|---|---|---|
| dr_flac.h | [mackron/dr_libs](https://github.com/mackron/dr_libs) | dfe8377631000664666519fdb83da193fd8037f4 | Unlicense OR MIT-0 |
| minimp3.h | [lieff/minimp3](https://github.com/lieff/minimp3) | ea99364f61c14656440e8d77e9c233ccf3124633 | CC0-1.0 |
| stb_vorbis.c.inc | [nothings/stb](https://github.com/nothings/stb) `stb_vorbis.c` | 2c980bb59875b0d32144a71867fbdebb2f77cd20 | MIT OR Unlicense |

The contents are copied unchanged. `stb_vorbis.c` is renamed to `stb_vorbis.c.inc` because it is
compiled only as part of `src/decoders/dec_vorbis.c`; Arduino compiles every `.c` file under
`src/`, and a second copy of the decoder would clash at link time.

Update a file by replacing it and the commit hash here.
