/* Layer 1 - FCCH and SCH burst handling */

/* (C) 2010 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <defines.h>
#include <debug.h>
#include <memory.h>
#include <byteorder.h>
#include <rffe.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/core/msgb.h>
#include <calypso/dsp_api.h>
#include <calypso/irq.h>
#include <calypso/tpu.h>
#include <calypso/tsp.h>
#include <calypso/dsp.h>
#include <calypso/timer.h>
#include <comm/sercomm.h>

#include <layer1/sync.h>
#include <layer1/afc.h>
#include <layer1/toa.h>
#include <layer1/tdma_sched.h>
#include <layer1/mframe_sched.h>
#include <layer1/tpu_window.h>
#include <layer1/l23_api.h>
#include <layer1/agc.h>

#include <l1ctl_proto.h>

#define FB0_RETRY_COUNT		3
#define AFC_RETRY_COUNT		30

extern uint16_t rf_arfcn; // TODO

struct mon_state {
	uint32_t fnr_report;	/* frame number when DSP reported it */
	int attempt;		/* which attempt was this ? */

	int16_t toa;
	uint16_t pm;
	uint16_t angle;
	uint16_t snr;

	/* computed values */
	int16_t freq_diff;

	/* Sync Burst (SB) */
	uint8_t bsic;
	struct gsm_time time;
};

struct l1a_fb_state {
	struct mon_state mon;
	struct l1ctl_fbsb_req req;
	int16_t initial_freq_err;
	uint8_t fb_retries;
	uint8_t afc_retries;
};

static struct l1a_fb_state fbs;
static struct mon_state *last_fb = &fbs.mon;
static int sb_det = 0;  //MTZ - This was added
static int fb_det = 0;  //MTZ - This was added
uint32_t old_tpu_offset = 0;  //MTZ - This was added
int total_sb_det = 0;

int16_t nb_fb_toa = 0;  //MTZ - This was added
uint16_t nb_fb_pm = 0;  //MTZ - This was added
uint16_t nb_fb_angle0 = 0;  //MTZ - This was added
uint16_t nb_fb_angle1 = 0;  //MTZ - This was added
uint16_t nb_fb_snr = 0;  //MTZ - This was added

static void dump_mon_state(struct mon_state *fb)
{
#if 0
	printf("(%"PRIu32":%u): TOA=%5u, Power=%4ddBm, Angle=%5dHz, "
		"SNR=%04x(%d.%u) OFFSET=%u SYNCHRO=%u\n",
		fb->fnr_report, fb->attempt, fb->toa,
		agc_inp_dbm8_by_pm(fb->pm)/8, ANGLE_TO_FREQ(fb->angle),
		fb->snr, l1s_snr_int(fb->snr), l1s_snr_fract(fb->snr),
		tpu_get_offset(), tpu_get_synchro());
#else
	printf("(%"PRIu32":%u): TOA=%5u, Power=%4ddBm, Angle=%5dHz\n",
		fb->fnr_report, fb->attempt, fb->toa,
		agc_inp_dbm8_by_pm(fb->pm)/8, ANGLE_TO_FREQ(fb->angle));
#endif
}

static int l1ctl_fbsb_resp(uint8_t res)
{
	struct msgb *msg;
	struct l1ctl_fbsb_conf *resp;

	msg = l1_create_l2_msg(L1CTL_FBSB_CONF, fbs.mon.time.fn,
				l1s_snr_int(fbs.mon.snr),
				fbs.req.band_arfcn);
	if (!msg)
		return -ENOMEM;

	resp = (struct l1ctl_fbsb_conf *) msgb_put(msg, sizeof(*resp));
	resp->initial_freq_err = htons(fbs.initial_freq_err);
	resp->result = res;
	resp->bsic = fbs.mon.bsic;

	/* no need to set BSIC, as it is never used here */
	l1_queue_for_l2(msg);

	return 0;
}

/* SCH Burst Detection ********************************************************/

/* determine the GSM time and BSIC from a Sync Burst */
static uint8_t l1s_decode_sb(struct gsm_time *time, uint32_t sb)
{
	uint8_t bsic = (sb >> 2) & 0x3f;
	uint8_t t3p;

	memset(time, 0, sizeof(*time));

	/* TS 05.02 Chapter 3.3.2.2.1 SCH Frame Numbers */
	time->t1 = ((sb >> 23) & 1) | ((sb >> 7) & 0x1fe) | ((sb << 9) & 0x600);
	time->t2 = (sb >> 18) & 0x1f;
	t3p = ((sb >> 24) & 1) | ((sb >> 15) & 6);
	time->t3 = t3p*10 + 1;

	/* TS 05.02 Chapter 4.3.3 TDMA frame number */
	time->fn = gsm_gsmtime2fn(time);
	printf("\n\nMTZ: time->fn = %d\n\n", time->fn);

	time->tc = (time->fn / 51) % 8;

	return bsic;
}

static void read_sb_result(struct mon_state *st, int attempt)
{
	st->toa = dsp_api.db_r->a_serv_demod[D_TOA];
	st->pm = dsp_api.db_r->a_serv_demod[D_PM]>>3;
	st->angle = dsp_api.db_r->a_serv_demod[D_ANGLE];
	st->snr = dsp_api.db_r->a_serv_demod[D_SNR];

	st->freq_diff = ANGLE_TO_FREQ(st->angle);
	st->fnr_report = l1s.current_time.fn;
	st->attempt = attempt;

	dump_mon_state(st);

	if (st->snr > AFC_SNR_THRESHOLD)
		afc_input(st->freq_diff, rf_arfcn, 1);
	else
		afc_input(st->freq_diff, rf_arfcn, 0);

	dsp_api.r_page_used = 1;
}

static void read_sb_result2(struct mon_state *st, int attempt)
{
	st->toa = dsp_api.db_r->a_serv_demod[D_TOA];
	st->pm = dsp_api.db_r->a_serv_demod[D_PM]>>3;
	st->angle = dsp_api.db_r->a_serv_demod[D_ANGLE];
	st->snr = dsp_api.db_r->a_serv_demod[D_SNR];

	st->freq_diff = ANGLE_TO_FREQ(st->angle);
	st->fnr_report = l1s.current_time.fn;
	st->attempt = attempt;

	dump_mon_state(st);

	printf("\n\n\nMTZ: st->freq_diff = %d\n\n\n", st->freq_diff);

	//MTZ - commenting out for now
	if (st->snr > AFC_SNR_THRESHOLD)
		afc_input(st->freq_diff, rf_arfcn, 1);
	else
		afc_input(st->freq_diff, rf_arfcn, 0);

	dsp_api.r_page_used = 1;
}

/* Note: When we get the SB response, it is 2 TDMA frames after the SB
 * actually happened, as it is a "C W W R" task */
#define SB2_LATENCY	2

static int l1s_sbdet_resp(__unused uint8_t p1, uint8_t attempt,
			  __unused uint16_t p3)
{
	uint32_t sb;
	int qbits, fn_offset;
	struct l1_cell_info *cinfo = &l1s.serving_cell;
	int fnr_delta, bits_delta;

	putchart('s');

	if (dsp_api.db_r->a_sch[0] & (1<<B_SCH_CRC)) {
		/* mark READ page as being used */
		dsp_api.r_page_used = 1;

		/* after 2nd attempt, we failed */
		if (attempt == 2) {
			last_fb->attempt = 13;
			l1s_compl_sched(L1_COMPL_FB);
		}

		/* after 1st attempt, we simply wait for 2nd */
		return 0;
	}

	printf("SB%d ", attempt);
	read_sb_result(last_fb, attempt);

	sb = dsp_api.db_r->a_sch[3] | dsp_api.db_r->a_sch[4] << 16;
	fbs.mon.bsic = l1s_decode_sb(&fbs.mon.time, sb);
	printf("=> SB 0x%08"PRIx32": BSIC=%u ", sb, fbs.mon.bsic);
	l1s_time_dump(&fbs.mon.time);

	l1s.serving_cell.bsic = fbs.mon.bsic;

	/* calculate synchronisation value (TODO: only complete for qbits) */
	last_fb->toa -= 23;
	qbits = last_fb->toa * 4;
	fn_offset = l1s.current_time.fn; // TODO

	if (qbits > QBITS_PER_TDMA) {
		qbits -= QBITS_PER_TDMA;
		fn_offset -= 1;
	} else if (qbits < 0)  {
		qbits += QBITS_PER_TDMA;
		fn_offset += 1;
	}

	fnr_delta = last_fb->fnr_report - attempt;
	bits_delta = fnr_delta * BITS_PER_TDMA;

	cinfo->fn_offset = fnr_delta;
	cinfo->time_alignment = qbits;
	cinfo->arfcn = rf_arfcn;

	if (last_fb->toa > bits_delta)
		printf("=> DSP reports SB in bit that is %d bits in the "
			"future?!?\n", last_fb->toa - bits_delta);
	else
		printf(" qbits=%u\n", qbits);

	synchronize_tdma(&l1s.serving_cell);

	/* if we have recived a SYNC burst, update our local GSM time */
	printf("\n\nMTZ: current_fn = %d, fn from SB = %d\n\n", gsm_gsmtime2fn(&l1s.current_time), fbs.mon.time.fn + SB2_LATENCY);
	gsm_fn2gsmtime(&l1s.current_time, fbs.mon.time.fn + SB2_LATENCY);
	/* compute next time from new current time */
	l1s.next_time = l1s.current_time;
	l1s_time_inc(&l1s.next_time, 1);

	/* If we call tdma_sched_reset(), which is only needed if there
	 * are further l1s_sbdet_resp() scheduled, we will bring
	 * dsp_api.db_r and dsp_api.db_w out of sync because we changed
	 * dsp_api.db_w for l1s_sbdet_cmd() and canceled
	 * l1s_sbdet_resp() which would change dsp_api.db_r. The DSP
	 * however expects dsp_api.db_w and dsp_api.db_r to be in sync
	 * (either "0 - 0" or "1 - 1"). So we have to bring dsp_api.db_w
	 * and dsp_api.db_r into sync again, otherwise NB reading will
	 * complain. We probably don't need the Abort command and could
	 * just bring dsp_api.db_w and dsp_api.db_r into sync.  */
	if (attempt != 2) {
		tdma_sched_reset();
		l1s_dsp_abort();
	}

	l1s_reset_hw();
	/* enable the MF Task for BCCH reading */
	mframe_enable(MF_TASK_BCCH_NORM);
	printf("\nMTZ: l1s.serving_cell.ccch_mode = %d\n", l1s.serving_cell.ccch_mode);
	if (l1s.serving_cell.ccch_mode == CCCH_MODE_COMBINED)
		mframe_enable(MF_TASK_CCCH_COMB);
	else if (l1s.serving_cell.ccch_mode == CCCH_MODE_NON_COMBINED)
		mframe_enable(MF_TASK_CCCH);
	else if (l1s.serving_cell.ccch_mode == CCCH_MODE_COMBINED_CBCH) {
		mframe_enable(MF_TASK_CCCH_COMB);
		mframe_enable(MF_TASK_SDCCH4_CBCH);
	}

	l1s_compl_sched(L1_COMPL_FB);

	//MTZ - delete this
	//mframe_enable(MF_TASK_TEST1);

	return 0;
}

static int l1s_sbdet_cmd(__unused uint8_t p1, __unused uint8_t p2,
			 __unused uint16_t p3)
{
	putchart('S');

	fbs.mon.bsic = 0;
	fbs.mon.time.fn = 0;

	dsp_api.db_w->d_task_md = SB_DSP_TASK;
	dsp_api.ndb->d_fb_mode = 0; /* wideband search */

	/* Program TPU */
	printf("\nMTZ: arfcn in l1s_sbdet_cmd = %d\n", rf_arfcn);
	l1s_rx_win_ctrl(rf_arfcn, L1_RXWIN_SB, 0);

	return 0;
}

static const struct tdma_sched_item sb_sched_set[];

/* This is how it is done by the TSM30 */
static const struct tdma_sched_item sb_sched_set[] = {
	SCHED_ITEM_DT(l1s_sbdet_cmd, 0, 0, 1),	SCHED_END_FRAME(),
	SCHED_ITEM_DT(l1s_sbdet_cmd, 0, 0, 2),	SCHED_END_FRAME(),
						SCHED_END_FRAME(),
	SCHED_ITEM(l1s_sbdet_resp, -4, 0, 1),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_sbdet_resp, -4, 0, 2),	SCHED_END_FRAME(),
	SCHED_END_SET()
};

void l1s_sb_test(uint8_t base_fn)
{
	tdma_schedule_set(base_fn, sb_sched_set, 0);
}
/* FCCH Burst *****************************************************************/

static int read_fb_result(struct mon_state *st, int attempt)
{
	st->toa = dsp_api.ndb->a_sync_demod[D_TOA];
	st->pm = dsp_api.ndb->a_sync_demod[D_PM]>>3;
	st->angle = dsp_api.ndb->a_sync_demod[D_ANGLE];
	st->snr = dsp_api.ndb->a_sync_demod[D_SNR];

	//last_fb->angle = clip_int16(last_fb->angle, AFC_MAX_ANGLE);
	st->freq_diff = ANGLE_TO_FREQ(last_fb->angle);
	st->fnr_report = l1s.current_time.fn;
	st->attempt = attempt;

	dump_mon_state(st);

	dsp_api.ndb->d_fb_det = 0;
	dsp_api.ndb->a_sync_demod[D_TOA] = 0; /* TSM30 does it (really needed ?) */

	/* Update AFC with current frequency offset */
	afc_correct(st->freq_diff, rf_arfcn);

	//tpu_dsp_frameirq_enable();
	return 1;
}

static void fbinfo2cellinfo(struct l1_cell_info *cinfo,
			    const struct mon_state *mon)
{
	int ntdma, qbits, fn_offset, fnr_delta, bits_delta;

	/* FIXME: where did this magic 23 come from? */
	last_fb->toa -= 23;

	if (last_fb->toa < 0) {
		qbits = (last_fb->toa + BITS_PER_TDMA) * 4;
		ntdma = -1;
	} else {
		ntdma = (last_fb->toa) / BITS_PER_TDMA;
		qbits = (last_fb->toa - ntdma * BITS_PER_TDMA) * 4;
	}

	fn_offset = l1s.current_time.fn - last_fb->attempt + ntdma;
	fnr_delta = last_fb->fnr_report - last_fb->attempt;
	bits_delta = fnr_delta * BITS_PER_TDMA;

	cinfo->fn_offset = fnr_delta;
	cinfo->time_alignment = qbits;
	cinfo->arfcn = rf_arfcn;

	if (last_fb->toa > bits_delta)
		printf("=> DSP reports FB in bit that is %d bits in "
			"the future?!?\n", last_fb->toa - bits_delta);
	else {
		int fb_fnr = (last_fb->fnr_report - last_fb->attempt)
				+ last_fb->toa/BITS_PER_TDMA;
		printf("=>FB @ FNR %u fn_offset=%d qbits=%u\n",
			fb_fnr, fn_offset, qbits);
	}
}

/* scheduler callback to issue a FB detection task to the DSP */
static int l1s_fbdet_cmd(__unused uint8_t p1, __unused uint8_t p2,
			 uint16_t fb_mode)
{
	if (fb_mode == 0) {
		putchart('F');
	} else {
		putchart('V');
	}

	l1s.fb.mode = fb_mode;

	/* Tell the RF frontend to set the gain appropriately */
	rffe_compute_gain(rxlev2dbm(fbs.req.rxlev_exp), CAL_DSP_TGT_BB_LVL);

	/* Program DSP */
	dsp_api.db_w->d_task_md = FB_DSP_TASK;	/* maybe with I/Q swap? */
	dsp_api.ndb->d_fb_mode = fb_mode;

	/* Program TPU */
	printf("\nMTZ: arfcn in l1s_fbdet_cmd = %d\n", fbs.req.band_arfcn);
	l1s_rx_win_ctrl(fbs.req.band_arfcn, L1_RXWIN_FB, 0);

	return 0;
}

#if 0
#define FB0_SNR_THRESH	2000
#define FB1_SNR_THRESH	3000
#else
#define FB0_SNR_THRESH	0
#define FB1_SNR_THRESH	0
#endif

static const struct tdma_sched_item fb_sched_set[];

/* scheduler callback to check for a FB detection response */
static int l1s_fbdet_resp(__unused uint8_t p1, uint8_t attempt,
			  uint16_t fb_mode)
{
	putchart('f');

	if (!dsp_api.ndb->d_fb_det) {
		/* we did not detect a FB */

		/* attempt < 12, do nothing */
		if (attempt < 12)
			return 0;

		/* attempt >= 12, we simply don't find one */

		/* If we don't reset here, we get DSP DMA errors */
		tdma_sched_reset();

		if (fbs.fb_retries < FB0_RETRY_COUNT) {
			/* retry once more */
			tdma_schedule_set(1, fb_sched_set, 0);
			fbs.fb_retries++;
		} else {
			last_fb->attempt = 13;
			l1s_compl_sched(L1_COMPL_FB);
		}

		return 0;
	}

	/* We found a frequency burst, reset everything */
	l1s_reset_hw();

	printf("FB%u ", dsp_api.ndb->d_fb_mode);
	read_fb_result(last_fb, attempt);

	/* if this is the first success, save freq err */
	if (!fbs.initial_freq_err)
		fbs.initial_freq_err = last_fb->freq_diff;

	/* If we don't reset here, we get DSP DMA errors */
	tdma_sched_reset();

	/* Immediately schedule further TDMA tasklets, if requested. Doing
	 * this directly from L1S means we can do this quickly without any
	 * additional delays */
	if (fb_mode == 0) {
		if (fbs.req.flags & L1CTL_FBSB_F_FB1) {
			/* If we don't reset here, we get DSP DMA errors */
			tdma_sched_reset();
			/* FIXME: don't only use the last but an average */
			if (abs(last_fb->freq_diff) < fbs.req.freq_err_thresh1 &&
			    last_fb->snr > FB0_SNR_THRESH) {
				/* continue with FB1 task in DSP */
				tdma_schedule_set(1, fb_sched_set, 1);
			} else {
				if (fbs.afc_retries < AFC_RETRY_COUNT) {
					tdma_schedule_set(1, fb_sched_set, 0);
					fbs.afc_retries++;
				} else {
					/* Abort */
					last_fb->attempt = 13;
					l1s_compl_sched(L1_COMPL_FB);
				}
			}
		} else
			l1s_compl_sched(L1_COMPL_FB);
	} else if (fb_mode == 1) {
		if (fbs.req.flags & L1CTL_FBSB_F_SB) {

	int ntdma, qbits;
	/* FIXME: where did this magic 23 come from? */
	last_fb->toa -= 23;

	if (last_fb->toa < 0) {
		qbits = (last_fb->toa + BITS_PER_TDMA) * 4;
		ntdma = -1;
	} else {
		ntdma = (last_fb->toa) / BITS_PER_TDMA;
		qbits = (last_fb->toa - ntdma * BITS_PER_TDMA) * 4;
	}


			int fn_offset = l1s.current_time.fn - last_fb->attempt + ntdma;
			int delay = fn_offset + 11 - l1s.current_time.fn - 1;
			printf("  fn_offset=%d (fn=%"PRIu32" + attempt=%u + ntdma = %d)\n",
				fn_offset, l1s.current_time.fn, last_fb->attempt, ntdma);
			printf("  delay=%d (fn_offset=%d + 11 - fn=%"PRIu32" - 1\n", delay,
				fn_offset, l1s.current_time.fn);
			printf("  scheduling next FB/SB detection task with delay %u\n", delay);
			if (abs(last_fb->freq_diff) < fbs.req.freq_err_thresh2 &&
			    last_fb->snr > FB1_SNR_THRESH) {
				/* synchronize before reading SB */
				fbinfo2cellinfo(&l1s.serving_cell, last_fb);
				synchronize_tdma(&l1s.serving_cell);
				tdma_schedule_set(delay, sb_sched_set, 0);
			} else
				tdma_schedule_set(delay, fb_sched_set, 1);
		} else
			l1s_compl_sched(L1_COMPL_FB);
	}

	return 0;
}

/* FB detection */
static const struct tdma_sched_item fb_sched_set[] = {
	SCHED_ITEM_DT(l1s_fbdet_cmd, 0, 0, 0),	SCHED_END_FRAME(),
						SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 1),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 2),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 3),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 4),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 5),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 6),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 7),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 8),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 9),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 10),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 11),	SCHED_END_FRAME(),
	SCHED_ITEM(l1s_fbdet_resp, -4, 0, 12),	SCHED_END_FRAME(),
	SCHED_END_SET()
};

/* Asynchronous completion handler for FB detection */
static void l1a_fb_compl(__unused enum l1_compl c)
{
	if (last_fb->attempt >= 13) {
		/* FB detection failed, signal this via L1CTL */
		l1ctl_fbsb_resp(255);
		return;
	}

	/* FIME: use l1s.neigh_cell[fbs.cinfo_idx] */
	fbinfo2cellinfo(&l1s.serving_cell, last_fb);

	/* send FBSB_CONF success message via L1CTL */
	l1ctl_fbsb_resp(0);
}

void l1s_fbsb_req(uint8_t base_fn, struct l1ctl_fbsb_req *req)
{
	/* copy + endian convert request data */
	fbs.req.band_arfcn = ntohs(req->band_arfcn);
	fbs.req.timeout = ntohs(req->timeout);
	fbs.req.freq_err_thresh1 = ntohs(req->freq_err_thresh1);
	fbs.req.freq_err_thresh2 = ntohs(req->freq_err_thresh2);
	fbs.req.num_freqerr_avg = req->num_freqerr_avg;
	fbs.req.flags = req->flags;
	fbs.req.sync_info_idx = req->sync_info_idx;
	fbs.req.rxlev_exp = req->rxlev_exp;

	/* clear initial frequency error */
	fbs.initial_freq_err = 0;
	fbs.fb_retries = 0;
	fbs.afc_retries = 0;

	/* Make sure we start at a 'center' AFCDAC output value */
	afc_reset();

	/* Reset the TOA loop counters */
	toa_reset();

	tdma_schedule_set(0, fb_sched_set, 0);	

	//MTZ - Changed
	//if (fbs.req.flags & L1CTL_FBSB_F_FB0)
	//	tdma_schedule_set(base_fn, fb_sched_set, 0);
	//else if (fbs.req.flags & L1CTL_FBSB_F_FB1)
	//	tdma_schedule_set(base_fn, fb_sched_set, 0);
	//else if (fbs.req.flags & L1CTL_FBSB_F_SB)
	//	tdma_schedule_set(base_fn, sb_sched_set, 0);

	//MTZ
	//l1ctl_test();
	//struct msgb *msg1 = l1ctl_msgb_alloc(L1CTL_TEST);

	//l1_queue_for_l2(msg1);

}

/* SB for Neighbours in dedicated mode
 *
 * Only when number of neighbor cells is > 0, perform synchronization.
 *
 * For each synchronization, l1s.neigh_pm.running is set. In case of an update
 * of neighbor cell list, this state is cleared, so a pending sync result would
 * be ignored.
 *
 * After a (new) list of neighbor cells are received, the measurements are not
 * yet valid. A valid state flag is used to indicate valid measurements. Until
 * there are no valid measurements, the synchronization is not performed.
 *
 * The task is to scan the 6 strongest neighbor cells by trying to synchronize
 * to it. This is done by selecting the strongest unscanned neighbor cell.
 * If 6 cells have been scanned or all cells (if less than 6) have been
 * scanned, the process clears all 'scanned' flags and starts over with the
 * strongest (now the strongest unscanned) cell.
 *
 * Each synchronization attempt is performed during the "search frame" (IDLE
 * frame). The process attempts to sync 11 times to ensure that it hits the
 * SCH of the neighbor's BCCH. (The alignment of BCCH shifts after every two
 * 26-multiframe in a way that the "search frame" is aligned with the SCH, at
 * least once for 11 successive "search frames".)
 *
 * If the synchronization attempt is successful, the BSIC and neighbor cell
 * offset is stored. These are indicated to layer23 with the measurement
 * results.
 *
 * When performing handover to a neighbor cell, the stored offset is used to
 * calculate new GSM time and tpu_offset.
 */

static void select_neigh_cell(void)
{
	uint8_t strongest = 0, strongest_unscanned = 0;
	int strongest_i = 0, strongest_unscanned_i = -1;
	int num_scanned = 0;
	int i;

	/* find strongest cell and strongest unscanned cell and count */
	for (i = 0; i < l1s.neigh_pm.n; i++) {
		if (l1s.neigh_pm.level[i] > strongest) {
			strongest = l1s.neigh_pm.level[i];
			strongest_i = i;
		}
		if (!(l1s.neigh_sb.flags_bsic[i] & NEIGH_PM_FLAG_SCANNED)) {
			if (l1s.neigh_pm.level[i] > strongest_unscanned) {
				strongest_unscanned = l1s.neigh_pm.level[i];
				strongest_unscanned_i = i;
			}
		} else
			num_scanned++;
	}

	/* no unscanned cell or we have scanned enough */
	if (strongest_unscanned_i < 0 || num_scanned >= 6) {
		/* flag all cells unscanned */
		for (i = 0; i < l1s.neigh_pm.n; i++)
			l1s.neigh_sb.flags_bsic[i] &= ~NEIGH_PM_FLAG_SCANNED;
		/* use strongest cell to begin scanning with */
		l1s.neigh_sb.index = strongest_i;
	} else {
		/* use strongest unscanned cell to begin scanning with */
		l1s.neigh_sb.index = strongest_unscanned_i;
	}
}

//MTZ - The function below has been taken from synchronize_tdma in sync.c and modified for whatever seemed necessary
void synchronize_tdma2()
{

	uint32_t tpu_shift;
	int ntdma, qbits;

	/* FIXME: where did this magic 23 come from? */
	nb_fb_toa -= 23;

	if (nb_fb_toa < 0) {
		qbits = (nb_fb_toa + BITS_PER_TDMA) * 4;
		ntdma = -1;
	} else {
		ntdma = (nb_fb_toa) / BITS_PER_TDMA;
		qbits = (nb_fb_toa - ntdma * BITS_PER_TDMA) * 4;
	}

	tpu_shift = qbits;

	old_tpu_offset = l1s.tpu_offset;

	/* NB detection only works if the TOA of the SB
	 * is within 0...8. We have to add 75 to get an SB TOA of 4. */
	tpu_shift += 75;

	tpu_shift = (l1s.tpu_offset + tpu_shift) % QBITS_PER_TDMA;

	//fn_offset = cinfo->fn_offset - 1;

	/* if we're already very close to the end of the TPU frame, the
	 * next interrupt will basically occur now and we need to
	 * compensate */
	//if (tpu_shift < SWITCH_TIME)
	//	fn_offset++;


	l1s.tpu_offset = tpu_shift;
	//puts("Synchronize_TDMA\n");
	/* request the TPU to adjust the SYNCHRO and OFFSET registers */
	tpu_enq_at(SWITCH_TIME);
	tpu_enq_sync(l1s.tpu_offset);
}

/*//////////////////////////////////////////////////////////////////////////////////////////////////////

FB/SB detection in dedicated mode

- First of all it must be noted that 3 additional frames have been added to the original idle frame 25 or 12 depending upon the half rate type. This gives us additional time to do any required time and frequency synchronization before detecting the synchronization burst of the neighbour as well as returning to the current settings before the TCH frame that follows. This can be seen in the file firmware/layer1/mframe_sched.c.

- The approach to detect FB and SB in dedicated mode must be kept in mind. In dedicated mode the traffic multiframe is running on the MS that consists of 26 frames with frame 25 as idle frame (in even mode). In order to read FB of neighbour we need to read its control channel that consists of 51 frames. As the control MF size is not a multiple of traffic MF size each time the traffic idle frame appears we are at a different frame number in the control MF of the neighbour. Hence the traffic idle frame coincides with a different control channel each time and actually traverses through it. By looking at the channel assignment accross the control MF it can be seen that we will coincide with a frequency burst on the control channel when we have an idle traffic frame every 10 or 11 idle frames. If we repeatedly search for FB on traffic idle frames we should be able to detect it every 10 or 11 idle frames. The SB would appear the second idle frame following the idle frame on which FB is detected not the next (this can be verified by hand). This is the approach we use.

- It was seen in idle mode FB/SB detection that FB is detected twice, first using FB mode 0 then using FB mode 1 which seem to provide different precision on frequency correction. After each FB detection frequency correction is performed in idle mode using afc_correct. Following FB mode 1 FB detection the quarter-bit synchronization is also performed using TOA (time of arrival of frequency burst) to synch the start of frame with the internal counters by modifying l1s.tpu_offset from what I could find. It is only after these two kinds of synchronizations/corrections that we can read the bursts on the channel properly. Synchronization burst is then read which is used to get the BSIC of the BTS and the absolute frame number it is on to update our internal variables/registers with it. Additionally further frequency correction is performed.

- The same steps need to be performed as above but this time in steps as we can only do this whenever the idle frame appears otherwise we would be in traffic mode and wouldn't have time to perform this. At each step values for frequency compensation/time correction are stored to be used in the next step. At the beginning of every idle frame set (as we have added three more idle frames) the required synchronization/correction is performed and at the end of it it is reversed to return to original settings. l1s_neigh_fbsb_sync is used to perform any synching/correction at the start of the idle frame set.

- fb_det and sb_det are used as control variables to guide through the synchronization process as follows:

	fb_det		sb_det		task

	0		0		FB detection mode 0
	1		0		FB detection mode 1
	0		1		Do nothing - this is the idle frame following FB detection
	0		2		SB detection
	0		3		Do nothing
	0		4		SB detection again

fb_det goes from 0 to 1 upon FB detection in FB mode 0. Then when FB is detected in FB mode 1 sb_det becomes 1 and fb_det goes to 0. Thereafter sb_det increments upon every idle frame. This is important to keep in mind.

* SB detection is done second time to make sure the SB isn't missed as I thought it might be in the middle of the frames. Just a cautious check.

- Whenever FB and/or SB are detected the results are stored in the following variables to be used in case of handover


	l1s.tpu_offsets_arfcn[ii] - The corresponding arfcn for the indices
	l1s.tpu_offsets[ii] - The quarter-bit offset required for start of frame
	l1s.nb_freq_diff[ii] - Frequency correction required from FB mode 0 + 1
	l1s.nb_sb_freq_diff[ii] - Additional freq correction from SB detection
	l1s.nb_frame_diff[ii] - The difference in frame of serving cell and the neighbour
	l1s.nb_sb_snr[ii] - The snr received (might not really be needed)

I know I could have used the l1s.neigh_sb struct but for now that's the way it is. This is so also because the stored number of neighbour gets reset whenever we enter a new dedicated mode but that can be worked around.

- The TOA obtained from FB detection gives the number of GSM bits from the start of the command to detect FB till the actual detection. As it can span more than 1 frame especially in idle mode it is broken down to ntdma and qbits. ntdma denotes the complete frames and qbits the number of quarter-bits following an integer number of frames (the remainder bits x 4). This is the actual difference from the frame start of the serving cell to the frame start of the neighbour cell.

- This schedule is activated using the multiframe scheduler whenever the idle frame comes in dedicated mode. select_neigh_cell() is used to select the next neighbour once SB of one neighbour is obtained or 15 tries to detect FB have failed (or SB is not detected following FB detection).

- l1s.neigh_sb is the struct used to keep track of neigbour bsic/power measurements etc. This can be enhanced to store the values menioned above as well.

- l1s.tpu_offset is the variable storing the quarter-bit offset to the start of frame for the cell to be read/transmitted to. afc_correct(...) is used to do frequency compensation.

- Things could have been coded in a better way but that's the way it is for now. Perhaps someone else can restructure the whole thing and come up with a better approach.

///////////////////////////////////////////////////////////////////////////////////////////////////// */

/* scheduler callback to issue a FB detection task to the DSP */
static int l1s_neigh_fbsb_sync(__unused uint8_t p1, __unused uint8_t p2,
			 uint16_t fb_mode)
{

	uint32_t tpu_shift;
	int ntdma, qbits;

	if (fb_det == 1) {
		//printf("afc_correct in l1s_neigh_fbsb_sync for FB1 - nb_fb_angle0\n\n");
		afc_correct(ANGLE_TO_FREQ(nb_fb_angle0), l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index]);
	}

	if ((sb_det == 2)||(sb_det == 4)) {
		//printf("\nMTZ - in l1s_neigh_fbsb_sync, old_tpu_offset = %d\n", l1s.tpu_offset);

		/* FIXME: where did this magic 23 come from? */
		nb_fb_toa -= 23; //MTZ - uncomment

		if (nb_fb_toa < 0) {
			qbits = (nb_fb_toa + BITS_PER_TDMA) * 4;
			ntdma = -1;
		} else {
			ntdma = (nb_fb_toa) / BITS_PER_TDMA;
			qbits = (nb_fb_toa - ntdma * BITS_PER_TDMA) * 4;
		}

		tpu_shift = qbits;

		old_tpu_offset = l1s.tpu_offset;

		/* NB detection only works if the TOA of the SB
		 * is within 0...8. We have to add 75 to get an SB TOA of 4. */
		tpu_shift += 75; //MTZ - uncomment

		tpu_shift = (l1s.tpu_offset + tpu_shift) % QBITS_PER_TDMA;

		//fn_offset = cinfo->fn_offset - 1;

		/* if we're already very close to the end of the TPU frame, the
		 * next interrupt will basically occur now and we need to
		 * compensate */
		//if (tpu_shift < SWITCH_TIME)
		//	fn_offset++;

		//printf("MTZ - old_tpu_offset = %d, tpu_shift = %d, qbits = %d\n", old_tpu_offset, tpu_shift, qbits);

		l1s.neigh_pm.tpu_offset[l1s.neigh_sb.index] = tpu_shift;

		int ii =0;
		for (ii=0; ii<64; ii++) {
			if (l1s.tpu_offsets_arfcn[ii] == l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index]) {
				l1s.tpu_offsets[ii] = tpu_shift;
				l1s.nb_freq_diff[ii] = ANGLE_TO_FREQ(nb_fb_angle0)+ANGLE_TO_FREQ(nb_fb_angle1);
				break;
			}
			if (l1s.tpu_offsets_arfcn[ii] == 0) {
				l1s.tpu_offsets_arfcn[ii] = l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index];
				l1s.tpu_offsets[ii] = tpu_shift;
				l1s.nb_freq_diff[ii] = ANGLE_TO_FREQ(nb_fb_angle0)+ANGLE_TO_FREQ(nb_fb_angle1);
				break;
			}
		}

		//printf("\n\nMTZ: Stored TPU Offsets, Angles:");
		//for (ii=0; ii<64; ii++) {
		//	if (l1s.tpu_offsets_arfcn[ii] == 0)
		//		break;
		//	printf("  %d,%d(%d)", l1s.tpu_offsets[ii], l1s.nb_freq_diff[ii], l1s.tpu_offsets_arfcn[ii]);
		//}
		//printf("\n\n");

		//MTZ - possibly remove the >=50 if statement
		if (nb_fb_toa >= 50) {
			l1s.tpu_offset = tpu_shift;
			//tpu_enq_at(SWITCH_TIME);
			//tpu_enq_sync(tpu_shift);
		}
		//printf("afc_correct in l1s_neigh_fbsb_sync for SB - nb_fb_angle0+nb_fb_angle1\n\n");
		afc_correct(ANGLE_TO_FREQ(nb_fb_angle0)+ANGLE_TO_FREQ(nb_fb_angle1), l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index]);
	}

}
/* scheduler callback to issue a FB and SB detection task to the DSP in dedicated mode */
// READ COMMENTS ABOVE l1s_neigh_fbsb_sync
static int l1s_neigh_fbsb_cmd(__unused uint8_t p1, __unused uint8_t p2,
			 uint16_t fb_mode)
{

	int index = l1s.neigh_sb.index;
	uint8_t last_gain;

	if (l1s.neigh_pm.n == 0)
		return 0;

	/* if measurements are not yet valid, wait */
	if (!l1s.neigh_pm.valid)
		return 0;

	/* check for cell to sync to */
	if (l1s.neigh_sb.count == 0) {
		/* there is no cell selected, search for cell */
		select_neigh_cell();
		index = l1s.neigh_sb.index;
	}

//	//MTZ - putting this for now as we wanted to repeatedly detect the remaining ones - remove
//	while (!((l1s.neigh_sb.flags_bsic[index] & NEIGH_PM_FLAG_BSIC) == 0)) {
////		printf("\nMTZ: BSIC has been decoded for ARFCN %d (flags_bsic[%d] = %d)\n\n", l1s.neigh_pm.band_arfcn[index], index, l1s.neigh_sb.flags_bsic[index]);
//		l1s.neigh_sb.count = 0;
//		l1s.neigh_sb.flags_bsic[index] |= NEIGH_PM_FLAG_SCANNED;
//		select_neigh_cell();
//		index = l1s.neigh_sb.index;
//	}

	if (sb_det == 0) {

		//l1s.fb.mode = fb_mode;

		//printf(" - detect FB arfcn %d (#%d) %d dbm\n", l1s.neigh_pm.band_arfcn[index], l1s.neigh_sb.count, rxlev2dbm(l1s.neigh_pm.level[index]));

		last_gain = rffe_get_gain();

		/* Tell the RF frontend to set the gain appropriately */
		rffe_compute_gain(rxlev2dbm(l1s.neigh_pm.level[index]), CAL_DSP_TGT_BB_LVL);

		/* Program DSP */
		dsp_api.db_w->d_task_md = TCH_FB_DSP_TASK;  /* maybe with I/Q swap? */
//		dsp_api.db_w->d_task_md = dsp_task_iq_swap(TCH_SB_DSP_TASK, l1s.neigh_pm.band_arfcn[index], 0); //MTZ - Commented originally
		if (fb_det == 1) {
			dsp_api.ndb->d_fb_mode = 1;
		} else {
			dsp_api.ndb->d_fb_mode = 0;
		}

		/* Program TPU */
		//l1s_rx_win_ctrl(l1s.neigh_pm.band_arfcn[index], L1_RXWIN_FB26, 5); //MTZ - Original - don't think works - as we have multiple idle frames now we can use TS 0
		l1s_rx_win_ctrl(l1s.neigh_pm.band_arfcn[index], L1_RXWIN_FB26, 0);

		/* restore last gain */
		rffe_set_gain(last_gain);

	} else if ((sb_det == 2)||(sb_det == 4)) {

//		printf(" - detect SB arfcn %d (#%d) %d dbm\n", l1s.neigh_pm.band_arfcn[index], l1s.neigh_sb.count, rxlev2dbm(l1s.neigh_pm.level[index]));

		last_gain = rffe_get_gain();

		/* Tell the RF frontend to set the gain appropriately */
		rffe_compute_gain(rxlev2dbm(l1s.neigh_pm.level[index]), CAL_DSP_TGT_BB_LVL);

		/* Program DSP */
		//MTZ Changed
		//dsp_api.db_w->d_task_md = TCH_SB_DSP_TASK;  /* maybe with I/Q swap? */
		dsp_api.db_w->d_task_md = SB_DSP_TASK;  /* maybe with I/Q swap? */
//		dsp_api.db_w->d_task_md = dsp_task_iq_swap(TCH_SB_DSP_TASK, l1s.neigh_pm.band_arfcn[index], 0); //MTZ - Commented originally
		dsp_api.ndb->d_fb_mode = 0;

//		//MTZ - Experimenting
//		dsp_api.ndb->a_sync_demod[D_TOA] = nb_fb_toa;
//		dsp_api.ndb->a_sync_demod[D_PM] = nb_fb_pm;
//		dsp_api.ndb->a_sync_demod[D_ANGLE] = nb_fb_angle;
//		dsp_api.ndb->a_sync_demod[D_SNR] = nb_fb_snr;


		/* Program TPU */
		//l1s_rx_win_ctrl(l1s.neigh_pm.band_arfcn[index], L1_RXWIN_SB26, 5); //MTZ - Original - don't think works - as we have multiple idle frames now we can use TS 0
		l1s_rx_win_ctrl(l1s.neigh_pm.band_arfcn[index], L1_RXWIN_SB, 0);

		/* restore last gain */
		rffe_set_gain(last_gain);

		l1s.neigh_sb.running = 1;

	}
	return 0;
}

/* scheduler callback to issue a FB and SB detection task to the DSP in dedicate mode */
// READ COMMENTS ABOVE l1s_neigh_fbsb_sync
static int l1s_neigh_fbsb_resp(__unused uint8_t p1, uint8_t attempt,
			  uint16_t fb_mode)
{
	int index = l1s.neigh_sb.index;
	uint32_t sb;
	uint8_t bsic;
	int sb_found = 0;

	if (sb_det == 0) {
		if (fb_det == 1) {
			//printf("afc_correct (-ve) in l1s_neigh_fbsb_resp for FB1 - nb_fb_angle0\n\n");
			afc_correct(-1*ANGLE_TO_FREQ(nb_fb_angle0), l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index]);
		}
		if (!dsp_api.ndb->d_fb_det) {
			printf("MTZ: ARFCN %d (index %d, power %d dbm, try #%d) FB%d found = 0\n", l1s.neigh_pm.band_arfcn[index], index, rxlev2dbm(l1s.neigh_pm.level[index]), l1s.neigh_sb.count, fb_det);

			/* next sync */
			if (++l1s.neigh_sb.count == 15) {
				//MTZ - a count of 0 will result in select_neigh_cell() being called and next cell being selected
				l1s.neigh_sb.count = 0;
				l1s.neigh_sb.flags_bsic[index] |= NEIGH_PM_FLAG_SCANNED;
				//MTZ - If 15 tries in FB mode 1 then set fb_det to 0
				if (fb_det == 1){
					fb_det = 0;
				}
			}

		} else {
			//MTZ - Capturing the readings from FB detection - these are stored in arrays upon SB detection for use in case handover is required
			nb_fb_toa = dsp_api.ndb->a_sync_demod[D_TOA];
			nb_fb_pm = dsp_api.ndb->a_sync_demod[D_PM];
			if (fb_det == 1)
				nb_fb_angle1 = dsp_api.ndb->a_sync_demod[D_ANGLE];
			else
				nb_fb_angle0 = dsp_api.ndb->a_sync_demod[D_ANGLE];
			nb_fb_snr = dsp_api.ndb->a_sync_demod[D_SNR];
			printf("\n\nMTZ: ARFCN %d (index %d, power %d dbm, try #%d) FB%d found = 1 >>> nb_fb_toa = %d, angle = %d\n\n", l1s.neigh_pm.band_arfcn[index], index, rxlev2dbm(l1s.neigh_pm.level[index]), l1s.neigh_sb.count, fb_det, nb_fb_toa, dsp_api.ndb->a_sync_demod[D_ANGLE]);
			//MTZ - If FB mode was 0 make it 1, it FB mode was one set sb_det to 1 to indicate SB detection step
			if (fb_det == 0) {
				fb_det = 1;
				l1s.neigh_sb.count = 1;
			} else {
				sb_det = 1;
				fb_det = 0;
			}
		}

		//l1s_reset_hw();
		tdma_sched_reset();
	} else {
		if ((sb_det == 2)||(sb_det == 4)) {
			/* check if sync was successful */

			//MTZ - This was the main change below - we need to read a_sch26 as opposed to a_sch
			//if (dsp_api.db_r->a_sch[0] & (1<<B_SCH_CRC)) {
			if (dsp_api.ndb->a_sch26[0] & (1<<B_SCH_CRC)) {
//				printf("\nSB found = 0 (ARFCN %d, power %d dbm)\n\n", l1s.neigh_pm.band_arfcn[index], rxlev2dbm(l1s.neigh_pm.level[index]));
			} else {

				uint32_t	fn;	/* FN count */
				uint16_t	t1;	/* FN div (26*51) */
				uint8_t		t2;	/* FN modulo 26 */
				uint8_t		t3;	/* FN modulo 51 */
				uint8_t		tc;
				uint8_t t3p;				

				sb_found = 1;
				sb = dsp_api.ndb->a_sch26[3] | dsp_api.ndb->a_sch26[4] << 16;
				bsic = (sb >> 2) & 0x3f;

				t1 = ((sb >> 23) & 1) | ((sb >> 7) & 0x1fe) | ((sb << 9) & 0x600);
				t2 = (sb >> 18) & 0x1f;
				t3p = ((sb >> 24) & 1) | ((sb >> 15) & 6);
				t3 = t3p*10 + 1;

				/* TS 05.02 Chapter 4.3.3 TDMA frame number */
				fn = (51 * ((t3 - t2 + 26) % 26) + t3 + (26 * 51 * t1)) + SB2_LATENCY;

				int ii =0;
				for (ii=0; ii<64; ii++) {
					if (l1s.tpu_offsets_arfcn[ii] == l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index]) {
						l1s.nb_frame_diff[ii] = fn - l1s.current_time.fn;
						l1s.nb_sb_freq_diff[ii] = ANGLE_TO_FREQ(dsp_api.db_r->a_serv_demod[D_ANGLE]);
						l1s.nb_sb_snr[ii] = dsp_api.db_r->a_serv_demod[D_SNR];
						break;
					}
				}


				total_sb_det++;
				//printf("=> SB 0x%08"PRIx32": BSIC=%u \n\n", sb, bsic);
				printf("\n----------------------------------------------------------------------------\nSB found = 1 (ARFCN %d, power %d dbm) => SB 0x%08"PRIx32": BSIC=%u, TOA=%d, Angle=%d (Total=%d)\n----------------------------------------------------------------------------\n\n", l1s.neigh_pm.band_arfcn[index], rxlev2dbm(l1s.neigh_pm.level[index]), sb, bsic, dsp_api.db_r->a_serv_demod[D_TOA], dsp_api.db_r->a_serv_demod[D_ANGLE], total_sb_det);
				l1s.neigh_sb.flags_bsic[index] = bsic | NEIGH_PM_FLAG_BSIC | NEIGH_PM_FLAG_SCANNED;
			}
		
			if ((sb_det == 2)||(sb_det == 4)) {

				//MTZ - testing this - possibly remove
				if (nb_fb_toa >= 50);
					l1s.tpu_offset = old_tpu_offset;
				//printf("afc_correct (-ve) in l1s_neigh_fbsb_resp for SB - nb_fb_angle0+nb_fb_angle1\n\n");
				afc_correct(-1*(ANGLE_TO_FREQ(nb_fb_angle0) + ANGLE_TO_FREQ(nb_fb_angle1)), l1s.neigh_pm.band_arfcn[l1s.neigh_sb.index]);

			}
	
			if ((sb_det == 4)||(sb_found == 1)) {
				l1s.neigh_sb.count = 0;
				//MTZ - need to change this statement based on detection
				l1s.neigh_sb.flags_bsic[index] |= NEIGH_PM_FLAG_SCANNED;

				l1s.neigh_sb.running = 0;

				//dsp_api.r_page_used = 1;

				if (sb_found == 0)
					printf("\n\n");
			}

		}

		if ((sb_det == 4)||(sb_found == 1))
			sb_det = 0;
		else
			sb_det++;
	}

	return 0;
}

//MTZ - THIS FUNCTION BELOW IS NOT USED!!!!!
static int l1s_neigh_sb_cmd(__unused uint8_t p1, __unused uint8_t p2,
                            __unused uint16_t p3)
{
	int index = l1s.neigh_sb.index;
	uint8_t last_gain;

	printf("\n\n\nMTZ: In neigh_sb_cmd, l1s.neigh_pm.n = %d\n\n\n", l1s.neigh_pm.n);

	if (l1s.neigh_pm.n == 0)
		return 0;

	/* if measurements are not yet valid, wait */
	if (!l1s.neigh_pm.valid)
		return 0;

	/* check for cell to sync to */
	if (l1s.neigh_sb.count == 0) {
		/* there is no cell selected, search for cell */
		select_neigh_cell();
	}

	printf("detect SB arfcn %d (#%d) %d dbm\n", l1s.neigh_pm.band_arfcn[index], l1s.neigh_sb.count, rxlev2dbm(l1s.neigh_pm.level[index]));

	last_gain = rffe_get_gain();

	/* Tell the RF frontend to set the gain appropriately */
	rffe_compute_gain(rxlev2dbm(l1s.neigh_pm.level[index]), CAL_DSP_TGT_BB_LVL);

	/* Program DSP */
	dsp_api.db_w->d_task_md = TCH_SB_DSP_TASK;  /* maybe with I/Q swap? */
//	dsp_api.db_w->d_task_md = dsp_task_iq_swap(TCH_SB_DSP_TASK, l1s.neigh_pm.band_arfcn[index], 0);
	dsp_api.ndb->d_fb_mode = 0;

	/* Program TPU */
	l1s_rx_win_ctrl(l1s.neigh_pm.band_arfcn[index], L1_RXWIN_SB26, 5);

	/* restore last gain */
	rffe_set_gain(last_gain);

	l1s.neigh_sb.running = 1;

	return 0;
}

//MTZ - THIS FUNCTION BELOW IS NOT USED!!!!!
static int l1s_neigh_sb_resp(__unused uint8_t p1, __unused uint8_t p2,
                             __unused uint16_t p3)
{
	int index = l1s.neigh_sb.index;
	uint32_t sb;

	if (l1s.neigh_pm.n == 0 || !l1s.neigh_sb.running)
		goto out;

	/* check if sync was successful */
	if (dsp_api.db_r->a_sch[0] & (1<<B_SCH_CRC)) {
		printf("SB error arfcn %d\n", l1s.neigh_pm.band_arfcn[index]);

		/* next sync */
		if (++l1s.neigh_sb.count == 11) {
			l1s.neigh_sb.count = 0;
			l1s.neigh_sb.flags_bsic[index] |= NEIGH_PM_FLAG_SCANNED;
		}
	} else {
		l1s.neigh_sb.count = 0;

		read_sb_result(last_fb, 1);
		sb = dsp_api.db_r->a_sch[3] | dsp_api.db_r->a_sch[4] << 16;
		l1s.neigh_sb.flags_bsic[index] =
			l1s_decode_sb(&fbs.mon.time, sb)
				| NEIGH_PM_FLAG_BSIC | NEIGH_PM_FLAG_SCANNED;
		printf("SB OK!!!!!! arfcn %d\n", l1s.neigh_pm.band_arfcn[index]);

		/* store time offset */
	}

out:
	l1s.neigh_sb.running = 0;

	dsp_api.r_page_used = 1;
	
	printf("\nMTZ: In l1s_neigh_sb_resp, l1s.serving_cell.arfcn = %d\n", l1s.serving_cell.arfcn);

	return 0;

}

///* NOTE: Prio 1 is below TCH's RX+TX prio 0 */
//const struct tdma_sched_item neigh_sync_sched_set[] = {
//	SCHED_ITEM_DT(l1s_neigh_sb_cmd, 1, 0, 1),	SCHED_END_FRAME(),
//							SCHED_END_FRAME(),
//	SCHED_ITEM(l1s_neigh_sb_resp, -4, 0, 1),	SCHED_END_FRAME(),
//	SCHED_END_SET()
//};

/* NOTE: Prio 1 is below TCH's RX+TX prio 0 */
const struct tdma_sched_item neigh_sync_sched_set[] = {
	SCHED_ITEM_DT(l1s_neigh_fbsb_sync, 1, 0, 1),	SCHED_END_FRAME(),
	SCHED_ITEM_DT(l1s_neigh_fbsb_cmd, 1, 0, 1),	SCHED_END_FRAME(),
							SCHED_END_FRAME(),
							SCHED_END_FRAME(),
	SCHED_ITEM(l1s_neigh_fbsb_resp, -4, 0, 1),	SCHED_END_FRAME(),
	SCHED_END_SET()
};

static __attribute__ ((constructor)) void l1s_prim_fbsb_init(void)
{
	l1s.completion[L1_COMPL_FB] = &l1a_fb_compl;
}
