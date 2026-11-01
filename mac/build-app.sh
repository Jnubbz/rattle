#!/bin/sh
# Builds "Radio Messages.app" (needs macOS with Xcode command line tools, and the
# rattle binary already built in ../build/rattle).
set -eu
cd "$(dirname "$0")"
APP="build/Radio Messages.app"
rm -rf build && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

swiftc -O -target arm64-apple-macos13.0 \
	-o "$APP/Contents/MacOS/RadioMessages" RadioMessages/main.swift

cp ../build/rattle "$APP/Contents/MacOS/rattle"
cp Info.plist "$APP/Contents/Info.plist"
cp rattle.ini "$APP/Contents/Resources/rattle.ini"

# App icon, drawn with AppKit.
swift icon.swift build/icon.png
mkdir -p build/AppIcon.iconset
for s in 16 32 128 256 512; do
	sips -z $s $s build/icon.png --out build/AppIcon.iconset/icon_${s}x${s}.png >/dev/null
	d=$((s * 2))
	sips -z $d $d build/icon.png --out build/AppIcon.iconset/icon_${s}x${s}@2x.png >/dev/null
done
iconutil -c icns build/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"

# Ad-hoc signature (no Apple developer account needed).
codesign --force -s - "$APP/Contents/MacOS/rattle"
codesign --force -s - "$APP"
codesign --verify --verbose "$APP"
echo "built $APP"
