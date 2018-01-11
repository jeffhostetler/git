#ifndef VIRTUALWORKDIR_H
#define VIRTUALWORKDIR_H

/*
 * Update the CE_SKIP_WORKTREE bits based on the virtual working directory.
 */
void apply_virtualworkdir(struct index_state *istate);

/*
 * Return 1 if the requested item is found in the virtual working directory,
 * 0 for not found and -1 for undecided.
 */
int is_included_in_virtualworkdir(const char *pathname, int pathlen);

/*
 * Return 1 for exclude, 0 for include and -1 for undecided.
 */
int is_excluded_from_virtualworkdir(const char *pathname, int pathlen, int dtype);

/*
 * Free the virtual working directory data structures.
 */
void free_virtualworkdir(void);

#endif
