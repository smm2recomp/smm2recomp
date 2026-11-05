#!/bin/bash
# usage: tools/make_ios_app.sh <ios | iossim> <executable> <out .app>   (make TARGET=ios-arm64 GAME=smm2 app runs it)
# The iOS / iPadOS app bundle around the runtime with the game code linked in (built on the Mac from your own dump:
# share it with nobody). Game files go in the app's Documents folder (Files app / Finder: extracted/, fonts/,
# sysdata/), not in the bundle.
#
# Signing (devices): RECOMP_IOS_IDENTITY (a codesigning identity, `security find-identity -p codesigning`) and
# RECOMP_IOS_PROFILE (a .mobileprovision for RECOMP_IOS_BUNDLE_ID that lists the device). The guest memory window
# sits at 0x7000000000-0x7320000000, far above iOS's default address space: the profile has to carry
# com.apple.developer.kernel.extended-virtual-addressing, and com.apple.developer.kernel.increased-memory-limit for the
# 3+ GB the game uses. A wildcard profile has neither: the app then installs but stops at start ("guest memory not
# reserved"). The Simulator needs no profile.
set -euo pipefail
. "$(dirname "$0")/ios_bundle_lib.sh"  # (ios_info_plist, ios_icons)
PLAT=$1 EXE=$2 APP=$3
NAME=SMM2Recomp
BUNDLE_ID=${RECOMP_IOS_BUNDLE_ID:-org.smcraft.smm2recomp}
VERSION=3.0.3
BUILD=$(git rev-list --count HEAD 2>/dev/null || echo 1)
rm -rf "$APP"
mkdir -p "$APP"
cp "$EXE" "$APP/$NAME"
ios_info_plist "$APP/Info.plist" "$BUNDLE_ID" "$VERSION" "$BUILD" "$PLAT"
printf 'APPL????' > "$APP/PkgInfo"
ios_icons "$APP" "$APP/Info.plist" "$PLAT"
# RECOMP_IOS_DATA=<data folder> (e.g. private/smm2): the game files go into the app as data/ (extracted/exefs and
# romfs, fonts, sysdata; runtime/paths.c links them into Documents), so a device needs nothing copied by hand - for
# your own device only: such an app holds the game. Staged next to the app once (rsync), then cloned into each new
# bundle (APFS clones: no second 3 GB copy).
if [ -n "${RECOMP_IOS_DATA:-}" ]; then
    stage="$(dirname "$APP")/data-stage"
    for d in extracted/exefs extracted/romfs fonts sysdata; do
        [ -d "$RECOMP_IOS_DATA/$d" ] || { echo "make_ios_app: $RECOMP_IOS_DATA/$d is missing" >&2; exit 1; }
        mkdir -p "$stage/$d"
        rsync -aL --delete "$RECOMP_IOS_DATA/$d/" "$stage/$d/"
    done
    # The shaders that data folder's runs met (and RECOMP_IOS_SHADERS="<dir>:<dir>...", other data folders'
    # cache/shaders), precompiled for the device: their first use there loads them instead of compiling the source.
    # RECOMP_IOS_NO_SHADERS=1: leave them out.
    if [ -z "${RECOMP_IOS_NO_SHADERS:-}" ]; then
        dirs=("$RECOMP_IOS_DATA/cache/shaders")
        IFS=: read -r -a more <<< "${RECOMP_IOS_SHADERS:-}"
        [ ${#more[@]} -eq 0 ] || dirs+=("${more[@]}")
        "$(dirname "$0")/precompile_shaders.sh" "$stage/metallib" ios "${dirs[@]}" ||
            echo "make_ios_app: shaders not precompiled (they compile on the device instead)" >&2
    fi
    cp -Rc "$stage" "$APP/data" 2>/dev/null || cp -R "$stage" "$APP/data"
fi
if [ "$PLAT" = iossim ]; then
    codesign --force --sign - --timestamp=none "$APP"
elif [ -n "${RECOMP_IOS_IDENTITY:-}" ] && [ -n "${RECOMP_IOS_PROFILE:-}" ]; then
    work=$(mktemp -d)
    cp "$RECOMP_IOS_PROFILE" "$APP/embedded.mobileprovision"
    security cms -D -i "$RECOMP_IOS_PROFILE" > "$work/profile.plist"
    /usr/libexec/PlistBuddy -x -c "Print :Entitlements" "$work/profile.plist" > "$work/ent.plist"
    appid=$(/usr/libexec/PlistBuddy -c "Print :Entitlements:application-identifier" "$work/profile.plist")
    team=$(/usr/libexec/PlistBuddy -c "Print :Entitlements:com.apple.developer.team-identifier" "$work/profile.plist")
    # (a wildcard profile: the app's own id in place of the wildcard)
    case "$appid" in *'*') /usr/libexec/PlistBuddy -c "Set :application-identifier $team.$BUNDLE_ID" "$work/ent.plist" ;; esac
    for k in com.apple.developer.kernel.extended-virtual-addressing com.apple.developer.kernel.increased-memory-limit; do
        /usr/libexec/PlistBuddy -c "Print :$k" "$work/ent.plist" >/dev/null 2>&1 || echo "make_ios_app: the profile lacks $k" >&2
    done
    codesign --force --sign "$RECOMP_IOS_IDENTITY" --entitlements "$work/ent.plist" --timestamp=none "$APP"
    rm -rf "$work"
    codesign --verify --strict "$APP" && echo "make_ios_app: signed $APP ($BUNDLE_ID)"
else
    echo "make_ios_app: $APP left unsigned (set RECOMP_IOS_IDENTITY and RECOMP_IOS_PROFILE)" >&2
fi
du -sh "$APP"
