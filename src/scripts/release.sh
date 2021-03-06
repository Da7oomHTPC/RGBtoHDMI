#!/bin/bash

# exit on error
set -e

# Check if any uncommitted changes in tracked files
if [ -n "$(git status --untracked-files=no --porcelain)" ]; then
    echo "Uncommitted changes; exiting....."
    exit
fi

# Lookup the last commit ID
VERSION="$(git rev-parse --short HEAD)"

NAME=RGBtoHDMI_$(date +"%Y%m%d")_$VERSION

DIR=releases/${NAME}
mkdir -p $DIR

for MODEL in rpi
do
    # compile debug kernel
    ./clobber.sh
    ./configure_${MODEL}.sh -DDEBUG=1
    make -B -j
    mv kernel${MODEL}.img ${DIR}/kernel${MODEL}_debug.img
    # compile normal kernel
    ./clobber.sh
    ./configure_${MODEL}.sh
    make -B -j
    mv kernel${MODEL}.img ${DIR}/kernel${MODEL}.img
done

cp -a firmware/* ${DIR}

# Create a simple README.txt file
cat >${DIR}/README.txt <<EOF
RGBtoHDMI

(c) 2018 David Banks (hoglet), Ian Bradbury (IanB), Dominic Plunkett (dp11) and Ed Spittles (BigEd)

  git version: $(grep GITVERSION gitversion.h  | cut -d\" -f2)
build version: ${NAME}
EOF

cp config.txt cmdline.txt ${DIR}
cd releases/${NAME}
zip -qr ../${NAME}.zip .
cd ../..

unzip -l releases/${NAME}.zip
