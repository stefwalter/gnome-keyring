#!/bin/sh -e

set -e

# --------------------------------------------------------------------

usage()
{
	echo "usage: unit-test-prep.sh -b base-name files.c ..." >&2
	exit 2
}

header_top()
{
cat << END
/* This is auto-generated code. Edit at your own peril. */

#include "cu-test/CuTest.h"
#include <stdio.h>
#include <gtk/gtk.h>

END
}

header_bottom()
{
cat << END
END
}

source_top()
{
cat << END
static void RunAllTests(void) 
{
    CuString *output = CuStringNew();
    CuSuite* suite = CuSuiteNew();

END
}

source_bottom()
{
cat << END
    CuSuiteRun(suite);
    CuSuiteSummary(suite, output);
    CuSuiteDetails(suite, output);
    printf("%s\\n", output->buffer);
}

int main(int argc, char* argv[])
{
    gtk_init(&argc, &argv);
    RunAllTests();
    return 0;
}
END
}

# --------------------------------------------------------------------

BASE=unit-test

while [ $# -gt 0 ]; do
	case "$1" in
	-b)
		BASE="$2"
		shift
		;;	
	--)
		shift
		break
		;;
	-*)
		usage
		;;
	*)
		break
		;;
	esac
	shift
done
	
# Must specify some files
if [ $# -eq 0 ]; then
	usage
fi

FILES=$*

(
	header_top
	cat $FILES | grep '^void unit_test_' | sed -e 's/$/;/'
	header_bottom
) > $BASE.h

(
	echo "/* This is auto-generated code. Edit at your own peril. */"
	echo "#include \"$BASE.h\""
	source_top
	cat $FILES | grep '^void unit_test_' | \
		sed -e 's/^void //' -e 's/(.*$//' \
                     -e 's/^/    SUITE_ADD_TEST(suite, /' -e 's/$/);/'
	source_bottom
) > $BASE.c
