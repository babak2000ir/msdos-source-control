# DOSGIT

> Version control for the terminally nostalgic.

If Git and a floppy disk had a very small child who learned to live in a beige box and only understood 8.3 filenames, this would be it.

`DOSGIT` is a tiny MS-DOS utility for keeping a snapshot of a project directory, comparing the current tree against the last committed state, and archiving the old baseline before replacing it. It is intentionally simple, stubbornly retro, and a bit more charming than a proper VCS.

It runs on the retro machine where the source lives, but you build it on a modern Windows box in VS Code using Open Watcom. That is the modern-world compromise the project insists on. We do not negotiate with the BIOS.

## What it does

Point it at a directory and it will:

- 📸 create a snapshot in `dosgit\`
- 🔍 compare the current directory against the last snapshot
- 🗃️ archive the previous snapshot under `dosgit\archive\`
- 🧮 record CRC-32 hashes for every file in `dosgit\HASH`
- 🧾 show a status report with new, changed, unchanged, and deleted files

There are no branches. There are no diffs. There is no Git drama. Just a snapshot, a manifest, and a full archive of the previous state. Very 1990s. Very efficient. Very suspiciously useful.

## Command line

Run this on the DOS machine in the project directory:

```text
git.exe [-commit | -history] [path]
```

| Command | What it does |
|---|---|
| `git` | Compares the current directory to the last committed snapshot |
| `git C:\PROJECT` | Compares a specific directory instead of the current one |
| `git -commit` | Archives the previous snapshot and writes a new one |
| `git -commit C:\PROJECT` | Same, but for a specific project directory |
| `git -history` | Lists archive folders from oldest to newest |
| `git -history C:\PROJECT` | Lists archive history for a specific project |

If no snapshot exists yet, it does this:

```text
No committed files were found. Commit first? [Y/N]
```

If you answer `Y`, it commits immediately. If you answer `N`, it declines to do any comparison and returns a polite little failure. The program is not rude, but it is firm.

## The DOS monitor look

This is the sort of output you are meant to see in a real DOS session, ideally with ANSI colors enabled and a machine that still knows how to beep at the right moment.

![DOSGIT monitor example](src/git%20monitor.PNG)

That screenshot is a good example of the real behavior: there is a header, a list of files with a status word, and the previous/current hash for each file. The visual style is intentionally compact because DOS screens do not have time for your nonsense.

## Sample output

```text
STATUS    FILE
          previous hash -> current hash
new       LEVEL3.C
          -------- -> 6B7E2A14
changed   MAIN.C
          42FC901A -> C3521F0B
unchanged README.TXT
          2A41D7E0 -> 2A41D7E0
deleted   OLDSTUFF.C
          8713AB20 -> --------
```

The status labels are exactly these:

- `new`: the file is present in the working tree but not in the snapshot
- `changed`: the file exists in both places, but the CRC-32 hash differs
- `unchanged`: the file exists and the hash matches
- `deleted`: the file existed in the snapshot but is gone in the current tree

The display uses ANSI colors when available:

- yellow for `new`
- green for `unchanged`
- blue for `changed`
- red for `deleted`

On DOS, this means `ANSI.SYS` in `CONFIG.SYS` is your friend if you want the colors to behave like a proper electronics showroom.

## How the comparison works

A file is considered changed if its CRC-32 value differs from the value stored in `dosgit\HASH`.

The hash manifest is a simple text file. Each line looks like this:

```text
relative/path/to/file<TAB>8-digit-CRC32
```

The program reads the current file, hashes it, and compares the value to the previous hash for that same relative path. If the path has been deleted, it reports `deleted`; if it is brand new, it reports `new`.

The manifest intentionally excludes itself and the executable, because if it included those, the system would start explaining to itself why it is tracking the tracker. That would be a bizarre and unproductive use of CPU cycles.

## Snapshot and archive rules

The snapshot lives under `dosgit\` and is created by `-commit`.

The current snapshot is copied to the archive before being replaced. Every new archive is named as:

```text
dosgit\archive\HHMMSS.DDD\
```

The `DDD` part is the day of the year, so a name like `143205.269` means roughly "2:32:05 PM on day 269 of the year." It is a compact DOS-era timestamp, which is both clever and a little bit cryptic. That is how the old-timers liked it.

Important detail: this is a full copy of the previous snapshot, not a diff. That means the archive folder is a museum of your project's past selves. Very useful. Very space-hungry. Very on-brand for a 90s programmer with a 500 MB drive and a dream.

`git -history` lists those archive directories in date/time order, oldest first. For each archive after the first, it compares the archive to the previous one and prints the file differences. Then it compares the current `dosgit\` snapshot to the newest archive, so you can see what changed since that last saved state.

## Build it

Even though the app runs on MS-DOS, you build it on a modern machine with [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2/releases), which is a perfectly respectable way to cross-compile a DOS utility from Windows without becoming a hobbyist blacksmith.

### 1. Install Open Watcom v2

- Download the Windows installer from the Open Watcom v2 releases page
- Install it to the default location, which is usually `C:\WATCOM`
- Make sure the DOS target components are included; the 16-bit and 32-bit DOS bits are the part that matters

### 2. Set environment variables

Check these values:

- `WATCOM=C:\WATCOM`
- `INCLUDE=C:\WATCOM\h`
- Add `C:\WATCOM\binnt64` (or `binnt`) to `PATH`

Then verify with:

```powershell
wcl386 -?
```

If you get help text, the toolchain is happy. If not, the machine is telling you to revisit your environment variables and stop making this harder than it needs to be.

### 3. Open in VS Code

The repo is already set up to build in a VS Code terminal. If you want to compile the DOS variants directly, the workspace includes build tasks for the 16-bit and 32-bit targets.

## The short version

`DOSGIT` is basically:

- snapshot the project
- compare it to the last snapshot
- archive the previous one before replacing it
- keep a CRC manifest so you know what changed

That is enough to make a retro project feel a bit more civilized without turning your `C:\` prompt into a Linux package manager.

## Why this exists

Because some people like their source control small, local, and stubbornly offline.

Because sometimes you want to know what changed since lunch and the machine in the corner still boots to `C:\>_` like a champion.

Because a dusty beige case, a mechanical keyboard, and a blinking cursor in Turbo C, Pascal, and even Visual Basic for DOS (Yeah! That's a thing!) makes some of us suddenly feeling things we probably shouldn't admit in polite company in a world that has become unreasonably shiny.

## License

Do whatever you want with it. It's a few hundred lines of C and a lot
of nostalgia, nobody's precious about it.