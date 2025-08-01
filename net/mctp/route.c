// SPDX-License-Identifier: GPL-2.0
/*
 * Management Component Transport Protocol (MCTP) - routing
 * implementation.
 *
 * This is currently based on a simple routing table, with no dst cache. The
 * number of routes should stay fairly small, so the lookup cost is small.
 *
 * Copyright (c) 2021 Code Construct
 * Copyright (c) 2021 Google
 */

#include <linux/idr.h>
#include <linux/kconfig.h>
#include <linux/mctp.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>

#include <kunit/static_stub.h>

#include <uapi/linux/if_arp.h>

#include <net/mctp.h>
#include <net/mctpdevice.h>
#include <net/netlink.h>
#include <net/sock.h>

#include <trace/events/mctp.h>

static const unsigned int mctp_message_maxlen = 64 * 1024;
static const unsigned long mctp_key_lifetime = 6 * CONFIG_HZ;

static void mctp_flow_prepare_output(struct sk_buff *skb, struct mctp_dev *dev);

/* route output callbacks */
static int mctp_dst_discard(struct mctp_dst *dst, struct sk_buff *skb)
{
	kfree_skb(skb);
	return 0;
}

static struct mctp_sock *mctp_lookup_bind_details(struct net *net,
						  struct sk_buff *skb,
						  u8 type, u8 dest,
						  u8 src, bool allow_net_any)
{
	struct mctp_skb_cb *cb = mctp_cb(skb);
	struct sock *sk;
	u8 hash;

	WARN_ON_ONCE(!rcu_read_lock_held());

	hash = mctp_bind_hash(type, dest, src);

	sk_for_each_rcu(sk, &net->mctp.binds[hash]) {
		struct mctp_sock *msk = container_of(sk, struct mctp_sock, sk);

		if (!allow_net_any && msk->bind_net == MCTP_NET_ANY)
			continue;

		if (msk->bind_net != MCTP_NET_ANY && msk->bind_net != cb->net)
			continue;

		if (msk->bind_type != type)
			continue;

		if (msk->bind_peer_set &&
		    !mctp_address_matches(msk->bind_peer_addr, src))
			continue;

		if (!mctp_address_matches(msk->bind_local_addr, dest))
			continue;

		return msk;
	}

	return NULL;
}

static struct mctp_sock *mctp_lookup_bind(struct net *net, struct sk_buff *skb)
{
	struct mctp_sock *msk;
	struct mctp_hdr *mh;
	u8 type;

	/* TODO: look up in skb->cb? */
	mh = mctp_hdr(skb);

	if (!skb_headlen(skb))
		return NULL;

	type = (*(u8 *)skb->data) & 0x7f;

	/* Look for binds in order of widening scope. A given destination or
	 * source address also implies matching on a particular network.
	 *
	 * - Matching destination and source
	 * - Matching destination
	 * - Matching source
	 * - Matching network, any address
	 * - Any network or address
	 */

	msk = mctp_lookup_bind_details(net, skb, type, mh->dest, mh->src,
				       false);
	if (msk)
		return msk;
	msk = mctp_lookup_bind_details(net, skb, type, MCTP_ADDR_ANY, mh->src,
				       false);
	if (msk)
		return msk;
	msk = mctp_lookup_bind_details(net, skb, type, mh->dest, MCTP_ADDR_ANY,
				       false);
	if (msk)
		return msk;
	msk = mctp_lookup_bind_details(net, skb, type, MCTP_ADDR_ANY,
				       MCTP_ADDR_ANY, false);
	if (msk)
		return msk;
	msk = mctp_lookup_bind_details(net, skb, type, MCTP_ADDR_ANY,
				       MCTP_ADDR_ANY, true);
	if (msk)
		return msk;

	return NULL;
}

/* A note on the key allocations.
 *
 * struct net->mctp.keys contains our set of currently-allocated keys for
 * MCTP tag management. The lookup tuple for these is the peer EID,
 * local EID and MCTP tag.
 *
 * In some cases, the peer EID may be MCTP_EID_ANY: for example, when a
 * broadcast message is sent, we may receive responses from any peer EID.
 * Because the broadcast dest address is equivalent to ANY, we create
 * a key with (local = local-eid, peer = ANY). This allows a match on the
 * incoming broadcast responses from any peer.
 *
 * We perform lookups when packets are received, and when tags are allocated
 * in two scenarios:
 *
 *  - when a packet is sent, with a locally-owned tag: we need to find an
 *    unused tag value for the (local, peer) EID pair.
 *
 *  - when a tag is manually allocated: we need to find an unused tag value
 *    for the peer EID, but don't have a specific local EID at that stage.
 *
 * in the latter case, on successful allocation, we end up with a tag with
 * (local = ANY, peer = peer-eid).
 *
 * So, the key set allows both a local EID of ANY, as well as a peer EID of
 * ANY in the lookup tuple. Both may be ANY if we prealloc for a broadcast.
 * The matching (in mctp_key_match()) during lookup allows the match value to
 * be ANY in either the dest or source addresses.
 *
 * When allocating (+ inserting) a tag, we need to check for conflicts amongst
 * the existing tag set. This requires macthing either exactly on the local
 * and peer addresses, or either being ANY.
 */

static bool mctp_key_match(struct mctp_sk_key *key, unsigned int net,
			   mctp_eid_t local, mctp_eid_t peer, u8 tag)
{
	if (key->net != net)
		return false;

	if (!mctp_address_matches(key->local_addr, local))
		return false;

	if (!mctp_address_matches(key->peer_addr, peer))
		return false;

	if (key->tag != tag)
		return false;

	return true;
}

/* returns a key (with key->lock held, and refcounted), or NULL if no such
 * key exists.
 */
static struct mctp_sk_key *mctp_lookup_key(struct net *net, struct sk_buff *skb,
					   unsigned int netid, mctp_eid_t peer,
					   unsigned long *irqflags)
	__acquires(&key->lock)
{
	struct mctp_sk_key *key, *ret;
	unsigned long flags;
	struct mctp_hdr *mh;
	u8 tag;

	mh = mctp_hdr(skb);
	tag = mh->flags_seq_tag & (MCTP_HDR_TAG_MASK | MCTP_HDR_FLAG_TO);

	ret = NULL;
	spin_lock_irqsave(&net->mctp.keys_lock, flags);

	hlist_for_each_entry(key, &net->mctp.keys, hlist) {
		if (!mctp_key_match(key, netid, mh->dest, peer, tag))
			continue;

		spin_lock(&key->lock);
		if (key->valid) {
			refcount_inc(&key->refs);
			ret = key;
			break;
		}
		spin_unlock(&key->lock);
	}

	if (ret) {
		spin_unlock(&net->mctp.keys_lock);
		*irqflags = flags;
	} else {
		spin_unlock_irqrestore(&net->mctp.keys_lock, flags);
	}

	return ret;
}

static struct mctp_sk_key *mctp_key_alloc(struct mctp_sock *msk,
					  unsigned int net,
					  mctp_eid_t local, mctp_eid_t peer,
					  u8 tag, gfp_t gfp)
{
	struct mctp_sk_key *key;

	key = kzalloc(sizeof(*key), gfp);
	if (!key)
		return NULL;

	key->net = net;
	key->peer_addr = peer;
	key->local_addr = local;
	key->tag = tag;
	key->sk = &msk->sk;
	key->valid = true;
	spin_lock_init(&key->lock);
	refcount_set(&key->refs, 1);
	sock_hold(key->sk);

	return key;
}

void mctp_key_unref(struct mctp_sk_key *key)
{
	unsigned long flags;

	if (!refcount_dec_and_test(&key->refs))
		return;

	/* even though no refs exist here, the lock allows us to stay
	 * consistent with the locking requirement of mctp_dev_release_key
	 */
	spin_lock_irqsave(&key->lock, flags);
	mctp_dev_release_key(key->dev, key);
	spin_unlock_irqrestore(&key->lock, flags);

	sock_put(key->sk);
	kfree(key);
}

static int mctp_key_add(struct mctp_sk_key *key, struct mctp_sock *msk)
{
	struct net *net = sock_net(&msk->sk);
	struct mctp_sk_key *tmp;
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&net->mctp.keys_lock, flags);

	if (sock_flag(&msk->sk, SOCK_DEAD)) {
		rc = -EINVAL;
		goto out_unlock;
	}

	hlist_for_each_entry(tmp, &net->mctp.keys, hlist) {
		if (mctp_key_match(tmp, key->net, key->local_addr,
				   key->peer_addr, key->tag)) {
			spin_lock(&tmp->lock);
			if (tmp->valid)
				rc = -EEXIST;
			spin_unlock(&tmp->lock);
			if (rc)
				break;
		}
	}

	if (!rc) {
		refcount_inc(&key->refs);
		key->expiry = jiffies + mctp_key_lifetime;
		timer_reduce(&msk->key_expiry, key->expiry);

		hlist_add_head(&key->hlist, &net->mctp.keys);
		hlist_add_head(&key->sklist, &msk->keys);
	}

out_unlock:
	spin_unlock_irqrestore(&net->mctp.keys_lock, flags);

	return rc;
}

/* Helper for mctp_route_input().
 * We're done with the key; unlock and unref the key.
 * For the usual case of automatic expiry we remove the key from lists.
 * In the case that manual allocation is set on a key we release the lock
 * and local ref, reset reassembly, but don't remove from lists.
 */
static void __mctp_key_done_in(struct mctp_sk_key *key, struct net *net,
			       unsigned long flags, unsigned long reason)
__releases(&key->lock)
{
	struct sk_buff *skb;

	trace_mctp_key_release(key, reason);
	skb = key->reasm_head;
	key->reasm_head = NULL;

	if (!key->manual_alloc) {
		key->reasm_dead = true;
		key->valid = false;
		mctp_dev_release_key(key->dev, key);
	}
	spin_unlock_irqrestore(&key->lock, flags);

	if (!key->manual_alloc) {
		spin_lock_irqsave(&net->mctp.keys_lock, flags);
		if (!hlist_unhashed(&key->hlist)) {
			hlist_del_init(&key->hlist);
			hlist_del_init(&key->sklist);
			mctp_key_unref(key);
		}
		spin_unlock_irqrestore(&net->mctp.keys_lock, flags);
	}

	/* and one for the local reference */
	mctp_key_unref(key);

	kfree_skb(skb);
}

#ifdef CONFIG_MCTP_FLOWS
static void mctp_skb_set_flow(struct sk_buff *skb, struct mctp_sk_key *key)
{
	struct mctp_flow *flow;

	flow = skb_ext_add(skb, SKB_EXT_MCTP);
	if (!flow)
		return;

	refcount_inc(&key->refs);
	flow->key = key;
}

static void mctp_flow_prepare_output(struct sk_buff *skb, struct mctp_dev *dev)
{
	struct mctp_sk_key *key;
	struct mctp_flow *flow;

	flow = skb_ext_find(skb, SKB_EXT_MCTP);
	if (!flow)
		return;

	key = flow->key;

	if (key->dev) {
		WARN_ON(key->dev != dev);
		return;
	}

	mctp_dev_set_key(dev, key);
}
#else
static void mctp_skb_set_flow(struct sk_buff *skb, struct mctp_sk_key *key) {}
static void mctp_flow_prepare_output(struct sk_buff *skb, struct mctp_dev *dev) {}
#endif

static int mctp_frag_queue(struct mctp_sk_key *key, struct sk_buff *skb)
{
	struct mctp_hdr *hdr = mctp_hdr(skb);
	u8 exp_seq, this_seq;

	this_seq = (hdr->flags_seq_tag >> MCTP_HDR_SEQ_SHIFT)
		& MCTP_HDR_SEQ_MASK;

	if (!key->reasm_head) {
		/* Since we're manipulating the shared frag_list, ensure it isn't
		 * shared with any other SKBs.
		 */
		key->reasm_head = skb_unshare(skb, GFP_ATOMIC);
		if (!key->reasm_head)
			return -ENOMEM;

		key->reasm_tailp = &(skb_shinfo(key->reasm_head)->frag_list);
		key->last_seq = this_seq;
		return 0;
	}

	exp_seq = (key->last_seq + 1) & MCTP_HDR_SEQ_MASK;

	if (this_seq != exp_seq)
		return -EINVAL;

	if (key->reasm_head->len + skb->len > mctp_message_maxlen)
		return -EINVAL;

	skb->next = NULL;
	skb->sk = NULL;
	*key->reasm_tailp = skb;
	key->reasm_tailp = &skb->next;

	key->last_seq = this_seq;

	key->reasm_head->data_len += skb->len;
	key->reasm_head->len += skb->len;
	key->reasm_head->truesize += skb->truesize;

	return 0;
}

static int mctp_dst_input(struct mctp_dst *dst, struct sk_buff *skb)
{
	struct mctp_sk_key *key, *any_key = NULL;
	struct net *net = dev_net(skb->dev);
	struct mctp_sock *msk;
	struct mctp_hdr *mh;
	unsigned int netid;
	unsigned long f;
	u8 tag, flags;
	int rc;

	msk = NULL;
	rc = -EINVAL;

	/* We may be receiving a locally-routed packet; drop source sk
	 * accounting.
	 *
	 * From here, we will either queue the skb - either to a frag_queue, or
	 * to a receiving socket. When that succeeds, we clear the skb pointer;
	 * a non-NULL skb on exit will be otherwise unowned, and hence
	 * kfree_skb()-ed.
	 */
	skb_orphan(skb);

	if (skb->pkt_type == PACKET_OUTGOING)
		skb->pkt_type = PACKET_LOOPBACK;

	/* ensure we have enough data for a header and a type */
	if (skb->len < sizeof(struct mctp_hdr) + 1)
		goto out;

	/* grab header, advance data ptr */
	mh = mctp_hdr(skb);
	netid = mctp_cb(skb)->net;
	skb_pull(skb, sizeof(struct mctp_hdr));

	if (mh->ver != 1)
		goto out;

	flags = mh->flags_seq_tag & (MCTP_HDR_FLAG_SOM | MCTP_HDR_FLAG_EOM);
	tag = mh->flags_seq_tag & (MCTP_HDR_TAG_MASK | MCTP_HDR_FLAG_TO);

	rcu_read_lock();

	/* lookup socket / reasm context, exactly matching (src,dest,tag).
	 * we hold a ref on the key, and key->lock held.
	 */
	key = mctp_lookup_key(net, skb, netid, mh->src, &f);

	if (flags & MCTP_HDR_FLAG_SOM) {
		if (key) {
			msk = container_of(key->sk, struct mctp_sock, sk);
		} else {
			/* first response to a broadcast? do a more general
			 * key lookup to find the socket, but don't use this
			 * key for reassembly - we'll create a more specific
			 * one for future packets if required (ie, !EOM).
			 *
			 * this lookup requires key->peer to be MCTP_ADDR_ANY,
			 * it doesn't match just any key->peer.
			 */
			any_key = mctp_lookup_key(net, skb, netid,
						  MCTP_ADDR_ANY, &f);
			if (any_key) {
				msk = container_of(any_key->sk,
						   struct mctp_sock, sk);
				spin_unlock_irqrestore(&any_key->lock, f);
			}
		}

		if (!key && !msk && (tag & MCTP_HDR_FLAG_TO))
			msk = mctp_lookup_bind(net, skb);

		if (!msk) {
			rc = -ENOENT;
			goto out_unlock;
		}

		/* single-packet message? deliver to socket, clean up any
		 * pending key.
		 */
		if (flags & MCTP_HDR_FLAG_EOM) {
			rc = sock_queue_rcv_skb(&msk->sk, skb);
			if (!rc)
				skb = NULL;
			if (key) {
				/* we've hit a pending reassembly; not much we
				 * can do but drop it
				 */
				__mctp_key_done_in(key, net, f,
						   MCTP_TRACE_KEY_REPLIED);
				key = NULL;
			}
			goto out_unlock;
		}

		/* broadcast response or a bind() - create a key for further
		 * packets for this message
		 */
		if (!key) {
			key = mctp_key_alloc(msk, netid, mh->dest, mh->src,
					     tag, GFP_ATOMIC);
			if (!key) {
				rc = -ENOMEM;
				goto out_unlock;
			}

			/* we can queue without the key lock here, as the
			 * key isn't observable yet
			 */
			mctp_frag_queue(key, skb);

			/* if the key_add fails, we've raced with another
			 * SOM packet with the same src, dest and tag. There's
			 * no way to distinguish future packets, so all we
			 * can do is drop; we'll free the skb on exit from
			 * this function.
			 */
			rc = mctp_key_add(key, msk);
			if (!rc) {
				trace_mctp_key_acquire(key);
				skb = NULL;
			}

			/* we don't need to release key->lock on exit, so
			 * clean up here and suppress the unlock via
			 * setting to NULL
			 */
			mctp_key_unref(key);
			key = NULL;

		} else {
			if (key->reasm_head || key->reasm_dead) {
				/* duplicate start? drop everything */
				__mctp_key_done_in(key, net, f,
						   MCTP_TRACE_KEY_INVALIDATED);
				rc = -EEXIST;
				key = NULL;
			} else {
				rc = mctp_frag_queue(key, skb);
				if (!rc)
					skb = NULL;
			}
		}

	} else if (key) {
		/* this packet continues a previous message; reassemble
		 * using the message-specific key
		 */

		/* we need to be continuing an existing reassembly... */
		if (!key->reasm_head)
			rc = -EINVAL;
		else
			rc = mctp_frag_queue(key, skb);

		if (rc)
			goto out_unlock;

		/* we've queued; the queue owns the skb now */
		skb = NULL;

		/* end of message? deliver to socket, and we're done with
		 * the reassembly/response key
		 */
		if (flags & MCTP_HDR_FLAG_EOM) {
			rc = sock_queue_rcv_skb(key->sk, key->reasm_head);
			if (!rc)
				key->reasm_head = NULL;
			__mctp_key_done_in(key, net, f, MCTP_TRACE_KEY_REPLIED);
			key = NULL;
		}

	} else {
		/* not a start, no matching key */
		rc = -ENOENT;
	}

out_unlock:
	rcu_read_unlock();
	if (key) {
		spin_unlock_irqrestore(&key->lock, f);
		mctp_key_unref(key);
	}
	if (any_key)
		mctp_key_unref(any_key);
out:
	kfree_skb(skb);
	return rc;
}

static int mctp_dst_output(struct mctp_dst *dst, struct sk_buff *skb)
{
	char daddr_buf[MAX_ADDR_LEN];
	char *daddr = NULL;
	int rc;

	skb->protocol = htons(ETH_P_MCTP);
	skb->pkt_type = PACKET_OUTGOING;

	if (skb->len > dst->mtu) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	/* direct route; use the hwaddr we stashed in sendmsg */
	if (dst->halen) {
		if (dst->halen != skb->dev->addr_len) {
			/* sanity check, sendmsg should have already caught this */
			kfree_skb(skb);
			return -EMSGSIZE;
		}
		daddr = dst->haddr;
	} else {
		/* If lookup fails let the device handle daddr==NULL */
		if (mctp_neigh_lookup(dst->dev, dst->nexthop, daddr_buf) == 0)
			daddr = daddr_buf;
	}

	rc = dev_hard_header(skb, skb->dev, ntohs(skb->protocol),
			     daddr, skb->dev->dev_addr, skb->len);
	if (rc < 0) {
		kfree_skb(skb);
		return -EHOSTUNREACH;
	}

	mctp_flow_prepare_output(skb, dst->dev);

	rc = dev_queue_xmit(skb);
	if (rc)
		rc = net_xmit_errno(rc);

	return rc;
}

/* route alloc/release */
static void mctp_route_release(struct mctp_route *rt)
{
	if (refcount_dec_and_test(&rt->refs)) {
		if (rt->dst_type == MCTP_ROUTE_DIRECT)
			mctp_dev_put(rt->dev);
		kfree_rcu(rt, rcu);
	}
}

/* returns a route with the refcount at 1 */
static struct mctp_route *mctp_route_alloc(void)
{
	struct mctp_route *rt;

	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return NULL;

	INIT_LIST_HEAD(&rt->list);
	refcount_set(&rt->refs, 1);
	rt->output = mctp_dst_discard;

	return rt;
}

unsigned int mctp_default_net(struct net *net)
{
	return READ_ONCE(net->mctp.default_net);
}

int mctp_default_net_set(struct net *net, unsigned int index)
{
	if (index == 0)
		return -EINVAL;
	WRITE_ONCE(net->mctp.default_net, index);
	return 0;
}

/* tag management */
static void mctp_reserve_tag(struct net *net, struct mctp_sk_key *key,
			     struct mctp_sock *msk)
{
	struct netns_mctp *mns = &net->mctp;

	lockdep_assert_held(&mns->keys_lock);

	key->expiry = jiffies + mctp_key_lifetime;
	timer_reduce(&msk->key_expiry, key->expiry);

	/* we hold the net->key_lock here, allowing updates to both
	 * then net and sk
	 */
	hlist_add_head_rcu(&key->hlist, &mns->keys);
	hlist_add_head_rcu(&key->sklist, &msk->keys);
	refcount_inc(&key->refs);
}

/* Allocate a locally-owned tag value for (local, peer), and reserve
 * it for the socket msk
 */
struct mctp_sk_key *mctp_alloc_local_tag(struct mctp_sock *msk,
					 unsigned int netid,
					 mctp_eid_t local, mctp_eid_t peer,
					 bool manual, u8 *tagp)
{
	struct net *net = sock_net(&msk->sk);
	struct netns_mctp *mns = &net->mctp;
	struct mctp_sk_key *key, *tmp;
	unsigned long flags;
	u8 tagbits;

	/* for NULL destination EIDs, we may get a response from any peer */
	if (peer == MCTP_ADDR_NULL)
		peer = MCTP_ADDR_ANY;

	/* be optimistic, alloc now */
	key = mctp_key_alloc(msk, netid, local, peer, 0, GFP_KERNEL);
	if (!key)
		return ERR_PTR(-ENOMEM);

	/* 8 possible tag values */
	tagbits = 0xff;

	spin_lock_irqsave(&mns->keys_lock, flags);

	/* Walk through the existing keys, looking for potential conflicting
	 * tags. If we find a conflict, clear that bit from tagbits
	 */
	hlist_for_each_entry(tmp, &mns->keys, hlist) {
		/* We can check the lookup fields (*_addr, tag) without the
		 * lock held, they don't change over the lifetime of the key.
		 */

		/* tags are net-specific */
		if (tmp->net != netid)
			continue;

		/* if we don't own the tag, it can't conflict */
		if (tmp->tag & MCTP_HDR_FLAG_TO)
			continue;

		/* Since we're avoiding conflicting entries, match peer and
		 * local addresses, including with a wildcard on ANY. See
		 * 'A note on key allocations' for background.
		 */
		if (peer != MCTP_ADDR_ANY &&
		    !mctp_address_matches(tmp->peer_addr, peer))
			continue;

		if (local != MCTP_ADDR_ANY &&
		    !mctp_address_matches(tmp->local_addr, local))
			continue;

		spin_lock(&tmp->lock);
		/* key must still be valid. If we find a match, clear the
		 * potential tag value
		 */
		if (tmp->valid)
			tagbits &= ~(1 << tmp->tag);
		spin_unlock(&tmp->lock);

		if (!tagbits)
			break;
	}

	if (tagbits) {
		key->tag = __ffs(tagbits);
		mctp_reserve_tag(net, key, msk);
		trace_mctp_key_acquire(key);

		key->manual_alloc = manual;
		*tagp = key->tag;
	}

	spin_unlock_irqrestore(&mns->keys_lock, flags);

	if (!tagbits) {
		mctp_key_unref(key);
		return ERR_PTR(-EBUSY);
	}

	return key;
}

static struct mctp_sk_key *mctp_lookup_prealloc_tag(struct mctp_sock *msk,
						    unsigned int netid,
						    mctp_eid_t daddr,
						    u8 req_tag, u8 *tagp)
{
	struct net *net = sock_net(&msk->sk);
	struct netns_mctp *mns = &net->mctp;
	struct mctp_sk_key *key, *tmp;
	unsigned long flags;

	req_tag &= ~(MCTP_TAG_PREALLOC | MCTP_TAG_OWNER);
	key = NULL;

	spin_lock_irqsave(&mns->keys_lock, flags);

	hlist_for_each_entry(tmp, &mns->keys, hlist) {
		if (tmp->net != netid)
			continue;

		if (tmp->tag != req_tag)
			continue;

		if (!mctp_address_matches(tmp->peer_addr, daddr))
			continue;

		if (!tmp->manual_alloc)
			continue;

		spin_lock(&tmp->lock);
		if (tmp->valid) {
			key = tmp;
			refcount_inc(&key->refs);
			spin_unlock(&tmp->lock);
			break;
		}
		spin_unlock(&tmp->lock);
	}
	spin_unlock_irqrestore(&mns->keys_lock, flags);

	if (!key)
		return ERR_PTR(-ENOENT);

	if (tagp)
		*tagp = key->tag;

	return key;
}

/* routing lookups */
static unsigned int mctp_route_netid(struct mctp_route *rt)
{
	return rt->dst_type == MCTP_ROUTE_DIRECT ?
		READ_ONCE(rt->dev->net) : rt->gateway.net;
}

static bool mctp_rt_match_eid(struct mctp_route *rt,
			      unsigned int net, mctp_eid_t eid)
{
	return mctp_route_netid(rt) == net &&
		rt->min <= eid && rt->max >= eid;
}

/* compares match, used for duplicate prevention */
static bool mctp_rt_compare_exact(struct mctp_route *rt1,
				  struct mctp_route *rt2)
{
	ASSERT_RTNL();
	return mctp_route_netid(rt1) == mctp_route_netid(rt2) &&
		rt1->min == rt2->min &&
		rt1->max == rt2->max;
}

/* must only be called on a direct route, as the final output hop */
static void mctp_dst_from_route(struct mctp_dst *dst, mctp_eid_t eid,
				unsigned int mtu, struct mctp_route *route)
{
	mctp_dev_hold(route->dev);
	dst->nexthop = eid;
	dst->dev = route->dev;
	dst->mtu = READ_ONCE(dst->dev->dev->mtu);
	if (mtu)
		dst->mtu = min(dst->mtu, mtu);
	dst->halen = 0;
	dst->output = route->output;
}

int mctp_dst_from_extaddr(struct mctp_dst *dst, struct net *net, int ifindex,
			  unsigned char halen, const unsigned char *haddr)
{
	struct net_device *netdev;
	struct mctp_dev *dev;
	int rc = -ENOENT;

	if (halen > sizeof(dst->haddr))
		return -EINVAL;

	rcu_read_lock();

	netdev = dev_get_by_index_rcu(net, ifindex);
	if (!netdev)
		goto out_unlock;

	if (netdev->addr_len != halen) {
		rc = -EINVAL;
		goto out_unlock;
	}

	dev = __mctp_dev_get(netdev);
	if (!dev)
		goto out_unlock;

	dst->dev = dev;
	dst->mtu = READ_ONCE(netdev->mtu);
	dst->halen = halen;
	dst->output = mctp_dst_output;
	dst->nexthop = 0;
	memcpy(dst->haddr, haddr, halen);

	rc = 0;

out_unlock:
	rcu_read_unlock();
	return rc;
}

void mctp_dst_release(struct mctp_dst *dst)
{
	mctp_dev_put(dst->dev);
}

static struct mctp_route *mctp_route_lookup_single(struct net *net,
						   unsigned int dnet,
						   mctp_eid_t daddr)
{
	struct mctp_route *rt;

	list_for_each_entry_rcu(rt, &net->mctp.routes, list) {
		if (mctp_rt_match_eid(rt, dnet, daddr))
			return rt;
	}

	return NULL;
}

/* populates *dst on successful lookup, if set */
int mctp_route_lookup(struct net *net, unsigned int dnet,
		      mctp_eid_t daddr, struct mctp_dst *dst)
{
	const unsigned int max_depth = 32;
	unsigned int depth, mtu = 0;
	int rc = -EHOSTUNREACH;

	rcu_read_lock();

	for (depth = 0; depth < max_depth; depth++) {
		struct mctp_route *rt;

		rt = mctp_route_lookup_single(net, dnet, daddr);
		if (!rt)
			break;

		/* clamp mtu to the smallest in the path, allowing 0
		 * to specify no restrictions
		 */
		if (mtu && rt->mtu)
			mtu = min(mtu, rt->mtu);
		else
			mtu = mtu ?: rt->mtu;

		if (rt->dst_type == MCTP_ROUTE_DIRECT) {
			if (dst)
				mctp_dst_from_route(dst, daddr, mtu, rt);
			rc = 0;
			break;

		} else if (rt->dst_type == MCTP_ROUTE_GATEWAY) {
			daddr = rt->gateway.eid;
		}
	}

	rcu_read_unlock();

	return rc;
}

static int mctp_route_lookup_null(struct net *net, struct net_device *dev,
				  struct mctp_dst *dst)
{
	int rc = -EHOSTUNREACH;
	struct mctp_route *rt;

	rcu_read_lock();

	list_for_each_entry_rcu(rt, &net->mctp.routes, list) {
		if (rt->dst_type != MCTP_ROUTE_DIRECT || rt->type != RTN_LOCAL)
			continue;

		if (rt->dev->dev != dev)
			continue;

		mctp_dst_from_route(dst, 0, 0, rt);
		rc = 0;
		break;
	}

	rcu_read_unlock();

	return rc;
}

static int mctp_do_fragment_route(struct mctp_dst *dst, struct sk_buff *skb,
				  unsigned int mtu, u8 tag)
{
	const unsigned int hlen = sizeof(struct mctp_hdr);
	struct mctp_hdr *hdr, *hdr2;
	unsigned int pos, size, headroom;
	struct sk_buff *skb2;
	int rc;
	u8 seq;

	hdr = mctp_hdr(skb);
	seq = 0;
	rc = 0;

	if (mtu < hlen + 1) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	/* keep same headroom as the original skb */
	headroom = skb_headroom(skb);

	/* we've got the header */
	skb_pull(skb, hlen);

	for (pos = 0; pos < skb->len;) {
		/* size of message payload */
		size = min(mtu - hlen, skb->len - pos);

		skb2 = alloc_skb(headroom + hlen + size, GFP_KERNEL);
		if (!skb2) {
			rc = -ENOMEM;
			break;
		}

		/* generic skb copy */
		skb2->protocol = skb->protocol;
		skb2->priority = skb->priority;
		skb2->dev = skb->dev;
		memcpy(skb2->cb, skb->cb, sizeof(skb2->cb));

		if (skb->sk)
			skb_set_owner_w(skb2, skb->sk);

		/* establish packet */
		skb_reserve(skb2, headroom);
		skb_reset_network_header(skb2);
		skb_put(skb2, hlen + size);
		skb2->transport_header = skb2->network_header + hlen;

		/* copy header fields, calculate SOM/EOM flags & seq */
		hdr2 = mctp_hdr(skb2);
		hdr2->ver = hdr->ver;
		hdr2->dest = hdr->dest;
		hdr2->src = hdr->src;
		hdr2->flags_seq_tag = tag &
			(MCTP_HDR_TAG_MASK | MCTP_HDR_FLAG_TO);

		if (pos == 0)
			hdr2->flags_seq_tag |= MCTP_HDR_FLAG_SOM;

		if (pos + size == skb->len)
			hdr2->flags_seq_tag |= MCTP_HDR_FLAG_EOM;

		hdr2->flags_seq_tag |= seq << MCTP_HDR_SEQ_SHIFT;

		/* copy message payload */
		skb_copy_bits(skb, pos, skb_transport_header(skb2), size);

		/* we need to copy the extensions, for MCTP flow data */
		skb_ext_copy(skb2, skb);

		/* do route */
		rc = dst->output(dst, skb2);
		if (rc)
			break;

		seq = (seq + 1) & MCTP_HDR_SEQ_MASK;
		pos += size;
	}

	consume_skb(skb);
	return rc;
}

int mctp_local_output(struct sock *sk, struct mctp_dst *dst,
		      struct sk_buff *skb, mctp_eid_t daddr, u8 req_tag)
{
	struct mctp_sock *msk = container_of(sk, struct mctp_sock, sk);
	struct mctp_sk_key *key;
	struct mctp_hdr *hdr;
	unsigned long flags;
	unsigned int netid;
	unsigned int mtu;
	mctp_eid_t saddr;
	int rc;
	u8 tag;

	KUNIT_STATIC_STUB_REDIRECT(mctp_local_output, sk, dst, skb, daddr,
				   req_tag);

	rc = -ENODEV;

	spin_lock_irqsave(&dst->dev->addrs_lock, flags);
	if (dst->dev->num_addrs == 0) {
		rc = -EHOSTUNREACH;
	} else {
		/* use the outbound interface's first address as our source */
		saddr = dst->dev->addrs[0];
		rc = 0;
	}
	spin_unlock_irqrestore(&dst->dev->addrs_lock, flags);
	netid = READ_ONCE(dst->dev->net);

	if (rc)
		goto out_release;

	if (req_tag & MCTP_TAG_OWNER) {
		if (req_tag & MCTP_TAG_PREALLOC)
			key = mctp_lookup_prealloc_tag(msk, netid, daddr,
						       req_tag, &tag);
		else
			key = mctp_alloc_local_tag(msk, netid, saddr, daddr,
						   false, &tag);

		if (IS_ERR(key)) {
			rc = PTR_ERR(key);
			goto out_release;
		}
		mctp_skb_set_flow(skb, key);
		/* done with the key in this scope */
		mctp_key_unref(key);
		tag |= MCTP_HDR_FLAG_TO;
	} else {
		key = NULL;
		tag = req_tag & MCTP_TAG_MASK;
	}

	skb->pkt_type = PACKET_OUTGOING;
	skb->protocol = htons(ETH_P_MCTP);
	skb->priority = 0;
	skb_reset_transport_header(skb);
	skb_push(skb, sizeof(struct mctp_hdr));
	skb_reset_network_header(skb);
	skb->dev = dst->dev->dev;

	/* set up common header fields */
	hdr = mctp_hdr(skb);
	hdr->ver = 1;
	hdr->dest = daddr;
	hdr->src = saddr;

	mtu = dst->mtu;

	if (skb->len + sizeof(struct mctp_hdr) <= mtu) {
		hdr->flags_seq_tag = MCTP_HDR_FLAG_SOM |
			MCTP_HDR_FLAG_EOM | tag;
		rc = dst->output(dst, skb);
	} else {
		rc = mctp_do_fragment_route(dst, skb, mtu, tag);
	}

	/* route output functions consume the skb, even on error */
	skb = NULL;

out_release:
	kfree_skb(skb);
	return rc;
}

/* route management */

/* mctp_route_add(): Add the provided route, previously allocated via
 * mctp_route_alloc(). On success, takes ownership of @rt, which includes a
 * hold on rt->dev for usage in the route table. On failure a caller will want
 * to mctp_route_release().
 *
 * We expect that the caller has set rt->type, rt->dst_type, rt->min, rt->max,
 * rt->mtu and either rt->dev (with a reference held appropriately) or
 * rt->gateway. Other fields will be populated.
 */
static int mctp_route_add(struct net *net, struct mctp_route *rt)
{
	struct mctp_route *ert;

	if (!mctp_address_unicast(rt->min) || !mctp_address_unicast(rt->max))
		return -EINVAL;

	if (rt->dst_type == MCTP_ROUTE_DIRECT && !rt->dev)
		return -EINVAL;

	if (rt->dst_type == MCTP_ROUTE_GATEWAY && !rt->gateway.eid)
		return -EINVAL;

	switch (rt->type) {
	case RTN_LOCAL:
		rt->output = mctp_dst_input;
		break;
	case RTN_UNICAST:
		rt->output = mctp_dst_output;
		break;
	default:
		return -EINVAL;
	}

	ASSERT_RTNL();

	/* Prevent duplicate identical routes. */
	list_for_each_entry(ert, &net->mctp.routes, list) {
		if (mctp_rt_compare_exact(rt, ert)) {
			return -EEXIST;
		}
	}

	list_add_rcu(&rt->list, &net->mctp.routes);

	return 0;
}

static int mctp_route_remove(struct net *net, unsigned int netid,
			     mctp_eid_t daddr_start, unsigned int daddr_extent,
			     unsigned char type)
{
	struct mctp_route *rt, *tmp;
	mctp_eid_t daddr_end;
	bool dropped;

	if (daddr_extent > 0xff || daddr_start + daddr_extent >= 255)
		return -EINVAL;

	daddr_end = daddr_start + daddr_extent;
	dropped = false;

	ASSERT_RTNL();

	list_for_each_entry_safe(rt, tmp, &net->mctp.routes, list) {
		if (mctp_route_netid(rt) == netid &&
		    rt->min == daddr_start && rt->max == daddr_end &&
		    rt->type == type) {
			list_del_rcu(&rt->list);
			/* TODO: immediate RTM_DELROUTE */
			mctp_route_release(rt);
			dropped = true;
		}
	}

	return dropped ? 0 : -ENOENT;
}

int mctp_route_add_local(struct mctp_dev *mdev, mctp_eid_t addr)
{
	struct mctp_route *rt;
	int rc;

	rt = mctp_route_alloc();
	if (!rt)
		return -ENOMEM;

	rt->min = addr;
	rt->max = addr;
	rt->dst_type = MCTP_ROUTE_DIRECT;
	rt->dev = mdev;
	rt->type = RTN_LOCAL;

	mctp_dev_hold(rt->dev);

	rc = mctp_route_add(dev_net(mdev->dev), rt);
	if (rc)
		mctp_route_release(rt);

	return rc;
}

int mctp_route_remove_local(struct mctp_dev *mdev, mctp_eid_t addr)
{
	return mctp_route_remove(dev_net(mdev->dev), mdev->net,
				 addr, 0, RTN_LOCAL);
}

/* removes all entries for a given device */
void mctp_route_remove_dev(struct mctp_dev *mdev)
{
	struct net *net = dev_net(mdev->dev);
	struct mctp_route *rt, *tmp;

	ASSERT_RTNL();
	list_for_each_entry_safe(rt, tmp, &net->mctp.routes, list) {
		if (rt->dst_type == MCTP_ROUTE_DIRECT && rt->dev == mdev) {
			list_del_rcu(&rt->list);
			/* TODO: immediate RTM_DELROUTE */
			mctp_route_release(rt);
		}
	}
}

/* Incoming packet-handling */

static int mctp_pkttype_receive(struct sk_buff *skb, struct net_device *dev,
				struct packet_type *pt,
				struct net_device *orig_dev)
{
	struct net *net = dev_net(dev);
	struct mctp_dev *mdev;
	struct mctp_skb_cb *cb;
	struct mctp_dst dst;
	struct mctp_hdr *mh;
	int rc;

	rcu_read_lock();
	mdev = __mctp_dev_get(dev);
	rcu_read_unlock();
	if (!mdev) {
		/* basic non-data sanity checks */
		goto err_drop;
	}

	if (!pskb_may_pull(skb, sizeof(struct mctp_hdr)))
		goto err_drop;

	skb_reset_transport_header(skb);
	skb_reset_network_header(skb);

	/* We have enough for a header; decode and route */
	mh = mctp_hdr(skb);
	if (mh->ver < MCTP_VER_MIN || mh->ver > MCTP_VER_MAX)
		goto err_drop;

	/* source must be valid unicast or null; drop reserved ranges and
	 * broadcast
	 */
	if (!(mctp_address_unicast(mh->src) || mctp_address_null(mh->src)))
		goto err_drop;

	/* dest address: as above, but allow broadcast */
	if (!(mctp_address_unicast(mh->dest) || mctp_address_null(mh->dest) ||
	      mctp_address_broadcast(mh->dest)))
		goto err_drop;

	/* MCTP drivers must populate halen/haddr */
	if (dev->type == ARPHRD_MCTP) {
		cb = mctp_cb(skb);
	} else {
		cb = __mctp_cb(skb);
		cb->halen = 0;
	}
	cb->net = READ_ONCE(mdev->net);
	cb->ifindex = dev->ifindex;

	rc = mctp_route_lookup(net, cb->net, mh->dest, &dst);

	/* NULL EID, but addressed to our physical address */
	if (rc && mh->dest == MCTP_ADDR_NULL && skb->pkt_type == PACKET_HOST)
		rc = mctp_route_lookup_null(net, dev, &dst);

	if (rc)
		goto err_drop;

	dst.output(&dst, skb);
	mctp_dst_release(&dst);
	mctp_dev_put(mdev);

	return NET_RX_SUCCESS;

err_drop:
	kfree_skb(skb);
	mctp_dev_put(mdev);
	return NET_RX_DROP;
}

static struct packet_type mctp_packet_type = {
	.type = cpu_to_be16(ETH_P_MCTP),
	.func = mctp_pkttype_receive,
};

/* netlink interface */

static const struct nla_policy rta_mctp_policy[RTA_MAX + 1] = {
	[RTA_DST]		= { .type = NLA_U8 },
	[RTA_METRICS]		= { .type = NLA_NESTED },
	[RTA_OIF]		= { .type = NLA_U32 },
	[RTA_GATEWAY]		= NLA_POLICY_EXACT_LEN(sizeof(struct mctp_fq_addr)),
};

static const struct nla_policy rta_metrics_policy[RTAX_MAX + 1] = {
	[RTAX_MTU]		= { .type = NLA_U32 },
};

/* base parsing; common to both _lookup and _populate variants.
 *
 * For gateway routes (which have a RTA_GATEWAY, and no RTA_OIF), we populate
 * *gatweayp. for direct routes (RTA_OIF, no RTA_GATEWAY), we populate *mdev.
 */
static int mctp_route_nlparse_common(struct net *net, struct nlmsghdr *nlh,
				     struct netlink_ext_ack *extack,
				     struct nlattr **tb, struct rtmsg **rtm,
				     struct mctp_dev **mdev,
				     struct mctp_fq_addr *gatewayp,
				     mctp_eid_t *daddr_start)
{
	struct mctp_fq_addr *gateway = NULL;
	unsigned int ifindex = 0;
	struct net_device *dev;
	int rc;

	rc = nlmsg_parse(nlh, sizeof(struct rtmsg), tb, RTA_MAX,
			 rta_mctp_policy, extack);
	if (rc < 0) {
		NL_SET_ERR_MSG(extack, "incorrect format");
		return rc;
	}

	if (!tb[RTA_DST]) {
		NL_SET_ERR_MSG(extack, "dst EID missing");
		return -EINVAL;
	}
	*daddr_start = nla_get_u8(tb[RTA_DST]);

	if (tb[RTA_OIF])
		ifindex = nla_get_u32(tb[RTA_OIF]);

	if (tb[RTA_GATEWAY])
		gateway = nla_data(tb[RTA_GATEWAY]);

	if (ifindex && gateway) {
		NL_SET_ERR_MSG(extack,
			       "cannot specify both ifindex and gateway");
		return -EINVAL;

	} else if (ifindex) {
		dev = __dev_get_by_index(net, ifindex);
		if (!dev) {
			NL_SET_ERR_MSG(extack, "bad ifindex");
			return -ENODEV;
		}
		*mdev = mctp_dev_get_rtnl(dev);
		if (!*mdev)
			return -ENODEV;
		gatewayp->eid = 0;

	} else if (gateway) {
		if (!mctp_address_unicast(gateway->eid)) {
			NL_SET_ERR_MSG(extack, "bad gateway");
			return -EINVAL;
		}

		gatewayp->eid = gateway->eid;
		gatewayp->net = gateway->net != MCTP_NET_ANY ?
			gateway->net :
			READ_ONCE(net->mctp.default_net);
		*mdev = NULL;

	} else {
		NL_SET_ERR_MSG(extack, "no route output provided");
		return -EINVAL;
	}

	*rtm = nlmsg_data(nlh);
	if ((*rtm)->rtm_family != AF_MCTP) {
		NL_SET_ERR_MSG(extack, "route family must be AF_MCTP");
		return -EINVAL;
	}

	if ((*rtm)->rtm_type != RTN_UNICAST) {
		NL_SET_ERR_MSG(extack, "rtm_type must be RTN_UNICAST");
		return -EINVAL;
	}

	return 0;
}

/* Route parsing for lookup operations; we only need the "route target"
 * components (ie., network and dest-EID range).
 */
static int mctp_route_nlparse_lookup(struct net *net, struct nlmsghdr *nlh,
				     struct netlink_ext_ack *extack,
				     unsigned char *type, unsigned int *netid,
				     mctp_eid_t *daddr_start,
				     unsigned int *daddr_extent)
{
	struct nlattr *tb[RTA_MAX + 1];
	struct mctp_fq_addr gw;
	struct mctp_dev *mdev;
	struct rtmsg *rtm;
	int rc;

	rc = mctp_route_nlparse_common(net, nlh, extack, tb, &rtm,
				       &mdev, &gw, daddr_start);
	if (rc)
		return rc;

	if (mdev) {
		*netid = mdev->net;
	} else if (gw.eid) {
		*netid = gw.net;
	} else {
		/* bug: _nlparse_common should not allow this */
		return -1;
	}

	*type = rtm->rtm_type;
	*daddr_extent = rtm->rtm_dst_len;

	return 0;
}

/* Full route parse for RTM_NEWROUTE: populate @rt. On success,
 * MCTP_ROUTE_DIRECT routes (ie, those with a direct dev) will hold a reference
 * to that dev.
 */
static int mctp_route_nlparse_populate(struct net *net, struct nlmsghdr *nlh,
				       struct netlink_ext_ack *extack,
				       struct mctp_route *rt)
{
	struct nlattr *tbx[RTAX_MAX + 1];
	struct nlattr *tb[RTA_MAX + 1];
	unsigned int daddr_extent;
	struct mctp_fq_addr gw;
	mctp_eid_t daddr_start;
	struct mctp_dev *dev;
	struct rtmsg *rtm;
	u32 mtu = 0;
	int rc;

	rc = mctp_route_nlparse_common(net, nlh, extack, tb, &rtm,
				       &dev, &gw, &daddr_start);
	if (rc)
		return rc;

	daddr_extent = rtm->rtm_dst_len;

	if (daddr_extent > 0xff || daddr_extent + daddr_start >= 255) {
		NL_SET_ERR_MSG(extack, "invalid eid range");
		return -EINVAL;
	}

	if (tb[RTA_METRICS]) {
		rc = nla_parse_nested(tbx, RTAX_MAX, tb[RTA_METRICS],
				      rta_metrics_policy, NULL);
		if (rc < 0) {
			NL_SET_ERR_MSG(extack, "incorrect RTA_METRICS format");
			return rc;
		}
		if (tbx[RTAX_MTU])
			mtu = nla_get_u32(tbx[RTAX_MTU]);
	}

	rt->type = rtm->rtm_type;
	rt->min = daddr_start;
	rt->max = daddr_start + daddr_extent;
	rt->mtu = mtu;
	if (gw.eid) {
		rt->dst_type = MCTP_ROUTE_GATEWAY;
		rt->gateway.eid = gw.eid;
		rt->gateway.net = gw.net;
	} else {
		rt->dst_type = MCTP_ROUTE_DIRECT;
		rt->dev = dev;
		mctp_dev_hold(rt->dev);
	}

	return 0;
}

static int mctp_newroute(struct sk_buff *skb, struct nlmsghdr *nlh,
			 struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct mctp_route *rt;
	int rc;

	rt = mctp_route_alloc();
	if (!rt)
		return -ENOMEM;

	rc = mctp_route_nlparse_populate(net, nlh, extack, rt);
	if (rc < 0)
		goto err_free;

	if (rt->dst_type == MCTP_ROUTE_DIRECT &&
	    rt->dev->dev->flags & IFF_LOOPBACK) {
		NL_SET_ERR_MSG(extack, "no routes to loopback");
		rc = -EINVAL;
		goto err_free;
	}

	rc = mctp_route_add(net, rt);
	if (!rc)
		return 0;

err_free:
	mctp_route_release(rt);
	return rc;
}

static int mctp_delroute(struct sk_buff *skb, struct nlmsghdr *nlh,
			 struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	unsigned int netid, daddr_extent;
	unsigned char type = RTN_UNSPEC;
	mctp_eid_t daddr_start;
	int rc;

	rc = mctp_route_nlparse_lookup(net, nlh, extack, &type, &netid,
				       &daddr_start, &daddr_extent);
	if (rc < 0)
		return rc;

	/* we only have unicast routes */
	if (type != RTN_UNICAST)
		return -EINVAL;

	rc = mctp_route_remove(net, netid, daddr_start, daddr_extent, type);
	return rc;
}

static int mctp_fill_rtinfo(struct sk_buff *skb, struct mctp_route *rt,
			    u32 portid, u32 seq, int event, unsigned int flags)
{
	struct nlmsghdr *nlh;
	struct rtmsg *hdr;
	void *metrics;

	nlh = nlmsg_put(skb, portid, seq, event, sizeof(*hdr), flags);
	if (!nlh)
		return -EMSGSIZE;

	hdr = nlmsg_data(nlh);
	hdr->rtm_family = AF_MCTP;

	/* we use the _len fields as a number of EIDs, rather than
	 * a number of bits in the address
	 */
	hdr->rtm_dst_len = rt->max - rt->min;
	hdr->rtm_src_len = 0;
	hdr->rtm_tos = 0;
	hdr->rtm_table = RT_TABLE_DEFAULT;
	hdr->rtm_protocol = RTPROT_STATIC; /* everything is user-defined */
	hdr->rtm_type = rt->type;

	if (nla_put_u8(skb, RTA_DST, rt->min))
		goto cancel;

	metrics = nla_nest_start_noflag(skb, RTA_METRICS);
	if (!metrics)
		goto cancel;

	if (rt->mtu) {
		if (nla_put_u32(skb, RTAX_MTU, rt->mtu))
			goto cancel;
	}

	nla_nest_end(skb, metrics);

	if (rt->dst_type == MCTP_ROUTE_DIRECT) {
		hdr->rtm_scope = RT_SCOPE_LINK;
		if (nla_put_u32(skb, RTA_OIF, rt->dev->dev->ifindex))
			goto cancel;
	} else if (rt->dst_type == MCTP_ROUTE_GATEWAY) {
		hdr->rtm_scope = RT_SCOPE_UNIVERSE;
		if (nla_put(skb, RTA_GATEWAY,
			    sizeof(rt->gateway), &rt->gateway))
			goto cancel;
	}

	nlmsg_end(skb, nlh);

	return 0;

cancel:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static int mctp_dump_rtinfo(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct mctp_route *rt;
	int s_idx, idx;

	/* TODO: allow filtering on route data, possibly under
	 * cb->strict_check
	 */

	/* TODO: change to struct overlay */
	s_idx = cb->args[0];
	idx = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(rt, &net->mctp.routes, list) {
		if (idx++ < s_idx)
			continue;
		if (mctp_fill_rtinfo(skb, rt,
				     NETLINK_CB(cb->skb).portid,
				     cb->nlh->nlmsg_seq,
				     RTM_NEWROUTE, NLM_F_MULTI) < 0)
			break;
	}

	rcu_read_unlock();
	cb->args[0] = idx;

	return skb->len;
}

/* net namespace implementation */
static int __net_init mctp_routes_net_init(struct net *net)
{
	struct netns_mctp *ns = &net->mctp;

	INIT_LIST_HEAD(&ns->routes);
	hash_init(ns->binds);
	mutex_init(&ns->bind_lock);
	INIT_HLIST_HEAD(&ns->keys);
	spin_lock_init(&ns->keys_lock);
	WARN_ON(mctp_default_net_set(net, MCTP_INITIAL_DEFAULT_NET));
	return 0;
}

static void __net_exit mctp_routes_net_exit(struct net *net)
{
	struct mctp_route *rt;

	rcu_read_lock();
	list_for_each_entry_rcu(rt, &net->mctp.routes, list)
		mctp_route_release(rt);
	rcu_read_unlock();
}

static struct pernet_operations mctp_net_ops = {
	.init = mctp_routes_net_init,
	.exit = mctp_routes_net_exit,
};

static const struct rtnl_msg_handler mctp_route_rtnl_msg_handlers[] = {
	{THIS_MODULE, PF_MCTP, RTM_NEWROUTE, mctp_newroute, NULL, 0},
	{THIS_MODULE, PF_MCTP, RTM_DELROUTE, mctp_delroute, NULL, 0},
	{THIS_MODULE, PF_MCTP, RTM_GETROUTE, NULL, mctp_dump_rtinfo, 0},
};

int __init mctp_routes_init(void)
{
	int err;

	dev_add_pack(&mctp_packet_type);

	err = register_pernet_subsys(&mctp_net_ops);
	if (err)
		goto err_pernet;

	err = rtnl_register_many(mctp_route_rtnl_msg_handlers);
	if (err)
		goto err_rtnl;

	return 0;

err_rtnl:
	unregister_pernet_subsys(&mctp_net_ops);
err_pernet:
	dev_remove_pack(&mctp_packet_type);
	return err;
}

void mctp_routes_exit(void)
{
	rtnl_unregister_many(mctp_route_rtnl_msg_handlers);
	unregister_pernet_subsys(&mctp_net_ops);
	dev_remove_pack(&mctp_packet_type);
}

#if IS_ENABLED(CONFIG_MCTP_TEST)
#include "test/route-test.c"
#endif
