#!/bin/bash

#echo "const char gitdesc[]=\\\"\`git describe --abbrev=2\`\\\"\;" > git_describe.txt
GIT_DESCRIBE=$(git describe --abbrev=2)
echo description: $GIT_DESCRIBE
echo directory: $1
echo const char gitdesc[]=\"$GIT_DESCRIBE\"\;  > $1/build/git_describe.h

