#!/bin/sh

DIR=`dirname $0`

export CLASSPATH="${DIR}/antlr-4.7.1-complete.jar:${CLASSPATH}"

mkdir -p ${DIR}/jstmp

echo Building Logo parser.
java -jar ${DIR}/antlr-4.7.1-complete.jar -Dlanguage=JavaScript logo.g4 -o ${DIR}/jstmp

echo Building assembler parser.
java -jar ${DIR}/antlr-4.7.1-complete.jar -Dlanguage=JavaScript asm.g4 -o ${DIR}/jstmp

cp ${DIR}/jstmp/*.js ${DIR}/logo-pack
