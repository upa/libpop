#!/bin/sh

# enable pr_debug()
pop=$(cd $(dirname $0); pwd)/boogiepop.c
debugctl="/sys/kernel/debug/dynamic_debug/control"

echo -n "file $pop +p" > $debugctl
