/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "utils/common/exec.h"

#include <stdlib.h>			// free
#include <time.h>			// localtime_r

#include "libknot/libknot.h"
#include "common/lists.h"		// list
#include "common/print.h"		// txt_print
#include "common/errcode.h"		// KNOT_EOK
#include "common/descriptor.h"		// KNOT_RRTYPE_
#include "utils/common/msg.h"		// WARN
#include "utils/common/params.h"	// params_t
#include "utils/common/netio.h"		// send_msg
#include "libknot/dnssec/sig0.h"
#include "libknot/dnssec/random.h"

static knot_lookup_table_t rtypes[] = {
	{ KNOT_RRTYPE_A,      "has IPv4 address" },
	{ KNOT_RRTYPE_NS,     "nameserver is" },
	{ KNOT_RRTYPE_CNAME,  "is an alias for" },
	{ KNOT_RRTYPE_SOA,    "start of authority is" },
	{ KNOT_RRTYPE_PTR,    "points to" },
	{ KNOT_RRTYPE_MX,     "mail is handled by" },
	{ KNOT_RRTYPE_TXT,    "description is" },
	{ KNOT_RRTYPE_AAAA,   "has IPv6 address" },
	{ KNOT_RRTYPE_LOC,    "location is" },
	{ KNOT_RRTYPE_DS,     "delegation signature is" },
	{ KNOT_RRTYPE_SSHFP,  "SSH fingerprint is" },
	{ KNOT_RRTYPE_RRSIG,  "RR set signature is" },
	{ KNOT_RRTYPE_DNSKEY, "DNSSEC key is" },
	{ KNOT_RRTYPE_TLSA,   "has TLS certificate" },
	{ 0, NULL }
};

static void print_header(const knot_pkt_t *packet, const style_t *style)
{
	char    flags[64] = "";
	uint8_t rcode_id, opcode_id;
	const char *rcode_str = "NULL";
	const char *opcode_str = "NULL";
	knot_lookup_table_t *rcode, *opcode;

	// Get codes.
	rcode_id = knot_wire_get_rcode(packet->wire);
	rcode = knot_lookup_by_id(knot_rcode_names, rcode_id);
	if (rcode != NULL) {
		rcode_str = rcode->name;
	}

	opcode_id = knot_wire_get_opcode(packet->wire);
	opcode = knot_lookup_by_id(knot_opcode_names, opcode_id);
	if (opcode != NULL) {
		opcode_str = opcode->name;
	}

	// Get flags.
	if (knot_wire_get_qr(packet->wire) != 0) {
		strcat(flags, " qr");
	}
	if (knot_wire_get_aa(packet->wire) != 0) {
		strcat(flags, " aa");
	}
	if (knot_wire_get_tc(packet->wire) != 0) {
		strcat(flags, " tc");
	}
	if (knot_wire_get_rd(packet->wire) != 0) {
		strcat(flags, " rd");
	}
	if (knot_wire_get_ra(packet->wire) != 0) {
		strcat(flags, " ra");
	}
	if (knot_wire_get_z(packet->wire) != 0) {
		strcat(flags, " z");
	}
	if (knot_wire_get_ad(packet->wire) != 0) {
		strcat(flags, " ad");
	}
	if (knot_wire_get_cd(packet->wire) != 0) {
		strcat(flags, " cd");
	}

	uint16_t id = knot_wire_get_id(packet->wire);
	uint16_t qdcount = knot_wire_get_qdcount(packet->wire);
	uint16_t ancount = knot_wire_get_ancount(packet->wire);
	uint16_t nscount = knot_wire_get_nscount(packet->wire);
	uint16_t arcount = knot_wire_get_arcount(packet->wire);

	if (knot_pkt_have_tsig(packet)) {
		arcount++;
	}

	// Print formatted info.
	switch (style->format) {
	case FORMAT_NSUPDATE:
		printf(";; ->>HEADER<<- opcode: %s; status: %s; id: %u\n"
		       ";; Flags:%1s; "
		       "ZONE: %u; PREREQ: %u; UPDATE: %u; ADDITIONAL: %u\n",
		       opcode_str, rcode_str, id, flags, qdcount, ancount,
		       nscount, arcount);
		break;
	default:
		printf(";; ->>HEADER<<- opcode: %s; status: %s; id: %u\n"
		       ";; Flags:%1s; "
		       "QUERY: %u; ANSWER: %u; AUTHORITY: %u; ADDITIONAL: %u\n",
		       opcode_str, rcode_str, id, flags, qdcount, ancount,
		       nscount, arcount);
		break;
	}
}

static void print_footer(const size_t total_len,
                         const size_t msg_count,
                         const size_t rr_count,
                         const net_t  *net,
                         const float  elapsed,
                         time_t       exec_time,
                         const bool   incoming)
{
	struct tm tm;
	char      date[64];

	// Get current timestamp.
	if (exec_time == 0) {
		exec_time = time(NULL);
	}

	// Create formated date-time string.
	localtime_r(&exec_time, &tm);
	strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &tm);

	// Print messages statistics.
	if (incoming) {
		printf(";; Received %zu B", total_len);
	} else {
		printf(";; Sent %zu B", total_len);
	}

	// If multimessage (XFR) print additional statistics.
	if (msg_count > 0) {
		printf(" (%zu messages, %zu records)\n", msg_count, rr_count);
	} else {
		printf("\n");
	}
	// Print date.
	printf(";; Time %s\n", date);

	// Print connection statistics.
	if (net != NULL) {
		if (incoming) {
			printf(";; From %s", net->remote_str);
		} else {
			printf(";; To %s", net->remote_str);
		}

		if (elapsed >= 0) {
			printf(" in %.1f ms\n", elapsed);
		} else {
			printf("\n");
		}
	}
}

static void print_section_opt(const knot_rrset_t *rr)
{
	uint8_t             ext_rcode_id = knot_edns_get_ext_rcode(rr);
	const char          *ext_rcode_str = "NULL";
	knot_lookup_table_t *ext_rcode;

	ext_rcode = knot_lookup_by_id(knot_rcode_names, ext_rcode_id);
	if (ext_rcode != NULL) {
		ext_rcode_str = ext_rcode->name;
	}

	printf("Version: %u; flags: %s; UDP size: %u B, status: %s\n",
	       knot_edns_get_version(rr),
	       (knot_edns_do(rr) != 0) ? "do" : "",
	       knot_edns_get_payload(rr),
	       ext_rcode_str);

	knot_rdata_t *rdata = knot_rdataset_at(&rr->rrs, 0);
	assert(rdata != NULL);

	uint16_t data_len = knot_rdata_rdlen(rdata);
	uint8_t *data = knot_rdata_data(rdata);
	int pos = 0;

	while (pos < data_len - 4) {
		uint16_t opt_code = knot_wire_read_u16(data + pos);
		uint16_t opt_len = knot_wire_read_u16(data + pos + 2);
		uint8_t *opt_data = data + pos + 4;

		if (opt_code == KNOT_EDNS_OPTION_NSID) {
			printf(";; NSID: ");
			short_hex_print(opt_data, opt_len);
			printf(";;     :  ");
			txt_print(opt_data, opt_len);
		} else {
			printf(";; Option (%u): ", opt_code);
			short_hex_print(opt_data, opt_len);
		}

		printf("\n");
		pos += 4 + opt_len;
	}
}

static void print_section_question(const knot_dname_t *owner,
                                   const uint16_t     qclass,
                                   const uint16_t     qtype,
                                   const style_t      *style)
{
	size_t buflen = 8192;
	char   *buf = calloc(buflen, 1);

	// Don't print zero TTL.
	knot_dump_style_t qstyle = style->style;
	qstyle.empty_ttl = true;

	knot_rrset_t *question = knot_rrset_new(owner, qtype, qclass, NULL);

	if (knot_rrset_txt_dump_header(question, 0, buf, buflen, &qstyle) < 0) {
		WARN("can't print whole question section\n");
	}

	printf("%s\n", buf);

	knot_rrset_free(&question, NULL);
	free(buf);
}

static void print_section_full(const knot_rrset_t *rrsets,
                               const uint16_t     count,
                               const style_t      *style,
                               const bool         no_tsig)
{
	size_t buflen = 8192;
	char   *buf = calloc(buflen, 1);

	for (size_t i = 0; i < count; i++) {
		// Ignore OPT records.
		if (rrsets[i].type == KNOT_RRTYPE_OPT) {
			continue;
		}

		// Exclude TSIG record.
		if (no_tsig && rrsets[i].type == KNOT_RRTYPE_TSIG) {
			continue;
		}

		while (knot_rrset_txt_dump(&rrsets[i], buf, buflen,
		                           &(style->style)) < 0) {
			buflen += 4096;
			// Oversize protection.
			if (buflen > 100000) {
				WARN("can't print whole section\n");
				break;
			}

			char *newbuf = realloc(buf, buflen);
			if (newbuf == NULL) {
				WARN("can't print whole section\n");
				break;
			}
			buf = newbuf;
		}
		printf("%s", buf);
	}

	free(buf);
}

static void print_section_dig(const knot_rrset_t *rrsets,
                              const uint16_t     count,
                              const style_t      *style)
{
	size_t buflen = 8192;
	char   *buf = calloc(buflen, 1);

	for (size_t i = 0; i < count; i++) {
		const knot_rrset_t *rrset = &rrsets[i];
		uint16_t rrset_rdata_count = rrset->rrs.rr_count;
		for (uint16_t j = 0; j < rrset_rdata_count; j++) {
			while (knot_rrset_txt_dump_data(rrset, j, buf, buflen,
			                                &(style->style)) < 0) {
				buflen += 4096;
				// Oversize protection.
				if (buflen > 100000) {
					WARN("can't print whole section\n");
					break;
				}

				char *newbuf = realloc(buf, buflen);
				if (newbuf == NULL) {
					WARN("can't print whole section\n");
					break;
				}
				buf = newbuf;
			}
			printf("%s\n", buf);
		}
	}

	free(buf);
}

static void print_section_host(const knot_rrset_t *rrsets,
                               const uint16_t     count,
                               const style_t      *style)
{
	size_t buflen = 8192;
	char   *buf = calloc(buflen, 1);

	for (size_t i = 0; i < count; i++) {
		const knot_rrset_t  *rrset = &rrsets[i];
		knot_lookup_table_t *descr;
		char                type[32] = "NULL";
		char                *owner;

		owner = knot_dname_to_str(rrset->owner);
		if (style->style.ascii_to_idn != NULL) {
			style->style.ascii_to_idn(&owner);
		}
		descr = knot_lookup_by_id(rtypes, rrset->type);

		uint16_t rrset_rdata_count = rrset->rrs.rr_count;
		for (uint16_t j = 0; j < rrset_rdata_count; j++) {
			if (rrset->type == KNOT_RRTYPE_CNAME &&
			    style->hide_cname) {
				continue;
			}

			while (knot_rrset_txt_dump_data(rrset, j, buf, buflen,
			                                &(style->style)) < 0) {
				buflen += 4096;
				// Oversize protection.
				if (buflen > 100000) {
					WARN("can't print whole section\n");
					break;
				}

				char *newbuf = realloc(buf, buflen);
				if (newbuf == NULL) {
					WARN("can't print whole section\n");
					break;
				}
				buf = newbuf;
			}

			if (descr != NULL) {
				printf("%s %s %s\n", owner, descr->name, buf);
			} else {
				knot_rrtype_to_string(rrset->type, type,
						      sizeof(type));
				printf("%s has %s record %s\n",
				       owner, type, buf);
			}
		}

		free(owner);
	}

	free(buf);
}

static void print_error_host(const uint8_t    code,
                             const knot_pkt_t *packet,
                             const style_t    *style)
{
	const char *rcode_str = "NULL";
	char type[32] = "NULL";
	char *owner;

	knot_lookup_table_t *rcode;

	owner = knot_dname_to_str(knot_pkt_qname(packet));
	if (style->style.ascii_to_idn != NULL) {
		style->style.ascii_to_idn(&owner);
	}
	rcode = knot_lookup_by_id(knot_rcode_names, code);
	if (rcode != NULL) {
		rcode_str = rcode->name;
	}
	knot_rrtype_to_string(knot_pkt_qtype(packet), type, sizeof(type));

	if (code == KNOT_RCODE_NOERROR) {
		printf("Host %s has no %s record\n", owner, type);
	} else {
		printf("Host %s type %s error: %s\n", owner, type, rcode_str);
	}

	free(owner);
}

knot_pkt_t* create_empty_packet(const size_t max_size)
{
	// Create packet skeleton.
	knot_pkt_t *packet = knot_pkt_new(NULL, max_size, NULL);
	if (packet == NULL) {
		DBG_NULL;
		return NULL;
	}

	// Set random sequence id.
	knot_wire_set_id(packet->wire, knot_random_uint16_t());

	return packet;
}

void print_header_xfr(const knot_pkt_t *packet, const style_t  *style)
{
	if (style == NULL) {
		DBG_NULL;
		return;
	}

	char xfr[16] = "AXFR";

	switch (knot_pkt_qtype(packet)) {
	case KNOT_RRTYPE_AXFR:
		break;
	case KNOT_RRTYPE_IXFR:
		xfr[0] = 'I';
		break;
	default:
		return;
	}

	if (style->show_header) {
		char *owner = knot_dname_to_str(knot_pkt_qname(packet));
		if (style->style.ascii_to_idn != NULL) {
			style->style.ascii_to_idn(&owner);
		}
		if (owner != NULL) {
			printf(";; %s for %s\n", xfr, owner);
			free(owner);
		}
	}
}

void print_data_xfr(const knot_pkt_t *packet,
                    const style_t    *style)
{
	if (packet == NULL || style == NULL) {
		DBG_NULL;
		return;
	}

	const knot_pktsection_t *answers = knot_pkt_section(packet,
	                                                    KNOT_ANSWER);

	switch (style->format) {
	case FORMAT_DIG:
		print_section_dig(answers->rr, answers->count, style);
		break;
	case FORMAT_HOST:
		print_section_host(answers->rr, answers->count, style);
		break;
	case FORMAT_FULL:
		print_section_full(answers->rr, answers->count, style, true);

		// Print TSIG record.
		if (style->show_tsig && knot_pkt_have_tsig(packet)) {
			print_section_full(packet->tsig_rr, 1, style, false);
		}
		break;
	default:
		break;
	}
}

void print_footer_xfr(const size_t  total_len,
                      const size_t  msg_count,
                      const size_t  rr_count,
                      const net_t   *net,
                      const float   elapsed,
                      const time_t  exec_time,
                      const style_t *style)
{
	if (style == NULL) {
		DBG_NULL;
		return;
	}

	if (style->show_footer) {
		print_footer(total_len, msg_count, rr_count, net, elapsed,
		             exec_time, true);
	}
}

void print_packet(const knot_pkt_t *packet,
                  const net_t      *net,
                  const size_t     size,
                  const float      elapsed,
                  const time_t     exec_time,
                  const bool       incoming,
                  const style_t    *style)
{
	if (packet == NULL || style == NULL) {
		DBG_NULL;
		return;
	}

	const knot_pktsection_t *answers = knot_pkt_section(packet,
	                                                    KNOT_ANSWER);
	const knot_pktsection_t *authority = knot_pkt_section(packet,
	                                                      KNOT_AUTHORITY);
	const knot_pktsection_t *additional = knot_pkt_section(packet,
	                                                       KNOT_ADDITIONAL);

	uint8_t rcode = knot_wire_get_rcode(packet->wire);
	uint16_t qdcount = knot_wire_get_qdcount(packet->wire);
	uint16_t ancount = knot_wire_get_ancount(packet->wire);
	uint16_t nscount = knot_wire_get_nscount(packet->wire);
	uint16_t arcount = knot_wire_get_arcount(packet->wire);

	// Disable additionals printing if there are no other records.
	// OPT record may be placed anywhere within additionals!
	if (knot_pkt_have_edns(packet) && arcount == 1) {
		arcount = 0;
	}

	// Print packet information header.
	if (style->show_header) {
		print_header(packet, style);
	}

	// Print EDNS section.
	if (style->show_edns && knot_pkt_have_edns(packet)) {
		printf("\n;; EDNS PSEUDOSECTION:\n;; ");
		print_section_opt(packet->opt_rr);
	}

	// Print DNS sections.
	switch (style->format) {
	case FORMAT_DIG:
		if (ancount > 0) {
			print_section_dig(answers->rr, ancount, style);
		}
		break;
	case FORMAT_HOST:
		if (ancount > 0) {
			print_section_host(answers->rr, ancount, style);
		} else {
			print_error_host(rcode, packet, style);
		}
		break;
	case FORMAT_NSUPDATE:
		if (style->show_question && qdcount > 0) {
			printf("\n;; ZONE SECTION:\n;; ");
			print_section_question(knot_pkt_qname(packet),
			                       knot_pkt_qclass(packet),
			                       knot_pkt_qtype(packet),
			                       style);
		}

		if (style->show_answer && ancount > 0) {
			printf("\n;; PREREQUISITE SECTION:\n");
			print_section_full(answers->rr, ancount, style, true);
		}

		if (style->show_authority && nscount > 0) {
			printf("\n;; UPDATE SECTION:\n");
			print_section_full(authority->rr, nscount, style, true);
		}

		if (style->show_additional && arcount > 0) {
			printf("\n;; ADDITIONAL DATA:\n");
			print_section_full(additional->rr, arcount, style, true);
		}
		break;
	case FORMAT_FULL:
		if (style->show_question && qdcount > 0) {
			printf("\n;; QUESTION SECTION:\n;; ");
			print_section_question(knot_pkt_qname(packet),
			                       knot_pkt_qclass(packet),
			                       knot_pkt_qtype(packet),
			                       style);
		}

		if (style->show_answer && ancount > 0) {
			printf("\n;; ANSWER SECTION:\n");
			print_section_full(answers->rr, ancount, style, true);
		}

		if (style->show_authority && nscount > 0) {
			printf("\n;; AUTHORITY SECTION:\n");
			print_section_full(authority->rr, nscount, style, true);
		}

		if (style->show_additional && arcount > 0) {
			printf("\n;; ADDITIONAL SECTION:\n");
			print_section_full(additional->rr, arcount, style, true);
		}
		break;
	default:
		break;
	}

	// Print TSIG section.
	if (style->show_tsig && knot_pkt_have_tsig(packet)) {
		printf("\n;; TSIG PSEUDOSECTION:\n");
		print_section_full(packet->tsig_rr, 1, style, false);
	}

	// Print packet statistics.
	if (style->show_footer) {
		printf("\n");
		print_footer(size, 0, 0, net, elapsed, exec_time, incoming);
	}
}

void free_sign_context(sign_context_t *ctx)
{
	if (ctx == NULL) {
		DBG_NULL;
		return;
	}

	if (ctx->tsig_key.name) {
		knot_tsig_key_free(&ctx->tsig_key);
	}

	if (ctx->dnssec_key.name) {
		knot_dnssec_key_free(&ctx->dnssec_key);
	}

	free(ctx->digest);

	memset(ctx, '\0', sizeof(sign_context_t));
}

int sign_packet(knot_pkt_t              *pkt,
                sign_context_t          *sign_ctx,
                const knot_key_params_t *key_params)
{
	int result;

	if (pkt == NULL || sign_ctx == NULL || key_params == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	uint8_t *wire = pkt->wire;
	size_t  *wire_size = &pkt->size;
	size_t  max_size = pkt->max_size;

	switch (knot_get_key_type(key_params)) {
	case KNOT_KEY_TSIG:
	{
		result = knot_tsig_key_from_params(key_params,
		                                   &sign_ctx->tsig_key);
		if (result != KNOT_EOK) {
			return result;
		}

		knot_tsig_key_t *key = &sign_ctx->tsig_key;

		sign_ctx->digest_size = knot_tsig_digest_length(key->algorithm);
		sign_ctx->digest = malloc(sign_ctx->digest_size);

		knot_pkt_reserve(pkt, tsig_wire_maxsize(key));

		result = knot_tsig_sign(wire, wire_size, max_size, NULL, 0,
		                        sign_ctx->digest, &sign_ctx->digest_size,
		                        key, 0, 0);

		return result;
	}
	case KNOT_KEY_DNSSEC:
	{
		result = knot_dnssec_key_from_params(key_params,
		                                     &sign_ctx->dnssec_key);
		if (result != KNOT_EOK) {
			return result;
		}

		knot_dnssec_key_t *key = &sign_ctx->dnssec_key;
		result = knot_sig0_sign(wire, wire_size, max_size, key);

		return result;
	}
	default:
		return KNOT_DNSSEC_EINVALID_KEY;
	}
}

int verify_packet(const knot_pkt_t        *pkt,
                  const sign_context_t    *sign_ctx,
                  const knot_key_params_t *key_params)
{
	int result;

	if (pkt == NULL || sign_ctx == NULL || key_params == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	const uint8_t *wire = pkt->wire;
	const size_t  *wire_size = &pkt->size;

	switch (knot_get_key_type(key_params)) {
	case KNOT_KEY_TSIG:
	{
		if (pkt->tsig_rr == NULL) {
			return KNOT_ENOTSIG;
		}

		result = knot_tsig_client_check(pkt->tsig_rr, wire, *wire_size,
		                                sign_ctx->digest,
		                                sign_ctx->digest_size,
		                                &sign_ctx->tsig_key, 0);

		return result;
	}
	case KNOT_KEY_DNSSEC:
	{
		// Uses public key cryptography, server cannot sign the
		// response, because the private key should be known only
		// to the client.
		return KNOT_EOK;
	}
	default:
		return KNOT_EINVAL;
	}
}
