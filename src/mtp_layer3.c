/* MTP layer3 main handling code */
/*
 * (C) 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2010 by On-Waves
 * All Rights Reserved
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <mtp_data.h>
#include <mtp_level3.h>
#include <bsc_data.h>
#include <cellmgr_debug.h>
#include <isup_types.h>
#include <counter.h>

#include <osmocore/talloc.h>

#include <osmocom/sccp/sccp.h>

#include <arpa/inet.h>

#include <string.h>

static void *tall_mtp_ctx = NULL;

static int mtp_int_submit(struct mtp_link_set *link, int pc, int sls, int type, const uint8_t *data, unsigned int length);

void mtp_link_submit(struct mtp_link *link, struct msgb *msg)
{
	rate_ctr_inc(&link->ctrg->ctr[MTP_LNK_OUT]);
	rate_ctr_inc(&link->set->ctrg->ctr[MTP_LSET_TOTA_OUT_MSG]);
	link->write(link, msg);
}


struct msgb *mtp_msg_alloc(struct mtp_link_set *link)
{
	struct mtp_level_3_hdr *hdr;
	struct msgb *msg = msgb_alloc_headroom(4096, 128, "mtp-msg");
	if (!msg) {
		LOGP(DINP, LOGL_ERROR, "Failed to allocate mtp msg\n");
		return NULL;
	}

	msg->l2h = msgb_put(msg, sizeof(*hdr));
	hdr = (struct mtp_level_3_hdr *) msg->l2h;
	hdr->addr = MTP_ADDR(0x0, link->dpc, link->opc);
	hdr->ni = link->ni;
	hdr->spare = link->spare;
	return msg;
}

static struct msgb *mtp_create_slta(struct mtp_link_set *link, int sls,
				    struct mtp_level_3_mng *in_mng, int l3_len)
{
	struct mtp_level_3_hdr *hdr;
	struct mtp_level_3_mng *mng;
	struct msgb *out = mtp_msg_alloc(link);

	if (!out)
		return NULL;

	hdr = (struct mtp_level_3_hdr *) out->l2h;
	hdr->ser_ind = MTP_SI_MNT_REG_MSG;
	hdr->addr = MTP_ADDR(sls, link->dpc, link->opc);

	mng = (struct mtp_level_3_mng *) msgb_put(out, sizeof(*mng));
	mng->cmn.h0 = MTP_TST_MSG_GRP;
	mng->cmn.h1 = MTP_TST_MSG_SLTA;
	mng->length =  l3_len - 2;
	msgb_put(out, mng->length);
	memcpy(mng->data, in_mng->data, mng->length);

	return out;
}


static struct msgb *mtp_base_alloc(struct mtp_link_set *link, int msg, int apoc)
{
	struct mtp_level_3_hdr *hdr;
	struct mtp_level_3_prohib *prb;
	struct msgb *out = mtp_msg_alloc(link);

	if (!out)
		return NULL;

	hdr = (struct mtp_level_3_hdr *) out->l2h;
	hdr->ser_ind = MTP_SI_MNT_SNM_MSG;
	prb = (struct mtp_level_3_prohib *) msgb_put(out, sizeof(*prb));
	prb->cmn.h0 = MTP_PROHIBIT_MSG_GRP;
	prb->cmn.h1 = msg;
	prb->apoc = MTP_MAKE_APOC(apoc);
	return out;
}

static struct msgb *mtp_tfp_alloc(struct mtp_link_set *link, int apoc)
{
	return mtp_base_alloc(link, MTP_PROHIBIT_MSG_SIG, apoc);
}

static struct msgb *mtp_tfa_alloc(struct mtp_link_set *link, int apoc)
{
	return mtp_base_alloc(link, MTP_PROHIBIT_MSG_TFA, apoc);
}

static struct msgb *mtp_tra_alloc(struct mtp_link_set *link, int opc)
{
	struct mtp_level_3_hdr *hdr;
	struct mtp_level_3_cmn *cmn;
	struct msgb *out = mtp_msg_alloc(link);

	if (!out)
		return NULL;

	hdr = (struct mtp_level_3_hdr *) out->l2h;
	hdr->ser_ind = MTP_SI_MNT_SNM_MSG;
	hdr->addr = MTP_ADDR(0x0, link->dpc, opc);
	cmn = (struct mtp_level_3_cmn *) msgb_put(out, sizeof(*cmn));
	cmn->h0 = MTP_TRF_RESTR_MSG_GRP;
	cmn->h1 = MTP_RESTR_MSG_ALLWED;
	return out;
}

static struct msgb *mtp_sccp_alloc_scmg(struct mtp_link_set *link,
					int type, int assn, int apoc, int sls)
{
	struct sccp_data_unitdata *udt;
	struct sccp_con_ctrl_prt_mgt *prt;
	struct mtp_level_3_hdr *hdr;
	uint8_t *data;


	struct msgb *out = mtp_msg_alloc(link);

	if (!out)
		return NULL;

	hdr = (struct mtp_level_3_hdr *) out->l2h;
	hdr->ser_ind = MTP_SI_MNT_SCCP;

	/* this appears to be round robin or such.. */
	hdr->addr = MTP_ADDR(sls % 16, link->dpc, link->sccp_opc);

	/* generate the UDT message... libsccp does not offer formating yet */
	udt = (struct sccp_data_unitdata *) msgb_put(out, sizeof(*udt));
	udt->type = SCCP_MSG_TYPE_UDT;
	udt->proto_class = SCCP_PROTOCOL_CLASS_0;
	udt->variable_called = 3;
	udt->variable_calling = 5;
	udt->variable_data = 7;

	/* put the called and calling address. It is LV */
	data = msgb_put(out, 2 + 1);
	data[0] = 2;
	data[1] = 0x42;
	data[2] = 0x1;

	data = msgb_put(out, 2 + 1);
	data[0] = 2;
	data[1] = 0x42;
	data[2] = 0x1;

	data = msgb_put(out, 1);
	data[0] = sizeof(*prt);

	prt = (struct sccp_con_ctrl_prt_mgt *) msgb_put(out, sizeof(*prt));
	prt->sst = type;
	prt->assn = assn;
	prt->apoc = apoc;
	prt->mul_ind = 0;

	return out;
}

void mtp_link_set_init(void)
{
	tall_mtp_ctx = talloc_named_const(NULL, 1, "mtp-link");
}

struct mtp_link_set *mtp_link_set_alloc(void)
{
	static int linkset_no = 0;
	struct mtp_link_set *link;

	link = talloc_zero(tall_mtp_ctx, struct mtp_link_set);
	if (!link)
		return NULL;

	link->ctrg = rate_ctr_group_alloc(link,
					  mtp_link_set_rate_ctr_desc(),
					  linkset_no++);
	if (!link->ctrg) {
		LOGP(DINP, LOGL_ERROR, "Failed to allocate counter.\n");
		return NULL;
	}


	link->ni = MTP_NI_NATION_NET;
	INIT_LLIST_HEAD(&link->links);

	return link;
}

void mtp_link_set_stop(struct mtp_link_set *link)
{
	struct mtp_link *lnk;
	llist_for_each_entry(lnk, &link->links, entry)
		mtp_link_stop_link_test(lnk);

	link->sccp_up = 0;
	link->running = 0;
	link->linkset_up = 0;
}

void mtp_link_set_reset(struct mtp_link_set *link)
{
	struct mtp_link *lnk;
	mtp_link_set_stop(link);
	link->running = 1;

	llist_for_each_entry(lnk, &link->links, entry)
		mtp_link_start_link_test(lnk);
}

static int send_tfp(struct mtp_link_set *link, int apoc)
{
	struct msgb *msg;
	msg = mtp_tfp_alloc(link, apoc);
	if (!msg)
		return -1;

	mtp_link_submit(link->slc[0], msg);
	return 0;
}

static int send_tra(struct mtp_link_set *link, int opc)
{
	struct msgb *msg;
	msg = mtp_tra_alloc(link, opc);
	if (!msg)
		return -1;
	mtp_link_submit(link->slc[0], msg);
	return 0;
}

static int send_tfa(struct mtp_link_set *link, int opc)
{
	struct msgb *msg;
	msg = mtp_tfa_alloc(link, opc);
	if (!msg)
		return -1;
	mtp_link_submit(link->slc[0], msg);
	return 0;
}

static int linkset_up(struct mtp_link_set *set)
{
	/* the link set is already up */
	if (set->linkset_up)
		return 0;

	if (send_tfp(set, 0) != 0)
		return -1;
	if (send_tfp(set, set->opc) != 0)
		return -1;
	if (set->sccp_opc != set->opc &&
	    send_tfp(set, set->sccp_opc) != 0)
		return -1;
	if (set->isup_opc != set->opc &&
	    send_tfp(set, set->isup_opc) != 0)
		return -1;

	/* Send the TRA for all PCs */
	if (send_tra(set, set->opc) != 0)
		return -1;
	if (set->sccp_opc != set->opc &&
	    send_tfa(set, set->sccp_opc) != 0)
		return -1;
	if (set->isup_opc != set->opc &&
	    send_tfa(set, set->isup_opc) != 0)
		return -1;

	set->linkset_up = 1;
	LOGP(DINP, LOGL_NOTICE,
	     "The linkset %p is considered running.\n", set);
	return 0;
}

static int mtp_link_sign_msg(struct mtp_link_set *link, struct mtp_level_3_hdr *hdr, int l3_len)
{
	struct mtp_level_3_cmn *cmn;
	uint16_t *apc;

	if (hdr->ni != link->ni || l3_len < 1) {
		LOGP(DINP, LOGL_ERROR, "Unhandled data (ni: %d len: %d)\n",
		     hdr->ni, l3_len);
		return -1;
	}

	cmn = (struct mtp_level_3_cmn *) &hdr->data[0];
	LOGP(DINP, LOGL_DEBUG, "reg msg: h0: 0x%x h1: 0x%x\n",
             cmn->h0, cmn->h1);

	switch (cmn->h0) {
	case MTP_TRF_RESTR_MSG_GRP:
		switch (cmn->h1) {
		case MTP_RESTR_MSG_ALLWED:
			LOGP(DINP, LOGL_INFO, "Received Restart Allowed. SST could be next: %p\n", link);
			link->sccp_up = 1;
			LOGP(DINP, LOGL_INFO, "SCCP traffic allowed. %p\n", link);
			return 0;
			break;
		}
		break;
	case MTP_PROHIBIT_MSG_GRP:
		switch (cmn->h1) {
		case MTP_PROHIBIT_MSG_SIG:
			if (l3_len < 3) {
				LOGP(DINP, LOGL_ERROR, "TFP is too short.\n");
				return -1;
			}

			apc = (uint16_t *) &hdr->data[1];
			LOGP(DINP, LOGL_INFO,
			     "TFP for the affected point code: %d\n", *apc);
			return 0;
			break;
		}
		break;
	}

	LOGP(DINP, LOGL_ERROR, "Unknown message:%d/%d %s\n", cmn->h0, cmn->h1, hexdump(&hdr->data[0], l3_len));
	return -1;
}

static int mtp_link_regular_msg(struct mtp_link *link, struct mtp_level_3_hdr *hdr, int l3_len)
{
	struct msgb *out;
	struct mtp_level_3_mng *mng;

	if (hdr->ni != link->set->ni || l3_len < 1) {
		LOGP(DINP, LOGL_ERROR, "Unhandled data (ni: %d len: %d)\n",
		     hdr->ni, l3_len);
		return -1;
	}

	if (MTP_ADDR_DPC(hdr->addr) != link->set->opc) {
		LOGP(DINP, LOGL_ERROR, "MSG for 0x%x not handled by 0x%x\n",
			MTP_ADDR_DPC(hdr->addr), link->set->opc);
		return -1;
	}

	mng = (struct mtp_level_3_mng *) &hdr->data[0];
	LOGP(DINP, LOGL_DEBUG, "reg msg: h0: 0x%x h1: 0x%x\n",
             mng->cmn.h0, mng->cmn.h1);

	switch (mng->cmn.h0) {
	case MTP_TST_MSG_GRP:
		switch (mng->cmn.h1) {
		case MTP_TST_MSG_SLTM:
			/* simply respond to the request... */
			out = mtp_create_slta(link->set,
					      MTP_LINK_SLS(hdr->addr),
					      mng, l3_len);
			if (!out)
				return -1;
			mtp_link_submit(link, out);
			return 0;
			break;
		case MTP_TST_MSG_SLTA:
			/* If this link is proven set it up */
			if (mtp_link_slta(link, l3_len, mng) == 0)
				linkset_up(link->set);
			break;
		}
		break;
	}

	return -1;
}

static int mtp_link_sccp_data(struct mtp_link_set *link, struct mtp_level_3_hdr *hdr, struct msgb *msg, int l3_len)
{
	struct msgb *out;
	struct sccp_con_ctrl_prt_mgt *prt;
	struct sccp_parse_result sccp;
	int type;

	msg->l2h = &hdr->data[0];
	if (msgb_l2len(msg) != l3_len) {
		LOGP(DINP, LOGL_ERROR, "Size is wrong after playing with the l2h header.\n");
		return -1;
	}

	if (!link->sccp_up) {
		LOGP(DINP, LOGL_ERROR, "SCCP traffic is not allowed.\n");
		return -1;
	}

	memset(&sccp, 0, sizeof(sccp));
	if (sccp_parse_header(msg, &sccp) != 0) {
		LOGP(DINP, LOGL_ERROR, "Failed to parsed SCCP header.\n");
		return -1;
	}

	/* check if it is a SST */
	if (sccp_determine_msg_type(msg) == SCCP_MSG_TYPE_UDT
	    && msg->l3h[0] == SCCP_SST) {
		if (msgb_l3len(msg) != 5) {
			LOGP(DINP, LOGL_ERROR,
			     "SCCP UDT msg of unexpected size: %u\n",
			     msgb_l3len(msg));
			return -1;
		}

		prt = (struct sccp_con_ctrl_prt_mgt *) &msg->l3h[0];
		if (prt->apoc != MTP_MAKE_APOC(link->sccp_opc)) {
			LOGP(DINP, LOGL_ERROR, "Unknown APOC: %u/%u\n",
			     ntohs(prt->apoc), prt->apoc);
			type = SCCP_SSP;
		} else if (prt->assn != 1 && prt->assn != 254 &&
			   prt->assn != 7 && prt->assn != 8 && prt->assn != 146) {
			LOGP(DINP, LOGL_ERROR, "Unknown affected SSN assn: %u\n",
			     prt->assn);
			type = SCCP_SSP;
		} else {
			type = SCCP_SSA;
		}

		out = mtp_sccp_alloc_scmg(link, type, prt->assn, prt->apoc,
					  MTP_LINK_SLS(hdr->addr));
		if (!out)
			return -1;

		mtp_link_submit(link->slc[MTP_LINK_SLS(hdr->addr)], out);
		return 0;
	}

	rate_ctr_inc(&link->ctrg->ctr[MTP_LSET_SCCP_IN_MSG]);
	mtp_link_set_forward_sccp(link, msg, MTP_LINK_SLS(hdr->addr));
	return 0;
}

int mtp_link_set_data(struct mtp_link *link, struct msgb *msg)
{
	int rc = -1;
	struct mtp_level_3_hdr *hdr;
	int l3_len;

	if (!msg->l2h || msgb_l2len(msg) < sizeof(*hdr))
		return -1;

	if (!link->set->running) {
		LOGP(DINP, LOGL_ERROR, "Link is not running. Call mtp_link_reset first: %p\n", link);
		return -1;
	}

	hdr = (struct mtp_level_3_hdr *) msg->l2h;
	l3_len = msgb_l2len(msg) - sizeof(*hdr);

	rate_ctr_inc(&link->ctrg->ctr[MTP_LNK_IN]);
	rate_ctr_inc(&link->set->ctrg->ctr[MTP_LSET_TOTA_IN_MSG]);

	switch (hdr->ser_ind) {
	case MTP_SI_MNT_SNM_MSG:
		rc = mtp_link_sign_msg(link->set, hdr, l3_len);
		break;
	case MTP_SI_MNT_REG_MSG:
		rc = mtp_link_regular_msg(link, hdr, l3_len);
		break;
	case MTP_SI_MNT_SCCP:
		rc = mtp_link_sccp_data(link->set, hdr, msg, l3_len);
		break;
	case MTP_SI_MNT_ISUP:
		msg->l3h = &hdr->data[0];
		rate_ctr_inc(&link->set->ctrg->ctr[MTP_LSET_IUSP_IN_MSG]);
		rc = mtp_link_set_isup(link->set, msg, MTP_LINK_SLS(hdr->addr));
		break;
	default:
		fprintf(stderr, "Unhandled: %u\n", hdr->ser_ind);
		break;
	}

	return rc;
}

int mtp_link_set_submit_sccp_data(struct mtp_link_set *link, int sls, const uint8_t *data, unsigned int length)
{

	if (!link->sccp_up) {
		LOGP(DINP, LOGL_ERROR, "SCCP msg after TRA and before SSA. Dropping it.\n");
		return -1;
	}

	if (sls == -1) {
		sls = link->last_sls;
		link->last_sls = (link->last_sls + 1) % 16;
	}

	rate_ctr_inc(&link->ctrg->ctr[MTP_LSET_SCCP_OUT_MSG]);
	return mtp_int_submit(link, link->sccp_opc, sls, MTP_SI_MNT_SCCP, data, length);
}

int mtp_link_set_submit_isup_data(struct mtp_link_set *link, int sls,
			      const uint8_t *data, unsigned int length)
{
	rate_ctr_inc(&link->ctrg->ctr[MTP_LSET_ISUP_OUT_MSG]);
	return mtp_int_submit(link, link->isup_opc, sls, MTP_SI_MNT_ISUP, data, length);
}

static int mtp_int_submit(struct mtp_link_set *link, int pc, int sls, int type,
			  const uint8_t *data, unsigned int length)
{
	uint8_t *put_ptr;
	struct mtp_level_3_hdr *hdr;
	struct msgb *msg;

	if (!link->slc[sls % 16])
		return -1;

	msg = mtp_msg_alloc(link);
	if (!msg)
		return -1;

	hdr = (struct mtp_level_3_hdr *) msg->l2h;
	hdr->ser_ind = type;

	hdr->addr = MTP_ADDR(sls % 16, link->dpc, pc);

	/* copy the raw sccp data */
	put_ptr = msgb_put(msg, length);
	memcpy(put_ptr, data, length);

	mtp_link_submit(link->slc[sls % 16], msg);
	return 0;
}

static struct mtp_link *find_next_link(struct mtp_link_set *set,
					struct mtp_link *data)
{
	int found = 0;
	struct mtp_link *next;

	if (llist_empty(&set->links))
		return NULL;

	if (data == NULL)
		found = 1;

	/* try to find the next one */
	llist_for_each_entry(next, &set->links, entry) {
		if (found && next->available)
			return next;
		if (next == data)
			found = 1;
	}

	/* try to find any one */
	llist_for_each_entry(next, &set->links, entry)
		if (next->available)
			return next;

	return NULL;
}

void mtp_link_set_init_slc(struct mtp_link_set *set)
{
	struct mtp_link *link = NULL;
	int i;


	for (i = 0; i < ARRAY_SIZE(set->slc); ++i) {
		link = find_next_link(set, link);
		set->slc[i] = link;
	}
}

int mtp_link_set_add_link(struct mtp_link_set *set, struct mtp_link *lnk)
{
	lnk->set = set;
	lnk->link_no = set->nr_links++;
	if (mtp_link_init(lnk) != 0)
		return -1;

	llist_add_tail(&lnk->entry, &set->links);
	mtp_link_set_init_slc(set);
	return 0;
}
