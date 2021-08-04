/*
 * derived from uqmi -- tiny QMI support implementation
 *
 * Copyright (C) 2014-2015 Felix Fietkau <nbd@openwrt.org>
 *
 * Copyright (C) 2021 Nihal Jere <nihal@nihaljere.xyz>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

/* If you want to see how PDU encoding works, this is the clearest,
 * most concises source that I found: https://en.wikipedia.org/wiki/GSM_03.40 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdu.h"

char
atoh(char hex)
{
	if (isdigit(hex)) return hex - '0';

	return hex - 'A' + 0xA;
}

char *htoa(char *str, char val)
{
	*str = '0' + ((val >> 4) & 0xf);
	if (*str > '9')
		*str = 'A' - 0xA + ((val >> 4) & 0xf);

	str++;

	*str = '0' + (val & 0xf);
	if (*str > '9')
		*str = 'A' - 0xA + (val & 0xf);

	str++;

	return str;
}

char
pairtohex(char *ptr)
{
	assert(isxdigit(ptr[0]));
	assert(isxdigit(ptr[1]));

	return (atoh(ptr[0]) << 4) + atoh(ptr[1]);
}

char
pairtohexswap(char *ptr)
{
	assert(isxdigit(ptr[0]));
	assert(isxdigit(ptr[1]));

	return atoh(ptr[0]) + (atoh(ptr[1]) << 4);
}

char *
hexswap(char *dest, char *src)
{
	*(dest++) = src[1];
	*(dest++) = src[0];
	return dest;
}

static int
put_unicode_char(char *dest, uint16_t c)
{
	if (c < 0x80) {
		*dest = c;
		return 1;
	} else if (c < 0x800) {
		*(dest++) = 0xc0 | ((c >> 6) & 0x1f);
		*dest = 0x80 | (c & 0x3f);
		return 2;
	} else {
		*(dest++) = 0xe0 | ((c >> 12) & 0xf);
		*(dest++) = 0x80 | ((c >> 6) & 0x3f);
		*dest = 0x80 | (c & 0x3f);
		return 3;
	}
}


static int
pdu_decode_7bit_char(char *dest, int len, unsigned char c, bool *escape)
{
	uint16_t conv_0x20[] = {
		0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
		0x00F2, 0x00E7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
		0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
		0x03A3, 0x0398, 0x039E, 0x00A0, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
	};
	uint16_t conv_0x5b[] = {
		0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7, 0x00BF,
	};
	uint16_t conv_0x7b[] = {
		0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0
	};
	int cur_len = 0;
	uint16_t outc;

	fprintf(stderr, " %02x", c);
	dest += len;
	if (*escape) {
		*escape = false;
		switch(c) {
		case 0x0A:
			*dest = 0x0C;
			return 1;
		case 0x14:
			*dest = 0x5E;
			return 1;
		case 0x28:
			*dest = 0x7B;
			return 1;
		case 0x29:
			*dest = 0x7D;
			return 1;
		case 0x2F:
			*dest = 0x5C;
			return 1;
		case 0x3C:
			*dest = 0x5B;
			return 1;
		case 0x3D:
			*dest = 0x7E;
			return 1;
		case 0x3E:
			*dest = 0x5D;
			return 1;
		case 0x40:
			*dest = 0x7C;
			return 1;
		case 0x65:
			outc = 0x20AC;
			goto out;
		case 0x1B:
			goto normal;
		default:
			/* invalid */
			*(dest++) = conv_0x20[0x1B];
			cur_len++;
			goto normal;
		}
	}

	if (c == 0x1b) {
		*escape = true;
		return 0;
	}

normal:
	if (c < 0x20)
		outc = conv_0x20[(int) c];
	else if (c == 0x40)
		outc = 0x00A1;
	else if (c >= 0x5b && c <= 0x60)
		outc = conv_0x5b[c - 0x5b];
	else if (c >= 0x7b && c <= 0x7f)
		outc = conv_0x7b[c - 0x7b];
	else
		outc = c;

out:
	return cur_len + put_unicode_char(dest, outc);
}

static int
pdu_decode_7bit_str(char *dest, const unsigned char *data, int data_len, int bit_offset)
{
	bool escape = false;
	int len = 0;
	int i;

	fprintf(stderr, "Raw text:");
	for (i = 0; i < data_len; i++) {
		int pos = (i + bit_offset) % 7;

		if (pos == 0) {
			len += pdu_decode_7bit_char(dest, len, data[i] & 0x7f, &escape);
		} else {
			if (i)
				len += pdu_decode_7bit_char(dest, len,
				                            (data[i - 1] >> (7 + 1 - pos)) |
				                            ((data[i] << pos) & 0x7f), &escape);

			if (pos == 6)
				len += pdu_decode_7bit_char(dest, len, (data[i] >> 1) & 0x7f,
				                            &escape);
		}
	}
	dest[len] = 0;
	fprintf(stderr, "\n");
	return len;
}

/*
static void decode_7bit_field(char *name, const unsigned char *data, int data_len, int bit_offset)
{
	char *dest = blobmsg_alloc_string_buffer(&status, name, 3 * data_len + 2);
	pdu_decode_7bit_str(dest, data, CEILDIV(data_len * 7, 8), bit_offset);
	dest[data_len] = 0;
	blobmsg_add_string_buffer(&status);
}
*/

static char *pdu_add_semioctet(char *str, char val)
{
	*str = '0' + (val & 0xf);
	if (*str > '9')
		*str = 'A' - 0xA + (val & 0xf);

	str++;

	*str = '0' + ((val >> 4) & 0xf);
	if (*str > '9')
		*str = 'A' - 0xA + ((val >> 4) & 0xf);

	str++;

	return str;
}

static void
pdu_decode_address(char *str, unsigned char *data, int len)
{
	unsigned char toa;
	char *temp = str;

	toa = pairtohex(data);
	data += 2;
	len -= 2;
	switch (toa & 0x70) {
	case 0x50:
		pdu_decode_7bit_str(str, data, len, 0);
		return;
	case 0x10:
		*(str++) = '+';
		/* fall through */
	default:
		while (len > 0) {
			str = hexswap(str, data);
			data += 2;
			len -= 2;
		}
	}

	if (*(str - 1) == 'F')
		*(str - 1) = 0;
	else
		*str = 0;
}

int
decode_ud(struct message *msg, char *raw, int len, enum dcs dcs)
{
	fprintf(stderr, "len = %d\n", len);
	switch (dcs) {
	case DCS_GSM: {
		char *data = calloc(len, 1);
		if (!data)
			return -1;

		int i = 0, c = 0;
		while (c < len) {
			data[i] = pairtohex(raw);
			raw += 2;
			c += 2;
			i++;
		}

		pdu_decode_7bit_str(msg->data, data, len / 2, 0);
		free(data);
		break;
	}
	default:
		fprintf(stderr, "unknown format %d\n", dcs);
		return -1;
	}

}

static int decode_udh(struct sms_deliver_msg *msg, const unsigned char *data)
{
	const unsigned char *end;
	unsigned int type, len, udh_len;

	udh_len = *(data++);
	end = data + udh_len;
	while (data < end) {
		const unsigned char *val;

		type = data[0];
		len = data[1];
		val = &data[2];
		data += 2 + len;
		if (data > end)
			break;

		switch (type) {
		case 0x00:
			msg->udh.ref = val[0];
			msg->udh.parts = val[1];
			msg->udh.part = val[2];
			break;
		case 0x08:
			msg->udh.ref = val[0] << 8 | val[1];
			msg->udh.parts = val[2];
			msg->udh.part = val[3];
			break;
		default:
			// TODO handle unknown UDH
			break;
		}
	}

	return udh_len + 1;
}

int
decode_sms_deliver(struct sms_deliver_msg *msg, char *raw, size_t len, char header)
{
	msg->mms = !!(header & 0x4);
	msg->udhi = (header >> 6) & 1;

	// TODO loop prevention, status report indication?
	raw += 2;
	len -= 2;

	msg->sender.len = pairtohex(raw);
	fprintf(stderr, "msg->: %d\n", msg->sender.len);
	raw += 2;
	len -= 2;

	char *str = calloc(1, msg->sender.len * 2);
	if (str == NULL)
		return -1;

	pdu_decode_address(str, raw, msg->sender.len + 2);

	fprintf(stderr, "smsdeliver: %s\n", str);

	if (strlen(str) > 15) {
		fprintf(stderr, "phone number too long\n");
		return -1;
	}

	strcpy(msg->sender.number, str);
	free(str);

	raw += 2 + (msg->sender.len & 1 ? msg->sender.len + 1 : msg->sender.len);
	len -= 2 + (msg->sender.len & 1 ? msg->sender.len + 1 : msg->sender.len);
	fprintf(stderr, "left: %s\n", raw);

	msg->pid = pairtohex(raw);
	raw += 2; len -= 2;

	msg->dcs = pairtohex(raw);
	raw += 2; len -= 2;

	msg->date.year = pairtohexswap(raw);
	raw += 2; len -= 2;

	msg->date.month = pairtohexswap(raw);
	raw += 2; len -= 2;

	msg->date.day = pairtohexswap(raw);
	raw += 2; len -= 2;

	msg->time.hour = pairtohex(raw);
	raw += 2; len -= 2;

	msg->time.minute = pairtohex(raw);
	raw += 2; len -= 2;

	msg->time.second = pairtohex(raw);
	raw += 2; len -= 2;

	msg->time.tz = pairtohex(raw);
	raw += 2; len -= 2;

	msg->msg.len = pairtohex(raw);
	raw += 2; len -= 2;

	msg->msg.data = calloc(1, msg->msg.len);
	if (!msg->msg.data)
		return -1;

	int nlen;
	switch (msg->udhi) {
	case 1:
		/* TODO THIS NEEDS TO BE TESTED */
		nlen = decode_udh(msg, raw);
		raw += nlen;
		len -= nlen;
	/* fallthrough */
	default:
		decode_ud(&msg->msg, raw, len, msg->dcs);
	}
	fprintf(stderr, "%s\n", msg->msg);
	return 0;
}

int
decode_pdu(struct pdu_msg *pdu_msg, char *raw)
{
	size_t len = strlen(raw);
	pdu_msg->smsc.len = pairtohex(raw);
	raw += 2;
	len -= 2;

	fprintf(stderr, "smsc.len: %d\n", pdu_msg->smsc.len);

	char *str = calloc(1, pdu_msg->smsc.len * 2);
	if (str == NULL)
		return -1;

	pdu_decode_address(str, raw, pdu_msg->smsc.len * 2);

	if (strlen(str) > 15) {
		fprintf(stderr, "phone number too long\n");
		return -1;
	}

	fprintf(stderr, "str: %s\n", str);
	strcpy(pdu_msg->smsc.number, str);
	free(str);

	raw += pdu_msg->smsc.len * 2;
	len -= pdu_msg->smsc.len * 2;

	char header = pairtohex(raw);
	pdu_msg->smstype = header & 0x3;
	switch (pdu_msg->smstype) {
	case SMS_DELIVER:
		decode_sms_deliver(&pdu_msg->d.d, raw, len, header);
		break;
	}
	return 0;
}

static int
pdu_encode_7bit_str(unsigned char *data, const char *str)
{
	unsigned char c;
	int len = 0;
	int ofs = 0;

	while(1) {
		c = *(str++) & 0x7f;
		if (!c)
			break;

		if (data) {
			switch(ofs) {
			case 0:
				data[len] = c;
				break;
			default:
				data[len++] |= c << (8 - ofs);
				data[len] = c >> ofs;
				break;
			}
		} else {
			if (ofs != 0)
				len++;
		}

		ofs = (ofs + 1) % 8;
	}

	return len + 1;
}

static int
pdu_encode_semioctet(unsigned char *dest, const char *str)
{
	int len = 0;
	bool lower = true;

	while (*str) {
		char digit = *str - '0';

		if (dest) {
			if (lower) {
				dest[len] = 0xf0 | digit;
			} else {
				dest[len++] &= (digit << 4) | 0xf;
			}
		} else {
			len += !lower;
		}

		lower = !lower;
		str++;
	}

	return lower ? len : (len + 1);
}

static int
pdu_encode_number(unsigned char *dest, const char *str, bool smsc)
{
	unsigned char format;
	bool ascii = false;
	int len = 0;
	int i;

	if (dest)
		dest[len] = 0;
	len++;
	if (*str == '+') {
		str++;
		format = 0x91;
	} else {
		format = 0x81;
	}

	for (i = 0; str[i]; i++) {
		if (str[i] >= '0' && str[i] <= '9')
			continue;

		ascii = true;
		break;
	}

	if (ascii)
		format |= 0x40;

	if (dest)
		dest[len] = format;
	len++;
	if (!ascii)
		len += pdu_encode_semioctet(dest ? &dest[len] : NULL, str);
	else
		len += pdu_encode_7bit_str(dest ? &dest[len] : NULL, str);

	if (dest) {
		if (smsc)
			dest[0] = len - 1;
		else
			dest[0] = strlen(str);
	}

	return len;
}

int
encode_pdu(char *dest, char *number, char *message)
{
	int len = 0;

	char *ptr = dest;
	// may want to set bit 6 for multi-part message
	if (dest)
		dest[len] = 1;
	len++;

	// message reference
	if (dest)
		dest[len] = 0;
	len++;

	len += pdu_encode_number(dest ? &dest[len] : NULL, number, 0);

	// PID
	if (dest)
		dest[len] = 0;
	len++;

	// DCS - needs to be determined from message - XXX don't assume GSM
	if (dest)
		dest[len] = 0;
	len++;

	// UDL - user data length - for now we just assume it is the number of octets in the message
	if (dest)
		dest[len] = strlen(message);
	len++;

	// User Data
	len += pdu_encode_7bit_str(dest ? &dest[len] : NULL, message);

	return len;
}
