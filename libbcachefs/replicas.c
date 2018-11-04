
#include "bcachefs.h"
#include "replicas.h"
#include "super-io.h"

struct bch_replicas_entry_padded {
	struct bch_replicas_entry	e;
	u8				pad[BCH_SB_MEMBERS_MAX];
};

static int bch2_cpu_replicas_to_sb_replicas(struct bch_fs *,
					    struct bch_replicas_cpu *);

/* Replicas tracking - in memory: */

static inline int u8_cmp(u8 l, u8 r)
{
	return (l > r) - (l < r);
}

static void replicas_entry_sort(struct bch_replicas_entry *e)
{
	bubble_sort(e->devs, e->nr_devs, u8_cmp);
}

#define for_each_cpu_replicas_entry(_r, _i)				\
	for (_i = (_r)->entries;					\
	     (void *) (_i) < (void *) (_r)->entries + (_r)->nr * (_r)->entry_size;\
	     _i = (void *) (_i) + (_r)->entry_size)

static inline struct bch_replicas_entry *
cpu_replicas_entry(struct bch_replicas_cpu *r, unsigned i)
{
	return (void *) r->entries + r->entry_size * i;
}

static void bch2_cpu_replicas_sort(struct bch_replicas_cpu *r)
{
	eytzinger0_sort(r->entries, r->nr, r->entry_size, memcmp, NULL);
}

static int replicas_entry_to_text(struct bch_replicas_entry *e,
				  char *buf, size_t size)
{
	char *out = buf, *end = out + size;
	unsigned i;

	out += scnprintf(out, end - out, "%u: [", e->data_type);

	for (i = 0; i < e->nr_devs; i++)
		out += scnprintf(out, end - out,
				 i ? " %u" : "%u", e->devs[i]);
	out += scnprintf(out, end - out, "]");

	return out - buf;
}

int bch2_cpu_replicas_to_text(struct bch_replicas_cpu *r,
			      char *buf, size_t size)
{
	char *out = buf, *end = out + size;
	struct bch_replicas_entry *e;
	bool first = true;

	for_each_cpu_replicas_entry(r, e) {
		if (!first)
			out += scnprintf(out, end - out, " ");
		first = false;

		out += replicas_entry_to_text(e, out, end - out);
	}

	return out - buf;
}

static void extent_to_replicas(struct bkey_s_c k,
			       struct bch_replicas_entry *r)
{
	if (bkey_extent_is_data(k.k)) {
		struct bkey_s_c_extent e = bkey_s_c_to_extent(k);
		const union bch_extent_entry *entry;
		struct extent_ptr_decoded p;

		extent_for_each_ptr_decode(e, p, entry)
			if (!p.ptr.cached)
				r->devs[r->nr_devs++] = p.ptr.dev;
	}
}

static void bkey_to_replicas(enum bkey_type type,
			     struct bkey_s_c k,
			     struct bch_replicas_entry *e)
{
	e->nr_devs = 0;

	switch (type) {
	case BKEY_TYPE_BTREE:
		e->data_type = BCH_DATA_BTREE;
		extent_to_replicas(k, e);
		break;
	case BKEY_TYPE_EXTENTS:
		e->data_type = BCH_DATA_USER;
		extent_to_replicas(k, e);
		break;
	default:
		break;
	}

	replicas_entry_sort(e);
}

static inline void devlist_to_replicas(struct bch_devs_list devs,
				       enum bch_data_type data_type,
				       struct bch_replicas_entry *e)
{
	unsigned i;

	BUG_ON(!data_type ||
	       data_type == BCH_DATA_SB ||
	       data_type >= BCH_DATA_NR);

	e->data_type	= data_type;
	e->nr_devs	= 0;

	for (i = 0; i < devs.nr; i++)
		e->devs[e->nr_devs++] = devs.devs[i];

	replicas_entry_sort(e);
}

static struct bch_replicas_cpu *
cpu_replicas_add_entry(struct bch_replicas_cpu *old,
		       struct bch_replicas_entry *new_entry)
{
	struct bch_replicas_cpu *new;
	unsigned i, nr, entry_size;

	entry_size = max_t(unsigned, old->entry_size,
			   replicas_entry_bytes(new_entry));
	nr = old->nr + 1;

	new = kzalloc(sizeof(struct bch_replicas_cpu) +
		      nr * entry_size, GFP_NOIO);
	if (!new)
		return NULL;

	new->nr		= nr;
	new->entry_size	= entry_size;

	for (i = 0; i < old->nr; i++)
		memcpy(cpu_replicas_entry(new, i),
		       cpu_replicas_entry(old, i),
		       old->entry_size);

	memcpy(cpu_replicas_entry(new, old->nr),
	       new_entry,
	       replicas_entry_bytes(new_entry));

	bch2_cpu_replicas_sort(new);
	return new;
}

static bool replicas_has_entry(struct bch_replicas_cpu *r,
			       struct bch_replicas_entry *search)
{
	return replicas_entry_bytes(search) <= r->entry_size &&
		eytzinger0_find(r->entries, r->nr,
				r->entry_size,
				memcmp, search) < r->nr;
}

noinline
static int bch2_mark_replicas_slowpath(struct bch_fs *c,
				struct bch_replicas_entry *new_entry)
{
	struct bch_replicas_cpu *old_gc, *new_gc = NULL, *old_r, *new_r = NULL;
	int ret = -ENOMEM;

	mutex_lock(&c->sb_lock);

	old_gc = rcu_dereference_protected(c->replicas_gc,
					   lockdep_is_held(&c->sb_lock));
	if (old_gc && !replicas_has_entry(old_gc, new_entry)) {
		new_gc = cpu_replicas_add_entry(old_gc, new_entry);
		if (!new_gc)
			goto err;
	}

	old_r = rcu_dereference_protected(c->replicas,
					  lockdep_is_held(&c->sb_lock));
	if (!replicas_has_entry(old_r, new_entry)) {
		new_r = cpu_replicas_add_entry(old_r, new_entry);
		if (!new_r)
			goto err;

		ret = bch2_cpu_replicas_to_sb_replicas(c, new_r);
		if (ret)
			goto err;
	}

	/* allocations done, now commit: */

	if (new_r)
		bch2_write_super(c);

	/* don't update in memory replicas until changes are persistent */

	if (new_gc) {
		rcu_assign_pointer(c->replicas_gc, new_gc);
		kfree_rcu(old_gc, rcu);
	}

	if (new_r) {
		rcu_assign_pointer(c->replicas, new_r);
		kfree_rcu(old_r, rcu);
	}

	mutex_unlock(&c->sb_lock);
	return 0;
err:
	mutex_unlock(&c->sb_lock);
	kfree(new_gc);
	kfree(new_r);
	return ret;
}

static int __bch2_mark_replicas(struct bch_fs *c,
				struct bch_replicas_entry *devs)
{
	struct bch_replicas_cpu *r, *gc_r;
	bool marked;

	rcu_read_lock();
	r = rcu_dereference(c->replicas);
	gc_r = rcu_dereference(c->replicas_gc);
	marked = replicas_has_entry(r, devs) &&
		(!likely(gc_r) || replicas_has_entry(gc_r, devs));
	rcu_read_unlock();

	return likely(marked) ? 0
		: bch2_mark_replicas_slowpath(c, devs);
}

int bch2_mark_replicas(struct bch_fs *c,
		       enum bch_data_type data_type,
		       struct bch_devs_list devs)
{
	struct bch_replicas_entry_padded search;

	if (!devs.nr)
		return 0;

	memset(&search, 0, sizeof(search));

	BUG_ON(devs.nr >= BCH_REPLICAS_MAX);

	devlist_to_replicas(devs, data_type, &search.e);

	return __bch2_mark_replicas(c, &search.e);
}

int bch2_mark_bkey_replicas(struct bch_fs *c,
			    enum bkey_type type,
			    struct bkey_s_c k)
{
	struct bch_replicas_entry_padded search;
	int ret;

	if (type == BKEY_TYPE_EXTENTS) {
		struct bch_devs_list cached = bch2_bkey_cached_devs(k);
		unsigned i;

		for (i = 0; i < cached.nr; i++)
			if ((ret = bch2_mark_replicas(c, BCH_DATA_CACHED,
						bch2_dev_list_single(cached.devs[i]))))
				return ret;
	}

	bkey_to_replicas(type, k, &search.e);

	return search.e.nr_devs
		? __bch2_mark_replicas(c, &search.e)
		: 0;
}

int bch2_replicas_gc_end(struct bch_fs *c, int ret)
{
	struct bch_replicas_cpu *new_r, *old_r;

	lockdep_assert_held(&c->replicas_gc_lock);

	mutex_lock(&c->sb_lock);

	new_r = rcu_dereference_protected(c->replicas_gc,
					  lockdep_is_held(&c->sb_lock));
	rcu_assign_pointer(c->replicas_gc, NULL);

	if (ret)
		goto err;

	if (bch2_cpu_replicas_to_sb_replicas(c, new_r)) {
		ret = -ENOSPC;
		goto err;
	}

	bch2_write_super(c);

	/* don't update in memory replicas until changes are persistent */

	old_r = rcu_dereference_protected(c->replicas,
					  lockdep_is_held(&c->sb_lock));

	rcu_assign_pointer(c->replicas, new_r);
	kfree_rcu(old_r, rcu);
out:
	mutex_unlock(&c->sb_lock);
	return ret;
err:
	kfree_rcu(new_r, rcu);
	goto out;
}

int bch2_replicas_gc_start(struct bch_fs *c, unsigned typemask)
{
	struct bch_replicas_cpu *dst, *src;
	struct bch_replicas_entry *e;

	lockdep_assert_held(&c->replicas_gc_lock);

	mutex_lock(&c->sb_lock);
	BUG_ON(c->replicas_gc);

	src = rcu_dereference_protected(c->replicas,
					lockdep_is_held(&c->sb_lock));

	dst = kzalloc(sizeof(struct bch_replicas_cpu) +
		      src->nr * src->entry_size, GFP_NOIO);
	if (!dst) {
		mutex_unlock(&c->sb_lock);
		return -ENOMEM;
	}

	dst->nr		= 0;
	dst->entry_size	= src->entry_size;

	for_each_cpu_replicas_entry(src, e)
		if (!((1 << e->data_type) & typemask))
			memcpy(cpu_replicas_entry(dst, dst->nr++),
			       e, dst->entry_size);

	bch2_cpu_replicas_sort(dst);

	rcu_assign_pointer(c->replicas_gc, dst);
	mutex_unlock(&c->sb_lock);

	return 0;
}

/* Replicas tracking - superblock: */

static struct bch_replicas_cpu *
__bch2_sb_replicas_to_cpu_replicas(struct bch_sb_field_replicas *sb_r)
{
	struct bch_replicas_entry *e, *dst;
	struct bch_replicas_cpu *cpu_r;
	unsigned nr = 0, entry_size = 0;

	if (sb_r)
		for_each_replicas_entry(sb_r, e) {
			entry_size = max_t(unsigned, entry_size,
					   replicas_entry_bytes(e));
			nr++;
		}

	cpu_r = kzalloc(sizeof(struct bch_replicas_cpu) +
			nr * entry_size, GFP_NOIO);
	if (!cpu_r)
		return NULL;

	cpu_r->nr		= nr;
	cpu_r->entry_size	= entry_size;

	nr = 0;

	if (sb_r)
		for_each_replicas_entry(sb_r, e) {
			dst = cpu_replicas_entry(cpu_r, nr++);
			memcpy(dst, e, replicas_entry_bytes(e));
			replicas_entry_sort(dst);
		}

	bch2_cpu_replicas_sort(cpu_r);
	return cpu_r;
}

int bch2_sb_replicas_to_cpu_replicas(struct bch_fs *c)
{
	struct bch_sb_field_replicas *sb_r;
	struct bch_replicas_cpu *cpu_r, *old_r;

	sb_r	= bch2_sb_get_replicas(c->disk_sb.sb);
	cpu_r	= __bch2_sb_replicas_to_cpu_replicas(sb_r);
	if (!cpu_r)
		return -ENOMEM;

	old_r = rcu_dereference_check(c->replicas, lockdep_is_held(&c->sb_lock));
	rcu_assign_pointer(c->replicas, cpu_r);
	if (old_r)
		kfree_rcu(old_r, rcu);

	return 0;
}

static int bch2_cpu_replicas_to_sb_replicas(struct bch_fs *c,
					    struct bch_replicas_cpu *r)
{
	struct bch_sb_field_replicas *sb_r;
	struct bch_replicas_entry *dst, *src;
	size_t bytes;

	bytes = sizeof(struct bch_sb_field_replicas);

	for_each_cpu_replicas_entry(r, src)
		bytes += replicas_entry_bytes(src);

	sb_r = bch2_sb_resize_replicas(&c->disk_sb,
			DIV_ROUND_UP(bytes, sizeof(u64)));
	if (!sb_r)
		return -ENOSPC;

	memset(&sb_r->entries, 0,
	       vstruct_end(&sb_r->field) -
	       (void *) &sb_r->entries);

	dst = sb_r->entries;
	for_each_cpu_replicas_entry(r, src) {
		memcpy(dst, src, replicas_entry_bytes(src));

		dst = replicas_entry_next(dst);

		BUG_ON((void *) dst > vstruct_end(&sb_r->field));
	}

	return 0;
}

static const char *check_dup_replicas_entries(struct bch_replicas_cpu *cpu_r)
{
	unsigned i;

	sort_cmp_size(cpu_r->entries,
		      cpu_r->nr,
		      cpu_r->entry_size,
		      memcmp, NULL);

	for (i = 0; i + 1 < cpu_r->nr; i++) {
		struct bch_replicas_entry *l =
			cpu_replicas_entry(cpu_r, i);
		struct bch_replicas_entry *r =
			cpu_replicas_entry(cpu_r, i + 1);

		BUG_ON(memcmp(l, r, cpu_r->entry_size) > 0);

		if (!memcmp(l, r, cpu_r->entry_size))
			return "duplicate replicas entry";
	}

	return NULL;
}

static const char *bch2_sb_validate_replicas(struct bch_sb *sb, struct bch_sb_field *f)
{
	struct bch_sb_field_replicas *sb_r = field_to_type(f, replicas);
	struct bch_sb_field_members *mi = bch2_sb_get_members(sb);
	struct bch_replicas_cpu *cpu_r = NULL;
	struct bch_replicas_entry *e;
	const char *err;
	unsigned i;

	for_each_replicas_entry(sb_r, e) {
		err = "invalid replicas entry: invalid data type";
		if (e->data_type >= BCH_DATA_NR)
			goto err;

		err = "invalid replicas entry: no devices";
		if (!e->nr_devs)
			goto err;

		err = "invalid replicas entry: too many devices";
		if (e->nr_devs >= BCH_REPLICAS_MAX)
			goto err;

		err = "invalid replicas entry: invalid device";
		for (i = 0; i < e->nr_devs; i++)
			if (!bch2_dev_exists(sb, mi, e->devs[i]))
				goto err;
	}

	err = "cannot allocate memory";
	cpu_r = __bch2_sb_replicas_to_cpu_replicas(sb_r);
	if (!cpu_r)
		goto err;

	err = check_dup_replicas_entries(cpu_r);
err:
	kfree(cpu_r);
	return err;
}

const struct bch_sb_field_ops bch_sb_field_ops_replicas = {
	.validate	= bch2_sb_validate_replicas,
};

int bch2_sb_replicas_to_text(struct bch_sb_field_replicas *r, char *buf, size_t size)
{
	char *out = buf, *end = out + size;
	struct bch_replicas_entry *e;
	bool first = true;

	if (!r) {
		out += scnprintf(out, end - out, "(no replicas section found)");
		return out - buf;
	}

	for_each_replicas_entry(r, e) {
		if (!first)
			out += scnprintf(out, end - out, " ");
		first = false;

		out += replicas_entry_to_text(e, out, end - out);
	}

	return out - buf;
}

/* Query replicas: */

bool bch2_replicas_marked(struct bch_fs *c,
			  enum bch_data_type data_type,
			  struct bch_devs_list devs)
{
	struct bch_replicas_entry_padded search;
	bool ret;

	if (!devs.nr)
		return true;

	memset(&search, 0, sizeof(search));

	devlist_to_replicas(devs, data_type, &search.e);

	rcu_read_lock();
	ret = replicas_has_entry(rcu_dereference(c->replicas), &search.e);
	rcu_read_unlock();

	return ret;
}

bool bch2_bkey_replicas_marked(struct bch_fs *c,
			       enum bkey_type type,
			       struct bkey_s_c k)
{
	struct bch_replicas_entry_padded search;
	bool ret;

	if (type == BKEY_TYPE_EXTENTS) {
		struct bch_devs_list cached = bch2_bkey_cached_devs(k);
		unsigned i;

		for (i = 0; i < cached.nr; i++)
			if (!bch2_replicas_marked(c, BCH_DATA_CACHED,
					bch2_dev_list_single(cached.devs[i])))
				return false;
	}

	bkey_to_replicas(type, k, &search.e);

	if (!search.e.nr_devs)
		return true;

	rcu_read_lock();
	ret = replicas_has_entry(rcu_dereference(c->replicas), &search.e);
	rcu_read_unlock();

	return ret;
}

struct replicas_status __bch2_replicas_status(struct bch_fs *c,
					      struct bch_devs_mask online_devs)
{
	struct bch_sb_field_members *mi;
	struct bch_replicas_entry *e;
	struct bch_replicas_cpu *r;
	unsigned i, nr_online, nr_offline;
	struct replicas_status ret;

	memset(&ret, 0, sizeof(ret));

	for (i = 0; i < ARRAY_SIZE(ret.replicas); i++)
		ret.replicas[i].nr_online = UINT_MAX;

	mi = bch2_sb_get_members(c->disk_sb.sb);
	rcu_read_lock();
	r = rcu_dereference(c->replicas);

	for_each_cpu_replicas_entry(r, e) {
		if (e->data_type >= ARRAY_SIZE(ret.replicas))
			panic("e %p data_type %u\n", e, e->data_type);

		nr_online = nr_offline = 0;

		for (i = 0; i < e->nr_devs; i++) {
			BUG_ON(!bch2_dev_exists(c->disk_sb.sb, mi,
						e->devs[i]));

			if (test_bit(e->devs[i], online_devs.d))
				nr_online++;
			else
				nr_offline++;
		}

		ret.replicas[e->data_type].nr_online =
			min(ret.replicas[e->data_type].nr_online,
			    nr_online);

		ret.replicas[e->data_type].nr_offline =
			max(ret.replicas[e->data_type].nr_offline,
			    nr_offline);
	}

	rcu_read_unlock();

	return ret;
}

struct replicas_status bch2_replicas_status(struct bch_fs *c)
{
	return __bch2_replicas_status(c, bch2_online_devs(c));
}

static bool have_enough_devs(struct replicas_status s,
			     enum bch_data_type type,
			     bool force_if_degraded,
			     bool force_if_lost)
{
	return (!s.replicas[type].nr_offline || force_if_degraded) &&
		(s.replicas[type].nr_online || force_if_lost);
}

bool bch2_have_enough_devs(struct replicas_status s, unsigned flags)
{
	return (have_enough_devs(s, BCH_DATA_JOURNAL,
				 flags & BCH_FORCE_IF_METADATA_DEGRADED,
				 flags & BCH_FORCE_IF_METADATA_LOST) &&
		have_enough_devs(s, BCH_DATA_BTREE,
				 flags & BCH_FORCE_IF_METADATA_DEGRADED,
				 flags & BCH_FORCE_IF_METADATA_LOST) &&
		have_enough_devs(s, BCH_DATA_USER,
				 flags & BCH_FORCE_IF_DATA_DEGRADED,
				 flags & BCH_FORCE_IF_DATA_LOST));
}

unsigned bch2_replicas_online(struct bch_fs *c, bool meta)
{
	struct replicas_status s = bch2_replicas_status(c);

	return meta
		? min(s.replicas[BCH_DATA_JOURNAL].nr_online,
		      s.replicas[BCH_DATA_BTREE].nr_online)
		: s.replicas[BCH_DATA_USER].nr_online;
}

unsigned bch2_dev_has_data(struct bch_fs *c, struct bch_dev *ca)
{
	struct bch_replicas_entry *e;
	struct bch_replicas_cpu *r;
	unsigned i, ret = 0;

	rcu_read_lock();
	r = rcu_dereference(c->replicas);

	for_each_cpu_replicas_entry(r, e)
		for (i = 0; i < e->nr_devs; i++)
			if (e->devs[i] == ca->dev_idx)
				ret |= 1 << e->data_type;

	rcu_read_unlock();

	return ret;
}
