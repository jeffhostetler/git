# ng-index-api-design.txt

This document describes a series of changes to the existing index/cache APIs.
My goal here is to hide the in-memory organization of the index/cache from
most of the code base and allow it to be more easily changed to handle larger
repositories and in the future possibly different on-disk index file formats.

Much like the on-going series of patches converting from "char[20] sha" to
"struct object_id oid" which have been gradually introduced to the code base,
I envision a similar sequence of small conversion steps.


## 1. NG Index API Goals


###Index Macros

The "NO_THE_INDEX_COMPATIBILITY_MACROS" macro is used to define "casual API
macros" for use by most of the source in the tree.  These macros hide some
"struct index_state" fields and define macro functions that always pass the
global variable "the_index" to the actual index functions.  For example:

    #define active_nr (the_index.cache_nr)
    #define read_cache() read_index(&the_index)

Currently only 10 source files do not use these macros.

**Goal 1:** gradually phase out the use of these macros in the rest of the
source.


### "the_index" Global Variable

Currently, the main in-memory index is stored in a global variable called
"the_index" and is implicilty referenced throughout the source.  This causes
various problems especially if we want to support submodules in the same
process.

**Goal 2:** gradually phase out "the_index" and replace it with a passed
parameter.  The signature of all routines that need to access the index will
be widened to include it.  This meshes nicely with the currently in-progress
"struct repository" work.  Work on this goal may need to be staged behind the
in-progress repository changes.  That is, if most functions are modified to
take a "struct repository", they can just use the "struct index_state" pointer
within it.


### Hide "cache_entry" Array

Currently, the main in-memory index is stored using an array of pointers to
"struct cache_entry" objects.  Very little attempt is made to hide this array
from the entire code base.

These are ordered by full relative pathname and then by stage.  Code
throughout the tree knows this and directly operates on the array.  There are
some helper routines to find, insert, and delete entries, but most access is
direct.

Since the index is linear table of pathnames, any iteration of the index is
implicitly a depth-first walk of the tracked files.  But it is not possible
to efficiently ask for hierarchy-related iterations.

Additionally, since files with multiple stages are stored in adjacent entries,
some iterations need to be adjusted to process or skip them.

**Goal 3:** introduce a set of iterators to initially hide the array and later
to allow alternative data structures to be considered.

**Goal 4:** will be to hide the stage-array details for unmerged entries.


### Hide or Eliminate Name/Dir Hash

Currently, the main in-memory index also contains 2 hash tables, the "name"
and "dir" hashes.  These allow for efficient case-insensitive lookups for
files and each unique directory prefix.  These are used on case-insensitive
platforms (like Windows and maybe Mac) to help correct case-sloppy command
line pathnames from the user.

These hash tables are very expensive to compute.  They only exist because
the existing array of index-entries.  That is, if we could change the in-memory
layout, we would not need the hash tables.

**Goal 5:** elminate these hash tables.


### Memory Allocation of "cache_entry"

Currently, the main in-memory index consists of an array of pointers to
individually-allocated "struct cache_entry" objects.  For index files with a
very large number of entries, there can be significant malloc overhead when
reading the index.

**Goal 6:** consider a memory-pool to block allocate them.  The pool should
be thread-aware to allow threaded operations to create new cache-entries
minimal thread contention.


## 2. NG Index API

### Iterator Types





## A1. Appendix 1: Future Work

* New on-disk index formats.
* Incremental update of on-disk index.

## A2. Appendix 2: TODO

* TODO Describe other common patterns like the following in
blame.c:fake_working_tre_commit()

    discard_cache();
    read_cache();

* TODO Define which source files are considered inside the "core" and
able to use the private fields that we are trying to hide.

