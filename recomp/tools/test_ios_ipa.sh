#!/bin/bash
# usage: tools/test_ios_ipa.sh <.ipa> with-files | no-files
# Checks an iPhone / iPad app the Mac app built (smm2recomp --build-ios, runtime/iosbuild.c): the layout, built for
# iOS devices, the bundle id, signed with both memory entitlements, and the game files inside (with-files) or not
# (no-files). Exit 0 when all pass.
set -euo pipefail
IPA=${1:?usage: test_ios_ipa.sh <.ipa> with-files|no-files}
MODE=${2:?usage: test_ios_ipa.sh <.ipa> with-files|no-files}
fail() { echo "test_ios_ipa: FAIL: $*" >&2; exit 1; }
[ -s "$IPA" ] || fail "no $IPA"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ditto -x -k "$IPA" "$work" || fail "$IPA doesn't unzip"
A=$work/Payload/SMM2Recomp.app
[ -x "$A/SMM2Recomp" ] || fail "no Payload/SMM2Recomp.app/SMM2Recomp"
[ "$(plutil -extract CFBundleIdentifier raw "$A/Info.plist" 2>/dev/null)" = org.smcraft.smm2recomp ] ||
    fail "Info.plist: the bundle id isn't org.smcraft.smm2recomp"
[ "$(plutil -extract CFBundleExecutable raw "$A/Info.plist")" = SMM2Recomp ] || fail "Info.plist: CFBundleExecutable"
ls "$A"/Assets.car >/dev/null 2>&1 || ls "$A"/AppIcon*.png >/dev/null 2>&1 || fail "no icon"
otool -l "$A/SMM2Recomp" | grep -q 'platform 2' || fail "the executable isn't built for iOS devices"
nm "$A/SMM2Recomp" | grep ' _f_7' >/dev/null || fail "the executable has no game code in it"
ent=$(codesign -d --entitlements - --xml "$A" 2>/dev/null) || fail "not signed"
for k in com.apple.developer.kernel.extended-virtual-addressing com.apple.developer.kernel.increased-memory-limit; do
    grep -q "$k" <<< "$ent" || fail "signed without $k"
done
codesign --verify --strict "$A" || fail "the signature doesn't verify"
case "$MODE" in
    with-files)
        for d in extracted/exefs extracted/romfs fonts sysdata; do [ -d "$A/data/$d" ] || fail "no data/$d"; done
        [ -s "$A/data/extracted/exefs/main" ] || fail "data/extracted/exefs/main is empty" ;;
    no-files) [ ! -e "$A/data" ] || fail "has data/ (built without the game files)" ;;
    *) fail "mode: with-files or no-files" ;;
esac
echo "test_ios_ipa: PASS ($IPA, $MODE, $(du -sh "$A" | cut -f1))"
