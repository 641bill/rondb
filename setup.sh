#!/bin/bash
#
# Prepare the MySQL source code tree for building
# with checked-out InnoDB Subversion directory.

# This script assumes that the current directory is storage/innobase.

set -eu

TARGETDIR=../storage/innobase

# link the build scripts
ln -sf $TARGETDIR/compile-innodb{,-debug} ../../BUILD

cd ../../mysql-test/t
ln -sf ../$TARGETDIR/mysql-test/*.test $TARGETDIR/mysql-test/*.opt .
cd ../r
ln -sf ../$TARGETDIR/mysql-test/*.result .
cd ../include
ln -sf ../$TARGETDIR/mysql-test/*.inc .
