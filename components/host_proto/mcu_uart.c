/*
 * Copyright 2013 Ayla Networks, Inc.  All rights reserved.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ayla/utypes.h>
#include <ayla/crc.h>
#include <ayla/endian.h>
#include <ayla/assert.h>
#include <ayla/conf_token.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/serial.h>
#include <ayla/nameval.h>
#include <ayla/ayla_spi_mcu.h>
#include <ayla/ayla_proto_mcu.h>
#include <ayla/timer.h>
#include <ada/err.h>
#include <ada/prop.h>
#include <net/net.h>

#include <host_proto/mcu_dev.h>
#include "host_proto_int.h"
#include "data_tlv.h"
#include "mcu_uart_int.h"
#include "host_decode.h"
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "host_proto_int.h"

/*
 * Define to log the decode of packets at the DEBUG level.
 */
#define MCU_UART_DECODE		/* define to include packet interpretation */

/*
 * Define level to log all bytes in packets.
 * If MCU_UART_DECODE is defined, this should be LOG_SEV_DEBUG2.
 * Undefine to omit logging.
 */
#define MCU_UART_LOG_BYTES_SEV	LOG_SEV_DEBUG2

/*
 * Define level to log ACK packets.  Undefine to omit logging.
 */
#define MCU_UART_LOG_ACK_SEV	LOG_SEV_DEBUG2

/*
 * Define to log raw format (with framing and escapes) at debug2 level.
 */
#undef MCU_UART_LOG_RAW

/* ppp frame size. 2 flag bytes, stuff(crc) */
#define MCU_UART_PPP_SIZE	4

/* ack message size. ppp + stuff(seq) + ptype */
#define MCU_UART_ACK_SIZE	(MCU_UART_PPP_SIZE + 3)

/* max receiving buffer length. recv interrupts will fill this */
#define MCU_UART_RX_SIZE	(ASPI_LEN_MAX *  2 + MCU_UART_ACK_SIZE * 2)

/* max recvd data from mcu */
#define MCU_UART_RX_DATA_SIZE	ASPI_LEN_MAX

/* max transmit data to mcu */
#define MCU_UART_TX_DATA_SIZE	(ASPI_LEN_MAX * 2 + MCU_UART_PPP_SIZE)

/* max len of ack queue */
#define	MCU_UART_MAX_ACKS	5

/* time to wait for an ack response for a packet before retransmitting (ms) */
#define MCU_UART_ACK_WAIT	5000

/* max # of retransmissions */
#define MCU_UART_MAX_RETRIES	2

enum muart_ptype {
	MP_NONE = 0,
	MP_DATA,
	MP_ACK,
};

/***** PPP Protocol *****/
#define	UART_PPP_FLAG_BYTE		0x7E
#define	UART_PPP_ESCAPE_BYTE		0x7D
#define	UART_PPP_XOR_BYTE		0x20
/************************/

#define MCU_UART_STATS

#ifndef MCU_UART_STATS
#define MUART_STATS(_muart, x)
#else
#define MUART_STATS(_muart, x)	do { (_muart)->stats.x++; } while (0)
/*
 * Optional debug counters for various conditions.
 * These can be deleted to save space but may help debugging.
 */
struct muart_stats {
	u16 tx_bytes;
	u16 tx_pkts;
	u16 tx_acks;
	u16 tx_data;
	u16 tx_resend;

	u16 rx_bytes;
	u16 rx_pkts;
	u16 rx_data;
	u16 rx_acks;

	u16 rx_frame_err;
	u16 rx_len_err;
	u16 rx_crc_err;
	u16 rx_ptype_err;
	u16 rx_seq_err;
	u16 rx_cmd_err;
};
#endif

struct muart_buffer {
	/* circular buffer implementing "keep one slot open" */
	u16 start;
	u16 end;
	u8 *buf;
	u32 size;
};

enum muart_tx_state {
	MTS_IDLE = 0,
	MTS_SENDING,
	MTS_ACK_FINISH,
	MTS_DATA_FINISH
};

struct mcu_uart_state {
	void	(*data_tlv_cb)(void);	/* non-NULL if it needs bufs */
	struct hp_buf *ack_queue;	/* ack queue */
	struct hp_buf *tx_queue;	/* transmit queue */
	struct net_callback rx_callback;/* rx data callback */
	struct net_callback tx_callback;/* tx callback */
	struct muart_buffer *tx_buf;	/* current buffer to be transmitted */
	enum	muart_tx_state mts;	/* current state of transmission */
	u8	retransmit_count;	/* # of retransmissions for packet */
	u8	saw_ppp_flag;		/* saw the first PPP flag byte */
	u8	tx_queue_len;		/* len of tx queue */
	u8	num_pkts_recvd;		/* # of complete recvd packets */
	u8	recved_seq_no;		/* seq # of the last recved packet */
	u8	tx_seq_no;		/* seq # of the last tx packet */
	u8	wait_for_ack;		/* 1 if waiting for ack to last tx */
	u8	resend:1;		/* resend last packet */
	u8	first_tx:1;		/* next tx is the 1st since init */
#ifdef MCU_UART_STATS
	struct muart_stats stats;	/* statistics counters */
#endif

	struct muart_buffer rx_buf;
	struct muart_buffer tx_ack_buf;
	struct muart_buffer tx_data_buf;

	u8 rx_area[MCU_UART_RX_SIZE + 1];
	u8 tx_data_area[MCU_UART_TX_DATA_SIZE + 1];
	u8 tx_ack_area[MCU_UART_ACK_SIZE + 1];

	struct timer tx_resend_timer;
};

static struct mcu_uart_state *muart_state;
static const struct mcu_dev mcu_uart_ops;

static void mcu_uart_send_next(struct mcu_uart_state *muart);
static int mcu_uart_can_recv(struct mcu_uart_state *);

/*
 * Dequeue from buffer and return pointer to data. Return null if empty
 */
static u8 *mcu_uart_buf_deq(struct muart_buffer *muart_buf)
{
	int start = muart_buf->start;
	u8 *data;

	if (start == muart_buf->end) {
		return NULL;
	}
	data = &muart_buf->buf[start];
	if (++start >= muart_buf->size) {
		start = 0;
	}
	muart_buf->start = start;
	return data;
}

/*
 * Timeout for handling data resend
 */
static void mcu_uart_tx_resend(struct timer *tm)
{
	struct mcu_uart_state *muart = muart_state;

	if (muart->retransmit_count < MCU_UART_MAX_RETRIES) {
		/* retransmit packet */
		muart->resend = 1;
		muart->retransmit_count++;
	} else {
		log_put_mod_sev(MOD_LOG_IO, LOG_SEV_WARN,
		    "uart_tx_resend: pkt dropped");
	}
	muart->wait_for_ack = 0;
	if (muart->mts == MTS_IDLE) {
		mcu_uart_send_next(muart);
	}
}

/*
 * Enqueue to buffer. Return -1 if buffer is full.
 */
static int mcu_uart_buf_enq(struct muart_buffer *muart_buf, u8 data)
{
	int end = muart_buf->end;

	if (++end >= muart_buf->size) {
		end = 0;
	}
	if (end == muart_buf->start) {
		return -1;
	}
	muart_buf->buf[muart_buf->end] = data;
	muart_buf->end = end;

	return 0;
}

/*
 * Generate a ack buf
 */
static int mcu_uart_gen_ack(struct mcu_uart_state *muart, u8 ack_seq)
{
	struct hp_buf *ack_buf;
	struct hp_buf *prev;
	struct hp_buf *bp;

	ack_buf = hp_buf_alloc(1);
	if (!ack_buf) {
		return -1;
	}
	if (ack_seq == 0) {
		/* lollipop seq #, starting over */
		while ((bp = muart->ack_queue) != NULL) {
			muart->ack_queue = bp->next;
			bp->next = NULL;
			hp_buf_free(bp);
		}
	}
	prev = muart->ack_queue;
	*(u8 *)ack_buf->payload = ack_seq;
	if (prev == NULL) {
		muart->ack_queue = ack_buf;
		return 0;
	}
	while (prev->next != NULL) {
		prev = prev->next;
	}
	prev->next = ack_buf;
	return 0;
}

/*
 * Process the recved data on the UART buffer.
 */
static void mcu_uart_recv_cb(void *arg)
{
	struct mcu_uart_state *muart = arg;
	struct muart_buffer *recv = &muart->rx_buf;
	u8 escape_found;
	u8 *data;
	u8 recv_buffer[MCU_UART_RX_DATA_SIZE * 2];
	u8 *data_ptr = recv_buffer + 2;
	size_t recv_len;
	void (*data_tlv_cb)(void);

process_next_packet:
	recv_len = 0;
	escape_found = 0;
	if (!muart->num_pkts_recvd) {
		if (!mcu_uart_can_recv(muart)) {
			/* overflow of recv buffer without complete packet */
			/* drop all of it */
			recv->start = 0;
			recv->end = 0;
			muart->saw_ppp_flag = 0;
		}
		goto finish;
	}
	/* skip over any ppp flags */
	data = mcu_uart_buf_deq(recv);
	while (data != NULL && *data == UART_PPP_FLAG_BYTE &&
	    muart->num_pkts_recvd) {
		muart->num_pkts_recvd--;
		if (muart->num_pkts_recvd) {
			data = mcu_uart_buf_deq(recv);
		}
	}

	if (data == NULL || !muart->num_pkts_recvd) {
		muart->num_pkts_recvd = 0;
		goto finish;
	}
	/* unstuff bytes */
	while (data != NULL && *data != UART_PPP_FLAG_BYTE) {
		if (recv_len == sizeof(recv_buffer)) {
			/* received packet too long */
			MUART_STATS(muart, rx_len_err);
			goto skip_packet;
		}
		if (escape_found) {
			recv_buffer[recv_len++] = *data ^ UART_PPP_XOR_BYTE;
			escape_found = 0;
		} else if (*data == UART_PPP_ESCAPE_BYTE) {
			escape_found = 1;
		} else {
			recv_buffer[recv_len++] = *data;
		}
		data = mcu_uart_buf_deq(recv);
	}
	muart->num_pkts_recvd--;
	if (data == NULL) {
		/* incomplete packet, just drop it */
		muart->num_pkts_recvd = 0;
		MUART_STATS(muart, rx_frame_err);
		goto finish;
	}
	serial_rx_unblock(SERIAL_MCU);

#ifdef MCU_UART_LOG_RAW
	log_put_mod_sev(MOD_LOG_IO, LOG_SEV_DEBUG2, "uart_raw_rx:");
	log_bytes_in_hex_sev(MOD_LOG_IO, LOG_SEV_DEBUG2, recv_buffer, recv_len);
#endif

	/* check min ppp len. ptype, seq #, 2-byte crc */
	if (recv_len < 4) {
		MUART_STATS(muart, rx_len_err);
		goto process_next_packet;
	}
	/* check crc */
	if (crc16(recv_buffer, recv_len, CRC16_INIT)) {
		/* bad crc, drop packet */
		MUART_STATS(muart, rx_crc_err);
		goto process_next_packet;
	}
	/* check ptype */
	if (recv_buffer[0] != MP_DATA && recv_buffer[0] != MP_ACK) {
		/* bad ptype, skip packet */
		MUART_STATS(muart, rx_ptype_err);
		goto process_next_packet;
	}
	if (recv_buffer[0] == MP_ACK) {
#ifdef MCU_UART_LOG_ACK_SEV
		log_put_mod_sev(MOD_LOG_IO, MCU_UART_LOG_ACK_SEV,
		    "uart_rx ack %#x", recv_buffer[1]);
#endif
		MUART_STATS(muart, rx_acks);

		if (muart->wait_for_ack &&
		    recv_buffer[1] == muart->tx_seq_no) {
			host_proto_timer_cancel(&muart->tx_resend_timer);
			muart->wait_for_ack = 0;
			if (muart->data_tlv_cb &&
			    muart->tx_queue_len < MAX_SERIAL_TX_PBUFS) {
				data_tlv_cb = muart->data_tlv_cb;
				muart->data_tlv_cb = NULL;
				data_tlv_cb();
			} else if (muart->mts == MTS_IDLE) {
				mcu_uart_send_next(muart);
			}
		}
		goto process_next_packet;
	}
	MUART_STATS(muart, rx_data);

	/* trim out the framing */
	recv_len -= 4;
#ifdef MCU_UART_LOG_BYTES_SEV
	log_put_mod_sev(MOD_LOG_IO, MCU_UART_LOG_BYTES_SEV,
	    "uart_rx seq %#x %zu bytes",
	    recv_buffer[1], recv_len);
	log_bytes_in_hex_sev(MOD_LOG_IO, MCU_UART_LOG_BYTES_SEV,
	    data_ptr, recv_len);
#endif

	if (mcu_uart_gen_ack(muart, recv_buffer[1])) {
		goto process_next_packet;
	}
	if (muart->mts == MTS_IDLE) {
		mcu_uart_send_next(muart);
	}
	/* check seq #, make sure its not the lollip seq # */
	if (recv_buffer[1] && recv_buffer[1] == muart->recved_seq_no) {
		/* already received and processed packet with this seq no */
		MUART_STATS(muart, rx_seq_err);
		goto process_next_packet;
	}

	muart->recved_seq_no = recv_buffer[1];

#ifdef MCU_UART_DECODE
	host_decode_log("rx", data_ptr, recv_len);
#endif
	if (data_tlv_process_mcu_pkt(data_ptr, recv_len)) {
		MUART_STATS(muart, rx_cmd_err);
	}
	goto process_next_packet;

finish:
	serial_rx_unblock(SERIAL_MCU);
	return;

skip_packet:
	data = mcu_uart_buf_deq(recv);
	while (data != NULL && *data != UART_PPP_FLAG_BYTE) {
		data = mcu_uart_buf_deq(recv);
	}
	muart->num_pkts_recvd--;
	if (data == NULL || !muart->num_pkts_recvd) {
		muart->num_pkts_recvd = 0;
		goto finish;
	}
	goto process_next_packet;
}

/*
 * Issue rx cb to handle the recvd data
 */
static void mcu_uart_issue_rx_cb(struct mcu_uart_state *muart)
{
	host_proto_callback_pend(&muart->rx_callback);
}

/*
 * Called by the receive interrupt handler in mcu_uart_platform.
 * Return 1 if the recv buffer can receive.
 */
static int mcu_uart_can_recv(struct mcu_uart_state *muart)
{
	struct muart_buffer *muart_buf;
	int end;

	muart_buf = &muart->rx_buf;
	end = muart_buf->end;
	if (++end >= muart_buf->size) {
		end = 0;
	}
	if (end == muart_buf->start) {
		mcu_uart_issue_rx_cb(muart);
		return 0;
	}

	return 1;
}

/*
 * Process UART receive interrupts.
 * Called by the callback in serial driver.
 * Return non-zero if nothing can be received; the caller must buffer the byte.
 */
static int mcu_uart_rx_intr(u8 dr)
{
	struct mcu_uart_state *muart = muart_state;
	struct muart_buffer *recv = &muart->rx_buf;

	if (!mcu_uart_can_recv(muart)) {
		return -1;
	}
	MUART_STATS(muart, rx_bytes);
	if (!muart->saw_ppp_flag) {
		if (dr == UART_PPP_FLAG_BYTE) {
			muart->saw_ppp_flag = 1;
		}
		/* saw bytes without the first ppp flag, just drop */
		return 0;
	}
	mcu_uart_buf_enq(recv, dr);
	if (dr == UART_PPP_FLAG_BYTE) {
		MUART_STATS(muart, rx_pkts);
		muart->num_pkts_recvd++;
		mcu_uart_issue_rx_cb(muart);
	}
	return 0;
}

/*
 * Helper function for mcu_uart_build_tx.
 * Enqueues the data, escaping where necessary.
 */
static int mcu_uart_build_tx_helper(struct muart_buffer *in,
		u8 *data, size_t len)
{
	int i;

	ASSERT(len <= MCU_UART_TX_DATA_SIZE);

	for (i = 0; i < len; i++) {
		if (data[i] == UART_PPP_FLAG_BYTE ||
		    data[i] == UART_PPP_ESCAPE_BYTE) {
			if (mcu_uart_buf_enq(in, UART_PPP_ESCAPE_BYTE)) {
				return -1;
			}
			if (mcu_uart_buf_enq(in,
			    data[i] ^ UART_PPP_XOR_BYTE)) {
				return -1;
			}
		} else {
			if (mcu_uart_buf_enq(in, data[i])) {
				return -1;
			}
		}
	}
	return 0;
}

/*
 * Build a packet with the PPP flag bytes + ptype + seq no + data + crc.
 * Starts from index 0.
 */
static int mcu_uart_build_tx(struct mcu_uart_state *muart,
	struct muart_buffer *output, enum muart_ptype ptype, u8 seq_no,
	u8 *data, u16 len)
{
	u8 head[2];
	u8 tail[2];
	u16 crc;

	output->start = 0;
	output->end = 0;
	head[0] = ptype;
	head[1] = seq_no;
	if (mcu_uart_buf_enq(output, UART_PPP_FLAG_BYTE)) {
		goto full;
	}
	if (mcu_uart_build_tx_helper(output, head, sizeof(head))) {
		goto full;
	}
	crc = crc16(head, sizeof(head), CRC16_INIT);
	if (data != NULL && len) {
		if (mcu_uart_build_tx_helper(output, data, len)) {
			goto full;
		}
		crc = crc16(data, len, crc);
	}
	put_ua_be16(tail, crc);
	if (mcu_uart_build_tx_helper(output, tail, sizeof(tail))) {
		goto full;
	}
	if (mcu_uart_buf_enq(output, UART_PPP_FLAG_BYTE)) {
		goto full;
	}

	return 0;
full:
	/* if full, cancel building this message */
	return -1;
}

/*
 * Determine next packet to send
 */
static void mcu_uart_send_next(struct mcu_uart_state *muart)
{
	struct hp_buf *sendbuf;
	struct hp_buf *next;
	u8 seq;

	if (muart->mts == MTS_SENDING) {
		/* still transmitting */
		return;
	}

	if (muart->mts == MTS_DATA_FINISH && muart->wait_for_ack) {
		/* just finished transmitting data */
		host_proto_timer_set(&muart->tx_resend_timer,
		    MCU_UART_ACK_WAIT);
	}
	muart->mts = MTS_IDLE;
	muart->tx_buf = NULL;

build_packet:
	if (muart->resend) {
		/* give resends a high priority */
		log_put_mod_sev(MOD_LOG_IO, LOG_SEV_DEBUG,
		    "uart_resend");

		muart->resend = 0;
		muart->tx_data_buf.start = 0;
		if (muart->tx_data_buf.end == 0) {
			goto build_packet;
		}
		muart->tx_buf = &muart->tx_data_buf;
		MUART_STATS(muart, tx_resend);
		MUART_STATS(muart, tx_pkts);
	} else if (muart->ack_queue) {
		/* prioritize acks over tx data */
		sendbuf = muart->ack_queue;
		seq = *(u8 *)sendbuf->payload;

#ifdef MCU_UART_LOG_ACK_SEV
		log_put_mod_sev(MOD_LOG_IO, MCU_UART_LOG_ACK_SEV,
		    "uart_tx ack %#x", seq);
#endif

		mcu_uart_build_tx(muart, &muart->tx_ack_buf, MP_ACK,
		    seq, NULL, 0);
		next = sendbuf->next;
		muart->ack_queue = next;
		sendbuf->next = NULL;
		hp_buf_free(sendbuf);
		sendbuf = NULL;
		muart->tx_buf = &muart->tx_ack_buf;
		MUART_STATS(muart, tx_acks);
		MUART_STATS(muart, tx_pkts);
	} else if (muart->tx_queue_len && !muart->wait_for_ack) {
		sendbuf = muart->tx_queue;
		if (!sendbuf) {
			goto start_tx;
		}
		muart->tx_seq_no++;
		if (muart->first_tx) {
			muart->tx_seq_no = 0;
			muart->first_tx = 0;
		} else if (!muart->tx_seq_no) {
			/* seq no can never be 0 after the first packet */
			muart->tx_seq_no = 1;
		}

#ifdef MCU_UART_DECODE
		host_decode_log("tx", sendbuf->payload, sendbuf->len);
#endif
#ifdef MCU_UART_LOG_BYTES_SEV
		log_put_mod_sev(MOD_LOG_IO, MCU_UART_LOG_BYTES_SEV,
		    "uart_tx seq %#x %zu bytes",
		    muart->tx_seq_no, sendbuf->len);
		log_bytes_in_hex_sev(MOD_LOG_IO, MCU_UART_LOG_BYTES_SEV,
		    sendbuf->payload, sendbuf->len);
#endif

		mcu_uart_build_tx(muart, &muart->tx_data_buf, MP_DATA,
		    muart->tx_seq_no, sendbuf->payload, sendbuf->len);
		muart->tx_queue = sendbuf->next;
		next = sendbuf->next;
		muart->tx_queue = next;
		sendbuf->next = NULL;
		muart->tx_queue_len--;
		muart->retransmit_count = 0;
		hp_buf_free(sendbuf);
		muart->tx_buf = &muart->tx_data_buf;
		MUART_STATS(muart, tx_data);
		MUART_STATS(muart, tx_pkts);
	}
start_tx:
	if (muart->tx_buf && muart->tx_buf->start != muart->tx_buf->end)  {
#ifdef MCU_UART_LOG_RAW
		log_put_mod_sev(MOD_LOG_IO, LOG_SEV_DEBUG2,
		    "uart_raw_tx:");
		log_bytes_in_hex_sev(MOD_LOG_IO, LOG_SEV_DEBUG2,
		    muart->tx_buf->buf + muart->tx_buf->start,
		    muart->tx_buf->end - muart->tx_buf->start);
#endif

		muart->mts = MTS_SENDING;
		serial_start_tx(SERIAL_MCU);
	}
}

/*
 * Transmit callback.
 */
static void mcu_uart_trans_cb(void *arg)
{
	struct mcu_uart_state *muart = arg;

	mcu_uart_send_next(muart);
}

/*
 * Returns the next byte to be transmitted. -1 if nothing to send.
 */
static int mcu_uart_get_tx(void)
{
	struct mcu_uart_state *muart = muart_state;
	u8 *data;

	if (muart->mts != MTS_SENDING) {
		return -1;
	}
	if (muart->tx_buf == NULL) {
		muart->mts = MTS_IDLE;
		return -1;
	}
	data = mcu_uart_buf_deq(muart->tx_buf);
	if (data == NULL) {
		if (muart->tx_buf == &muart->tx_data_buf) {
			muart->mts = MTS_DATA_FINISH;
			muart->wait_for_ack = 1;
		} else {
			muart->mts = MTS_ACK_FINISH;
		}
		host_proto_callback_pend(&muart->tx_callback);
		return -1;
	}
	MUART_STATS(muart, tx_bytes);
	return (int)*data;
}

/*
 * Queue buf for sending
 */
static void mcu_uart_enq_tx(struct hp_buf *bp)
{
	struct mcu_uart_state *muart = muart_state;
	struct hp_buf *prev;

	if (!muart) {
		hp_buf_free(bp);
		return;
	}
	ASSERT(bp->next == NULL);
	ASSERT(bp->len <= ASPI_LEN_MAX);

	prev = muart->tx_queue;
	if (prev == NULL) {
		muart->tx_queue = bp;
	} else {
		while (prev->next != NULL) {
			prev = prev->next;
		}
		prev->next = bp;
	}
	muart->tx_queue_len++;
	if (muart->mts == MTS_IDLE) {
		mcu_uart_send_next(muart);
	}
}

/*
 * Setup UART Interface to MCU
 * Add 1 to circular buf sizes so we know when they're full vs. empty
 */
const struct mcu_dev *mcu_uart_init(void)
{
	struct mcu_uart_state *muart;

	ASSERT(!muart_state);
	muart = calloc(1, sizeof(*muart));
	if (!muart) {
		return NULL;
	}
	ayla_timer_init(&muart->tx_resend_timer, mcu_uart_tx_resend);

	mcu_feature_mask |= MCU_DATAPOINT_CONFIRM;
	mcu_feature_mask_min |= MCU_DATAPOINT_CONFIRM;

	muart->rx_buf.buf = muart->rx_area;
	muart->rx_buf.size = sizeof(muart->rx_area);
	net_callback_init(&muart->rx_callback, mcu_uart_recv_cb, muart);
	net_callback_init(&muart->tx_callback, mcu_uart_trans_cb, muart);

	muart->tx_ack_buf.buf = muart->tx_ack_area;
	muart->tx_ack_buf.size = sizeof(muart->tx_ack_area);

	muart->tx_data_buf.buf = muart->tx_data_area;
	muart->tx_data_buf.size = sizeof(muart->tx_data_area);

	muart->first_tx = 1;
	muart_state = muart;

	serial_init(SERIAL_MCU, 0, mcu_uart_get_tx, mcu_uart_rx_intr);

	return &mcu_uart_ops;
}

static void mcu_uart_show(void)
{
#ifdef MCU_UART_STATS
	struct muart_stats *stats = &muart_state->stats;

	printcli("UART statistics:");
	printcli("  tx: bytes %u pkts: %u data %u acks %u resend %u",
	    stats->tx_bytes, stats->tx_pkts, stats->tx_data,
	    stats->tx_acks, stats->tx_resend);

	printcli("  rx: bytes %u pkts: %u data %u acks %u "
	    "errs: frame %u len %u crc %u ptype %u seq %u cmd %u",
	    stats->rx_bytes, stats->rx_pkts, stats->rx_data, stats->rx_acks,
	    stats->rx_frame_err, stats->rx_len_err,
	    stats->rx_crc_err, stats->rx_ptype_err,
	    stats->rx_seq_err, stats->rx_cmd_err);
#endif
}

static void mcu_uart_noop(void)
{
}

/*
 * Ping.  Repeat buffer back to master.
 */
static void mcu_uart_ping(void *buf, size_t len)
{
}

static const struct mcu_dev mcu_uart_ops = {
	.enq_tx = mcu_uart_enq_tx,
	.set_ads = mcu_uart_noop,
	.clear_ads = mcu_uart_noop,
	.show = mcu_uart_show,
	.ping = mcu_uart_ping,
};
