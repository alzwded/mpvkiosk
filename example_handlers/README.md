example handlers
================

Example handler scripts for [jakserver(1)](../jakserver.1).

The two `echo_*_handler.sh` scripts are HTML echo services. The `env` variant wants
the request body in the `$REQBODY` environment variable. The `stdin` variant wants
the request body on standard input.

The `digest_auth.sh` handler is an example how to set up HTTP Digest authentication,
the one that's one step less evil than HTTP Basic authentication. There are indications
how to use it as a wrapper for something else.
