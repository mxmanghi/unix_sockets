UNIX_SOCKETS
============

This package adds support for Unix domain sockets to Tcl.

Client
------

~~~tcl
package require unix_sockets

set chan    [unix_sockets::connect /tmp/.foo]
~~~

Server
------

~~~tcl
package require unix_sockets

proc accept chan {
    # As for socket channels
}

set listen  [unix_sockets::listen /tmp/.bar accept]

vwait ::forever
~~~

License
-------

Licensed under the same terms as the Tcl Core
