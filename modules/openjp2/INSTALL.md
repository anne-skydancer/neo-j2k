<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# neo-j2k for your viewer

neo-j2k is a JPEG 2000 codec that decodes (and encodes) Second Life textures exactly as your viewer's current
library does, bit for bit, and faster. This zip holds it in the form your viewer can use without being changed,
rebuilt or told: `neoj2k-viewer.dll` answers to the file name and the calls the viewer already makes for
textures. You put it in the viewer's folder under the name the viewer looks for, and you can put the old file
back at any time.

**Status.** Built and tested against the reference library's own header and neo-j2k's test suite (the same
decodes as OpenJPEG 2.5.4, the library the viewers use today, bit for bit). It has not yet been tried in a viewer.
If you try it, please say how it went: <https://github.com/anne-skydancer/neo-j2k/issues>.

## What you need

- **Windows, 64-bit**, and a **64-bit viewer**. The file is x64 only.
- **A processor with AVX2**: Intel from Haswell (2013) on, AMD from Ryzen (2017) on, and most machines since.
  On an older one the library refuses to start and the viewer shows no textures (put the old file back).
- A viewer that keeps its JPEG 2000 library in a **separate `openjp2.dll`** next to its program. Look in the
  viewer's folder: if there is no `openjp2.dll`, there is nothing to swap. (Vulkanstorm-Release 1.0.1 has none:
  it already uses neo-j2k, as `neoj2k.dll`. A viewer with the library built into its program has no file to swap
  either.)

## Install

1. **Unblock the zip before you unpack it**: right-click the zip, Properties, tick *Unblock* if it is there, OK.
   Then extract it anywhere.
2. **Close the viewer** completely (check Task Manager for a leftover process).
3. **Find the viewer's folder**: the one that holds the viewer's program and an `openjp2.dll`
   (usually under `C:\Program Files`). Each viewer, and each install of it, has its own.
4. **Keep the old file.** In that folder, rename `openjp2.dll` to `openjp2.dll.original`. Do not delete it.
5. **Copy in the new one** and **name it `openjp2.dll`**: copy `neoj2k-viewer.dll` from this zip into that folder,
   then rename the copy to `openjp2.dll`, because that is the name the viewer looks for. A folder under
   *Program Files* asks for administrator permission.
6. **Check the copy** (optional): in PowerShell, in that folder,

   ```
   Get-FileHash .\openjp2.dll -Algorithm SHA256
   ```

   The hash must equal the line for `neoj2k-viewer.dll` in this zip's `SHA256SUMS`.
7. **Start the viewer.** The first start downloads textures again: the viewer keys its texture cache on the
   decoder's version, and neo-j2k's version string is a new one. It does so again after each neo-j2k release.
   In the viewer's *About* box, the J2C decoder line should now read
   `Jpeg2000 2.5.4 provided by neo-j2k 0.1.1` (viewers that show it).

## Go back

Close the viewer, delete the `openjp2.dll` you put in, and rename `openjp2.dll.original` back to `openjp2.dll`.

## Things to know

- **A viewer update puts its own `openjp2.dll` back.** Repeat the steps after an update.
- **The file is not code-signed.** Windows or an antivirus may warn about it. The zip's `SHA256SUMS` and the
  source (<https://github.com/anne-skydancer/neo-j2k>, BSD-2-Clause) let you check what you have.
- **Nothing else changes.** The viewer's settings and your account are untouched; the swap is that one file.
- **What is not there.** Features a viewer does not use are not built: decoding by tile, decode areas, the
  codestream index, and encoding to JP2 files. A call for one of them fails.
- **Licences.** `LICENSE`, `LICENSE.OpenJPEG` and `NOTICE.md` in this zip are the licences of neo-j2k and of the
  OpenJPEG code it is built on.

## If something is wrong

- *No textures, or grey ones, right after the swap:* the processor probably lacks AVX2. Go back.
- *The viewer does not start:* put the old file back, then tell us which viewer and version it is.
- *A texture looks different from before:* it should not (the decodes are the same, bit for bit), so please send
  the texture's UUID and the viewer.
