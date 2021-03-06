/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Multi-packet state handling
 * @file main/state.c
 *
 * @ingroup AVP
 *
 * @copyright 2014 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/state.h>
#include <freeradius-devel/rad_assert.h>

typedef struct state_entry_t {
	uint8_t		state[AUTH_VECTOR_LEN];

	time_t		cleanup;
	struct state_entry_t *prev;
	struct state_entry_t *next;

	int		tries;

	VALUE_PAIR	*vps;

	void 		*data;
} state_entry_t;

struct fr_state_t {
	int max_sessions;
	rbtree_t *tree;

	state_entry_t *head, *tail;

#ifdef HAVE_PTHREAD_H
	pthread_mutex_t mutex;
#endif
};

static fr_state_t *global_state = NULL;

#ifdef HAVE_PTHREAD_H

#define PTHREAD_MUTEX_LOCK pthread_mutex_lock
#define PTHREAD_MUTEX_UNLOCK pthread_mutex_unlock

#else
/*
 *	This is easier than ifdef's throughout the code.
 */
#define PTHREAD_MUTEX_LOCK(_x)
#define PTHREAD_MUTEX_UNLOCK(_x)

#endif

/*
 *	rbtree callback.
 */
static int state_entry_cmp(void const *one, void const *two)
{
	state_entry_t const *a = one;
	state_entry_t const *b = two;

	return memcmp(a->state, b->state, sizeof(a->state));
}

/*
 *	When an entry is free'd, it's removed from the linked list of
 *	cleanup times.
 *
 *	Note that
 */
static void state_entry_free(fr_state_t *state, state_entry_t *entry)
{
	state_entry_t *prev, *next;

	/*
	 *	If we're deleting the whole tree, don't bother doing
	 *	all of the fixups.
	 */
	if (!state || !state->tree) return;

	prev = entry->prev;
	next = entry->next;

	if (prev) {
		rad_assert(state->head != entry);
		prev->next = next;
	} else if (state->head) {
		rad_assert(state->head == entry);
		state->head = next;
	}

	if (next) {
		rad_assert(state->tail != entry);
		next->prev = prev;
	} else if (state->tail) {
		rad_assert(state->tail == entry);
		state->tail = prev;
	}

	if (entry->data) talloc_free(entry->data);

#ifdef WITH_VERIFY_PTR
	(void) talloc_get_type_abort(entry, state_entry_t);
#endif
	rbtree_deletebydata(state->tree, entry);
	talloc_free(entry);
}

fr_state_t *fr_state_init(TALLOC_CTX *ctx, int max_sessions)
{
	fr_state_t *state;

	if (!ctx) {
		if (global_state) return global_state;

		state = global_state = talloc_zero(NULL, fr_state_t);
		if (!state) return 0;

		state->max_sessions = main_config.max_requests * 2;
	} else {
		state = talloc_zero(NULL, fr_state_t);
		if (!state) return 0;

		state->max_sessions = max_sessions;
		fr_link_talloc_ctx_free(ctx, state);
	}

#ifdef HAVE_PTHREAD_H
	if (pthread_mutex_init(&state->mutex, NULL) != 0) {
		talloc_free(state);
		return NULL;
	}
#endif

	state->tree = rbtree_create(state, state_entry_cmp, NULL, 0);
	if (!state->tree) {
		talloc_free(state);
		return NULL;
	}

	return state;
}

void fr_state_delete(fr_state_t *state)
{
	rbtree_t *my_tree;

	if (!state) return;

	PTHREAD_MUTEX_LOCK(&state->mutex);

	/*
	 *	Tell the talloc callback to NOT delete the entry from
	 *	the tree.  We're deleting the entire tree.
	 */
	my_tree = state->tree;
	state->tree = NULL;

	rbtree_free(my_tree);
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	if (state == global_state) global_state = NULL;

	talloc_free(state);
}

/*
 *	Create a new entry.  Called with the mutex held.
 */
static state_entry_t *state_entry_create(fr_state_t *state, RADIUS_PACKET *packet, state_entry_t *old)
{
	size_t i;
	uint32_t x;
	time_t now = time(NULL);
	VALUE_PAIR *vp;
	state_entry_t *entry, *next;

	/*
	 *	Clean up old entries.
	 */
	for (entry = state->head; entry != NULL; entry = next) {
		next = entry->next;

		if (entry == old) continue;

		/*
		 *	Too old, we can delete it.
		 */
		if (entry->cleanup < now) {
			state_entry_free(state, entry);
			continue;
		}

		/*
		 *	Unused.  We can delete it, even if now isn't
		 *	the time to clean it up.
		 */
		if (!entry->vps && !entry->data) {
			state_entry_free(state, entry);
			continue;
		}

		break;
	}

	if (rbtree_num_elements(state->tree) >= (uint32_t) state->max_sessions) {
		return NULL;
	}

	/*
	 *	Allocate a new one.
	 */
	entry = talloc_zero(state->tree, state_entry_t);
	if (!entry) return NULL;

	/*
	 *	Limit the lifetime of this entry based on how long the
	 *	server takes to process a request.  Doing it this way
	 *	isn't perfect, but it's reasonable, and it's one less
	 *	thing for an administrator to configure.
	 */
	entry->cleanup = now + main_config.max_request_time * 10;

	/*
	 *	Hacks for EAP, until we convert EAP to using the state API.
	 *
	 *	The EAP module creates it's own State attribute, so we
	 *	want to use that one in preference to one we create.
	 */
	vp = pairfind(packet->vps, PW_STATE, 0, TAG_ANY);

	/*
	 *	If possible, base the new one off of the old one.
	 */
	if (old) {
		entry->tries = old->tries + 1;

		rad_assert(old->vps == NULL);

		/*
		 *	Track State
		 */
		if (!vp) {
			memcpy(entry->state, old->state, sizeof(entry->state));

			entry->state[0] = entry->tries;
			entry->state[1] = entry->state[0] ^ entry->tries;
			entry->state[8] = entry->state[2] ^ ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 16) & 0xff);
			entry->state[10] = entry->state[2] ^ ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 8) & 0xff);
			entry->state[12] = entry->state[2] ^ (((uint32_t) HEXIFY(RADIUSD_VERSION)) & 0xff);
		}

		/*
		 *	The old one isn't used any more, so we can free it.
		 */
		if (!old->data) state_entry_free(state, old);

	} else if (!vp) {
		/*
		 *	16 octets of randomness should be enough to
		 *	have a globally unique state.
		 */
		for (i = 0; i < sizeof(entry->state) / sizeof(x); i++) {
			x = fr_rand();
			memcpy(entry->state + (i * 4), &x, sizeof(x));
		}

		/*
		 *	Allow a portion ofthe State attribute to be set.
		 *
		 *	This allows load-balancing proxies to be much
		 *	less stateful.
		 */
		if (main_config.state_seed < 256) {
			entry->state[3] = main_config.state_seed;
		}
	}

	/*
	 *	If EAP created a State, use that.  Otherwise, use the
	 *	one we created above.
	 */
	if (vp) {
		if (rad_debug_lvl && (vp->vp_length > sizeof(entry->state))) {
			WARN("State should be %zd octets!",
			     sizeof(entry->state));
		}
		memcpy(entry->state, vp->vp_octets, sizeof(entry->state));

	} else {
		vp = paircreate(packet, PW_STATE, 0);
		pairmemcpy(vp, entry->state, sizeof(entry->state));
		pairadd(&packet->vps, vp);
	}

	if (!rbtree_insert(state->tree, entry)) {
		talloc_free(entry);
		return NULL;
	}

	/*
	 *	Link it to the end of the list, which is implicitely
	 *	ordered by cleanup time.
	 */
	if (!state->head) {
		entry->prev = entry->next = NULL;
		state->head = state->tail = entry;
	} else {
		rad_assert(state->tail != NULL);

		entry->prev = state->tail;
		state->tail->next = entry;

		entry->next = NULL;
		state->tail = entry;
	}

	return entry;
}


/*
 *	Find the entry, based on the State attribute.
 */
static state_entry_t *state_entry_find(fr_state_t *state, RADIUS_PACKET *packet)
{
	VALUE_PAIR *vp;
	state_entry_t *entry, my_entry;

	vp = pairfind(packet->vps, PW_STATE, 0, TAG_ANY);
	if (!vp) return NULL;

	if (vp->vp_length != sizeof(my_entry.state)) return NULL;

	memcpy(my_entry.state, vp->vp_octets, sizeof(my_entry.state));

	entry = rbtree_finddata(state->tree, &my_entry);

#ifdef WITH_VERIFY_PTR
	if (entry)  (void) talloc_get_type_abort(entry, state_entry_t);
#endif

	return entry;
}

/*
 *	Called when sending Access-Reject, so that all State is
 *	discarded.
 */
void fr_state_discard(REQUEST *request, RADIUS_PACKET *original)
{
	state_entry_t *entry;
	fr_state_t *state = global_state;

	pairfree(&request->state);
	request->state = NULL;

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, original);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return;
	}

	state_entry_free(state, entry);
	PTHREAD_MUTEX_UNLOCK(&state->mutex);
	return;
}

/*
 *	Get the VPS from the state.
 */
void fr_state_get_vps(REQUEST *request, RADIUS_PACKET *packet)
{
	state_entry_t *entry;
	fr_state_t *state = global_state;

	rad_assert(request->state == NULL);

	/*
	 *	No State, don't do anything.
	 */
	if (!pairfind(request->packet->vps, PW_STATE, 0, TAG_ANY)) {
		RDEBUG3("session-state: No State attribute");
		return;
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, packet);

	/*
	 *	This has to be done in a mutex lock, because talloc
	 *	isn't thread-safe.
	 */
	if (entry) {
		pairfilter(request, &request->state, &entry->vps, 0, 0, TAG_ANY);
		RDEBUG2("session-state: Found cached attributes");
		rdebug_pair_list(L_DBG_LVL_1, request, request->state, NULL);

	} else {
		RDEBUG2("session-state: No cached attributes");
	}

	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	VERIFY_REQUEST(request);
	return;
}


/*
 *	Put request->state into the State attribute.  Put the State
 *	attribute into the vps list.  Delete the original entry, if it
 *	exists.
 */
bool fr_state_put_vps(REQUEST *request, RADIUS_PACKET *original, RADIUS_PACKET *packet)
{
	state_entry_t *entry, *old;
	fr_state_t *state = global_state;

	if (!request->state) {
		RDEBUG3("session-state: Nothing to cache");
		return true;
	}

	RDEBUG2("session-state: Saving cached attributes");
	rdebug_pair_list(L_DBG_LVL_1, request, request->state, NULL);

	PTHREAD_MUTEX_LOCK(&state->mutex);

	if (original) {
		old = state_entry_find(state, original);
	} else {
		old = NULL;
	}

	entry = state_entry_create(state, packet, old);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return false;
	}

	/*
	 *	This has to be done in a mutex lock, because talloc
	 *	isn't thread-safe.
	 */
	pairfilter(entry, &entry->vps, &request->state, 0, 0, TAG_ANY);
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	rad_assert(request->state == NULL);
	VERIFY_REQUEST(request);
	return true;
}

/*
 *	Find the opaque data associated with a State attribute.
 *	Leave the data in the entry.
 */
void *fr_state_find_data(fr_state_t *state, RADIUS_PACKET *packet)
{
	void *data;
	state_entry_t *entry;

	if (!state) return false;

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, packet);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return NULL;
	}

	data = entry->data;
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	return data;
}


/*
 *	Get the opaque data associated with a State attribute.
 *	and remove the data from the entry.
 */
void *fr_state_get_data(fr_state_t *state, RADIUS_PACKET *packet)
{
	void *data;
	state_entry_t *entry;

	if (!state) return NULL;

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, packet);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return NULL;
	}

	data = entry->data;
	entry->data = NULL;
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	return data;
}


/*
 *	Get the opaque data associated with a State attribute.
 *	and remove the data from the entry.
 */
bool fr_state_put_data(fr_state_t *state, RADIUS_PACKET *original, RADIUS_PACKET *packet,
		       void *data)
{
	state_entry_t *entry, *old;

	if (!state) return false;

	PTHREAD_MUTEX_LOCK(&state->mutex);

	if (original) {
		old = state_entry_find(state, original);
	} else {
		old = NULL;
	}

	entry = state_entry_create(state, packet, old);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return false;
	}

	/*
	 *	If we're moving the data, ensure that we delete it
	 *	from the old state.
	 */
	if (old && (old->data == data)) {
		old->data = NULL;
	}

	entry->data = data;

	PTHREAD_MUTEX_UNLOCK(&state->mutex);
	return true;
}
