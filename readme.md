# DOSGIT

> Version control for the terminally nostalgic.

If Git and a floppy disk had a very small child, this would be it.

`DOSGIT` is a tiny single-file C program that gives your DOS-era or
retro-machine project something suspiciously like version control: a
snapshot, a diff, and a timestamped archive.

**The split that matters:** you *run* `DOSGIT` on the MS-DOS box or
retro machine where your source actually lives, to track it — but you
*build* `DOSGIT` on a modern machine in VS Code. One machine for coding
and versioning, another for compiling. More on that in [Build](#build).

## What it actually does

Point it at a directory — on your MS-DOS machine, tracking your source
as you work — and it will:

- 📸 **Snapshot** your files into a hidden `DOSGIT\` folder (`-commit`)
- 🔍 **Compare** your working files against the last snapshot
- 🗃️ **Archive** the previous snapshot before replacing it, so nothing
  is ever truly lost — just buried a little deeper

No content hashing, no diffing algorithms, no fancy branches yet
(future plans). Just size + timestamp comparisons and good
old-fashioned file copying. Simple enough to compile in the time it
takes your machine to beep.

## Usage

Run these on the MS-DOS/retro machine, in the directory where your
source lives:

```
git.exe [-commit] [path]
```

| Command | What happens |
|---|---|
| `git` | Compares the current directory against the last snapshot |
| `git C:\PROJECT` | Compares a specific directory instead |
| `git -commit` | Archives the old snapshot and takes a new one |
| `git -commit C:\PROJECT` | Same, but for a specific directory |

Run it with no snapshot yet, and it'll politely ask:

```
No committed files were found. Commit first? [Y/N]
```

Say yes. It's not going to judge you. (It might, quietly, in `stderr`.)

## Sample output

```
STATUS    FILE
          PREVIOUS DATE       PREV SIZE  NEW DATE            NEW SIZE
NEW       LEVEL3.C
          -------------------        -1  2026-09-26 14:02:11       842
CHANGED   MAIN.C
          2026-09-20 09:15:03       512  2026-09-26 14:01:47       540
UNCHANGED README.TXT
          2026-09-01 08:00:00       128  2026-09-01 08:00:00       128
DELETED   OLDSTUFF.C
          2026-08-15 11:22:09       310  -------------------        -1
```

## How it decides "changed"

A file counts as **CHANGED** if its size or modified-time differs from
the snapshot's copy — that's it. No byte-for-byte comparison. This
means:

- ⚡ It's fast. Practically instant on a directory of any size.
- 🎭 It can be fooled. Touch a file without editing it, and `DOSGIT`
  will still call it CHANGED. Edit content but preserve size *and*
  timestamp (deliberately or by cosmic coincidence), and it'll call it
  UNCHANGED.

Basically: it trusts your filesystem's word for it. Very DOS of it.

## The archive

Every `-commit` after the first one archives the *entire* previous
snapshot into:

```
DOSGIT\ARCHIVE\HHMMSS.DDD\
```

where `DDD` is the day of the year — so `143205.269` means "2:32:05 PM
on the 269th day of the year." Sortable, unique-ish, delightfully
cryptic (8.3 if you know, you know).

Each archive is a full copy, not a diff. Commit often and you'll build
up quite the little museum of your project's past selves. Disk space:
not included, sorry.

## Build

Even though `DOSGIT` *runs* on MS-DOS or a retro machine, you *build*
it on a modern machine using [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2/releases),
a C compiler that targets MS-DOS but installs and runs fine on
Windows, inside VS Code. (Open Watcom also ships a Fortran 77
compiler! I'm not joking!)

### 1. Install Open Watcom v2

- Go to the [Open Watcom v2 releases page](https://github.com/open-watcom/open-watcom-v2/releases)
  on GitHub.
- Download the latest Windows installer
  (`open-watcom-c-x64-installer.exe` or similar).
- Run it. The default install path is `C:\WATCOM` — keep it.
- During install, make sure the **DOS target components** (16-bit and
  32-bit DOS libraries/headers) are selected. They're usually checked
  by default, but double-check.

### 2. Set environment variables

Open Watcom needs a few environment variables. The installer may set
these for you, but verify them manually:

- `WATCOM` = `C:\WATCOM`
- Add to `PATH`: `C:\WATCOM\binnt64` (or `C:\WATCOM\binnt` if you
  installed the 32-bit tools)
- `INCLUDE` = `C:\WATCOM\h`

To set these permanently on Windows 10/11:

1. Search "Environment Variables" in the Start menu → "Edit the
   system environment variables"
2. Click "Environment Variables"
3. Add/edit `WATCOM` and `INCLUDE`, and append the bin folder to
   `PATH`

Open a new terminal after this and verify with:

```powershell
wcl386 -?
```

You should see Open Watcom's compiler help output. If you get
"command not found," recheck `PATH`.

### 3. Install VS Code extensions

**C/C++** (Microsoft) — for syntax highlighting and IntelliSense

## Runs on MS-DOS, builds either way

`DOSGIT` runs natively under **MS-DOS** and compiles cleanly on a modern
machine, in VS Code, as either:

- 🕹️ **16-bit** — the classic real-mode build; should be good enough
  for most retro setups
- 🚀 **32-bit** — for DOS extenders or 386+ protected-mode setups, if
  your project is huge and you'd like your nostalgia with a bit more
  headroom

## Why does this exist

Because sometimes you just want to know what changed since lunch—and there's something strangely seductive about doing it on a machine that still boots to <div style="background-color: black; color: white; padding: 10px;">`C:\>_`</div>

Because some of us have a thing for old hardware.

We refurbish vintage machines. We coax MS-DOS back to life. We whisper sweet nothings to Turbo C, Pascal, and even Visual Basic for DOS (Yeah! That's a thing!). Give us a beige case, a mechanical keyboard, and a blinking cursor, and suddenly we're feeling things we probably shouldn't admit in 
polite company.

## License

Do whatever you want with it. It's a few hundred lines of C and a lot
of nostalgia — nobody's precious about it.