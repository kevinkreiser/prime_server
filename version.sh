#!/bin/sh

#grabs the version string from the header file for use autotools
#which then throws it into a pc file so that pkg-config can use it

if [ ! -f prime_server/prime_server.hpp ]; then
	echo "Could not find version information"
	exit 1
fi

major=$(grep -F VERSION_MAJOR prime_server/prime_server.hpp | sed -e "s/.*MAJOR \([0-9][0-9]*\)/\1/g" -e 1q)
minor=$(grep -F VERSION_MINOR prime_server/prime_server.hpp | sed -e "s/.*MINOR \([0-9][0-9]*\)/\1/g" -e 1q)
patch=$(grep -F VERSION_PATCH prime_server/prime_server.hpp | sed -e "s/.*PATCH \([0-9][0-9]*\)/\1/g" -e 1q)

if [ -z $major ] || [ -z $minor ] || [ -z $patch ]; then
	echo "Malformed version information"
	exit 1
fi

printf "%s" "$major.$minor.$patch"
