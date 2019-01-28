#!/bin/sh
set -ex
if [ -n "$PMIX_BRANCH" ]; then
    git clone --depth=1 -b $PMIX_BRANCH https://github.com/pmix/pmix pmix-$PMIX_BRANCH
    cd pmix-$PMIX_BRANCH
    ./autogen.pl && ./configure --prefix=/usr --disable-picky $* && make && sudo make install
elif [ -n "$PMIX_RELEASE" ]; then
    wget https://github.com/pmix/pmix/releases/download/v${PMIX_RELEASE}/pmix-${PMIX_RELEASE}.tar.bz2 && tar xvfj pmix-${PMIX_RELEASE}.tar.bz2
    cd pmix-${PMIX_RELEASE}
    ./configure --prefix=/usr --disable-picky $* && make && sudo make install
else
    echo no PMIX_BRANCH nor PMIX_RELEASE, check the environment/travis matrix
fi
