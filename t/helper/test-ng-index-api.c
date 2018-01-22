#include "cache.h"
#include "ng-index-api.h"

int cmd_main(int argc, const char **argv)
{
	setup_git_directory();

	read_index(&the_index);
	test__ngi_unmerged_iter(&the_index);
	discard_index(&the_index);

	return 0;
}
