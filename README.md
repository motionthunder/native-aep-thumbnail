# Native AEP Thumbnail

![Native AEP Thumbnail — real previews for your AEP files](docs/banner.png)

**Real previews for your `.aep` files, right in Windows Explorer.**

Browse a folder of After Effects projects and see what is actually inside them —
the rendered frame, not an icon and not a metadata card.

> **Status:** beta `0.1.0`. Windows only. Works, tested on real project
> libraries, but the main-comp heuristic and the render settings are still being
> tuned.

<p align="center">
  <img src="docs/example.png" alt="Three AEP files showing their rendered content as Explorer thumbnails" width="880">
</p>

## Download

**[Get the installer from Releases](https://github.com/motionthunder/native-aep-thumbnail/releases/latest)** —
`NativeAEPThumbnail-0.1.0-Setup.exe`, about 2 MB.

Run it, click **Next** a few times, then open any folder with `.aep` files.
Previews appear within a few seconds. No building, no command line.

> **Windows SmartScreen** may say it *protected your PC*, because the installer
> is not code-signed yet. Click **More info → Run anyway**.

---

## Why this does not already exist

A PSD codec does not render anything. Photoshop stores a flattened composite
inside the file, and the codec simply decodes it. That is why PSD thumbnails
are instant and why third-party PSD codecs exist at all.

**An `.aep` contains no pixels whatsoever.**

Scan one and you will find zero JPEG/PNG/TIFF signatures anywhere in the file.
Its XMP packet carries only `CreatorTool`, some dates and a `DocumentID` — no
`xmp:Thumbnails`. An `.aep` is a big-endian RIFF container (`RIFX` + `Egg!`)
holding the project *graph*: compositions, layer parameters, keyframes and
references to footage.

There is nothing to decode. To see what a project looks like you have to
render it, and only the After Effects engine can do that — effects, text
layout, shapes, expressions, blend modes and all.

So AepThumb splits the job in two:

| Component | Role | Cost |
| --- | --- | --- |
| `aepthumb.dll` | Shell thumbnail provider. Displays an already-rendered frame from a cache. Renders nothing itself. | instant |
| `aepbake.exe` | Drives After Effects to render one frame per project into that cache. | ~15 s to start AE, then ~1.3 s per project |

Baking happens once per project version. After that the tile is free forever.

---

## How it behaves

1. You open a folder of projects — or save a new one from After Effects.
2. A project that has not been rendered yet shows the standard `.aep` icon for
   a few seconds.
3. The provider spools a copy of every such project, queues it, and starts the
   baker.
4. After Effects renders a frame per project in a **separate, hidden instance**,
   started once for the whole batch — even while you are working in your own
   After Effects, which is never touched.
5. The baker tells Explorer which files changed, and their tiles turn into the
   rendered frames by themselves. No refresh, no clicks. From then on they are
   instant, until the project is edited.

There is nothing to configure and no service to run.

Two things are deliberately never handed to Explorer while a frame is pending.
A placeholder image would be written to Explorer's own thumbnail cache and then
shown instead of the real frame, so the provider returns nothing and the
standard icon stays up. And a project that cannot render — no compositions, or
After Effects failing on it — is remembered for a day and shown as a metadata
card, instead of relaunching After Effects every time its folder is opened.

> **Why a spooled copy?** The shell hands a thumbnail provider an `IStream`,
> never a filename. That is exactly what allows the provider to stay inside the
> isolated `dllhost.exe` rather than being loaded into `explorer.exe`. Since it
> has no path to pass along, it writes the bytes it was given into a spool
> folder and lets the baker open that. The copy is deleted once the frame is
> cached.

---

## Requirements

- Windows 10 or 11, 64-bit
- Adobe After Effects (any recent version; located via the registry)
- In After Effects: **Edit → Preferences → Scripting and Expressions → "Allow
  Scripts to Write Files and Access Network"** must be enabled.
  Without it every render fails silently. The installer offers to switch it on
  for you (ticked by default); from the command line it is
  `aepbake --enable-scripting`.

Without After Effects the provider still loads, but every tile stays a
metadata card — there is nothing to render with.

Check everything at once:

```
aepbake --doctor
```

```
AepThumb self-check

  [ ok ] After Effects: C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\AfterFX.exe
  [ ok ] Scripts may write files (AE 25.6)
  [ ok ] Render script: C:\Program Files\AepThumb\bake_batch.jsx
  [ ok ] Explorer uses this provider for .aep

  cache  : 14 baked, 0 pending, 0 spooled
  baker  : idle
  AE now : closed
```

---

## Install

### With the installer

Download `NativeAEPThumbnail-<version>-Setup.exe` from
[Releases](https://github.com/motionthunder/native-aep-thumbnail/releases) and
run it. Setup:

- installs to `Program Files\AepThumb` and registers the handler machine-wide;
- offers to switch on the After Effects scripting setting rendering needs,
  after asking you to close After Effects if it is open, and backing up its
  preferences file first (`… Prefs.txt.aepthumb-backup`);
- adds Start menu entries: **Turn previews on or off**, **Check setup**,
  **Project page**;
- optionally refreshes thumbnails Explorer has already drawn.

Uninstall from *Settings → Apps* puts the previous `.aep` handler back and
asks whether to keep the preview cache.

Silent install for deployment:

```bat
NativeAEPThumbnail-0.1.0-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS="scripting"
```

### From source

Needs Visual Studio 2022 C++ build tools and the Windows SDK.

```bat
build.cmd
install.cmd     :: asks for elevation
```

### Build the installer

Needs [Inno Setup 6](https://jrsoftware.org/isdl.php)
(`winget install JRSoftware.InnoSetup`).

```bat
make-installer.cmd
```

Builds the binaries and produces `dist\NativeAEPThumbnail-<version>-Setup.exe`.
The version lives in `src\version.h` and `installer\AepThumb.iss`; bump both.
Installer artwork is generated from `docs\banner.png` by
`installer\assets\make-assets.ps1`.

The installer is not code-signed, so SmartScreen warns on first run. Signing
`dist\*.exe` with a code-signing certificate removes that.

### What installation touches

| Where | What | Why |
| --- | --- | --- |
| `HKLM\Software\Classes\CLSID\{8E76F525-…}` | the COM class | the shell refuses to activate a thumbnail provider registered per-user |
| `HKLM\Software\Classes\.aep\ShellEx\{e357fccd-…}` | the association, for `.aep`, `.aet` and the AE ProgID | so it works for every user on the machine |
| `%LOCALAPPDATA%\AepThumb\` | cache, queue, spool, logs | per-user, disposable |
| `%APPDATA%\Adobe\After Effects\<ver>\… Prefs.txt` | one setting, only if you let setup change it | rendering needs scripts to be allowed to write files; a backup is kept beside it |

Process isolation is **left on** — there is no `DisableProcessIsolation`, so a
fault in this DLL cannot take Explorer down with it.

Any handler that already claimed `.aep` is saved under the CLSID's
`PreviousHandlers` key and restored on uninstall. On the machine this was built
on that was the Ardfry PSD codec, which claims `.aep` but cannot produce
anything for it — which is precisely why AEP tiles were blank.

---

## Commands

```
aepbake --scan  <folder> [-r]    bake every project in a folder now
aepbake --watch <folder> [-r]    bake, then keep baking as files change
aepbake --queue <project.aep>    bake one project
aepbake --doctor                 check the whole setup
aepbake --status                 cache and queue counts
aepbake --clear                  drop cached frames and failure marks so they re-bake
aepbake --enable-scripting       turn on the After Effects setting rendering needs
```

`--scan` is only for warming a library up front; normal browsing does not need
it.

### Panic switch

```bat
toggle.cmd
```

Run it once and previews are off: the handler is unregistered, the previous
`.aep` handler is put back, the baker is stopped and the Windows thumbnail
cache is flushed, so files look exactly as they did before AepThumb existed.
Run it again and everything comes back.

Nothing is uninstalled and no baked frames are discarded, so switching back on
is instant. The file is self-contained — copy it anywhere.

### Inspect a project without touching the shell

```bat
build\aepinfo.exe "project.aep" -o out.png -s 512
```

Prints the parsed structure, says whether a frame is cached, and renders the
tile it would hand to Explorer.

---

## Known limits

- **Offline footage renders as colour bars.** If a project's media lives on a
  drive that is not mounted, After Effects renders its own media-offline bars
  and that is what gets cached. Mount the drive and run `aepbake --clear`.
  Projects built from shapes and text are unaffected.
- The first frame of each project costs a few seconds.
- A tile turns into its frame on its own when the file is in an open Explorer
  window or on the desktop — that is where the baker looks to tell Explorer.
  A file shown somewhere else, such as an Open dialog, gets its frame the next
  time it is displayed.
- Frames are rendered at draft quality and half resolution, then cached with
  the longest side at 1024px. Good for tiles, not a preview replacement.
- `.aepx` (the XML project format) is not handled; the parser expects RIFX.
- Projects over 256 MB are not spooled on first view — point `--scan` at them.

---

## How the cache works

```
%LOCALAPPDATA%\AepThumb\
├─ cache\<key>.png     baked frame, longest side 1024
├─ cache\<key>.fail    this project could not render; retried after a day
├─ queue\<key>.job     pending request
├─ spool\<key>.aep     copy awaiting a bake, deleted afterwards
├─ aepbake.log         what the baker did
├─ bake_jsx.log        what After Effects reported
└─ provider.log        only written when the provider hits a problem
```

The key is an FNV-1a hash of the project's **bytes**, not its path. This is
what lets the provider work from a stream, and it has two useful consequences:
editing a project changes its key, so the cache invalidates itself, and two
copies of the same project share one baked frame.

## Which frame you get

The comp is whichever one the parser scores highest: render-target names
(`main`, `final`, `render`, `master`, `preview`) win, shallow position in the
project folder tree and delivery-sized dimensions help, layer count breaks
ties, and rig comps (`control`, `guide`, `null`, …) are penalised. After
Effects falls back to the largest comp if that name is gone.

The frame is the comp's **stored time indicator** — the author's own chosen
frame, the same one After Effects shows in its Project panel. If that sits on
frame 0, which is usually still black, it falls back to a quarter of the way
in.

Until a frame is baked, the tile falls back to a rendered preview beside the
project (`name.aep.png`, `name.png`, `_previews\name.png`), then the largest
still the project references, then a metadata card. Real frames are drawn
plain — no badge, no caption.

---

## AEP format notes

Reverse-engineered while building this; useful if you are parsing `.aep`
yourself.

Chunk tree: `RIFX` `Egg!` → top-level chunks → `LIST:Fold` → one `LIST:Item`
per project item. An item carries `idta` (type) and `Utf8` (name), then:

| Child | Means |
| --- | --- |
| `cdta` | the item is a composition |
| `LIST:Pin ` | the item is footage |
| `LIST:Sfdr` | the item is a folder; recurse into it |
| `LIST:Layr` | one layer of a comp (count them) |

Footage paths are JSON inside the `alas` chunk, in the `fullpath` field.

Fields inside `cdta`, verified against four unrelated projects including comps
sized 796×854, 606×54 and 216×19:

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x8c` | `uint16` | width |
| `0x8e` | `uint16` | height |
| `0x9c` | 16.16 fixed | frame rate |

The value at `0x2c` is **identical for every comp in a project**, including
precomps of completely different lengths — it is not per-comp duration. This
tool deliberately does not report duration.

---

## After Effects automation notes

Every one of these cost real debugging time. Measured on AE 25.6.

- **Never call `app.beginSuppressDialogs()` around a render.** With suppression
  on, any project whose render would raise a warning — a missing effect,
  offline footage — stops instantly as `USER_STOPPED` (3018) and writes
  nothing. With suppression off the same project finishes as `ERR_STOPPED`
  (3019) and the frame lands on disk. Test for the output file; never trust the
  queue status.
- `AfterFX.exe -noui -r <script>` works, but the path after `-r` must **not**
  be quoted. Quote it and AE exits in about 5 seconds having run nothing. Pass
  the 8.3 short path so spaces survive unquoted.
- `comp.saveFrameToPng` and `comp.saveDraftFrameToPng` exist and throw nothing,
  but silently write no file when AE runs headless. Use the render queue.
- An output module's `Format` is read-only through `setSettings`; the format has
  to come from `applyTemplate`. `TIFF Sequence with Alpha` is a stock template
  and GDI+ can decode its output.
- Applying a **render-settings** template resets the time span. Set your
  single-frame range *after* `applyTemplate`, or you will render the entire comp
  (300 frames and 2.9 GB, in one memorable case).
- A sequence output module appends a frame number, so the output path needs
  `[#####]` before the extension or the final rename fails with error 784.
- `app.exitCode` does not reach the process exit code, and a script blocked from
  writing files leaves no trace at all. To prove a script ran, time a
  deliberate busy-loop against a baseline launch.
- Locate After Effects via `HKLM\SOFTWARE\Adobe\After Effects\<ver>\InstallPath`
  rather than guessing folder names.
- `AfterFX.exe -m -noui -r <script>` runs the script in a **new, separate
  instance** even when After Effects is already open. Verified with a project
  open in the running session: the script saw an empty project of its own, a
  second `AfterFX.exe` did the work, and the session's project was untouched.
  Without `-m`, a script can end up in the running instance.
- Finding "is After Effects open" by window title does not work: the title
  carries the project name. Check for the `AfterFX.exe` process.

## Explorer notes

- **Any bitmap a thumbnail provider returns is cached by Explorer** and shown
  from then on. Returning a "rendering…" placeholder therefore pins the
  placeholder in place of the real frame. While a frame is pending, return a
  failure: Explorer keeps the standard icon and caches nothing.
- The shell gives a provider in the isolated host an `IStream`. Its `STATSTG`
  (requested with `STATFLAG_DEFAULT`) carries the file's leaf name, but never
  its folder.
- To make an open window re-ask for one file, send
  `SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path)`. The folders on screen
  come from `IShellWindows` → `IShellBrowser` → `IFolderView::GetFolder`; the
  desktops are not in that list and have to be added separately.
- The isolated thumbnail host (`dllhost.exe`) may still write to the user's
  profile and start processes. That is what lets the provider queue work and
  wake the baker without giving up process isolation.

---

## Layout

```
src/aep.{h,cpp}         RIFX parser and main-comp heuristic
src/cache.{h,cpp}       content-keyed cache, queue, spool, baker mutex
src/render.{h,cpp}      tile drawing: baked frame, or the card for unrenderable projects
src/thumb.cpp           COM thumbnail provider, registration, spool-and-queue
src/bake.cpp            aepbake.exe: queue, batching, watch, doctor, AE setting
src/version.h           version number and publisher, shared by the resources
src/*.rc                version info and icon for the binaries
tools/bake_batch.jsx    the script After Effects runs
tools/aepinfo.cpp       CLI: dump a project, render its tile
installer/AepThumb.iss  Inno Setup script for the end-user installer
installer/assets/       installer artwork and the script that generates it
build.cmd               build all three binaries
make-installer.cmd      build, then produce dist\...-Setup.exe
install.cmd             register (elevates)
uninstall.cmd           unregister and remove
toggle.cmd              panic switch: off, then on again
refresh-thumbnails.cmd  flush the Windows thumbnail cache
```

---

## Contributing

Issues and pull requests are welcome. The parts most likely to need work from
other people's project libraries:

- the **main-comp heuristic** in `AepPickMainComp` — which comp a human would
  call the project's cover shot;
- **`cdta` fields** beyond width, height and frame rate;
- `.aepx` (XML project) support, which is not implemented at all.

`build\aepinfo.exe "project.aep" -o out.png -s 512` prints what the parser sees
and renders the tile, which is the fastest way to check a change without
touching the shell.

## License

[MIT](LICENSE) — open source and free, do what you like with it.

## Trademarks

Adobe and After Effects are trademarks or registered trademarks of Adobe Inc.
in the United States and/or other countries. This project is not affiliated
with, endorsed by, or sponsored by Adobe.
