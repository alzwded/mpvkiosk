This Android app mostly mirrors the [web client](../README.md) of mpvkiosk
(and even relies on it for Browsing server local media). It exists to allow
you to "share" URLs from another app to your server.

At this time, you have to build the app yourself, though.

In `local.properties`, you can set

    sdk.dir=/......
    default.server.url=http://myserver.local:8080
    default.server.path=/my/media/disk

before building to have reasonable defaults for your local setup.
