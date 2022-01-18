#! /usr/bin/tclsh

# Demonstration of communication with mpv media player using JSON IPC
# Documentation here: https://mpv.io/manual/stable/#json-ipc
# When running this example, start an mpv player in a different console:
# mpv file.mkv --input-ipc-server=/tmp/mpvsocket
# Then you communicate with mpv through a unix socket.
# The example lacks error checking for obvious reasons

package require unix_sockets

set ipc [unix_sockets::connect /tmp/mpvsocket]
puts "ipc: $ipc"
# The JSON command is { "command": ["get_property", "playback-time"] }
# with all the braces and the quotes. Put the command in another pair
# of curly braces to ignore every special character in the string
set cmd {{ "command": ["get_property", "playback-time"] }}
puts $ipc "$cmd"
flush $ipc
# This is a blocking read on purpose
chan gets $ipc response
puts "response received: $response"

close $ipc
