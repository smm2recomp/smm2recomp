# The iOS app's Info.plist and icon, shared by tools/make_ios_app.sh (the app built here) and tools/make_ios_kit.sh
# (the kit the Mac app builds players' iPhone / iPad apps from). Sourced (bash); run from recomp/.

# ios_info_plist <out plist> <bundle id> <CFBundleShortVersionString> <CFBundleVersion> <ios | iossim>
ios_info_plist() {
    local out=$1 bundle_id=$2 version=$3 build=$4 sdk=iphoneos platform=iPhoneOS
    [ "$5" = iossim ] && sdk=iphonesimulator platform=iPhoneSimulator
    cat > "$out" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>SMM2Recomp</string>
    <key>CFBundleIdentifier</key><string>$bundle_id</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>SMM2Recomp</string>
    <key>CFBundleDisplayName</key><string>$(sed -n 's/^GAME_MOBILE_TITLE *:= *//p' games/smm2/game.mk)</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$build</string>
    <key>CFBundleSupportedPlatforms</key><array><string>$platform</string></array>
    <key>DTPlatformName</key><string>$sdk</string>
    <key>MinimumOSVersion</key><string>17.0</string>
    <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>UILaunchScreen</key><dict/>
    <key>UIRequiresFullScreen</key><true/>
    <key>UIStatusBarHidden</key><true/>
    <key>UIViewControllerBasedStatusBarAppearance</key><false/>
    <key>UIApplicationSupportsIndirectInputEvents</key><true/>
    <key>UIApplicationSceneManifest</key>
    <dict><key>UIApplicationSupportsMultipleScenes</key><false/></dict>
    <key>UISupportedInterfaceOrientations</key>
    <array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
    <key>UISupportedInterfaceOrientations~ipad</key>
    <array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
    <!-- Documents (extracted/, fonts/, sysdata/, saves, settings, caches) in the Files app and Finder -->
    <key>UIFileSharingEnabled</key><true/>
    <key>LSSupportsOpeningDocumentsInPlace</key><true/>
    <key>GCSupportsControllerUserInteraction</key><true/>
    <key>GCSupportedGameControllers</key>
    <array><dict><key>ProfileName</key><string>ExtendedGamepad</string></dict></array>
    <key>CADisableMinimumFrameDurationOnPhone</key><true/>
</dict>
</plist>
EOF
    plutil -lint "$out" >/dev/null
}

# ios_icons <out dir> <plist to add the icon keys to> <ios | iossim>
# The app icon (docs/branding/ios, tools/make_icons.sh): an asset catalog compiled by actool (Xcode's command line
# tools: Assets.car plus the home-screen PNGs and the Info.plist keys) from the 1024 master size; without actool the
# PNGs alone, named in CFBundleIcons.
ios_icons() {
    local out=$1 plist=$2 sdk=iphoneos work aset
    [ "$3" = iossim ] && sdk=iphonesimulator
    local icons
    icons="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/docs/branding/ios"
    mkdir -p "$out"
    # (absolute: actool hands the job to a shared agent that resolves a relative path against its own directory - another
    # build's, when one ran actool first - and fails with "the output directory does not exist")
    out=$(cd "$out" && pwd)
    if xcrun --find actool >/dev/null 2>&1; then
        work=$(mktemp -d)
        aset=$work/Assets.xcassets/AppIcon.appiconset
        mkdir -p "$aset"
        cp "$icons/AppIcon1024x1024.png" "$aset/icon-1024.png"
        printf '%s\n' '{"images":[{"filename":"icon-1024.png","idiom":"universal","platform":"ios","size":"1024x1024"}],' \
            '"info":{"author":"xcode","version":1}}' > "$aset/Contents.json"
        printf '%s\n' '{"info":{"author":"xcode","version":1}}' > "$work/Assets.xcassets/Contents.json"
        xcrun actool --compile "$out" --platform "$sdk" --minimum-deployment-target 17.0 --target-device iphone \
            --target-device ipad --app-icon AppIcon --output-partial-info-plist "$work/icon.plist" \
            --output-format human-readable-text "$work/Assets.xcassets" > "$work/actool.log" 2>&1 ||
            { cat "$work/actool.log" >&2; rm -rf "$work"; return 1; }
        /usr/libexec/PlistBuddy -c "Merge $work/icon.plist" "$plist" >/dev/null
        rm -rf "$work"
    else
        cp "$icons"/AppIcon*.png "$out/"
        /usr/libexec/PlistBuddy -c 'Add :CFBundleIcons dict' -c 'Add :CFBundleIcons:CFBundlePrimaryIcon dict' \
            -c 'Add :CFBundleIcons:CFBundlePrimaryIcon:CFBundleIconFiles array' \
            -c 'Add :CFBundleIcons:CFBundlePrimaryIcon:CFBundleIconFiles:0 string AppIcon60x60' \
            -c 'Add :CFBundleIcons~ipad dict' -c 'Add :CFBundleIcons~ipad:CFBundlePrimaryIcon dict' \
            -c 'Add :CFBundleIcons~ipad:CFBundlePrimaryIcon:CFBundleIconFiles array' \
            -c 'Add :CFBundleIcons~ipad:CFBundlePrimaryIcon:CFBundleIconFiles:0 string AppIcon60x60' \
            -c 'Add :CFBundleIcons~ipad:CFBundlePrimaryIcon:CFBundleIconFiles:1 string AppIcon76x76' \
            -c 'Add :CFBundleIcons~ipad:CFBundlePrimaryIcon:CFBundleIconFiles:2 string AppIcon83.5x83.5' "$plist"
    fi
}
