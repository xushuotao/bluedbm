#!/bin/bash

mkdir -p tools
cd tools;

#new connectal
git clone https://github.com/cambridgehackers/connectal.git connectal
cd connectal;
git reset --hard 696f3faa5ea3cf1073fe3f0b38a530df331ca796
cd ../;
#connectal version: 696f3faa5ea3cf1073fe3f0b38a530df331ca796
#working on flash @ 1.2GB/s

#old xbsv
git clone https://github.com/cambridgehackers/xbsv.git xbsv
cd xbsv;
#git reset --hard 8cd24334a5968b1f37809b44c3c422010e2ca27a
#git reset --hard f77dd6e4cf225e0fef530d3a7eeb4d88449a134a
#git reset --hard 9aae77e6cf62b4069524cb76e92f03dbd5778939
git reset --hard 3618398258e31297941a5fb4fe2734a540ad0d5d
cd ../;

git clone https://github.com/cambridgehackers/fpgamake.git
git clone https://github.com/cambridgehackers/buildcache.git

