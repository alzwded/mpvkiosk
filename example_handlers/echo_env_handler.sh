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

# run with
#     jakserver -x ./echo_env_handler.sh

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

# echo :-)
cat <<EOT
HTTP/1.1 200
Content-Type: text/html;charset=UTF-8

<!DOCTYPE html>
<html><head><title>${REQPATH}</title></head>
<body>
<p>${REQMETHOD} ${REQPATH} ${REQQUERY}</p>
<p>OK</p>
<p>Headers:</p>
<pre>${REQHEADERS}</pre>
<p>Body:</p>
<pre>${REQBODY}</pre>
</body>
</html>
EOT
