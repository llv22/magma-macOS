#!/bin/bash
# git status > add.list
# then modify unnecessary files
input="./add.list"
while IFS= read -r line
do
  rm -rf $line
done < "$input"
