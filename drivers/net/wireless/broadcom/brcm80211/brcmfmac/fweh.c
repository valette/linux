// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2012 Broadcom Corporation
 */
#include <linux/netdevice.h>

#include "brcmu_wifi.h"
#include "brcmu_utils.h"

#include "cfg80211.h"
#include "core.h"
#include "debug.h"
#include "tracepoint.h"
#include "fweh.h"
#include "fwil.h"
#include "proto.h"
#include "bus.h"
#include "fwvid.h"
/**
 * struct brcmf_fweh_queue_item - event item on event queue.
 *
 * @q: list element for queuing.
 * @code: event code.
 * @ifidx: interface index related to this event.
 * @ifaddr: ethernet address for interface.
 * @emsg: common parameters of the firmware event message.
 * @datalen: length of the data array
 * @data: event specific data part of the firmware event.
 */
struct brcmf_fweh_queue_item {
	struct list_head q;
	u32 code;
	u8 ifidx;
	u8 ifaddr[ETH_ALEN];
	struct brcmf_event_msg_be emsg;
	u32 datalen;
	u8 data[] __counted_by(datalen);
};

/*
 * struct brcmf_fweh_event_name - code, name mapping entry.
 */
struct brcmf_fweh_event_name {
	enum brcmf_fweh_event_code code;
	const char *name;
};

#ifdef DEBUG
#define BRCMF_ENUM_DEF(id, val) \
	{ val, #id },

/* array for mapping code to event name */
static struct brcmf_fweh_event_name fweh_event_names[] = {
	BRCMF_FWEH_EVENT_ENUM_DEFLIST
};
#undef BRCMF_ENUM_DEF

/**
 * brcmf_fweh_event_name() - returns name for given event code.
 *
 * @code: code to lookup.
 */
const char *brcmf_fweh_event_name(enum brcmf_fweh_event_code code)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(fweh_event_names); i++) {
		if (fweh_event_names[i].code == code)
			return fweh_event_names[i].name;
	}
	return "unknown";
}
#else
const char *brcmf_fweh_event_name(enum brcmf_fweh_event_code code)
{
	return "nodebug";
}
#endif
BRCMF_EXPORT_SYMBOL_GPL(brcmf_fweh_event_name);

/**
 * brcmf_fweh_queue_event() - create and queue event.
 *
 * @fweh: firmware event handling info.
 * @event: event queue entry.
 */
static void brcmf_fweh_queue_event(struct brcmf_fweh_info *fweh,
				   struct brcmf_fweh_queue_item *event)
{
	ulong flags;

	spin_lock_irqsave(&fweh->evt_q_lock, flags);
	list_add_tail(&event->q, &fweh->event_q);
	spin_unlock_irqrestore(&fweh->evt_q_lock, flags);
	schedule_work(&fweh->event_work);
}

static int brcmf_fweh_call_event_handler(struct brcmf_pub *drvr,
					 struct brcmf_if *ifp,
					 u32 fwcode,
					 struct brcmf_event_msg *emsg,
					 void *data)
{
	struct brcmf_fweh_info *fweh;
	int err = -EINVAL;

	if (ifp) {
		fweh = ifp->drvr->fweh;

		/* handle the event if valid interface and handler */
		if (fweh->evt_handler[fwcode])
			err = fweh->evt_handler[fwcode](ifp, emsg, data);
		else
			bphy_err(drvr, "unhandled fwevt %d ignored\n", fwcode);
	} else {
		bphy_err(drvr, "no interface object\n");
	}
	return err;
}

/**
 * brcmf_fweh_handle_if_event() - handle IF event.
 *
 * @drvr: driver information object.
 * @emsg: event message object.
 * @data: event object.
 */
static void brcmf_fweh_handle_if_event(struct brcmf_pub *drvr,
				       struct brcmf_event_msg *emsg,
				       void *data)
{
	struct brcmf_if_event *ifevent = data;
	struct brcmf_if *ifp;
	bool is_p2pdev;

	brcmf_dbg(EVENT, "action: %u ifidx: %u bsscfgidx: %u flags: %u role: %u\n",
		  ifevent->action, ifevent->ifidx, ifevent->bsscfgidx,
		  ifevent->flags, ifevent->role);

	/* The P2P Device interface event must not be ignored contrary to what
	 * firmware tells us. Older firmware uses p2p noif, with sta role.
	 * This should be accepted when p2pdev_setup is ongoing. TDLS setup will
	 * use the same ifevent and should be ignored.
	 */
	is_p2pdev = ((ifevent->flags & BRCMF_E_IF_FLAG_NOIF) &&
		     (ifevent->role == BRCMF_E_IF_ROLE_P2P_CLIENT ||
		      ((ifevent->role == BRCMF_E_IF_ROLE_STA) &&
		       (drvr->fweh->p2pdev_setup_ongoing))));
	if (!is_p2pdev && (ifevent->flags & BRCMF_E_IF_FLAG_NOIF)) {
		brcmf_dbg(EVENT, "event can be ignored\n");
		return;
	}
	if (ifevent->ifidx >= BRCMF_MAX_IFS) {
		bphy_err(drvr, "invalid interface index: %u\n", ifevent->ifidx);
		return;
	}

	ifp = drvr->iflist[ifevent->bsscfgidx];

	if (ifevent->action == BRCMF_E_IF_ADD) {
		brcmf_dbg(EVENT, "adding %s (%pM)\n", emsg->ifname,
			  emsg->addr);
		ifp = brcmf_add_if(drvr, ifevent->bsscfgidx, ifevent->ifidx,
				   is_p2pdev, emsg->ifname, emsg->addr);
		if (IS_ERR(ifp))
			return;
		if (!is_p2pdev)
			brcmf_proto_add_if(drvr, ifp);
		if (!drvr->fweh->evt_handler[BRCMF_E_IF])
			if (brcmf_net_attach(ifp, false) < 0)
				return;
	}

	if (ifp && ifevent->action == BRCMF_E_IF_CHANGE)
		brcmf_proto_reset_if(drvr, ifp);

	brcmf_fweh_call_event_handler(drvr, ifp, emsg->event_code, emsg,
				      data);

	if (ifp && ifevent->action == BRCMF_E_IF_DEL) {
		bool armed = brcmf_cfg80211_vif_event_armed(drvr->config);

		/* Default handling in case no-one waits for this event */
		if (!armed)
			brcmf_remove_interface(ifp, false);
	}
}

static void brcmf_fweh_map_event_code(struct brcmf_fweh_info *fweh,
				      enum brcmf_fweh_event_code code,
				      u32 *fw_code)
{
	int i;

	if (WARN_ON(!fw_code))
		return;

	*fw_code = code;
	if (fweh->event_map) {
		for (i = 0; i < fweh->event_map->n_items; i++) {
			if (fweh->event_map->items[i].code == code) {
				*fw_code = fweh->event_map->items[i].fwevt_code;
				break;
			}
		}
	}
}

static void brcmf_fweh_map_fwevt_code(struct brcmf_fweh_info *fweh, u32 fw_code,
				      enum brcmf_fweh_event_code *code)
{
	int i;

	if (WARN_ON(!code))
		return;

	*code = fw_code;
	if (fweh->event_map) {
		for (i = 0; i < fweh->event_map->n_items; i++) {
			if (fweh->event_map->items[i].fwevt_code == fw_code) {
				*code = fweh->event_map->items[i].code;
				break;
			}
		}
	}
}

/**
 * brcmf_fweh_dequeue_event() - get event from the queue.
 *
 * @fweh: firmware event handling info.
 */
static struct brcmf_fweh_queue_item *
brcmf_fweh_dequeue_event(struct brcmf_fweh_info *fweh)
{
	struct brcmf_fweh_queue_item *event = NULL;
	ulong flags;

	spin_lock_irqsave(&fweh->evt_q_lock, flags);
	if (!list_empty(&fweh->event_q)) {
		event = list_first_entry(&fweh->event_q,
					 struct brcmf_fweh_queue_item, q);
		list_del(&event->q);
	}
	spin_unlock_irqrestore(&fweh->evt_q_lock, flags);

	return event;
}

/**
 * brcmf_fweh_event_worker() - firmware event worker.
 *
 * @work: worker object.
 */
static void brcmf_fweh_event_worker(struct work_struct *work)
{
	struct brcmf_pub *drvr;
	struct brcmf_if *ifp;
	struct brcmf_fweh_info *fweh;
	struct brcmf_fweh_queue_item *event;
	int err = 0;
	struct brcmf_event_msg_be *emsg_be;
	struct brcmf_event_msg emsg;

	fweh = container_of(work, struct brcmf_fweh_info, event_work);
	drvr = fweh->drvr;

	while ((event = brcmf_fweh_dequeue_event(fweh))) {
		enum brcmf_fweh_event_code code;

		brcmf_fweh_map_fwevt_code(fweh, event->code, &code);
		brcmf_dbg(EVENT, "event %s (%u:%u) ifidx %u bsscfg %u addr %pM\n",
			  brcmf_fweh_event_name(code), code, event->code,
			  event->emsg.ifidx, event->emsg.bsscfgidx,
			  event->emsg.addr);
		if (event->emsg.bsscfgidx >= BRCMF_MAX_IFS) {
			bphy_err(drvr, "invalid bsscfg index: %u\n",
				 event->emsg.bsscfgidx);
			goto event_free;
		}

		/* convert event message */
		emsg_be = &event->emsg;
		emsg.version = be16_to_cpu(emsg_be->version);
		emsg.flags = be16_to_cpu(emsg_be->flags);
		emsg.event_code = code;
		emsg.status = be32_to_cpu(emsg_be->status);
		emsg.reason = be32_to_cpu(emsg_be->reason);
		emsg.auth_type = be32_to_cpu(emsg_be->auth_type);
		emsg.datalen = be32_to_cpu(emsg_be->datalen);
		memcpy(emsg.addr, emsg_be->addr, ETH_ALEN);
		memcpy(emsg.ifname, emsg_be->ifname, sizeof(emsg.ifname));
		emsg.ifidx = emsg_be->ifidx;
		emsg.bsscfgidx = emsg_be->bsscfgidx;

		brcmf_dbg(EVENT, "  version %u flags %u status %u reason %u\n",
			  emsg.version, emsg.flags, emsg.status, emsg.reason);
		brcmf_dbg_hex_dump(BRCMF_EVENT_ON(), event->data,
				   min_t(u32, emsg.datalen, 64),
				   "event payload, len=%d\n", emsg.datalen);

		/* special handling of interface event */
		if (event->code == BRCMF_E_IF) {
			brcmf_fweh_handle_if_event(drvr, &emsg, event->data);
			goto event_free;
		}

		if (event->code == BRCMF_E_TDLS_PEER_EVENT)
			ifp = drvr->iflist[0];
		else
			ifp = drvr->iflist[emsg.bsscfgidx];
		err = brcmf_fweh_call_event_handler(drvr, ifp, event->code,
						    &emsg, event->data);
		if (err) {
			bphy_err(drvr, "event handler failed (%d)\n",
				 event->code);
			err = 0;
		}
event_free:
		kfree(event);
	}
}

/**
 * brcmf_fweh_p2pdev_setup() - P2P device setup ongoing (or not).
 *
 * @ifp: ifp on which setup is taking place or finished.
 * @ongoing: p2p device setup in progress (or not).
 */
void brcmf_fweh_p2pdev_setup(struct brcmf_if *ifp, bool ongoing)
{
	ifp->drvr->fweh->p2pdev_setup_ongoing = ongoing;
}

/**
 * brcmf_fweh_attach() - initialize firmware event handling.
 *
 * @drvr: driver information object.
 */
int brcmf_fweh_attach(struct brcmf_pub *drvr)
{
	struct brcmf_fweh_info *fweh;
	int err;

	err = brcmf_fwvid_alloc_fweh_info(drvr);
	if (err < 0)
		return err;

	fweh = drvr->fweh;
	fweh->drvr = drvr;

	fweh->event_mask_len = DIV_ROUND_UP(fweh->num_event_codes, 8);
	fweh->event_mask = kzalloc(fweh->event_mask_len, GFP_KERNEL);
	if (!fweh->event_mask)
		return -ENOMEM;

	INIT_WORK(&fweh->event_work, brcmf_fweh_event_worker);
	spin_lock_init(&fweh->evt_q_lock);
	INIT_LIST_HEAD(&fweh->event_q);
	return 0;
}

/**
 * brcmf_fweh_detach() - cleanup firmware event handling.
 *
 * @drvr: driver information object.
 */
void brcmf_fweh_detach(struct brcmf_pub *drvr)
{
	struct brcmf_fweh_info *fweh = drvr->fweh;

	if (!fweh)
		return;

	/* cancel the worker if initialized */
	if (fweh->event_work.func) {
		cancel_work_sync(&fweh->event_work);
		WARN_ON(!list_empty(&fweh->event_q));
	}
	drvr->fweh = NULL;
	kfree(fweh->event_mask);
	kfree(fweh);
}

/**
 * brcmf_fweh_register() - register handler for given event code.
 *
 * @drvr: driver information object.
 * @code: event code.
 * @handler: handler for the given event code.
 */
int brcmf_fweh_register(struct brcmf_pub *drvr, enum brcmf_fweh_event_code code,
			brcmf_fweh_handler_t handler)
{
	struct brcmf_fweh_info *fweh = drvr->fweh;
	u32 evt_handler_idx;

	brcmf_fweh_map_event_code(fweh, code, &evt_handler_idx);

	if (fweh->evt_handler[evt_handler_idx]) {
		bphy_err(drvr, "event code %d already registered\n", code);
		return -ENOSPC;
	}

	fweh->evt_handler[evt_handler_idx] = handler;
	brcmf_dbg(TRACE, "event handler registered for %s\n",
		  brcmf_fweh_event_name(code));
	return 0;
}
BRCMF_EXPORT_SYMBOL_GPL(brcmf_fweh_register);

/**
 * brcmf_fweh_unregister() - remove handler for given code.
 *
 * @drvr: driver information object.
 * @code: event code.
 */
void brcmf_fweh_unregister(struct brcmf_pub *drvr,
			   enum brcmf_fweh_event_code code)
{
	u32 evt_handler_idx;

	brcmf_dbg(TRACE, "event handler cleared for %s\n",
		  brcmf_fweh_event_name(code));
	brcmf_fweh_map_event_code(drvr->fweh, code, &evt_handler_idx);
	drvr->fweh->evt_handler[evt_handler_idx] = NULL;
}

/**
 * brcmf_fweh_activate_events() - enables firmware events registered.
 *
 * @ifp: primary interface object.
 */
int brcmf_fweh_activate_events(struct brcmf_if *ifp)
{
	struct brcmf_fweh_info *fweh = ifp->drvr->fweh;
	enum brcmf_fweh_event_code code;
	int i, err;

	memset(fweh->event_mask, 0, fweh->event_mask_len);
	for (i = 0; i < fweh->num_event_codes; i++) {
		if (fweh->evt_handler[i]) {
			brcmf_fweh_map_fwevt_code(fweh, i, &code);
			brcmf_dbg(EVENT, "enable event %s\n",
				  brcmf_fweh_event_name(code));
			setbit(fweh->event_mask, i);
		}
	}

	/* want to handle IF event as well */
	brcmf_dbg(EVENT, "enable event IF\n");
	setbit(fweh->event_mask, BRCMF_E_IF);

	/* allow per-vendor method to activate firmware events */
	if (!brcmf_fwvid_activate_events(ifp))
		return 0;

	err = brcmf_fil_iovar_data_set(ifp, "event_msgs", fweh->event_mask,
				       fweh->event_mask_len);
	if (err)
		bphy_err(fweh->drvr, "Set event_msgs error (%d)\n", err);
	return err;
}

/**
 * brcmf_fweh_process_event() - process skb as firmware event.
 *
 * @drvr: driver information object.
 * @event_packet: event packet to process.
 * @packet_len: length of the packet
 * @gfp: memory allocation flags.
 *
 * If the packet buffer contains a firmware event message it will
 * dispatch the event to a registered handler (using worker).
 */
void brcmf_fweh_process_event(struct brcmf_pub *drvr,
			      struct brcmf_event *event_packet,
			      u32 packet_len, gfp_t gfp)
{
	u32 fwevt_idx;
	struct brcmf_fweh_info *fweh = drvr->fweh;
	struct brcmf_fweh_queue_item *event;
	void *data;
	u32 datalen;

	/* get event info */
	fwevt_idx = get_unaligned_be32(&event_packet->msg.event_type);
	datalen = get_unaligned_be32(&event_packet->msg.datalen);
	data = &event_packet[1];

	if (fwevt_idx >= fweh->num_event_codes)
		return;

	if (fwevt_idx != BRCMF_E_IF && !fweh->evt_handler[fwevt_idx])
		return;

	if (datalen > BRCMF_DCMD_MAXLEN ||
	    datalen + sizeof(*event_packet) > packet_len)
		return;

	event = kzalloc(struct_size(event, data, datalen), gfp);
	if (!event)
		return;

	event->code = fwevt_idx;
	event->datalen = datalen;
	event->ifidx = event_packet->msg.ifidx;

	/* use memcpy to get aligned event message */
	memcpy(&event->emsg, &event_packet->msg, sizeof(event->emsg));
	memcpy(event->data, data, datalen);
	memcpy(event->ifaddr, event_packet->eth.h_dest, ETH_ALEN);

	brcmf_fweh_queue_event(fweh, event);
}
