#!/bin/bash

declare -A headers
headers[crmcommon]="include/crm/common include/crm/crm.h"
headers[crmcluster]="include/crm/cluster.h"
headers[crmservice]="include/crm/services.h"
headers[transitioner]="include/crm/transition.h"
headers[cib]="include/crm/cib.h include/crm/cib/util.h"
headers[pe_rules]="include/crm/pengine/rules.h"
headers[pe_status]="include/crm/pengine/common.h  include/crm/pengine/complex.h  include/crm/pengine/rules.h  include/crm/pengine/status.h"
headers[pengine]="include/crm/pengine/common.h  include/crm/pengine/complex.h  include/crm/pengine/rules.h  include/crm/pengine/status.h"
headers[stonithd]="include/crm/stonith-ng.h"
headers[lrmd]="include/crm/lrmd.h"

if [ ! -z $1 ]; then
    LAST_RELEASE=$1
else
    LAST_RELEASE=`test -e /Volumes || git tag -l | grep Pacemaker | grep -v rc | sort -Vr | head -n 1`
fi
libs=$(find . -name "*.am" -exec grep "lib.*_la_LDFLAGS.*version-info"  \{\} \; | sed -e s/_la_LDFLAGS.*// -e s/^lib//)
for lib in $libs; do
    if [ -z "${headers[$lib]}" ]; then
	echo "Unknown headers for lib$lib"
	exit 0
    fi
    git diff -w $LAST_RELEASE..HEAD ${headers[$lib]}
    echo ""

    am=`find . -name Makefile.am -exec grep -lr "lib${lib}_la.*version-info" \{\} \;`
    am_dir=`dirname $am`

    if
	grep "lib${lib}_la_SOURCES.*\\\\" $am
    then
	echo -e "\033[1;35m -- Sources list for lib$lib is probably truncated! --\033[0m"
	echo ""
    fi

    sources=`grep "lib${lib}_la_SOURCES" $am | sed s/.*=// | sed 's:$(top_builddir)/::' | sed 's:$(top_srcdir)/::' | sed 's:\\\::' | sed 's:$(libpe_rules_la_SOURCES):rules.c\ common.c:'`

    full_sources=""
    for f in $sources; do
	if
	    echo $f | grep -q "/"
	then
	    full_sources="$full_sources $f"
	else
	    full_sources="$full_sources $am_dir/$f"
	fi
    done

    lines=`git diff -w $LAST_RELEASE..HEAD ${headers[$lib]} $full_sources | wc -l`

    if [ $lines -gt 0 ]; then
	echo "- Headers: ${headers[$lib]}"
	echo "- Sources: $full_sources"
	echo "- Changed Sources since $LAST_RELEASE:"
	git diff -w $LAST_RELEASE..HEAD --stat $full_sources
	echo ""
	echo "New arguments to functions or changes to the middle of structs are incompatible additions"
	echo ""
	echo "Where possible:"
	echo "- move new fields to the end of structs"
	echo "- use bitfields instead of booleans"
	echo "- when adding arguments, create new functions that the old version can call"
	echo ""
	read -p "Are the changes to lib$lib: [a]dditions, [i]ncompatible additions, [r]emovals or [f]ixes? [None]: " CHANGE

	git show $LAST_RELEASE:$am | grep version-info
	VER=`git show $LAST_RELEASE:$am | grep "lib.*${lib}_la.*version-info" | sed s/.*version-info// | awk '{print $1}'`
	VER_NOW=`grep "lib.*${lib}_la.*version-info" $am | sed s/.*version-info// | awk '{print $1}'`
	VER_1=`echo $VER | awk -F: '{print $1}'`
	VER_2=`echo $VER | awk -F: '{print $2}'`
	VER_3=`echo $VER | awk -F: '{print $3}'`
	VER_1_NOW=`echo $VER_NOW | awk -F: '{print $1}'`

	case $CHANGE in
	    i|I)
		echo "New version with incompatible extensions: x+1:0:0"
		VER_1=`expr $VER_1 + 1`
		VER_2=0
		VER_3=0
		for h in ${headers[$lib]}; do
		    sed -i.sed  "s/lib${lib}.so.${VER_1_NOW}/lib${lib}.so.${VER_1}/" $h
		done
		;;
	    a|A)
		echo "New version with backwards compatible extensions: x+1:0:z+1"
		VER_1=`expr $VER_1 + 1`
		VER_2=0
		VER_3=`expr $VER_3 + 1`
		;;
	    R|r)
		echo "New backwards incompatible version: x+1:0:0"
		VER_1=`expr $VER_1 + 1`
		VER_2=0
		VER_3=0
		for h in ${headers[$lib]}; do
		    sed -i.sed  "s/lib${lib}.so.${VER_1_NOW}/lib${lib}.so.${VER_1}/" $h
		done
		;;
	    F|f)
		echo "Bugfix: x:y+1:z"
		VER_2=`expr $VER_2 + 1`
		;;
	esac
	VER_NEW=$VER_1:$VER_2:$VER_3

	if [ ! -z $CHANGE ]; then
	    if [ $VER_NEW != $VER_NOW ]; then
		echo "Updating $lib library version: $VER -> $VER_NEW"
		sed -i.sed  "s/version-info\ $VER_NOW/version-info\ $VER_NEW/" $am
	    else
		echo "No further version changes needed"
		sed -i.sed  "s/version-info\ $VER_NOW/version-info\ $VER_NEW/" $am
	    fi
	else
	    echo "Skipping $lib version"
	fi
    else
	echo "No changes to $lib interface"
    fi

    read -p "Continue?"
    echo ""
done

git diff -w
