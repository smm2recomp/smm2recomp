# iPhone and iPad

The Mac app builds an iPhone / iPad version of the game from your own game files. You get an `.ipa` file,
and you sign and install it with a tool of your choice. Nothing of the game is downloaded: the game code is
recompiled on your Mac from your dump, as for the Mac version.

## What you need

- A Mac with Apple silicon (M1 or later) and the Mac app, with the game already set up in it.
- **Xcode**, from the App Store. Open it once, and add iOS under Xcode > Settings > Components. Apple
  only lets Xcode compile for iPhone and iPad, so the Mac app can't do it without Xcode.
- About 8 GB of free disk space (3 GB if you leave the game files out).
- An iPhone or iPad with iOS / iPadOS 17 or later. An iPad or iPhone with 8 GB of memory or more is best:
  the game uses over 3 GB.

## Building it

On the setup screen, under **iPhone and iPad**, choose **Build for iPhone / iPad**. It takes about as long
as the Mac version's first start, and it saves `SMMaker2-<version>.ipa` in your Downloads folder.

From Terminal, the same build:

    "/Applications/Super Mario Maker 2.app/Contents/MacOS/smm2recomp" --build-ios [--no-game-files] [<file.ipa>]

**Include the game files** (on by default) puts them in the app, about 3 GB: there is nothing to copy
afterwards. Without them the app is small, and you copy `extracted/`, `fonts/` and `sysdata/` from the
Mac app's data folder (`~/Library/Application Support/org.smcraft.smm2recomp`) into the app's Documents
folder yourself, with Finder (your device in the sidebar, then Files) or the Files app.

The `.ipa` contains your game. Keep it for your own devices, and don't share it.

## Installing it

The `.ipa` is signed ad hoc, which no iPhone accepts as it is: the tool you install it with signs it with
your Apple account. Any of these works:

- [AltStore](https://altstore.io) or [SideStore](https://sidestore.io)
- [Sideloadly](https://sideloadly.io)
- [LiveContainer](https://github.com/LiveContainer/LiveContainer)
- Xcode, with your own signing set up (see [recomp/README.md](../recomp/README.md), "Building (iOS / iPadOS)")

### Memory permissions

The game needs two permissions from Apple:

- **Extended Virtual Addressing**: room for the game's 12.5 GB memory window
- **Increased Memory Limit**: for the 3+ GB the game uses

The app asks for both. Whether it gets them depends on your Apple account and on the signing tool:

- A **paid developer account** can grant both, with an App ID that has the two capabilities.
- A **free account** may not keep them. Its apps also have to be signed again every 7 days.

Without them the app stops at start with "can't reserve the game's memory window", or iOS closes it for
using too much memory.

## Problems

The log is `smm2recomp.log` in the app's Documents folder. When the build itself fails, the Mac app's
log (`smm2recomp.log` in its data folder) has the details. Please report problems in
[Issues](https://github.com/smm2recomp/smm2recomp/issues), with both logs.
