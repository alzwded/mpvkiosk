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

# mpv remote control handler program for jakserver(1)

trap 'echo exited 1>&2' EXIT
set -x

REQMETHOD="$1"
REQPATHANDQUERY="$2"
if echo "$REQPATHANDQUERY" | grep -q '?' ; then
    REQPATH="${REQPATHANDQUERY%%\?*}"
    REQQUERY="${REQPATHANDQUERY#*\?}"
else
    REQPATH="${REQPATHANDQUERY}"
    REQQUERY=""
fi

# keep this around, needed for pgrep -fx
MPVCMDLINE="mpv --terminal=no --no-osc --input-ipc-server=~/mpv.sock --idle=yes"

# send an error response with human readable html, and exit
error() {
    cat <<EOT
HTTP/1.1 $1
Content-Type: text/html;charset=UTF-8
Cache-Control: no-cache

<!DOCTYPE html>
<html><head>
<title>${REQPATH}</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body {
    font-size: 12pt;
}
</style>
</head>
<body>
<p>${REQMETHOD} ${REQPATH}: $2</p>
</body>
</html>
EOT
    exit 1
}

# send back a 204 with no body, and exit
no_content() {
    cat <<EOT
HTTP/1.1 204 No content.
EOT
    exit 0
}

# naive urldecoder
urldecode() {
    local Q
    Q="${1//%/\\x}"
    Q="${Q//+/ }"
    printf '%b' "$Q"
}

# parse query string
# assumes it's only a query string, no ;parameters or anything else that's
# in the RFC but nobody actually ever uses ever
parse_query() {
    #declare -A kvs
    kvs=()
    OLDIFS="$IFS"
    IFS='&'
    for kv in $REQQUERY ; do
        kvs["${kv%%=*}"]="$(urldecode "${kv#*=}")"
    done
    IFS="$OLDIFS"
}

# parse url encoded form data
parse_body() {
    #declare -A body_kvs
    body_kvs=()
    OLDIFS="$IFS"
    IFS='&'
    for kv in $REQBODY ; do
        body_kvs["${kv%%=*}"]="$(urldecode "${kv#*=}")"
    done
    IFS="$OLDIFS"
}

# check if we want to play a file through mpv
# the list of extensions is arbitrary
# using file(1) and checking for media mime types would be a better idea
supported() {
    # whitelist extensions
    EXT="${1##*.}"

    case "$EXT" in
        mp4|mkv|avi|mp3|wma|wav|flac|vp9|mov|webm|ogv|m4v|m4a|aac|ogg)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

# talk to the mpv socket
say() {
    echo "$@" | nc -NU ~/mpv.sock 1>&2
}

# start mpv daemonized; WAYLAND_DISPLAY or DISPLAY must be set in the environment
# if you want this running on tty1 without a display server, then you
# need to run it through getty and ignore the fact that this script can
# start it itself; perhaps remove startdaemon, ison and turnof in that case
startdaemon() {
    echo Starting mpv 1>&2

    # note to self
    # wl_display_add_socket(display, NULL) would use WAYLAND_DISPLAY to
    # decide what number to use; sway doesn't do that, it starts from 1
    # and piks the first free slot up to 32 (?!)
    # so let's unga bunga this
    #
    # you can browse XDG_RUNTIME_DIR or /proc/*/environ to figure what
    # the thing you want is if you have multiple things running. Or use
    # a sane compositor which you can control better. Or X11, X11 behaves
    # itself.

    if [[ -z "$DISPLAY" && -z "$WAYLAND_DISPLAY" ]] ; then
        echo 'Neither DISPLAY nor WAYLAND_DISPLAY set' 1>&2
        # if you require some complicated figuring out of the display number
        # (X11 or Wayland), do so here, then export DISPLAY WAYLAND_DISPLAY
        #
        # You could find the exact process you're interested in and grab its
        # /proc/pid/environ and export it here entirely, it will be exactly
        # like running mpv in that display server session!
    fi

    ( ${MPVCMDLINE} 1>&2 & ) &
    # it won't set up the socket immediately, so wait
    sleep 5
}
# check if mpv is running as intended
ison() {
    pgrep -fx "${MPVCMDLINE}" > /dev/null
}
# turn off the mpv we started; this is a panic button
turnoff() {
    if ison ; then
        # ask it nicely to shutdown
        say quit
        sleep 2
        # if it's still running, tell the OS to shut it down
        ison && kill -9 "$(pgrep -fx "${MPVCMDLINE}")"
    fi
}

# render the player controls, and exit
render_main_page() {
    ON_HTML='<form action="/controls/turnoff" method="POST" target="dummyframe"><input type="submit" value="Turn off"/></form>'
    # echo :-)
    cat <<EOT
HTTP/1.1 200
Content-Type: text/html;charset=UTF-8
Cache-Control: no-cache

<!DOCTYPE html>
<html><head>
<title>Player</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body {
    font-size: 12pt;
}
</style>
</head>
<body>
<iframe name="dummyframe" id="dummyframe" style="display:none;"></iframe>
<p>${ON_HTML}</p>
<p><form style="display:inline" action="/controls/loadfile" method="POST" target="dummyframe">
     <input type="text" name="path"/>
     <input type="submit" value="Load"/>
   </form>
   <a href="/browse?path=%2Fmnt%2FBAK_DISK">Browse</a>
</p>
<p>
  <form style="display:inline" action="/controls/playpause" method="POST" target="dummyframe">
    <input type="submit" value="|&gt;"/>
  </form>
  <form style="display:inline" action="/controls/stop" method="POST" target="dummyframe">
    <input type="submit" value="[ ]"/>
  </form>
  <form style="display:inline" action="/controls/showprogress" method="POST" target="dummyframe">
    <input type="submit" value="progress"/>
  </form>
</p>
  <form style="display:inline" action="/controls/seek" method="POST" target="dummyframe">
    <input type="hidden" name="value" value="-600"/>
    <input type="submit" value="-10'"/>
  </form>
  <form style="display:inline" action="/controls/seek" method="POST" target="dummyframe">
    <input type="hidden" name="value" value="-60"/>
    <input type="submit" value="-1'"/>
  </form>
  <form style="display:inline" action="/controls/seek" method="POST" target="dummyframe">
    <input type="hidden" name="value" value="-10"/>
    <input type="submit" value="-10&#34;"/>
  </form>
  <form style="display:inline" action="/controls/seek" method="POST" target="dummyframe">
    <input type="hidden" name="value" value="10"/>
    <input type="submit" value="+10&#34;"/>
  </form>
  <form style="display:inline" action="/controls/seek" method="POST" target="dummyframe">
    <input type="hidden" name="value" value="60"/>
    <input type="submit" value="+1'"/>
  </form>
  <form style="display:inline" action="/controls/seek" method="POST" target="dummyframe">
    <input type="hidden" name="value" value="600"/>
    <input type="submit" value="+10'"/>
  </form>
</p>
<p>
  <form style="display:inline" action="/controls/prev" method="POST" target="dummyframe">
    <input type="submit" value="&lt; prev"/>
  </form>
  <form style="display:inline" action="/controls/next" method="POST" target="dummyframe">
    <input type="submit" value="next &gt;"/>
  </form>
  <br/>
  <form style="display:inline" action="/controls/jump" method="POST" target="dummyframe">
    <input name="index" value="1"/>
    <input type="submit" value="jump"/>
  </form>
</p>
</body>
</html>
EOT
    exit 0
}

#############
# endpoints #
#############

if [[ "$REQPATH" = "/" || "$REQPATH" = "/player" ]] ; then
    # the main page is the player controls
    if [[ "$REQMETHOD" != GET ]] ; then
        error 400 "Method not supported"
    fi
    render_main_page
elif [[ "$REQPATH" = "/controls/playpause" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    say cycle pause

    no_content
elif [[ "$REQPATH" = "/controls/stop" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    say stop

    no_content
elif [[ "$REQPATH" = "/controls/showprogress" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    say show-progress

    no_content
elif [[ "$REQPATH" = "/controls/next" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    say playlist-next

    no_content
elif [[ "$REQPATH" = "/controls/jump" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    parse_body

    if [[ -z "${body_kvs[index]}" ]] ; then
        error 400 "missing index"
    fi

    IDX="$( printf %d "${body_kvs[index]}" )"

    say set playlist-pos-1 "$IDX"

    no_content
elif [[ "$REQPATH" = "/controls/prev" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    say playlist-prev

    no_content
elif [[ "$REQPATH" = "/controls/seek" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || error 500 "Not running"

    parse_body
    VALUE="${body_kvs[value]}"

    if [[ -z "$VALUE" ]] ; then
        error 400 "Missing value="
    fi

    if echo "$VALUE" | grep -q "^\w*-\{0,1\}[0-9]\{1,\}\w*$" ; then
        VALUE=$( printf %d "$VALUE" )
        say seek "$VALUE" relative
        no_content
    else
        error 400 "Bad value="
    fi
elif [[ "$REQPATH" = "/controls/turnoff" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    turnoff

    no_content
elif [[ "$REQPATH" = "/controls/loadfile" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || startdaemon
    parse_body

    # we need something to play
    if [[ -z "${body_kvs[path]}" ]] ; then
        error 404 "No such file: ${body_kvs[path]}"
    fi

    # if it's a local file, check if it's something we don't want to play
    if [[ -f "${body_kvs[path]}" ]] ; then
        supported "${body_kvs[path]}" || error 403 "Only video/audio files are allowed"
    fi
    # perhaps whitelist remote URLs here, e.g. only youtube

    # load it!
    say loadfile '"'"${body_kvs[path]//\"/\\\"}"'"' replace

    no_content
elif [[ "$REQPATH" = "/controls/loaddir" ]] ; then
    if [[ "$REQMETHOD" != POST ]] ; then
        error 400 "Method not supported"
    fi
    ison || startdaemon
    parse_body

    # we need something to play
    if [[ -z "${body_kvs[path]}" ]] ; then
        error 404 "No such file: ${body_kvs[path]}"
    fi

    # check it's a dir
    if [[ ! -d "${body_kvs[path]}" ]] ; then
        error 404 "Not found"
    fi

    OLDIFS="$IFS"
    IFS='
'
    ALLFILES=( "${body_kvs[path]}"/* )
    IFS="$OLDIFS"
    declare -a FILTEREDFILES
    FILTEREDFILES=()
    for FF in "${ALLFILES[@]}" ; do
        if [[ ! -f "$FF" ]] ; then
            continue
        fi
        if supported "$FF" ; then
            FILTEREDFILES+=("$FF")
        fi
    done

    if [[ ${#FILTEREDFILES[@]} -eq 0 ]] ; then
        error 404 "No compatible files in ${body_kvs[path]}"
    fi

    # load the first one
    say loadfile '"'"${FILTEREDFILES[0]}"'"' replace

    # for everything else, append to playlist
    for (( i=1 ; i < ${#FILTEREDFILES[@]} ;  i++ )) ; do
        say loadfile '"'"${FILTEREDFILES[${i}]}"'"' append
    done

    no_content
elif [[ "$REQPATH" = "/browse" ]] ; then
    # local file browser
    if [[ "$REQMETHOD" != GET ]] ; then
        error 400 "Method not supported"
    fi

    parse_query

    if [[ -z "${kvs[path]}" ]] ; then
        error 400 "Missing ?path="
    fi

    # list directory
    IFS=''
    RAW="$(ls -1 "${kvs[path]}")"
    files=()
    IFS='
'
    PP="${kvs[path]%/}"
    # add parent directory manually
    files[0]="<li><a href='/browse?path=${PP%/*}/'>..</a></li>"
    # for each file, render a list item
    for f in $RAW ; do
        # do some normalization and ecaping of the path
        PP="${kvs[path]}/$f"
        PP="${PP%/}"
        PP="${PP//\/\//\/}"
        ENCODEDPP="${PP//\'/&\#39;}"
        # if it's a directory, link it back to this file browser
        if [[ -d "$PP" ]] ; then
            files[${#files[@]}]="<li><a href='/browse?path=${ENCODEDPP}'>$f</a></li>"
        # if it's some file we want to play, add a form linking it to the loadfile control
        elif [ -f "$PP" ] && supported "$PP" ; then
            files[${#files[@]}]="<li><form style='display:inline' action='/controls/loadfile' method='POST'><input type='hidden' name='path' value='${ENCODEDPP}'/><input type='submit' value='${f//\'/&\#39;}'/></form></li>"
        # else, just list it for information purposes
        # it would probably be better to skip them
        else
            files[${#files[@]}]="<li>$f</li>"
        fi
    done
    IFS="$OLDIFS"

    ENCODEDPATH="${kvs[path]}"
    ENCODEDPATH="${ENCODEDPATH//\'/&\#39;}"
    # render response and exit
    cat <<EOT
HTTP/1.1 200
Content-Type: text/html;charset=UTF-8
Cache-Control: no-cache

<!DOCTYPE html>
<html><head>
<title>ls ${kvs[path]}</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body {
    font-size: 12pt;
}
</style>
</head>
<body>
<p><a href="/player">Player Controls</a></p>
<p>ls ${kvs[path]}</p>
<p><form style='display:inline' action="/controls/loaddir" method="POST">
   <input type="hidden" name="path" value='$ENCODEDPATH'/>
   <input type="submit" value="Play all"/>
   </form></p>
<ul>${files[@]}</ul>
</body>
</html>
EOT
    exit 0
else
    error 404 "Not found"
fi

error 500 "Internal server error"
