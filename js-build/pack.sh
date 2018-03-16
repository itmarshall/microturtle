#!/bin/sh
cd logo-pack
browserify index.js -o dist/logo.js
minify dist/logo.js -o dist/logo.min.js 
cd -
