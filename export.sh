#!/bin/bash
#
# export current working directory in a format suitable for sending to MySQL
# as a snapshot. also generates the actual snapshot and sends it to MySQL.

set -eu

die () {
  echo $*
  exit 1
}

if [ $# -ne 2 ] ; then
  die "Usage: export.sh revision-number-of-last-snapshot current-revision-number"
fi

set +u
if test -z $EDITOR; then
  die "\$EDITOR is not set"
fi
set -u

rm -rf to-mysql
mkdir to-mysql{,/storage,/patches,/mysql-test{,/t,/r,/include}}
svn log -v -r "$(($1 + 1)):BASE" > to-mysql/log
svn export -q . to-mysql/storage/innobase

seq $(($1+1)) $2|while read REV
do
  PATCH=to-mysql/patches/r$REV.patch
  svn log -v -r$REV > $PATCH
  if [ $(wc -c < $PATCH) -gt 73 ]
  then
    svn diff -r$(($REV-1)):$REV >> $PATCH
  else
    rm $PATCH
  fi
done

cd to-mysql/storage/innobase
mv handler ../../sql

mv mysql-test/*.test mysql-test/*.opt ../../mysql-test/t
mv mysql-test/*.result ../../mysql-test/r
mv mysql-test/*.inc ../../mysql-test/include
rmdir mysql-test

rm setup.sh export.sh revert_gen.sh compile-innodb-debug compile-innodb

cd ../..
$EDITOR log
cd ..

fname="innodb-5.1-ss$2.tar.gz"

rm -f $fname
tar czf $fname to-mysql
scp $fname mysql:snapshots
rm $fname
rm -rf to-mysql

echo "Sent $fname to MySQL"
