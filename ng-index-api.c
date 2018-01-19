#include "cache.h"
#include "trace.h"
#include "ng-index-api.h"

static struct trace_key trace_ngi = TRACE_KEY_INIT(NGI);

static inline void ngi_unmerged_iter__zero_results(
	struct ngi_unmerged_iter *iter)
{
	iter->name = NULL;

	iter->ce_stages[1] = NULL;
	iter->ce_stages[2] = NULL;
	iter->ce_stages[3] = NULL;

	iter->stagemask = 0;

	iter->private.pos[1] = -1;
	iter->private.pos[2] = -1;
	iter->private.pos[3] = -1;
}

int ngi_unmerged_iter__begin(struct ngi_unmerged_iter *iter,
			     struct index_state *index)
{
	ngi_unmerged_iter__zero_results(iter);

	iter->index = index;
	iter->private.pos_next = 0;

	return ngi_unmerged_iter__next(iter);
}

int ngi_unmerged_iter__next(struct ngi_unmerged_iter *iter)
{
	struct cache_entry *ce_k, *ce_j;
	int k, j, stage_k, stage_j;

	if (!iter || !iter->index)
		BUG("uninitialized ngi_unmerged_iter");

	ngi_unmerged_iter__zero_results(iter);

	for (k = iter->private.pos_next; k < iter->index->cache_nr; k++)
		if (ce_stage(iter->index->cache[k]))
			goto found_one;

	iter->private.pos_next = iter->index->cache_nr;
	return 1;

found_one:
	ce_k = iter->index->cache[k];
	stage_k = ce_stage(ce_k);

	iter->name = ce_k->name;

	iter->ce_stages[stage_k] = ce_k;
	iter->stagemask |= (1 << (stage_k - 1));
	iter->private.pos[stage_k] = k;

	for (j = k + 1; j < iter->index->cache_nr; j++) {
		ce_j = iter->index->cache[j];
		stage_j = ce_stage(ce_j);

		if (!stage_j || strcmp(ce_j->name, iter->name))
			break;

		iter->ce_stages[stage_j] = ce_j;
		iter->stagemask |= (1 << (stage_j - 1));
		iter->private.pos[stage_j] = j;
	}

	trace_printf_key(&trace_ngi, "ngi_unmerged_iter: [%d %d %d] '%s'",
			 iter->private.pos[1],
			 iter->private.pos[2],
			 iter->private.pos[3],
			 iter->name);

	iter->private.pos_next = j;
	return 0;
}

int ngi_unmerged_iter__find(struct ngi_unmerged_iter *iter,
			    struct index_state *index,
			    const char *name)
{
	int pos;

	ngi_unmerged_iter__zero_results(iter);

	iter->index = index;

	pos = index_name_pos(index, name, strlen(name));
	if (pos < 0) {
		iter->private.pos_next = -pos-1;
		return ngi_unmerged_iter__next(iter);
	}

	iter->private.pos_next = iter->index->cache_nr;
	return 1;
}

void test__ngi_unmerged_iter(struct index_state *index)
{
	struct ngi_unmerged_iter iter;
	int result;

	for (result = ngi_unmerged_iter__begin(&iter, &the_index);
	     result == 0;
	     result = ngi_unmerged_iter__next(&iter)) {

		printf("ngi_unmerged_iter: %d %d %d '%s'\n",
		       iter.private.pos[1],
		       iter.private.pos[2],
		       iter.private.pos[3],
		       iter.name);
	}
}
