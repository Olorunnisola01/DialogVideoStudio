# Dialog Video Studio

A native Windows desktop app (Qt 6 / C++20, no Python at runtime) that turns an
MP3 voiceover plus a Whisper-style SRT into a finished MP4, with each speaker
getting their own coloured subtitle box in their own place on screen.

## Download (no install)

`portable/DialogVideoStudio-Portable.exe` is a single self-contained file.
Download it, put it somewhere writable, and double-click - there is nothing to
install. No Qt, no Visual C++ redistributable, no ONNX Runtime, no ffmpeg on
PATH, and no model download.

First run unpacks ~313 MB into a `DialogVideoStudio\` folder beside the exe
(about 13 seconds, with a progress window) and then starts the app. Later runs
start immediately. Allow ~600 MB of free space next to the exe.

Rebuild it with `.\packaginguild_portable.ps1` after building the app into
`dist\`.

Workflow:

1. **Audio** - open the MP3. It is decoded to a waveform on the timeline.
2. **Subtitles** - open the SRT. Whisper cue boundaries land mid-sentence, so
   the app rebuilds the text into one subtitle per sentence.
3. **Split speakers** - assigns each line to a speaker from the audio itself.
4. **Pair English** - folds each spoken translation into the line it translates
   so both are on screen together.
5. **Style** - drag any box on the live preview: the speaker's caption, the
   English line, or an overlay. Colours and fonts are on the right.
6. **Images** - drop scene pictures onto the timeline and drag their edges.
7. **Overlays** - add a logo, a subscribe button and a title banner, positioned
   by dragging.
8. **Export MP4**.

The right-hand panel has three tabs - **Speakers**, **English**, **Overlays** -
and switching tabs moves the drag handles in the preview onto the box that tab
edits.

## Building

Requires Qt 6.8.1 (msvc2022_64), Visual Studio 2022 build tools, and CMake.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.1/msvc2022_64"
```

```bash
cmake --build build --config Release
```

Kill any running instance before rebuilding:

```bash
powershell -c "Get-Process DialogVideoStudio -EA SilentlyContinue | Stop-Process -Force"
```

### One-time dependency fetch

Three binary dependencies are too large to commit and are fetched by a script -
run this once before the first build:

```bash
powershell -ExecutionPolicy Bypass -File scripts/fetch_deps.ps1
```

It installs `tools/ffmpeg.exe` (copied from an existing install if there is
one), ONNX Runtime 1.20.1 + DirectML into `third_party/onnxruntime/`, and
`models/wespeaker_en_voxceleb_CAM++.onnx` (~28 MB, from the sherpa-onnx GitHub
releases). Each step degrades gracefully: without the model, speaker splitting
falls back to the text check described below; without a bundled ffmpeg, the app
uses one from `PATH`, `DVS_FFMPEG`, or Settings.

### Packaging

One command rebuilds the app *and* refreshes both `dist/` and the ZIP:

```bash
cmake --build build --config Release --target dist
```

`dist/` is a self-contained folder (~310 MB, mostly ffmpeg and DirectML) that
runs on a machine with no Qt installed; the ZIP beside it is ~135 MB.

> **Run `dist` after every build you intend to hand out.** `dist/` is a snapshot,
> not a link — a stale one launches an older app whose newer features simply
> aren't there, which looks exactly like features having been removed. Compare
> the timestamps of `dist/DialogVideoStudio.exe` and
> `build/Release/DialogVideoStudio.exe` if anything looks missing. During
> development, run `build/Release/DialogVideoStudio.exe` directly.

## How speaker splitting works

Reliability comes from three signals that are cross-checked rather than from
trusting any one of them:

1. **Voice embeddings.** Energy VAD drops silence, then a 1.5 s window hopping
   every 0.25 s is turned into 80-bin Kaldi-compatible log-mel features
   (`src/core/Fbank.cpp`) and run through the CAM++ ONNX model. The embeddings
   are clustered with average-linkage agglomerative clustering on cosine
   distance. Windows deliberately ignore subtitle boundaries, so a speaker
   change *inside* a line is still found and the line is split there.
2. **Language of the text.** Each line is scored German-vs-English from
   diacritics and stopwords. This is computed independently of the audio.
3. **Reconciliation.** Where the two disagree, or where the window vote is not
   decisive, the line is highlighted in the table with the reason in its
   tooltip. A file with no highlighted rows is trustworthy; a highlighted row
   tells you exactly which line to listen to. Every row stays editable - pick a
   speaker from the dropdown, or select several rows and press Ctrl+1..4.

If the model is missing or cannot run, the app says so and falls back to the
language check alone rather than guessing silently.

On this machine the DirectML EP fails on the model's `AveragePool` kernel
(an AMD driver issue), so the app detects that on a probe inference and reloads
the session on the CPU automatically. A 43 s file takes ~25 s to diarize on CPU.

## Word colours

Any word can be given its own colour (and optionally bold) so the target
vocabulary stands out inside the caption:

- **Right-click any line** in the segment table → *Colour word* → pick the word.
  This is the fast path; the swatch next to a word shows it is already coloured.
- **Ctrl+H** (or the *Word colours* toolbar button) opens the full table, where
  you can add words by hand, pick from the words that actually occur in your
  subtitles (ordered by frequency), change colours and toggle bold.

Matching ignores capitals and punctuation, so one entry for `wohl` covers
`Wohl`, `wohl?` and `wohl,`. Only the letters take the accent colour -
punctuation stays in the normal text colour. The rule is project-wide, so a
word stays highlighted in every line it appears in.

From the CLI:

```bash
build/Release/dvs_cli.exe --project x.dvsproj --highlight "fuehlst=#1E6FE8" --highlight "wohl=#E8341E:bold" --frame 5000 --png out.png
```

## The second subtitle line

There are two ways to fill it, on the **English** tab.

### 1. Pair it out of the recording (offline, no key)

Preferred when your audio speaks each sentence and then its translation - you
get your own wording back, nothing leaves the machine. **Ctrl+T** (or *Pair
English* on the toolbar) folds the two together: the caption and its
translation appear at the same time, and the pair stays on screen for the whole
stretch of audio that covers both.

Pairing handles both rhythms found in this material:

- *German, English, German, English* - the pair becomes one row spanning both.
- *German, German, English, English* - each source line is matched with the
  translation in the same position, and the pair is shown again while its own
  translation is read. The extra rows are marked in their tooltip; **Undo
  pairing** removes them exactly.

Lines the scorer cannot confidently classify are left alone, and the
**Translation** column in the segment table is always editable, so anything the
recording does not cover can be typed in by hand.

### 2. Machine translation

For material where the translation is not spoken. **Translating sends your
subtitle text to the service you choose** - the app names the service and the
line count and asks before it does.

| Service | Key | Notes |
|---|---|---|
| Google Translate | none | The endpoint Google Translate's own web page calls. Undocumented, so it can rate-limit or change without notice. Good quality, best default for a quick pass. |
| MyMemory | none | Documented free tier, modest daily allowance. Noticeably weaker - it mistranslated "halb neun" as "half past two" in testing. A fallback, not a first choice. |
| OpenAI, Anthropic, Groq, OpenRouter | yes | Best quality. Sent in batches with the neighbouring lines as context, so register and continuity hold across a scene. |

The keyed services get a system prompt describing the job (subtitles for a
language-learning video, natural spoken register, never merge or drop a line)
and are asked for a JSON array the app maps back onto the segments. If a reply
comes back with the wrong number of lines the batch is retried one line at a
time rather than risking a silent off-by-one across the whole scene.

The Anthropic path uses **structured outputs** (`output_config.format` with a
JSON schema), so the reply is guaranteed to parse - no fenced-JSON guesswork -
and opts into `fallbacks: "default"` so a request declined by a safety
classifier is re-run on Anthropic's recommended fallback model inside the same
call instead of failing. It reads the text content block by type rather than by
index, because thinking blocks come first and carry no text.

**API keys** are encrypted with the Windows Data Protection API before being
stored, so the value in the registry is tied to your Windows account and is
useless if copied elsewhere. Keys are never displayed once saved and never
written to the project file. *Test* sends a single line so you can check a key
and a language pair without touching the project.

The English line has its own style - background colour, opacity, font,
position - on the **English** tab. It defaults to white on a 35%-black band low
in the frame; set the opacity to 0 for text with no background at all.

## Merging a speaker's consecutive lines

Sentence splitting cuts at every full stop. That is right when speakers
alternate, but when one person says two sentences in a row it leaves their turn
as two captions flashing one after the other.

**Ctrl+M** (or *Merge lines* on the toolbar) folds each run of consecutive lines
by the *same* speaker into one caption that stays up for the whole run. On the
sample file, *"Bitte benutze diese Tür."* + *"Sie ist geöffnet."* become a single
caption spanning 9.2 s to 13.0 s.

Three guards stop it producing captions nobody can read, all adjustable in the
dialog: the largest gap it will join across (900 ms), the longest merged caption
(180 characters) and the longest time on screen (9 s). Lines with no speaker are
never merged. *Undo merging* restores the original lines exactly — each one is
kept verbatim inside the merged caption, because joining the texts with a space
is not reversible on its own.

Order does not matter: merging before or after pairing gives the same result, and
the two compose well. On the sample, `--merge --pair` turns 12 raw lines into 5
captions, each carrying a merged German line and its merged English translation.

## Joining batches into one long video

Build each stretch of a video as its own project — its own artwork, caption
positions, overlays and title — then join them. **File → Join batches into one
video**, or *Join batches* on the toolbar.

The list takes two kinds of part, in any mix and any order:

- **Saved projects** (`.dvsproj`), rendered when you build.
- **Videos you already have**, used as they are.

Everything is brought to one shape first, so the finished file never changes
size mid-play. Projects are rendered at the chosen size directly; an existing
video of a different shape is **fitted inside and padded**, never cropped or
stretched — a landscape clip dropped into a portrait video is letterboxed.

Because every part is written with identical codec and audio settings, the join
itself is normally a **stream copy**: instant, and the parts are never
re-compressed a second time. If something still does not line up the join falls
back to a re-encode rather than failing, and the dialog says which happened.

Verified on three parts (two projects plus an existing landscape MP4):
129.42 s out, exactly 43.1 × 3, joined by stream copy.

> Unsaved changes are not picked up — parts are rendered from their saved files,
> so the dialog offers to save the current project first.

## Caption fades

Captions do not pop in and out. Each one fades over **140 ms** by default
(*Video → Settings → Caption fade*; 0 restores hard cuts), and the shape of the
fade depends on how much room the caption has:

- **With a gap either side**, the fade straddles the timestamp — half before,
  half after — so the caption changes exactly on time with a soft edge.
- **Where two captions touch** (the common case after pairing), they hand over
  *sequentially*: the outgoing one is fully gone at the instant the incoming one
  starts. Cross-dissolving them is smoother in the abstract, but two different
  sentences drawn over each other at half opacity is an unreadable
  double-exposure — the point is to be easier on the eyes, not harder.

The body of every caption is untouched: a frame in the middle of a line renders
**pixel-identically** with fades on or off, so nothing shifts in time and
nothing loses contrast where it matters. The ramp uses a smoothstep curve, so
there is no visible corner where the fade starts or stops.

The translation line fades with its caption — they are one unit. Overlays with a
time range fade the same way; overlays that run for the whole video never fade.

*Caption slide* (default 0) adds a small upward drift as a caption fades in.

Fades cost nothing at export: the frame cache is keyed on the quantised opacity,
so a fade re-renders once per visible step and static stretches still re-send the
same buffer. Measured on the sample project, 1080x1920: 68 s with fades, 71 s
without.

## Overlays

The **Overlays** tab pins pictures and text over the video, so no separate
editor is needed:

- **Logo** - pick a PNG; lands bottom-left.
- **Subscribe** - pick a PNG; lands bottom-right.
- **Title** - type the text; a coloured banner across the top.
- **Image** - any other picture, e.g. a watermark or an end-card.

Each overlay has opacity, an optional time range (or the whole video), a
*keep proportions* toggle for pictures, and a *draw over the subtitles* toggle.
Position and size come from dragging the box in the preview, exactly like a
caption. Transparent PNGs are composited with their alpha intact.

## Rendering

`src/render/FrameRenderer.cpp` composes every frame with QPainter and is used by
*both* the preview and the exporter, so what you position on screen is what
lands in the MP4. Draw order is scene image, overlays, caption, English line,
then any overlay marked *draw over the subtitles*.

All geometry is stored as fractions of the canvas. Box positions follow each
axis; type sizes, padding and radii follow the canvas's *shorter* side, so
portrait video does not get absurdly large text. Switching between a wide and a
vertical preset offers to rearrange the boxes, since a 16:9 layout is unusable
in 9:16.

The exporter pipes raw BGRA frames straight into ffmpeg's stdin - no PNG temp
files. The picture only changes at segment and scene boundaries, so a 43 s
1080p30 export renders ~12 frames and re-sends the buffer for the other ~1280.

## Headless driver

`dvs_cli.exe` runs the whole pipeline without the UI, which is the fastest way
to check a change:

```bash
build/Release/dvs_cli.exe --srt in.srt --audio in.mp3 --speakers 2 --dump-segments
```

```bash
build/Release/dvs_cli.exe --srt in.srt --audio in.mp3 --image a.png --image b.png --out out.mp4
```

Useful flags: `--frame <ms> --png out.png` to render a single frame,
`--project x.dvsproj` to load/save a project, `--no-diarize`, `--size 1080x1920`,
`--pair` / `--unpair`, `--logo path`, `--subscribe path`, `--title "Im Café"`,
`--translate <provider> --source-lang de --target-lang en [--model id] [--api-key k]`,
`--transition <ms>` / `--rise <fraction>`, `--merge` / `--unmerge`
(`--merge-gap`, `--merge-chars`).

Joining is a standalone mode:

```bash
build/Release/dvs_cli.exe --stitch final.mp4 --size 1080x1920 --add a.dvsproj --add b.dvsproj --add extra.mp4
```

A full vertical build in one command:

```bash
build/Release/dvs_cli.exe --srt in.srt --audio in.mp3 --speakers 2 --pair --size 1080x1920 --image scene.png --logo logo.png --subscribe sub.png --title "Im Café" --out out.mp4
```

## Layout

| Path | What lives there |
|---|---|
| `src/core/SrtParser.*` | SubRip parsing, tolerant of Whisper's variations |
| `src/core/Segmenter.*` | cues -> word stream -> sentence segments |
| `src/core/AudioDecoder.*` | ffmpeg-backed decode to mono 16 kHz + waveform peaks |
| `src/core/Fbank.*` | Kaldi-compatible log-mel filterbank |
| `src/core/Diarizer.*` | embeddings, clustering, language cross-check |
| `src/core/Translator.*` | pairing each line with its spoken translation |
| `src/core/TranslationService.*` | machine translation across the six providers |
| `src/core/SecretStore.*` | DPAPI-encrypted API-key storage |
| `src/core/GpuDevice.*` | DXGI adapter pick + DirectML EP, CPU fallback |
| `src/core/Project.*` | the `.dvsproj` model |
| `src/render/VideoStitcher.*` | joining several batches into one video |
| `src/render/` | frame rendering and MP4 export |
| `src/ui/StyleEditor.*` | the shared style form used by all three layers |
| `src/ui/` | main window, preview canvas, segment table, panels, timeline |
