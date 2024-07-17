#!/bin/bash

for i in `seq 10000` ; do
    ( curl -X POST -d 'hello' -H potato\; -H brotato:bruh localhost:8080 > /dev/null 2>/dev/null ) &
    #( curl -X GET  -H potato\; -H brotato:bruh localhost:8080 > /dev/null 2>/dev/null ) &
done
