#!/bin/bash
#
# DeepLux CLI wrapper
# Usage: deeplux <command> [options]
#

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Determine the executable path
if [ -x "$SCRIPT_DIR/DeepLux" ]; then
    EXEC_PATH="$SCRIPT_DIR/DeepLux"
elif [ -x "$SCRIPT_DIR/deeplux" ]; then
    EXEC_PATH="$SCRIPT_DIR/deeplux"
elif [ -x "/usr/local/bin/deeplux" ]; then
    EXEC_PATH="/usr/local/bin/deeplux"
elif [ -x "/usr/bin/deeplux" ]; then
    EXEC_PATH="/usr/bin/deeplux"
else
    EXEC_PATH="$SCRIPT_DIR/DeepLux"
fi

# Check if we're being called as 'deeplux' or via the binary directly
# If no arguments and not in a terminal, launch GUI
if [ $# -eq 0 ] && [ ! -t 0 ]; then
    # No arguments and not in terminal - launch GUI
    exec "$EXEC_PATH" --gui "$@"
else
    # Has arguments - run in CLI mode
    exec "$EXEC_PATH" "$@"
fi
