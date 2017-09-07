#ifndef OBJECT_FILTER_H
#define OBJECT_FILTER_H

#include "parse-options.h"

/*
 * Common declarations and utilities for filtering objects (such as omitting
 * large blobs) in list_objects:traverse_commit_list() and git-rev-list.
 */

struct object_filter_options {
	/*
	 * file pathname or blob-ish path/OID (that get_sha1_with_context() can
	 * containing the sparse-checkout specification.
	 * only used when use_blob or use_path is set.
	 */
	const char *sparse_value;
	struct object_id sparse_oid;

	/*
	 * blob size byte limit for filtering.  only blobs smaller than this
	 * value will be included.  a value of zero, omits all blobs.
	 * only used when omit_large_blobs is set.  Integer and string versions
	 * of this are kept for convenience.
	 */
	unsigned long large_byte_limit;
	const char *large_byte_limit_string;

	/* valid filter types (only one may be used at a time) */
	unsigned omit_all_blobs : 1;
	unsigned omit_large_blobs : 1;
	unsigned use_blob : 1;
	unsigned use_path : 1;

	/* true if the filter should output a manifest of the omitted objects. */
	unsigned print_manifest : 1;

	/* true to suppress missing object errors during consistency checks */
	unsigned relax : 1;
};

/*
 * Return true if a filter is enabled.
 */
inline int object_filter_enabled(const struct object_filter_options *p)
{
	return p->omit_all_blobs ||
		p->omit_large_blobs ||
		p->use_blob ||
		p->use_path;
}

/* Normalized command line arguments */
#define CL_ARG_FILTER_OMIT_ALL_BLOBS     "filter-omit-all-blobs"
#define CL_ARG_FILTER_OMIT_LARGE_BLOBS   "filter-omit-large-blobs"
#define CL_ARG_FILTER_USE_BLOB           "filter-use-blob"
#define CL_ARG_FILTER_USE_PATH           "filter-use-path"
#define CL_ARG_FILTER_PRINT_MANIFEST     "filter-print-manifest"
#define CL_ARG_FILTER_RELAX              "filter-relax"

/*
 * Common command line argument parsing for object-filter-related
 * arguments (whether from a hand-parsed or parse-options style
 * parser.
 */
int parse_filter_omit_all_blobs(struct object_filter_options *filter_options);
int parse_filter_omit_large_blobs(struct object_filter_options *filter_options,
				  const char *arg);
int parse_filter_use_blob(struct object_filter_options *filter_options,
			  const char *arg);
int parse_filter_use_path(struct object_filter_options *filter_options,
			  const char *arg);
int parse_filter_print_manifest(struct object_filter_options *filter_options);
int parse_filter_relax(struct object_filter_options *filter_options);

/*
 * Common command line argument parsers for object-filter-related
 * arguments comming from parse-options style parsers.
 */

int opt_parse_filter_omit_all_blobs(const struct option *opt,
				    const char *arg, int unset);
int opt_parse_filter_omit_large_blobs(const struct option *opt,
				      const char *arg, int unset);
int opt_parse_filter_use_blob(const struct option *opt,
			      const char *arg, int unset);
int opt_parse_filter_use_path(const struct option *opt,
			      const char *arg, int unset);
int opt_parse_filter_print_manifest(const struct option *opt,
				    const char *arg, int unset);
int opt_parse_filter_relax(const struct option *opt,
			   const char *arg, int unset);

#define OPT_PARSE_FILTER_OMIT_ALL_BLOBS(fo) \
	{ OPTION_CALLBACK, 0, CL_ARG_FILTER_OMIT_ALL_BLOBS, fo, NULL, \
	  N_("omit all blobs from result"), PARSE_OPT_NOARG | PARSE_OPT_NONEG, \
	  opt_parse_filter_omit_all_blobs }

#define OPT_PARSE_FILTER_OMIT_LARGE_BLOBS(fo) \
	{ OPTION_CALLBACK, 0, CL_ARG_FILTER_OMIT_LARGE_BLOBS, fo, N_("size"), \
	  N_("omit large blobs from result"), PARSE_OPT_NONEG, \
	  opt_parse_filter_omit_large_blobs }

#define OPT_PARSE_FILTER_USE_BLOB(fo) \
	{ OPTION_CALLBACK, 0, CL_ARG_FILTER_USE_BLOB, fo, N_("object"), \
	  N_("filter results using sparse-checkout specification"), PARSE_OPT_NONEG, \
	  opt_parse_filter_use_blob }

#define OPT_PARSE_FILTER_USE_PATH(fo) \
	{ OPTION_CALLBACK, 0, CL_ARG_FILTER_USE_PATH, fo, N_("path"), \
	  N_("filter results using sparse-checkout specification"), PARSE_OPT_NONEG, \
	  opt_parse_filter_use_path }

#define OPT_PARSE_FILTER_PRINT_MANIFEST(fo) \
	{ OPTION_CALLBACK, 0, CL_ARG_FILTER_PRINT_MANIFEST, fo, NULL,	\
	  N_("print manifest of omitted objects"), PARSE_OPT_NOARG | PARSE_OPT_NONEG, \
	  opt_parse_filter_print_manifest }

#define OPT_PARSE_FILTER_RELAX(fo) \
	{ OPTION_CALLBACK, 0, CL_ARG_FILTER_RELAX, fo, NULL, \
	  N_("relax consistency checks for previously omitted objects"), \
	  PARSE_OPT_NOARG | PARSE_OPT_NONEG, opt_parse_filter_relax }

/*
 * Hand parse known object-filter command line options.
 * Use this when the caller DOES NOT use the normal OPT_
 * routines.
 *
 * Here we assume args of the form "--<key>" or "--<key>=<value>".
 * Note the literal dash-dash and equals.
 *
 * Returns 1 if we handled the argument.
 */
int object_filter_hand_parse_arg(struct object_filter_options *filter_options,
				 const char *arg,
				 int allow_print_manifest,
				 int allow_relax);

/*
 * Hand parse known object-filter protocol lines.
 *
 * Here we assume args of the form "<key>" or "<key> <value>".
 * Note the literal space before between the key and value.
 */ 
int object_filter_hand_parse_protocol(struct object_filter_options *filter_options,
				      const char *arg,
				      int allow_print_manifest,
				      int allow_relax);

#endif /* OBJECT_FILTER_H */
