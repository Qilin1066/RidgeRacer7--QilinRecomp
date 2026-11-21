# The game's data files

What is in `PS3_GAME/USRDIR` beside the program, as read from the owner's disc on the owner's PC (2026-11-21, v0.2.0). `qilin data <game>` prints the list and what the headers of the sound banks and films say; `Analyse game.bat` writes it to `analysis-out\data.txt`.

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

Surveyed by sector (2048 bytes), with no directory to go by:

- 2,554,587 sectors; 212 of them hold nothing but zeros, in short gaps between 65 runs of data. The largest unbroken run is 1,717,767 sectors (3.5 GB) from sector 281.
- **The file doesn't begin with a directory.** Its first `0x360` bytes are zero, and what follows is a file's contents. So where each file lies is recorded somewhere else: *in the program, most likely* (the read-only data segment is 1.3 MB), to be found when the decrypted program is read.
- **At least some files are compressed** with a byte-oriented LZ scheme: the first one is a text file whose words can be read between the control bytes. A megabyte sampled at 39 places across the archive has 6.2 to 7.9 bits of entropy per byte, so it is a mix of compressed and plain data.
- Tags that begin a sector many times, and so look like the starts of files of the game's own formats: `ArcL` (88 times), `R7W` (84), `R7T` (53), `RR7 ` (44). *What they are is not known yet.*

No `qilin` command reads the archive yet: without its directory there is nothing sound to list.

## What this decides

- **Films need an MPEG-2 video decoder**, and three of them an ATRAC3plus decoder. MPEG-2 video's patents have expired (by public accounts: to be checked before any decoder ships), and a decoder for it is a small, well-specified thing next to AVC's: the project can write its own, with no library added (`roadmap-and-design-targets.md`).
- **Music needs an ATRAC3plus decoder, in 5.1.** There is no way round it: the music is the game. This is the larger piece of work; the choice between writing one and taking a library is still open.
- The announcer's sounds need no decoder.
- Whether the game decodes these itself (on SPUs) or through the system's libraries is for the imports to say: the program links `libatrac3plus_stub`, `libadec_stub`, `libvdec_stub`, `libdmux_stub` and `libpamf_stub`, which points at the system's decoders.
