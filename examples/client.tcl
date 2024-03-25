#!/usr/bin/env tclsh
#
#   client.tcl sends a 'hello, world' message to the server. 
#   This procedure can be loaded into the shell and used as a playground
#   for examining fully asynchronous handling of messages, events and errors
#
#   sending an "EXIT" messages causes the server to shutdown. This can be 
#   used to examine the event sequence brought about by a server shutdown
#

lappend auto_path .

package require unix_sockets

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

proc send_to_server {chanid msg} {

    chan puts $chanid $msg
    chan flush $chanid
    vwait ::done

}

set con [open_channel]
send_to_server $con "hello, world"

chan close $con
