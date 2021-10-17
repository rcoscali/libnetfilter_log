#!/bin/sh -e

BUILD_MAN=doxygen/build_man.sh

# Allow to override build_man.sh url for local testing
# E.g. export NFQ_URL=file:///usr/src/libnetfilter_queue
curl ${NFQ_URL:-https://git.netfilter.org/libnetfilter_queue/plain}/$BUILD_MAN\
  -o$BUILD_MAN
chmod a+x $BUILD_MAN

autoreconf -fi
rm -Rf autom4te.cache
