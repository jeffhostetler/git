#include "cache.h"
#include "commit.h"
#include "config.h"
#include "revision.h"
#include "list-objects.h"
#include "oidset2.h"
#include "list-objects-filter-all.h"
#include "list-objects-filter-large.h"
#include "list-objects-filter-sparse.h"
#include "object-filter.h"

int parse_filter_omit_all_blobs(struct object_filter_options *filter_options)
{
	if (object_filter_enabled(filter_options))
		die(_("multiple object filter types cannot be combined"));

	filter_options->omit_all_blobs = 1;
	return 0;
}

int parse_filter_omit_large_blobs(struct object_filter_options *filter_options,
				  const char *arg)
{
	if (object_filter_enabled(filter_options))
		die(_("multiple object filter types cannot be combined"));

	filter_options->omit_large_blobs = 1;

	/* we allow "<digits>[kmg]" */
	if (!git_parse_ulong(arg, &filter_options->large_byte_limit))
		die(_("invalid size limit for large object filter"));

	filter_options->large_byte_limit_string = strdup(arg);
	return 0;
}

int parse_filter_use_blob(struct object_filter_options *filter_options,
			  const char *arg)
{
	struct object_context oc;

	if (object_filter_enabled(filter_options))
		die(_("multiple object filter types cannot be combined"));

	filter_options->use_blob = 1;

	/*
	 * The command line argument needs to resolve to an known OID
	 * representing the content of the desired sparse-checkout file.
	 *
	 * We allow various syntax forms for the convenience of the user.
	 * See sha1_name.c:get_oid_with_context_1().
	 *
	 * Try to evaluate the arg locally in case they use one of the
	 * convenience patterns.  This must resolve to a blob.
	 */
	if (get_oid_with_context(arg, GET_OID_BLOB,
				 &filter_options->sparse_oid, &oc)) {
		/*
		 * If that fails, keep the original string in case a client
		 * command wants to send it to the server.  This allows the
		 * client to name an OID for a blob they don't have.
		 */
		filter_options->sparse_value = strdup(arg);
		oidcpy(&filter_options->sparse_oid, &null_oid);
	} else {
		/*
		 * Round-trip the found OID to normalize it.
		 */
		filter_options->sparse_value =
			strdup(oid_to_hex(&filter_options->sparse_oid));
	}
	return 0;
}

int parse_filter_use_path(struct object_filter_options *filter_options,
			  const char *arg)
{
	if (object_filter_enabled(filter_options))
		die(_("multiple object filter types cannot be combined"));

	filter_options->use_path = 1;

	/*
	 * The command line argument needs to resolve to a local file
	 * containing the content of the desired sparse-checkout file.
	 */
	filter_options->sparse_value = strdup(arg);
	return 0;
}

int parse_filter_print_omitted(struct object_filter_options *filter_options)
{
	filter_options->print_omitted = 1;
	return 0;
}

int parse_filter_print_missing(struct object_filter_options *filter_options)
{
	filter_options->print_missing = 1;
	return 0;
}

int parse_filter_relax(struct object_filter_options *filter_options)
{
	filter_options->relax = 1;
	return 0;
}

int opt_parse_filter_omit_all_blobs(const struct option *opt,
				    const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(!arg);
	assert(!unset);

	return parse_filter_omit_all_blobs(filter_options);
}

int opt_parse_filter_omit_large_blobs(const struct option *opt,
				      const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(arg);
	assert(!unset);

	return parse_filter_omit_large_blobs(filter_options, arg);
}

int opt_parse_filter_use_blob(const struct option *opt,
			      const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(arg);
	assert(!unset);

	return parse_filter_use_blob(filter_options, arg);
}

int opt_parse_filter_use_path(const struct option *opt,
			      const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(arg);
	assert(!unset);

	return parse_filter_use_path(filter_options, arg);
}

int opt_parse_filter_print_omitted(const struct option *opt,
				   const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(!arg);
	assert(!unset);

	return parse_filter_print_omitted(filter_options);
}

int opt_parse_filter_print_missing(const struct option *opt,
				   const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(!arg);
	assert(!unset);

	return parse_filter_print_missing(filter_options);
}

int opt_parse_filter_relax(const struct option *opt,
			   const char *arg, int unset)
{
	struct object_filter_options *filter_options = opt->value;

	assert(!arg);
	assert(!unset);

	return parse_filter_relax(filter_options);
}

int object_filter_hand_parse_arg(struct object_filter_options *filter_options,
				 const char *arg,
				 int allow_print_omitted,
				 int allow_print_missing,
				 int allow_relax)
{
	if (!strcmp(arg, ("--"CL_ARG_FILTER_OMIT_ALL_BLOBS))) {
		parse_filter_omit_all_blobs(filter_options);
		return 1;
	}
	if (skip_prefix(arg, ("--"CL_ARG_FILTER_OMIT_LARGE_BLOBS"="), &arg)) {
		parse_filter_omit_large_blobs(filter_options, arg);
		return 1;
	}
	if (skip_prefix(arg, ("--"CL_ARG_FILTER_USE_BLOB"="), &arg)) {
		parse_filter_use_blob(filter_options, arg);
		return 1;
	}
	if (skip_prefix(arg, ("--"CL_ARG_FILTER_USE_PATH"="), &arg)) {
		parse_filter_use_path(filter_options, arg);
		return 1;
	}

	if (allow_print_omitted &&
	    !strcmp(arg, ("--"CL_ARG_FILTER_PRINT_OMITTED))) {
		parse_filter_print_omitted(filter_options);
		return 1;
	}

	if (allow_print_missing &&
	    !strcmp(arg, ("--"CL_ARG_FILTER_PRINT_MISSING))) {
		parse_filter_print_missing(filter_options);
		return 1;
	}

	if (allow_relax && !strcmp(arg, ("--"CL_ARG_FILTER_RELAX))) {
		parse_filter_relax(filter_options);
		return 1;
	}

	return 0;
}

int object_filter_hand_parse_protocol(struct object_filter_options *filter_options,
				      const char *arg,
				      int allow_print_omitted,
				      int allow_print_missing,
				      int allow_relax)
{
	if (!strcmp(arg, CL_ARG_FILTER_OMIT_ALL_BLOBS)) {
		parse_filter_omit_all_blobs(filter_options);
		return 1;
	}
	if (skip_prefix(arg, (CL_ARG_FILTER_OMIT_LARGE_BLOBS" "), &arg)) {
		parse_filter_omit_large_blobs(filter_options, arg);
		return 1;
	}
	if (skip_prefix(arg, (CL_ARG_FILTER_USE_BLOB" "), &arg)) {
		parse_filter_use_blob(filter_options, arg);
		return 1;
	}
	if (skip_prefix(arg, (CL_ARG_FILTER_USE_PATH" "), &arg)) {
		parse_filter_use_path(filter_options, arg);
		return 1;
	}

	if (allow_print_omitted &&
	    !strcmp(arg, CL_ARG_FILTER_PRINT_OMITTED)) {
		parse_filter_print_omitted(filter_options);
		return 1;
	}
	if (allow_print_missing &&
	    !strcmp(arg, CL_ARG_FILTER_PRINT_MISSING)) {
		parse_filter_print_missing(filter_options);
		return 1;
	}
	if (allow_relax && !strcmp(arg, CL_ARG_FILTER_RELAX)) {
		parse_filter_relax(filter_options);
		return 1;
	}

	return 0;
}
