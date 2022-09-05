#!/bin/bash

# Copyright (c) Dalton Durst, 2018
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LINK_BASE="https://github.com/halium/initramfs-tools-halium/releases/download/continuous/initrd.img-touch-"

ANROID_ARCH=$1
TARGET=$2

# Decide architecture by target name
case $ANROID_ARCH in
    "arm") ARCH="armhf";;
    "arm64") ARCH="arm64";;
    "x86") ARCH="i386";;
esac

LINK_FULL="$LINK_BASE$ARCH"

echo "Downloading $LINK_FULL"
curl --location $LINK_FULL --output "$TARGET" --silent