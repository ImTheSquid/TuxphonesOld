#!/bin/bash
rm -r build/
npm install --openssl_fips=X
./node_modules/.bin/node-gyp --openssl_fips=X rebuild --target=13.4.0 --arch=x64 --dist-url=https://electronjs.org/headers/ --debug

# Copy binary data into index.js to bundle
cp index.js build.js
sed -i '/module\.exports = (Plugin, Library) => {/d' build.js
sed -i "/.*const nativeCodeHex = 'PLACEHOLDER';/d" build.js

if [[ -f "build/Release/tuxphones.node" ]]; then
    native=`xxd -p build/Release/tuxphones.node | tr -d '\n'`
else
    exit 0
fi

echo "1iconst nativeCodeHex = '$native';" > build/native.sed
sed -i  -f build/native.sed build.js
sed -i '1i"use strict";' build.js
sed -i '1imodule.exports = (Plugin, Library) => {' build.js

# Build plugin
npm run build_plugin Tuxphones --prefix ../../
rm build.js

# Copy completed plugin back for release and clear undefined data
cp ../../release/Tuxphones.plugin.js .
# Remove all header data that is undefined
sed -i '/^ \* @.*undefined$/d' ./Tuxphones.plugin.js