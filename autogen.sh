#!/bin/bash
set -e
aclocal -I m4
autoreconf -fi --warning=no-portability
automake --add-missing
