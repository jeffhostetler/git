#!/bin/sh
# Generate test data repository using the given parameters.
#
# Usage: [-r repo] [-d depth] [-w width] [-f files] [-p prefix]
#
# -r repo: path to the new repo to be generated -or- extended
#          (this must be a previously generated repo).
#          When omitted, we create "./gen-many-refs.git".
#
# -d depth: the depth of sub-directories.
#
# -w width: the number of sub-directories at each level.
#
# -f files: the number of files created in each directory.
#           each file will be created in its own commit.
#
# -p prefix: override the default "dataset" prefix used for
#            directories and branches.
#

####set -e
####set -v
####set -x

# This script will create synthetic directories, files, and branches
# in a well-known pattern.  This is called a "dataset".  This script
# will either create an initial dataset or extend an existing repo
# with another dataset.
#
# Each dataset will contain the following number of NEW branches
# (not including "master"):
#
#       1 + [ Sum_{k=1}^{depth} width^{k} ]
#
# w\d|  1    2      3       4        5          6           7            8
# ---+--------------------------------------------------------------------
#  1 |  2    3      4       5        6          7           8            9
#  2 |  3    7     15      31       63        127         255          511
#  3 |  4   13     40     121      364      1,093       3,280        9,841
#  4 |  5   21     85     341    1,365      5,461      21,845       87,381
#  5 |  6   31    156     781    3,906     19,531      97,656      488,281
#  6 |  7   43    259   1,555    9,331     55,987     335,923    2,015,539
#  7 |  8   57    400   2,801   19,608    137,257     960,800    6,725,601
#  8 |  9   73    585   4,681   37,449    299,593   2,396,745   19,173,961
#  9 | 10   91    820   7,381   66,430    597,871   5,380,840   48,427,561
# 10 | 11  111  1,111  11,111  111,111  1,111,111  11,111,111  111,111,111
#
# Each branch will contain `files` files (created using individual commits).
# Merge commits will gather up branches in a topic-n-feature model (like
# L1 / L2 / L3 ... Ldepth).
#
# Each dataset has a unique namespace ($prefix-$d-$w-$f) for directories
# and branches.  You can add additional datasets to the repo by varying
# any of these terms.

depth=8
width=2
files=5
prefix="dataset"

while test "$#" -ne 0
do
    case "$1" in
	-r)
	    shift;
	    test "$#" -ne 0 || { echo 'error: -r requires an argument' >&2; exit 1; }
	    repo=$1;
	    shift ;;
	-d)
	    shift;
	    test "$#" -ne 0 || { echo 'error: -d requires an argument' >&2; exit 1; }
	    depth=$1;
	    shift ;;
	-w)
	    shift;
	    test "$#" -ne 0 || { echo 'error: -w requires an argument' >&2; exit 1; }
	    width=$1;
	    shift ;;
	-f)
	    shift;
	    test "$#" -ne 0 || { echo 'error: -f requires an argument' >&2; exit 1; }
	    files=$1;
	    shift ;;
	-p)
	    shift;
	    test "$#" -ne 0 || { echo 'error: -p requires an argument' >&2; exit 1; }
	    prefix=$1;
	    shift ;;
	*)
	    echo "error: unknown option '$1'" >&2; exit 1 ;;
	esac
done

create_files() {
	local arg_path=$1
	local arg_files=$2

	local f=1
	while test "$f" -le "$arg_files"
	do
		echo "$arg_path file $f" >$arg_path/"file"$f
		git add $arg_path
		git commit -q -m "$arg_path file $f of $arg_files"

		f=$(( $f + 1 ))
	done
}

create_dataset() {
	local arg_path=$1
	local arg_depth=$2
	local arg_width=$3
	local arg_files=$4

	local br_feature=$(git branch --show-current)

	local w=1
	while test "$w" -le "$arg_width"
	do
		local p=$arg_path/"d"$w
		local br_topic=$(echo $p | tr / -)

		git checkout -q $br_feature
		git checkout -q -b $br_topic
		mkdir -p $p

		create_files $p $arg_files

		if test "$arg_depth" -gt "1"
		then
			local d=$(( $arg_depth - 1 ))
			create_dataset $p $d $arg_width $arg_files
		fi

		# Merge the topic back into our feature.  Use the OID so that
		# we leave the topic branch at the tip of the sequence (and
		# not rolled up into the merge).
		#
		local oid_topic=$(git rev-parse HEAD)
		git checkout -q $br_feature
		git merge -q --no-ff -m "Merge $br_topic into $br_feature" $oid_topic
		
		w=$(( $w + 1 ))
	done
}

# Create a synthetic repo.
#
[ -z "$repo" ] && repo=gen-many-refs.git

if test -d $repo
then
	# If we already have a repo at this path, verify that it is a
	# generated repo (so that we don't step on our source tree by
	# accident).
	#
	git checkout -q tag-many-refs-initial
	if test $? -eq 0
	then
		echo "adding refs to existing repo: $repo"
	else
		echo "repo already exists and does not appear to be generated: $repo"
		exit 1
	fi
else
	echo "creating a new synthetic repo: $repo"

	mkdir $repo
	cd $repo
	git -c init.defaultBranch=master init .

	# Create an initial commit just to define the "master" branch
	# with an (almost) empty work tree.
	#
	touch many-refs.empty
	git add .
	git commit -q -m initial

	# Create a special tag to help us identify a generated repo
	# so that we can later add more refs if we want (without
	# accidentally adding refs to our source tree).
	#
	git tag tag-many-refs-initial
fi

# Create a base branch for everything we create in this batch
# of refs (and the worktree content).
#
# If this base branch already exists, we already created a dataset
# for this combination of parameters.  Ask the user to give us a
# different prefix.
#
dataset="${prefix}-${depth}-${width}-${files}"
git checkout -b $dataset tag-many-refs-initial
if test $? -ne 0
then
	echo "repo already contains this dataset, try another prefix or dimension"
	exit 1
fi

mkdir $dataset
echo "$dataset" >$dataset/params
git add $dataset
git commit -q -m "$dataset"
git tag $dataset-tag-root

# Create this dataset.
#
create_dataset "$dataset" $depth $width $files
git tag $dataset-tag-tip

# All done.  Checkout the master branch to put repo in canonical state.
#
git checkout -q master

nr_branches=$(git branch -vv | wc -l)
echo "Repository "$repo" now contains $nr_branches branches."

exit 0
