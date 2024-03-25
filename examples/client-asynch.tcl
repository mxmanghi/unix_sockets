#!/usr/bin/env tclsh
#
#   client.tcl sends a 'hello, world' message to the server but it
#   can be loaded into the shell and used as a playground
#   for examining fully asynchronous handling of messages, events and errors
#   For this purpose create a client channel with
#
#   set connection [open_channel]
#
# and send messages with
#
#   send_to_server $connection <msg>
#
#   sending an "EXIT" string causes the server to shutdown. This can be 
#   used to examine the event sequence brought about by such condition
#

lappend auto_path .

package require unix_sockets

# actual asynchronous data reader.
# The procedure checks for the eof condition
# and in case closes the channel associated with
# the socket

proc readable {con} {
    if {[chan eof $con]} { 
        puts "eof detected"
        chan close $con
        return
    }

    set msg [chan gets $con]
    puts "got response: ($msg)"
    incr ::done
}

# open_channel
#
# creates a channel connected to the server socket and
# returns the identification of such channel

proc open_channel {} {
    set con [unix_sockets::connect /tmp/example.socket]
    chan event $con readable [list readable $con]
    return $con
}

# 

proc send_to_server {chanid msg} {

    chan puts $chanid $msg
    chan flush $chanid
    vwait ::done

}

set con [open_channel]
send_to_server $con "hello, world"

chan close $con
