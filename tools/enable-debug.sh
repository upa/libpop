#!/bin/sh

# enable pr_debug()
pop=`cd ../kmod && pwd`/pop.c
debugctl="/sys/kernel/debug/dynamic_debug/control"

echo -n "file $pop  +p"  > $debugctl
