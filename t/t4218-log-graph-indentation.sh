#!/bin/sh

test_description='git log --graph visual root indentations'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-log-graph.sh

test_expect_success 'setup' '
	git checkout --orphan orphan-1 && test_commit A1 && test_commit A2 &&
	git checkout --orphan orphan-2 && test_commit B1 && test_commit B2 &&
	git checkout --orphan orphan-3 && test_commit C1 &&

	git checkout --orphan cascade && test_commit root && test_commit child &&
	git checkout --orphan cascade-1 && test_commit E1 &&
	git checkout --orphan cascade-2 && test_commit F1 &&
	git checkout --orphan cascade-3 && test_commit G1 &&

	git checkout --orphan octopus && test_commit I1 &&
	git checkout --orphan octopus-2 && test_commit J1 &&
	git checkout --orphan octopus-3 && test_commit K1 &&
	git checkout octopus &&
	TREE=$(git write-tree) &&
	MERGE=$(git commit-tree $TREE -p octopus -p octopus-2 -p octopus-3 -m octopus) &&
	git reset --hard $MERGE &&

	git checkout --orphan chain && test_commit H1 && test_commit H2 && test_commit H3 &&

	git checkout --orphan two-merge && test_commit L1 &&
	git checkout --orphan two-merge-2 && test_commit M1 &&
	git checkout two-merge &&
	TREE=$(git write-tree) &&
	MERGE=$(git commit-tree $TREE -p two-merge -p two-merge-2 -m merge) &&
	git reset --hard $MERGE &&

	git checkout --orphan solo && test_commit N1 &&

	git checkout --orphan single-1 && test_commit O1 &&
	git checkout --orphan single-2 && test_commit P1 &&
	git checkout --orphan single-3 && test_commit Q1
'

test_expect_success 'single root commit' '
	check_graph solo <<-\EOF
	* N1
	EOF
'

test_expect_success 'two orphan roots' '
	check_graph B1 A2 <<-\EOF
	  * B1
	* A2
	* A1
	EOF
'

test_expect_success 'visual root indentation with multi-line log' '
	cat >expect <<-\EOF &&
	  * P1
	    description
	    second-line
	* O1
	  description
	  second-line
	EOF
	lib_test_cmp_graph --format="%s%ndescription%nsecond-line" single-2 single-1
'

test_expect_success 'three orphan roots' '
	check_graph orphan-3 orphan-2 orphan-1 <<-\EOF
	  * C1
	* B2
	 \
	  * B1
	* A2
	* A1
	EOF
'

test_expect_success 'visual root with children multi-line log' '
	cat >expect <<-\EOF &&
	  * C1
	    description
	    second-line
	* B2
	| description
	| second-line
	 \
	  * B1
	    description
	    second-line
	* A2
	| description
	| second-line
	* A1
	  description
	  second-line
	EOF
	lib_test_cmp_graph --format="%s%ndescription%nsecond-line" orphan-3 orphan-2 orphan-1
'

test_expect_success 'disconnected roots cascade' '
	check_graph cascade-3 cascade-2 cascade-1 cascade <<-\EOF
	* G1
	  * F1
	    * E1
	* child
	* root
	EOF
'

test_expect_success 'last root does not cascade' '
	check_graph cascade-3 cascade-2 cascade-1 <<-\EOF
	* G1
	  * F1
	* E1
	EOF
'

test_expect_success 'orphan chain before merge' '
	check_graph chain octopus <<-\EOF
	* H3
	* H2
	 \
	  * H1
	*-.   octopus
	|\ \
	| | * K1
	| * J1
	* I1
	EOF
'

test_expect_success 'merge then unrelated roots' '
	check_graph octopus orphan-3 orphan-2 orphan-1 <<-\EOF
	*-.   octopus
	|\ \
	| | * K1
	| * J1
	* I1
	  * C1
	* B2
	 \
	  * B1
	* A2
	* A1
	EOF
'

test_expect_success 'two-parent merge of orphans' '
	check_graph two-merge <<-\EOF
	*   merge
	|\
	| * M1
	* L1
	EOF
'

test_expect_success 'setup filtered parent scenario' '
	git checkout --orphan orphan-x &&
	echo test >other.txt &&
	git add other.txt &&
	git commit -m "B-filtered" &&

	echo test >foo.txt &&
	git add foo.txt &&
	git commit -m "has-parent" &&

	git checkout --orphan orphan-y &&
	echo test >foo.txt &&
	git add foo.txt &&
	git commit -m "unrelated"
'

test_expect_success 'commit with filtered parent is indented as visual root' '
	check_graph orphan-x orphan-y -- foo.txt <<-\EOF
	  * has-parent
	* unrelated
	EOF
'

# The walker simplifies the commit for the current one and its parents, removing
# the filtered parents, but it doesn't go one step ahead, this causes some edge
# cases with the lookahead.
# Given A (orphan), the walker only processes A, and when we lookahead for B
# (child of C) even tho C will be filtered, it hasn't been simplified yet, so we
# don't see B as a visual root, therefore cascade indentation isn't applied to A.
# (cascade indentation starts the indentation at the second visual root, to avoid
# redundant indentation). So A gets an extra indent, and once B is processed,
# when rendering it, C has been removed, B is a visual root and as the last commit
# isn't considered a visual root as it cannot have unrelated commits below it,
# cascading isn't also applied, giving B another indent.
#
# The final result is an extra indent for A and B:
#
#	  A
#	    B
#	D
#
# This will happen for any commit with filtered parents, e.g: merge commits with
# filtered parents, etc.
#
# instead of the expected:
test_expect_failure 'filtered parent cascading edge case' '
	git checkout --orphan filtered-base &&
	git rm -rf . &&
	echo test >other.txt &&
	git add other.txt &&
	git commit -m "C-filtered" &&

	echo test >foo.txt &&
	git add foo.txt &&
	git commit -m "B (child of filtered)" &&

	git checkout --orphan filtered-orphan &&
	git rm -rf . &&
	echo test >foo.txt &&
	git add foo.txt &&
	git commit -m "A (visual root)" &&

	git checkout --orphan filtered-last &&
	git rm -rf . &&
	echo test >foo.txt &&
	git add foo.txt &&
	git commit -m "D (last)" &&

	check_graph filtered-orphan filtered-base filtered-last -- foo.txt <<-\EOF
	* A (visual root)
	  * B (child of filtered)
	* D (last)
	EOF
'

test_done
