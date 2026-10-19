#!/bin/bash
# Plugin Studio for macOS — called by the app's applet.
#   launch.sh start   first-run setup, start the Studio server, print its PID
#   launch.sh open    open the Studio page again
#
# Your work lives in ~/Plugin Cookbook (the home folder, which macOS does not put behind a permission
# prompt, unlike Documents): plugins/ (your recipes and images) and build/ (built plugins). The app
# refreshes everything else there from the copy it carries, so updating the app updates the framework
# without touching your plugins.

set -u
RES="$(cd "$(dirname "$0")" && pwd)"
WORK="$HOME/Plugin Cookbook"
PORT=8765
URL="http://127.0.0.1:$PORT/"
ARCH="$(uname -m)"
[ -x "$RES/python-$ARCH/bin/python3" ] || ARCH=x86_64
PY="$RES/python-$ARCH/bin/python3"
CMAKE_BIN="$RES/$(cat "$RES/cmake-bin.txt")"
# Apps start with a bare PATH: add our Python and CMake, and where Claude Code and Homebrew live.
export PATH="$RES/python-$ARCH/bin:$CMAKE_BIN:$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

# Shows a dialog; prints the button clicked. Buttons are passed as AppleScript text, e.g. '{"Not now", "Install"}'.
ask() {
    osascript -e "display dialog \"$1\" with title \"Plugin Studio\" buttons $2 default button $3 with icon note" \
              -e 'button returned of result' 2>/dev/null
}

open_page() { open "$URL"; }

if [ "${1:-start}" = "open" ]; then
    open_page
    exit 0
fi

# Already running (e.g. the app was reopened): just show it.
if curl -s -o /dev/null --max-time 2 "$URL"; then
    open_page
    echo ""
    exit 0
fi

# 1. Workspace: copy the framework in, keep the user's plugins/ and build/.
mkdir -p "$WORK/plugins"
for d in framework tools dpf dpf-widgets examples prompts docs; do
    [ -d "$RES/cookbook/$d" ] && rsync -a --delete "$RES/cookbook/$d/" "$WORK/$d/"
done
find "$RES/cookbook" -maxdepth 1 -type f -exec cp -p {} "$WORK/" \;
[ -f "$WORK/plugins/README.md" ] || cp "$RES/cookbook/plugins/README.md" "$WORK/plugins/" 2>/dev/null

# 2. The C++ compiler (Apple's Command Line Tools). Without it the Studio still opens; Test and Build need it.
if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang++ >/dev/null 2>&1; then
    choice=$(ask "To test and build plugins, Plugin Studio needs Apple's free Command Line Tools (about 1 GB, installed once).\n\nClick Install and follow the window that opens. You can keep using the Studio meanwhile." '{"Not now", "Install"}' '"Install"')
    [ "$choice" = "Install" ] && xcode-select --install >/dev/null 2>&1
fi

# 3. Claude Code for the AI chat (optional; uses the person's Claude subscription).
if ! command -v claude >/dev/null 2>&1 && [ ! -f "$WORK/.skip-claude" ]; then
    choice=$(ask "The AI chat uses Claude Code with your Claude account.\n\nInstall it now? A Terminal window opens to install it and sign you in. Without it you can still edit recipes, design the interface and build." '{"Never", "Not now", "Install"}' '"Install"')
    case "$choice" in
        Install)
            osascript -e 'tell application "Terminal"' -e 'activate' \
                      -e 'do script "curl -fsSL https://claude.ai/install.sh | bash && ~/.local/bin/claude auth login && echo && echo Done: go back to Plugin Studio and send your message."' \
                      -e 'end tell' >/dev/null 2>&1 ;;
        Never) touch "$WORK/.skip-claude" ;;
    esac
fi

# 4. The Studio. It opens the page in the default browser.
cd "$WORK" || exit 1
nohup "$PY" tools/recipe.py studio --port "$PORT" >"$WORK/studio.log" 2>&1 &
echo $!
