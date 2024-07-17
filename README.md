mpvkiosk
========

This little project is written to allow easy control of mpv running on a
little raspberry pi which has a monitor connected to it, but no keyboard.

The project is build with two components. [*jakserver*](./jakserver.c) is the tiniest HTTP
server, which fully delegates request interpretation to a subprocess running
a single possible script. See [*jakserver(1)*](./jakserver.1) for more
information on it. It's great for prototyping and not worrying about configuring
a full apache or thttpd or whatever.

The other component is the [*handler.sh*](./handler.sh) which implements the
"remote control" web page. This makes sure to run a daemonized *mpv(1)* and
talks to it by way of its inbuilt `--input-ipc-server`.

In my case, *mpv(1)* is running in a *sway(1)* session.

Installation
------------

Install *jakserver*

    make
    make install

Put the *handler.sh* script somewhere intelligent (e.g. /var/www/handler.sh),
then run *jakserver*

    install -m 755 -D handler.sh /var/www/handler.sh
    jakserver -x /var/www/handler.sh -p 8080

You should set up a `chroot(1)` in `/var/www` and/or run `jakserver` as
a somewhat limitted user.

You can customize the `handler.sh` script to do what you want. E.g. add
support for *feh(1)* to look at pictures, or playlist support, etc.

There's an [example systemd unit](./mpvkiosk.service) which you can drop
in `/etc/systemd/system/mpvkiosk.service` and turn it on at boot. In my case,
I have *sway(1)* autostart and autologin, and it has the advantage that any
app is effectively full screen by default.
