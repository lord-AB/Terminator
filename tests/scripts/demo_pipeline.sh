#!/bin/bash
# Demo script for Terminator: Pipelines, Variables, and Redirection
MSG="Hello from Terminator Scripting"
echo "Running: $MSG"
echo "Listing top 5 lines:"
ls -la | head -5
echo "Testing arithmetic test:"
VAL=10
if [ "$VAL" -ge 5 ]; then
    echo "Condition passed: VAL is >= 5"
fi
exit 0
