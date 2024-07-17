example handlers
================

Example handler scripts for [jakserver(1)](../jakserver.1).

The two `echo_*_handler.sh` scripts are HTML echo services. The [`env`](./echo_env_handler.sh)
variant wants the request body in the `$REQBODY` environment variable.
The [`stdin`](./echo_stdin_handler.sh) variant wants the request body on standard input.

The [`digest_auth.sh`](./digest_auth.sh) handler is an example how to set up
HTTP Digest authentication, the HTTP Authentication method that's one unit less
evil than HTTP Basic authentication. There are indications how to use it as a
wrapper for something else.
