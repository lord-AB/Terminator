#!/bin/bash
# Demo script for Terminator: Conditionals and Nested Ifs
TARGET="README.md"
if [ -f "$TARGET" ]; then
    echo "Found $TARGET"
    if [ -s "$TARGET" ]; then
        echo "$TARGET is non-empty!"
    fi
elif [ -d "$TARGET" ]; then
    echo "$TARGET is a directory"
else
    echo "$TARGET does not exist"
fi
