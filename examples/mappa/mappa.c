
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "mappa.h"
#include "mappa_impl.h"

#include "ctlra.h"

/* per ctlra-device we need a look-up table, which points to the
 * correct target function pointer to call based on the current layer.
 *
 * These layers of "ctlra-to-target" pairs are the mappings that need to
 * be saved/restored. Somehow, the uint32 group/item id pairs need to be
 * consistent in the host to re-connect correctly? Or use group_id / item_id?
 */

/* LAYER notes;
 * - Need to be able to display clearly to user
 * - Individual layers need to make sense
 * - "mask" layers that only overwrite certain parts
 * --- mask layer implementation returns which events were handled?
 * --- if not handled, go "down the stack" of layers?
 * --- enables "masked-masked" layers etc... complexity worth it?
 * - Display each layer as tab along top of mapping UI?
 * --- A tab button [+] to create a new layer?
 * --- Adding a new layer adds Group "mappa" : Item "Layer 1" ?
 */

void mappa_feedback_func(struct ctlra_dev_t *dev, void *d)
{
}

void mappa_event_func(struct ctlra_dev_t* dev, uint32_t num_events,
		       struct ctlra_event_t** events, void *userdata)
{
	struct mappa_lut_t *lut = userdata;

	struct mappa_sw_target_t *t = 0;

	for(uint32_t i = 0; i < num_events; i++) {
		struct ctlra_event_t *e = events[i];
		switch(e->type) {
		case CTLRA_EVENT_BUTTON:
			break;

		case CTLRA_EVENT_ENCODER:
			break;

		case CTLRA_EVENT_SLIDER: {
			uint32_t id = e->slider.id;
			t = &lut->target_types[CTLRA_EVENT_SLIDER][id];
			printf("id %d, group %d, item %d\n",
			       id, t->group_id, t->item_id);
			if(t->func) {
				t->func(t->group_id, t->item_id,
					e->slider.value, t->userdata);
			}
			}
			break;

		case CTLRA_EVENT_GRID:
			break;

		default:
			break;
		};
	}
}

/* perform a deep copy of the target so it doesn't rely on app memory */
static struct target_t *
target_create_copy_for_list(const struct mappa_sw_target_t *t)
{
	/* allocate a size for the list-pointer-enabled struct */
	struct target_t *n = malloc(sizeof(struct target_t));

	/* dereference the current target, shallow copy values */
	n->target = *t;

	/* deep copy strings (to not depend on app provided strings) */
	if(t->group_name)
		n->target.group_name = strdup(t->group_name);
	if(t->item_name)
		n->target.item_name = strdup(t->item_name);

	return n;
}

static void
target_destroy(struct target_t *t)
{
	assert(t != NULL);

	if(t->target.group_name)
		free(t->target.group_name);
	if(t->target.item_name)
		free(t->target.item_name);

	free(t);
}

int32_t
mappa_sw_target_add(struct mappa_t *m, struct mappa_sw_target_t *t)
{
	struct target_t * n= target_create_copy_for_list(t);
	TAILQ_INSERT_HEAD(&m->target_list, n, tailq);

	printf("added %d %d\n", n->target.group_id, n->target.item_id);

	/* TODO: must each group_id and item_id be unique? Do we need to
	 * check this before add? How does remove work if we don't have
	 * a unique id?
	 */
	return 0;
}

int32_t
mappa_sw_target_remove(struct mappa_t *m, uint32_t group_id,
		       uint32_t item_id)
{
	struct target_t *t;
	TAILQ_FOREACH(t, &m->target_list, tailq) {
		if((t->target.group_id == group_id) &&
		   (t->target.item_id  == item_id))
		{
			printf("removing target: group %d, item %d\n",
			       t->target.group_id, t->target.item_id);
			/* nuke any controller mappings to this */
			TAILQ_REMOVE(&m->target_list, t, tailq);
			target_destroy(t);
			return 0;
		}
	}

	return -EINVAL;
}

void
mappa_remove_func(struct ctlra_dev_t *dev, int unexpected_removal,
		  void *userdata)
{
	struct ctlra_dev_info_t info;
	ctlra_dev_get_info(dev, &info);
	printf("mappa_app: removing %s %s\n", info.vendor, info.device);
}


int32_t mappa_bind_ctlra_to_target(struct mappa_t *m,
				   uint32_t cltra_dev_id,
				   uint32_t control_id,
				   uint32_t gid,
				   uint32_t iid,
				   uint32_t layer)
{
	/* TODO: Error check this */
	struct mappa_sw_target_t *dev_target =
		&m->lut->target_types[CTLRA_EVENT_SLIDER][control_id];

	struct target_t *t;
	TAILQ_FOREACH(t, &m->target_list, tailq) {
		if((t->target.group_id == gid) &&
		   (t->target.item_id  == iid)) {
			dev_target->item_id  = iid;
			dev_target->group_id = gid;
			dev_target->func     = t->target.func;
			dev_target->userdata = t->target.userdata;
			break;
		}
	}

	return 0;
}


struct mappa_lut_t *
lut_create_for_dev(struct ctlra_dev_t *dev,
		   const struct ctlra_dev_info_t *info)
{
	struct mappa_lut_t * lut = calloc(1, sizeof(*lut));
	if(!lut)
		return 0;

	for(int i = 0; i < CTLRA_EVENT_T_COUNT; i++) {
		uint32_t c = info->control_count[i];
		printf("type %d, count %d\n", i, c);
		lut->target_types[i] = calloc(c, sizeof(struct mappa_sw_target_t));
		if(!lut->target_types[i])
			goto fail;
	}

	return lut;
fail:
	for(int i = 0; i < CTLRA_EVENT_T_COUNT; i++) {
		if(lut->target_types[i])
			free(lut->target_types[i]);
	}
	return 0;
}

int
mappa_accept_func(struct ctlra_t *ctlra, const struct ctlra_dev_info_t *info,
		  struct ctlra_dev_t *dev, void *userdata)
{
	struct mappa_t *m = userdata;

	struct mappa_lut_t *lut = lut_create_for_dev(dev, info);

	ctlra_dev_set_event_func(dev, mappa_event_func);
	ctlra_dev_set_feedback_func(dev, mappa_feedback_func);
	ctlra_dev_set_remove_func(dev, mappa_remove_func);

	/* the callback here is set per *DEVICE* - NOT the mappa pointer!
	 * The struct being passed is used directly to avoid lookup of the
	 * correct device from a list. The structure has a mappa_t back-
	 * pointer in order to communicate with "self" if required.
	 */
	lut->self = m;
	m->lut = lut;
	ctlra_dev_set_callback_userdata(dev, lut);

	/* TODO: check for default map to load for this device */

	return 1;
}

int32_t
mappa_iter(struct mappa_t *m)
{
	ctlra_idle_iter(m->ctlra);
	return 0;
}

struct mappa_t *
mappa_create(struct mappa_opts_t *opts)
{
	(void)opts;
	struct mappa_t *m = calloc(1, sizeof(struct mappa_t));
	if(!m)
		goto fail;

	struct ctlra_t *c = ctlra_create(NULL);
	if(!c)
		goto fail;

	m->ctlra = c;

	TAILQ_INIT(&m->target_list);

	int num_devs = ctlra_probe(c, mappa_accept_func, m);
	printf("mappa connected to %d devices\n", num_devs);

	return m;
fail:
	if(m)
		free(m);
	return 0;
}

void
mappa_destroy(struct mappa_t *m)
{
	struct target_t *t;
	while (!TAILQ_EMPTY(&m->target_list)) {
		t = TAILQ_FIRST(&m->target_list);
		TAILQ_REMOVE(&m->target_list, t, tailq);
		target_destroy(t);
	}

	/* iterate over all allocated resources and free them */
	ctlra_exit(m->ctlra);
	free(m);
}
