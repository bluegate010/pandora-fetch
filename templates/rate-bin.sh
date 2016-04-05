#!/bin/bash

PANDORA_DIR="__PANDORA_DIR__"
CONFIG_DIR="__PANDORA_HOME__/.config/pianobar"

case "$1" in
	love|ban)
		rating="$1"
		;;
	*)
		echo "Usage: <executable> {love | ban}"
		exit 1
esac

if [ ! -f "$PANDORA_DIR/templates/config-auth" ]; then
	echo "Error: please provide config-auth file in $PANDORA_DIR/templates"
	exit 1
elif [ ! -f "$PANDORA_DIR/bin/pianobar" ]; then
	echo "Error: please compile pianobar from $PANDORA_DIR/pianobar-source and move the binary to $PANDORA_DIR/bin/pianobar"
	exit 1
fi

# Create config file, only if necessary.
if [ ! -f "$CONFIG_DIR/config" ]; then
	addedConfig=true
	sudo -u __PANDORA_ID__ cp "$PANDORA_DIR/templates/config-auth" "$CONFIG_DIR/config"
	sudo -u __PANDORA_ID__ rm -f "$CONFIG_DIR/state"
fi

# Placeholders replaced by eventcmd.sh
export AUTORATE_TRACK_TOKEN="__TRACK_TOKEN__"
export AUTORATE_STATION_ID="__STATION_ID__"
export AUTORATE_RATING="$rating"

# pianobar examines $HOME, so we need to overwrite it here.
export HOME="__PANDORA_HOME__"

# -E to pass in environment variables we just set.
sudo -E -u __PANDORA_ID__ $PANDORA_DIR/bin/pianobar

if [ "$addedConfig" = true ]; then
	sudo -u __PANDORA_ID__ rm -f "$CONFIG_DIR/config" "$CONFIG_DIR/state"
fi
