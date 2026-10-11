# Mario Party 4 for PS3
=============

This is a Fan-made Port of MP4 for PS3.

# AI Disclosure
The code for porting to PS3 is AI-generated with Claude, but the tests are made manually.

# Some more stuff
Version Completion:

- `GMPE01_00`: Rev 0 (USA) ❌
- `GMPE01_01`: Rev 1 (USA) ✅ < currently being tested
- `GMPP01_00`: Rev 0 (PAL) ❌
- `GMPP01_01`: Rev 1 (PAL) ❌
- `GMPP01_02`: Rev 2 (PAL) ❌
- `GMPJ01_00`: Rev 0 (JP)  ❌

Dependencies
============

Windows
--------

On Windows, it's **highly recommended** to use native tooling. WSL or msys2 are **not** required.  
When running under WSL, [objdiff](#diffing) is unable to get filesystem notifications for automatic rebuilds.

- Install [Python](https://www.python.org/downloads/) and add it to `%PATH%`.
  - Also available from the [Windows Store](https://apps.microsoft.com/store/detail/python-311/9NRWMJP3717K).
- Download [ninja](https://github.com/ninja-build/ninja/releases) and add it to `%PATH%`.
  - Quick install via pip: `pip install ninja`

macOS
------
- Install [ninja](https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages):
  ```
  brew install ninja
  ```
- Install [wine-crossover](https://github.com/Gcenx/homebrew-wine):
  ```
  brew install --cask --no-quarantine gcenx/wine/wine-crossover
  ```

After OS upgrades, if macOS complains about `Wine Crossover.app` being unverified, you can unquarantine it using:
```sh
sudo xattr -rd com.apple.quarantine '/Applications/Wine Crossover.app'
```

Linux
------
- Install [ninja](https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages).
- For non-x86(_64) platforms: Install wine from your package manager.
  - For x86(_64), [wibo](https://github.com/decompals/wibo), a minimal 32-bit Windows binary wrapper, will be automatically downloaded and used.

Building
========

- Clone the repository:
  ```
  git clone https://github.com/mariopartyrd/marioparty4.git
  ```

- Initialize and update submodules:

  ```sh
  git submodule update --init --recursive
  ```

- Copy your game's disc image to `orig/[GAMEID]`. The supported game IDs are listed above.
  - Supported formats: ISO (GCM), RVZ, WIA, WBFS, CISO, NFS, GCZ, TGC
  - After the initial build, the disc image can be deleted to save space.

- Configure:
  ```
  python configure.py
  ```

  To choose a version other than the USA Rev 0 one, add `--version [GAMEID]` to the command. 

- Build:
  ```
  ninja
  ```

Diffing
=======

Once the initial build succeeds, an `objdiff.json` should exist in the project root. 

Download the latest release from [encounter/objdiff](https://github.com/encounter/objdiff). Under project settings, set `Project directory`. The configuration should be loaded automatically. 

Select an object from the left sidebar to begin diffing. Changes to the project will rebuild automatically: changes to source files, headers, `configure.py`, `splits.txt` or `symbols.txt`.
