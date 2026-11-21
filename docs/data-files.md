# The game's data files

What is in `PS3_GAME/USRDIR` beside the program, as read from the owner's disc on the owner's PC (2026-11-21, v0.2.0; the archive's directory and the shaders at v0.5.0). `qilin data <game>` prints the list and what the headers of the sound banks and films say; `Analyse game.bat` writes it to `analysis-out\data.txt`.

Everything here was read from the files themselves. The layouts are **as observed**: nothing documents them for this project, so each field below is one whose meaning the files make plain (a count that matches, an offset that lands), and what is only inferred says so. The readers (`recomp/src/media.*`) stop with an error on anything else: a sound of an unknown kind, a stream of an unknown coding, a table that doesn't add up.

| File | Bytes | |
|---|---|---|
| `RR7.DAT` | 5,231,794,176 | the game's archive |
| `BGM.nub` | 615,863,504 | sound bank: the music |
| `Game_DJ.nub` | 152,147,600 | sound bank: the announcer |
| `ending.pam` | 614,176,768 | film |
| `intro.pam` | 164,446,208 | film |
| `e3.pam` | 151,226,368 | film |
| `title2.pam` | 76,324,864 | film |
| `title.pam` | 73,951,232 | film |
| `auroravision.pam` | 59,576,320 | film |
| `menu.pam` | 49,606,656 | film |

## Films (`.pam`)

All seven are **PAMF version `0041`**: a 2048-byte header, then an MPEG-2 program stream.

| Film | Seconds | Picture | Sound |
|---|---|---|---|
| `intro` | 53.5 | MPEG-2 video, 1280 × 720 | ATRAC3plus |
| `ending` | 302.9 | MPEG-2 video, 1280 × 720 | ATRAC3plus |
| `e3` | 68.9 | MPEG-2 video, 1280 × 720 | ATRAC3plus |
| `menu` | 35.0 | MPEG-2 video, 1280 × 720 | none |
| `title` | 30.0 | MPEG-2 video, 1280 × 720 | none |
| `title2` | 30.0 | MPEG-2 video, 1280 × 720 | none |
| `auroravision` | 465.5 | MPEG-2 video, 320 × 176 | none |

**The films' picture is MPEG-2 video, not AVC**, in every one of them, and the stream's rate is at most 6,000,000 bytes a second. Three have one sound stream each, ATRAC3plus. (`PS3_GAME/ICON1.PAM`, the animated icon the console's menu shows, is the one AVC film on the disc; the game doesn't play it.)

The header's fields the reader uses, all big-endian:

| Offset | |
|---|---|
| `0x00` | `PAMF`, then the version as four digits |
| `0x08`, `0x0c` | where the stream starts and how long it is, in units of 2048 bytes |
| `0x50` | the size of the table that follows: `0x34` and `0x30` for each stream |
| `0x56`, `0x5c` | the first and last presentation times, 48 bits each, 90,000 to the second |
| `0x62` | the stream's greatest rate, in units of 50 bytes a second |
| `0x86` | the number of streams |
| `0x88` | the streams, `0x30` bytes each: the coding at `+0` (`0x02` MPEG-2 video, `0x1b` AVC, `0xdc` ATRAC3plus), the program stream's id at `+4` (`0xe0` picture, `0xbd` private) and a sub-id at `+5`; for a picture, its width and height in 16-pixel blocks at `+0x18` and `+0x1a`, and for MPEG-2 in pixels at `+0x1c` and `+0x1e` |

In a sound stream's entry the byte at `+0x12` is 6 in all three films. *That it is the number of channels (5.1, as the music is) is inferred.* The times and the rate are repeated further on in the header; the reader takes the first copy.

## Sound banks (`.nub`)

A 32-byte header, a table of offsets (one for each sound), the sounds' headers, then the sounds' data. Big-endian.

| | Sounds | Format | In all |
|---|---|---|---|
| `BGM.nub` | 43 | **ATRAC3plus, 6 channels, 48,000 Hz**, 48,000 bytes a second (384 kbit/s) in blocks of 2048 bytes | 12,830 seconds |
| `Game_DJ.nub` | 714 | **16-bit big-endian PCM, 1 channel, 44,100 Hz** | 1,724 seconds |

So the music is 5.1 ATRAC3plus and the announcer is plain PCM.

The bank's header: `0x00020100` (a version), 0, the bank's id (`0x2bc` and `0x258` here), the number of sounds, where the data starts, its size, where the table of offsets is, where the first sound's header is.

A sound's header (`0xd0` bytes for PCM, `0x100` for ATRAC3plus):

| Offset | |
|---|---|
| `0x00` | its kind: `wav` or `at3`, and a zero byte |
| `0x04` | the bank's id |
| `0x08` | its number, from 0 |
| `0x14`, `0x18` | its data's size, and offset within the bank's data |
| `0x1c` | the size of its format block |
| `0xbc` | the format block: for `wav`, the 16 bytes of a wave file's PCM format, big-endian; for `at3`, the block an ATRAC3plus RIFF file would hold (`WAVE_FORMAT_EXTENSIBLE` with ATRAC3plus's sub-format id), **little-endian** as it is in such a file |

`BGM.nub`'s sounds lie end to end and fill the data exactly. `Game_DJ.nub`'s lie within it but not in order. In an `at3` header the two words at `0x20` and `0x24` are sample counts within the sound's length (zero in the last sound): *loop points, probably.* The floats between `0x30` and `0xbc` (1.0, -99.0, -100.0 and the like) are *presumably volume and placement settings*; the reader doesn't use them.

## The archive (`RR7.DAT`)

**Its directory is in the program** (v0.5.0): two tables in the program's data, found and checked by `qilin archive` (`recomp/src/archive.*`). `qilin archive <game> --files` lists every pack and file; `Analyse game.bat` writes that to `analysis-out\archive.txt`.

| | |
|---|---|
| Files | 12,810, under 466 different names |
| Packs | 2,088, each with a name of its own |
| Compressed packs | 908: 4,698,407,702 bytes stored, 10,220,050,505 unpacked |
| Plain packs | 1,180: 532,465,664 bytes |
| The archive's size by the directory | 5,231,794,176 bytes: **`RR7.DAT`'s size exactly** |

- The archive is a run of **packs**, one after another in 2048-byte sectors from sector 0 to the last, with no gaps. A pack is a few files that are loaded together: a car, a course's part, a menu screen.
- A pack is stored plain or **compressed as a whole**. A compressed pack's bytes are pushed to the *end* of its sectors: the slack is at the front (which is why the archive begins with `0x363` zero bytes, and why zero sectors turn up between runs of data). That is the layout for unpacking in place: read the sectors into the front of a buffer the size of the unpacked pack, and unpack from the back of what was read towards the front. *Inferred from the layout; the unpacking routine is in the program and will be lifted with it.*
- The file table (20 bytes a file): a position in sectors, the sectors it spans, its alignment (64 bytes, or 2048), its size, its offset in its pack's unpacked bytes, and the address of its name. File names are not unique: 845 packs have a `Tex`, 654 a `Mdl`.
- The pack table (36 bytes a pack, straight after the files): first sector, sectors, the first file's number and one past the last's, whether it is compressed, the stored and unpacked sizes, three more words (the first small and usually 0: *not understood*), and the address of its name.
- Five files belong to no pack, and two packs are empty. The last pack is one sector named for the archive itself.
- A file's first field is its first sector when its pack is plain. For a compressed pack it is some position in the unpacked image: *not understood, and not needed*.

What the names say is in it: textures and their shadow maps at several sizes (the bulk of the bytes), models, materials, the courses' collision and road data (`RoadPoly`, `RunOffArea`, `Danmen`), per-car packs (27 cars, by the names of their sound files), engine sounds (416 `EngineWave` packs), the menus' screens (`Obj_...`), fonts, text in each language, replays and ghosts.

**The compression is not decoded yet.** It is byte-oriented LZ with literal runs (text can be read between the control bytes). The tools don't need it for the game to run (the game's own routine is lifted), only for looking inside the packs.

## The shaders

**They are not in the archive: the program carries them** (`qilin shaders`, `recomp/src/shaders.*`). Its data holds **242 compiled Cg programs** end to end at `0x101ae628`: **121 vertex programs** (profile 7003) and **121 fragment programs** (profile 7004), binary format revision 6, 897,728 bytes of which 142,672 are microcode, with 11,348 parameters named in them. No file in the archive is named for a shader.

So the roadmap's open question is answered as hoped: the shaders are a closed set, known at build time, and can be translated to SPIR-V ahead of time from the player's own program. *Still to check: that nothing in the archive's material files carries microcode of its own.*

## What this decides

- **Films need an MPEG-2 video decoder**, and three of them an ATRAC3plus decoder. MPEG-2 video's patents have expired (by public accounts: to be checked before any decoder ships), and a decoder for it is a small, well-specified thing next to AVC's: the project can write its own, with no library added (`roadmap-and-design-targets.md`).
- **Music needs an ATRAC3plus decoder, in 5.1.** There is no way round it: the music is the game. This is the larger piece of work; the choice between writing one and taking a library is still open.
- The announcer's sounds need no decoder.
- Whether the game decodes these itself (on SPUs) or through the system's libraries is for the imports to say: the program links `libatrac3plus_stub`, `libadec_stub`, `libvdec_stub`, `libdmux_stub` and `libpamf_stub`, which points at the system's decoders.
