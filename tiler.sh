#!/bin/bash

# Default values
SCREEN_WIDTH=""
SCREEN_HEIGHT=""
BORDER_X=2
BORDER_Y=2
TITLEBAR=24
ACTION=""

# Helper: print usage
usage() {
  echo "Usage: $0 [--width WIDTH] [--height HEIGHT] [--border-x PX] [--border-y PX] [--titlebar PX] ACTION"
  echo "  ACTION = center | fullscreen | left | right | up | down"
  exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --width)
      SCREEN_WIDTH="$2"; shift 2 ;;
    --height)
      SCREEN_HEIGHT="$2"; shift 2 ;;
    --border-x)
      BORDER_X="$2"; shift 2 ;;
    --border-y)
      BORDER_Y="$2"; shift 2 ;;
    --titlebar)
      TITLEBAR="$2"; shift 2 ;;
    -*)
      echo "Unknown option: $1"; usage ;;
    *)
      ACTION="$1"; shift ;;
  esac
done

# Detect resolution if not provided
if [[ -z "$SCREEN_WIDTH" || -z "$SCREEN_HEIGHT" ]]; then
  RES=$(xrandr | grep '*' | head -n1 | awk '{print $1}')
  SCREEN_WIDTH=${RES%x*}
  SCREEN_HEIGHT=${RES#*x}
fi

# Calculate usable dimensions
let "OVERHEAD_X = 2 * BORDER_X"
let "OVERHEAD_Y = 2 * BORDER_Y + TITLEBAR"
let "USE_WIDTH = SCREEN_WIDTH - OVERHEAD_X"
let "USE_HEIGHT = SCREEN_HEIGHT - OVERHEAD_Y"
let "HALF_WIDTH = USE_WIDTH / 2"
let "HALF_HEIGHT = USE_HEIGHT / 2"

# Action handler
case "$ACTION" in
  center)
    xdotool getactivewindow windowmove $(( (SCREEN_WIDTH - 1024)/2 )) $(( (SCREEN_HEIGHT - 600)/2 )) windowsize 1024 600
    ;;

  fullscreen)
    xdotool getactivewindow windowmove 0 0 windowsize "$USE_WIDTH" "$USE_HEIGHT"
    ;;

  left)
    xdotool getactivewindow windowmove 0 0 windowsize "$HALF_WIDTH" "$USE_HEIGHT"
    ;;

  right)
    xdotool getactivewindow windowmove "$HALF_WIDTH" 0 windowsize "$HALF_WIDTH" "$USE_HEIGHT"
    ;;

  down)
    width=$(xdotool getwindowgeometry --shell $(xdotool getactivewindow) | grep -oP 'WIDTH=\K\d+')
    x=$(xdotool getwindowgeometry --shell $(xdotool getactivewindow) | grep -oP 'X=\K\d+')
    if [[ "$width" -lt "$((HALF_WIDTH + 1))" ]]; then
      if [[ "$x" -lt "$((SCREEN_WIDTH / 2))" ]]; then
        xdotool getactivewindow windowmove 0 "$HALF_HEIGHT" windowsize "$HALF_WIDTH" "$HALF_HEIGHT"
      else
        xdotool getactivewindow windowmove "$HALF_WIDTH" "$HALF_HEIGHT" windowsize "$HALF_WIDTH" "$HALF_HEIGHT"
      fi
    else
      xdotool getactivewindow windowmove 0 "$HALF_HEIGHT" windowsize "$USE_WIDTH" "$HALF_HEIGHT"
    fi
    ;;

  up)
    width=$(xdotool getwindowgeometry --shell $(xdotool getactivewindow) | grep -oP 'WIDTH=\K\d+')
    x=$(xdotool getwindowgeometry --shell $(xdotool getactivewindow) | grep -oP 'X=\K\d+')
    if [[ "$width" -lt "$((HALF_WIDTH + 1))" ]]; then
      if [[ "$x" -lt "$((SCREEN_WIDTH / 2))" ]]; then
        xdotool getactivewindow windowmove 0 0 windowsize "$HALF_WIDTH" "$HALF_HEIGHT"
      else
        xdotool getactivewindow windowmove "$HALF_WIDTH" 0 windowsize "$HALF_WIDTH" "$HALF_HEIGHT"
      fi
    else
      xdotool getactivewindow windowmove 0 0 windowsize "$USE_WIDTH" "$HALF_HEIGHT"
    fi
    ;;

  *)
    usage ;;
esac