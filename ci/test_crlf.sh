#!/bin/bash

MY_PATH="`dirname \"$0\"`"
cd $MY_PATH/..

file $(find *) .dockerignore .gitattributes .gitignore .gitlab-ci.yml | grep "with CRLF"
if [ $? -eq 1 ]; then
    exit 0
else
    exit 1
fi
