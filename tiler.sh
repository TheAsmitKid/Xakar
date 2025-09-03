#!/bin/bash

FIFO="$HOME/.tiler.sock"

ACTION="$1"

case "$ACTION" in
  center|fullscreen|left|right|up|down)
    echo "$ACTION" > "$FIFO"
    ;;
  *)
    echo "Usage: $0 [center|fullscreen|left|right|up|down]"
    exit 1
    ;;
esac