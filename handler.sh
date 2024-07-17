#!/bin/bash
set -x
# Copyright 2024 Vlad Mesco
# 
# Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
# 
# 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
# 
# 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

REQMETHOD="$1"
REQPATHANDQUERY="$2"
if echo "$REQPATHANDQUERY" | grep -q '\?' ; then
    REQPATH="${REQPATHANDQUERY%%\?*}"
    REQQUERY="${REQPATHANDQUERY#*\?}"
else
    REQPATH="${REQPATHANDQUERY}"
    REQQUERY=""
fi

MPVCMDLINE="mpv --terminal=no --no-osc --input-ipc-server=~/mpv.sock --idle=yes"

error() {
    cat <<EOT
HTTP/1.1 $1
Cotnent-Type: text/html
Cache-Control: no-cache

<!DOCTYPE html>
<html><head><title>${REQPATH}</title></head>
<body>
<p>${REQMETHOD} ${REQPATH}: $2</p>
</body>
</html>
EOT
    exit 1
}

no_content() {
    cat <<EOT
HTTP/1.1 204 No content.
EOT
    exit 0
}

urldecode() {
    local Q
    Q="${1//%/\\x}"
    Q="${Q//+/ }"
    printf '%b' "$Q"
}

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

say() {
    echo "$@" | nc -NU ~/mpv.sock 1>&2
}

startdaemon() {
    ( DISPLAY=${DISPLAY:-:0} ${MPVCMDLINE} 1>&2 & ) &
    sleep 1
}
ison() {
    pgrep -fx "${MPVCMDLINE}" > /dev/null
}
turnoff() {
    if ison ; then
        say quit
        sleep 2
        ison && kill -9 "$(pgrep -fx "${MPVCMDLINE}")"
    fi
}

render_main_page() {
    ON_HTML='<form action="/controls/turnoff" method="POST" target="dummyframe"><input type="submit" value="Turn off"/></form>'
    # echo :-)
    cat <<EOT
HTTP/1.1 200
Cotnent-Type: text/html
Cache-Control: no-cache

<!DOCTYPE html>
<html><head><title>Player</title></head>
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
</body>
</html>
EOT
    exit 0
}

if [[ "$REQPATH" = "/" || "$REQPATH" = "/player" ]] ; then
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
        say seek $VALUE relative
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

    if [[ -z "${body_kvs[path]}" ]] ; then
        error 404 "No such file: ${body_kvs[path]}"
    fi

    if [[ -f "${body_kvs[path]}" ]] ; then
        supported "${body_kvs[path]}" || error 403 "Only video/audio files are allowed"
    fi

    say loadfile '"'"${body_kvs[path]}"'"' replace

    no_content
elif [[ "$REQPATH" = "/browse" ]] ; then
    if [[ "$REQMETHOD" != GET ]] ; then
        error 400 "Method not supported"
    fi

    parse_query

    if [[ -z "${kvs[path]}" ]] ; then
        error 400 "Missing ?path="
    fi

    IFS=''
    RAW="$(ls -1 "${kvs[path]}")"
    files=()
    IFS='
'
    PP="${kvs[path]%/}"
    files[0]="<li><a href='/browse?path=${PP%/*}/'>..</a></li>"
    for f in $RAW ; do
        PP="${kvs[path]}/$f"
        PP="${PP%/}"
        PP="${PP//\/\//\/}"
        ENCODEDPP="${PP//\'/&\#39;}"
        if [[ -d "$PP" ]] ; then
            files[${#files[@]}]="<li><a href='/browse?path=${ENCODEDPP}'>$f</a></li>"
        elif [ -f "$PP" ] && supported "$PP" ; then
            files[${#files[@]}]="<li><form style='display:inline' action='/controls/loadfile' method='POST'><input type='hidden' name='path' value='${ENCODEDPP}'/><input type='submit' value='${f//\'/&\#39;}'/></form></li>"
        else
            files[${#files[@]}]="<li>$f</li>"
        fi
    done
    IFS="$OLDIFS"

    # echo :-)
    cat <<EOT
HTTP/1.1 200
Cotnent-Type: text/html
Cache-Control: no-cache

<!DOCTYPE html>
<html><head><title>ls ${kvs[path]}</title></head>
<body>
<p>ls ${kvs[path]}</p>
<p>OK</p>
<ul>${files[@]}</ul>
</body>
</html>
EOT
    exit 0
else
    error 404 "Not found"
fi

error 500 "Internal server error"
