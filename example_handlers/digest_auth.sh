#!/bin/bash

# Copyright 2024 Vlad Mesco
# 
# Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
# 
# 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
# 
# 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# example handler program for jakserver(1)
#
# This handler gives an example how to implement Digest authentication,
# which is less dramatically bad over unencrypted HTTP compared to Basic authentication.
# It uses the "old style" MD5 based digest protocol, because the modern ones are
# the same, but more complicated. Read up on MDN or similar web focused website
# (or the RFC itself) how you're supposed to implement this with SHA-512 or w/e
# the hashing algorithm du jour is

# run with
#     jakserver -x ./digest_auth.sh
#
# experiment with
#     curl --digest -u jim:password -v localhost:8080

trap 'echo exited 1>&2' EXIT
set -x

INSECURE_USER=jim
INSECURE_PASSWORD=password
INSECURE_REALM=home

REQMETHOD="$1"
REQPATH="$2"

# ignore Chromium spam
if echo "$REQPATH" | grep -q 'favico' ; then
    cat <<EOT
HTTP/1.1 404 Not found.

EOT
    exit 0
fi

# Find the authorization header; assuming it all fits on a single line and the
# client didn't use continuations
AUTHORIZATION="$(echo "$REQHEADERS" | grep "authorization: [Dd]igest \(.*\)\(,.*\)*")"

# mixin a recent timestamp to avoid repeat attacks;
# technically the nonce should be usable only once, but that requires some sort of state
RECENT=$((  $( date +%s ) >> 6  ))

send_401() {
    # generate a nonce
    # SECRET/salt should be added, I guess
    NONCE="$( echo -n "${SECRET:-potato}$(date +%s%N)"  | md5sum | cut -d' ' -f 1 )$RECENT"
    cat <<EOT
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Digest realm=$INSECURE_REALM, nonce=$NONCE

EOT
    exit 0
}

if [[ -z "$AUTHORIZATION" ]] ; then
    # if the Authorization: header isn't there, challenge the client
    send_401
else
    # parse the Authorization header.
    # we assume the whole thing is on one line; it is allowed to not be, in which
    # case subsequent lines start with a tab or whitespace (see RFC)

    # the client response
    resp="$( echo "$AUTHORIZATION" | grep -o "response=\"\{0,1\}[a-zA-Z0-9+/=]*\"\{0,1\}" | sed -e 's/response=//' | sed -e 's/"//g' )"
    # the nonce used
    nonce="$( echo "$AUTHORIZATION" | grep -o "nonce=\"\{0,1\}[a-zA-Z0-9+/=]*\"\{0,1\}" | sed -e 's/nonce=//' | sed -e 's/"//g' )"
    # the uri they're accessing; we'll believe them, I don't want to check
    uri="$( echo "$AUTHORIZATION" | grep -o "uri=\"\{0,1\}[a-zA-Z0-9+/=]*\"\{0,1\}" | sed -e 's/uri=//' | sed -e 's/"//g' )"
    # get the username
    username="$( echo "$AUTHORIZATION" | grep -o "username=\"\{0,1\}[a-zA-Z0-9+/=]*\"\{0,1\}" | sed -e 's/username=//' | sed -e 's/"//g' )"

    # check nonce is recent
    crecent="${nonce:32}"

    # compute HA1, HA2 and the digest on our end
    #HA1="$(echo -n "$INSECURE_USER:$INSECURE_REALM:$INSECURE_PASSWORD" | md5sum | cut -d' ' -f 1)"
    HA1="$(echo -n "$username:$INSECURE_REALM:$INSECURE_PASSWORD" | md5sum | cut -d' ' -f 1)"
    HA2="$(echo -n "${REQMETHOD}:${uri}" | md5sum | cut -d' ' -f 1)"
    ref="$(echo -n "${HA1}:${nonce}:${HA2}" | md5sum | cut -d' ' -f 1)"

    # if digests match and the nonce was recent enough
    if [[ "$resp" = "$ref"  &&  "$crecent" = "$RECENT" ]] ; then
        # you get to see the response!
        cat <<EOT
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 3
X-IsSecretToEveryone: yes

OK
EOT
        # alternatively, you can exec off to some other script that actually does
        # something interesting, now that jim has logged in
    else
        # important; send 401 and not 403; 401 allows you to log in again e.g.
        # if you messed up the password, or the clock rolled over etc.
        #
        # Use 403 e.g. only if jim is not allowed to access some path
        send_401
    fi
fi
