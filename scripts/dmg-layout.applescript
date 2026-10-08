-- CPack passes the mounted volume name, including a suffix if it is already mounted.
on run argv
    tell application "Finder"
        tell disk (item 1 of argv)
            open
            set current view of container window to icon view
            set toolbar visible of container window to false
            set statusbar visible of container window to false
            set bounds of container window to {200, 160, 780, 562}
            set options to icon view options of container window
            set arrangement of options to not arranged
            set icon size of options to 96
            set text size of options to 13
            set background picture of options to file ".background:background.tiff"
            set position of item "disquisition.app" to {145, 225}
            set position of item "Applications" to {435, 225}
            close
            open
            update without registering applications
            delay 2
            close
        end tell
    end tell
end run
