/* IPSec VPN client compatible with Cisco equipment.
   Copyright (C) 2002      Geoffrey Keating
   Copyright (C) 2003-2004 Maurice Massar
   Copyright (C) 2004      Tomas Mraz
   Copyright (C) 2004      Martin von Gagern

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>

#include <gcrypt.h>

#include "sysdep.h"
#include "config.h"
#include "isakmp-pkt.h"
#include "math_group.h"
#include "dh.h"
#include "vpnc.h"

#define ISAKMP_PORT (500)

struct sa_block oursa[1];

static int sockfd = -1;
static struct sockaddr *dest_addr;
static uint16_t local_port; /* in network byte order */
static uint16_t encap_mode = IPSEC_ENCAP_TUNNEL;
static int timeout = 5000; /* 5 seconds */
static uint8_t *resend_hash = NULL;

static uint8_t r_packet[2048];
static ssize_t r_length;

extern void vpnc_doit(unsigned long tous_spi,
	const unsigned char *tous_key,
	struct sockaddr_in *tous_dest,
	unsigned long tothem_spi,
	const unsigned char *tothem_key,
	struct sockaddr_in *tothem_dest,
	int tun_fd, int md_algo, int cry_algo,
	uint8_t * kill_packet_p, size_t kill_packet_size_p,
	struct sockaddr *kill_dest_p,
	uint16_t encap_mode, int isakmp_fd,
	const char *pidfile);

extern int find_local_addr(struct sockaddr_in *dest,
	struct sockaddr_in *source);

enum supp_algo_key {
	SUPP_ALGO_NAME,
	SUPP_ALGO_MY_ID,
	SUPP_ALGO_IKE_SA,
	SUPP_ALGO_IPSEC_SA
};

enum algo_group {
	SUPP_ALGO_DH_GROUP,
	SUPP_ALGO_HASH,
	SUPP_ALGO_CRYPT,
	SUPP_ALGO_AUTH
};

supported_algo_t supp_dh_group[] = {
	{"nopfs", 0, 0, 0, 0},
	{"dh1", OAKLEY_GRP_1, IKE_GROUP_MODP_768, IKE_GROUP_MODP_768, 0},
	{"dh2", OAKLEY_GRP_2, IKE_GROUP_MODP_1024, IKE_GROUP_MODP_1024, 0},
	{"dh5", OAKLEY_GRP_5, IKE_GROUP_MODP_1536, IKE_GROUP_MODP_1536, 0},
	/*{ "dh7", OAKLEY_GRP_7, IKE_GROUP_EC2N_163K, IKE_GROUP_EC2N_163K, 0 } note: code missing */
	{NULL, 0, 0, 0, 0}
};

supported_algo_t supp_hash[] = {
	{"md5", GCRY_MD_MD5, IKE_HASH_MD5, IPSEC_AUTH_HMAC_MD5, 0},
	{"sha1", GCRY_MD_SHA1, IKE_HASH_SHA, IPSEC_AUTH_HMAC_SHA, 0},
	{NULL, 0, 0, 0, 0}
};

supported_algo_t supp_crypt[] = {
	/*note: working, but not recommended */
	{"des", GCRY_CIPHER_DES, IKE_ENC_DES_CBC, ISAKMP_IPSEC_ESP_DES, 0},
	{"3des", GCRY_CIPHER_3DES, IKE_ENC_3DES_CBC, ISAKMP_IPSEC_ESP_3DES, 0},
	{"aes128", GCRY_CIPHER_AES128, IKE_ENC_AES_CBC, ISAKMP_IPSEC_ESP_AES, 128},
	{"aes192", GCRY_CIPHER_AES192, IKE_ENC_AES_CBC, ISAKMP_IPSEC_ESP_AES, 192},
	{"aes256", GCRY_CIPHER_AES256, IKE_ENC_AES_CBC, ISAKMP_IPSEC_ESP_AES, 256},
	{NULL, 0, 0, 0, 0}
};

supported_algo_t supp_auth[] = {
	{"psk", 0, IKE_AUTH_PRESHARED, 0, 0},
	{"psk+xauth", 0, IKE_AUTH_XAUTHInitPreShared, 0, 0},
	{NULL, 0, 0, 0, 0}
};

const supported_algo_t *get_algo(enum algo_group what, enum supp_algo_key key, int id,
	const char *name, int keylen)
{
	supported_algo_t *sa = NULL;
	int i = 0, val = 0;
	const char *valname = NULL;

	switch (what) {
	case SUPP_ALGO_DH_GROUP:
		sa = supp_dh_group;
		break;
	case SUPP_ALGO_HASH:
		sa = supp_hash;
		break;
	case SUPP_ALGO_CRYPT:
		sa = supp_crypt;
		break;
	case SUPP_ALGO_AUTH:
		sa = supp_auth;
		break;
	default:
		abort();
	}

	for (i = 0; sa[i].name != NULL; i++) {
		switch (key) {
		case SUPP_ALGO_NAME:
			valname = sa[i].name;
			break;
		case SUPP_ALGO_MY_ID:
			val = sa[i].my_id;
			break;
		case SUPP_ALGO_IKE_SA:
			val = sa[i].ike_sa_id;
			break;
		case SUPP_ALGO_IPSEC_SA:
			val = sa[i].ipsec_sa_id;
			break;
		default:
			abort();
		}
		if ((key == SUPP_ALGO_NAME) ? !strcasecmp(name, valname) : (val == id))
			if (keylen == sa[i].keylen)
				return sa + i;
	}

	return NULL;
}

const supported_algo_t *get_dh_group_ike(void)
{
	return get_algo(SUPP_ALGO_DH_GROUP, SUPP_ALGO_NAME, 0, config[CONFIG_IKE_DH], 0);
}
const supported_algo_t *get_dh_group_ipsec(int server_setting)
{
	const char *pfs_setting = config[CONFIG_IPSEC_PFS];

	if (!strcmp(config[CONFIG_IPSEC_PFS], "server")) {
		/* treat server_setting == -1 (unknown) as 0 */
		pfs_setting = (server_setting == 1) ? "dh2" : "nopfs";
	}

	return get_algo(SUPP_ALGO_DH_GROUP, SUPP_ALGO_NAME, 0, pfs_setting, 0);
}

static __inline__ int min(int a, int b)
{
	return (a < b) ? a : b;
}

static void addenv(const void *name, const char *value)
{
	char *strbuf = NULL, *oldval;

	oldval = getenv(name);
	if (oldval != NULL) {
		strbuf = xallocc(strlen(oldval) + 1 + strlen(value) + 1);
		strcat(strbuf, oldval);
		strcat(strbuf, " ");
		strcat(strbuf, value);
	}

	setenv(name, strbuf ? strbuf : value, 1);

	if (strbuf)
		free(strbuf);
}

static void addenv_ipv4(const void *name, uint8_t * data)
{
	addenv(name, inet_ntoa(*((struct in_addr *)data)));
}

static int make_socket(uint16_t port)
{
	int sock;
	struct sockaddr_in name;

	/* Create the socket. */
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		error(1, errno, "making socket");

	/* Give the socket a name. */
	name.sin_family = AF_INET;
	name.sin_port = port;
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr *)&name, sizeof(name)) < 0)
		error(1, errno, "binding to port %d", ntohs(port));

	return sock;
}

static struct sockaddr *init_sockaddr(const char *hostname, uint16_t port)
{
	struct hostent *hostinfo;
	struct sockaddr_in *result;

	result = malloc(sizeof(struct sockaddr_in));
	if (result == NULL)
		error(1, errno, "out of memory");

	result->sin_family = AF_INET;
	result->sin_port = htons(port);
	if (inet_aton(hostname, &result->sin_addr) == 0) {
		hostinfo = gethostbyname(hostname);
		if (hostinfo == NULL)
			error(1, 0, "unknown host `%s'\n", hostname);
		result->sin_addr = *(struct in_addr *)hostinfo->h_addr;
	}
	return (struct sockaddr *)result;
}

static void setup_tunnel(struct sa_block *s)
{
	if (config[CONFIG_IF_NAME])
		memcpy(s->tun_name, config[CONFIG_IF_NAME], strlen(config[CONFIG_IF_NAME]));

	s->tun_fd = tun_open(s->tun_name);
	DEBUG(2, printf("using interface %s\n", s->tun_name));

	if (s->tun_fd == -1)
		error(1, errno, "can't initialise tunnel interface");
}

void config_tunnel(struct sa_block *s)
{
	setenv("TUNDEV", s->tun_name, 1);
	setenv("VPNGATEWAY", inet_ntoa(((struct sockaddr_in *)dest_addr)->sin_addr), 1);

	system(config[CONFIG_CONFIG_SCRIPT]);
}

static int recv_ignore_dup(void *recvbuf, size_t recvbufsize)
{
	uint8_t *resend_check_hash;
	int recvsize, hash_len;
	struct sockaddr_in recvaddr;
	socklen_t recvaddr_size = sizeof(recvaddr);
	char ntop_buf[32];

	recvsize = recvfrom(sockfd, recvbuf, recvbufsize, 0,
		(struct sockaddr *)&recvaddr, &recvaddr_size);
	if (recvsize == -1)
		error(1, errno, "receiving packet");
	if (recvsize > 0) {
		if (recvaddr_size != sizeof(recvaddr)
			|| recvaddr.sin_family != dest_addr->sa_family
			|| recvaddr.sin_port != ((struct sockaddr_in *)dest_addr)->sin_port
			|| memcmp(&recvaddr.sin_addr,
				&((struct sockaddr_in *)dest_addr)->sin_addr,
				sizeof(struct in_addr)) != 0) {
			error(0, 0, "got response from unknown host %s:%d",
				inet_ntop(recvaddr.sin_family, &recvaddr.sin_addr,
					ntop_buf, sizeof(ntop_buf)), ntohs(recvaddr.sin_port));
			return -1;
		}

		hash_len = gcry_md_get_algo_dlen(GCRY_MD_SHA1);
		resend_check_hash = malloc(hash_len);
		gcry_md_hash_buffer(GCRY_MD_SHA1, resend_check_hash, recvbuf, recvsize);
		if (resend_hash && memcmp(resend_hash, resend_check_hash, hash_len) == 0) {
			free(resend_check_hash);
			return -1;
		}
		if (!resend_hash) {
			resend_hash = resend_check_hash;
		} else {
			memcpy(resend_hash, resend_check_hash, hash_len);
			free(resend_check_hash);
		}
	}
	return recvsize;
}

/* Send TOSEND of size SENDSIZE to the socket.  Then wait for a new packet,
   resending TOSEND on timeout, and ignoring duplicate packets; the
   new packet is put in RECVBUF of size RECVBUFSIZE and the actual size
   of the new packet is returned.  */

static ssize_t
sendrecv(void *recvbuf, size_t recvbufsize, void *tosend, size_t sendsize, int sendonly)
{
	struct pollfd pfd;
	int tries = 0;
	int recvsize = -1;
	time_t start = time(NULL);
	time_t end = 0;
	void *realtosend;

	pfd.fd = sockfd;
	pfd.events = POLLIN;
	tries = 0;

	if ((tosend != NULL)&&(encap_mode != IPSEC_ENCAP_TUNNEL)) {
		DEBUG(2, printf("NAT-T mode, adding non-esp marker\n"));
		realtosend = xallocc(sendsize+4);
		memcpy(realtosend+4, tosend, sendsize);
		sendsize += 4;
	} else {
		realtosend = tosend;
	}

	for (;;) {
		int pollresult;

		if (realtosend != NULL)
			if (sendto(sockfd, realtosend, sendsize, 0,
					dest_addr, sizeof(struct sockaddr_in)) != (int)sendsize)
				error(1, errno, "can't send packet");
		if (sendonly)
			break;
		
		do {
			pollresult = poll(&pfd, 1, timeout << tries);
		} while (pollresult == -1 && errno == EINTR);
		
		if (pollresult == -1)
			error(1, errno, "can't poll socket");
		if (pollresult != 0) {
			recvsize = recv_ignore_dup(recvbuf, recvbufsize);
			end = time(NULL);
			if (recvsize != -1)
				break;
			continue;
		}
		
		if (tries > 5)
			error(1, 0, "no response from target");
		tries++;
	}

	if ((tosend != NULL)&&(encap_mode != IPSEC_ENCAP_TUNNEL))
		free(realtosend);

	if (sendonly)
		return 0;

	if (encap_mode != IPSEC_ENCAP_TUNNEL) {
		recvsize -= 4; /* 4 bytes non-esp marker */
		memcpy(recvbuf, recvbuf+4, recvsize);
	}

	/* Wait at least 2s for a response or 4 times the time it took
	 * last time.  */
	if (start == end)
		timeout = 2000;
	else
		timeout = 4000 * (end - start);

	return recvsize;
}

int isakmp_crypt(struct sa_block *s, uint8_t * block, size_t blocklen, int enc)
{
	unsigned char *new_iv, *iv = NULL;
	int info_ex;
	gcry_cipher_hd_t cry_ctx;

	if (blocklen < ISAKMP_PAYLOAD_O || ((blocklen - ISAKMP_PAYLOAD_O) % s->ivlen != 0))
		abort();

	if (!enc && (memcmp(block + ISAKMP_I_COOKIE_O, s->i_cookie, ISAKMP_COOKIE_LENGTH) != 0
		|| memcmp(block + ISAKMP_R_COOKIE_O, s->r_cookie, ISAKMP_COOKIE_LENGTH) != 0)) {
		DEBUG(2, printf("got paket with wrong cookies\n"));
		return ISAKMP_N_INVALID_COOKIE;
	}
	
	info_ex = block[ISAKMP_EXCHANGE_TYPE_O] == ISAKMP_EXCHANGE_INFORMATIONAL;
	
	if (memcmp(block + ISAKMP_MESSAGE_ID_O, s->current_iv_msgid, 4) != 0) {
		gcry_md_hd_t md_ctx;

		gcry_md_open(&md_ctx, s->md_algo, 0);
		gcry_md_write(md_ctx, s->initial_iv, s->ivlen);
		gcry_md_write(md_ctx, block + ISAKMP_MESSAGE_ID_O, 4);
		gcry_md_final(md_ctx);
		if (info_ex) {
			iv = xallocc(s->ivlen);
			memcpy(iv, gcry_md_read(md_ctx, 0), s->ivlen);
		} else {
			memcpy(s->current_iv, gcry_md_read(md_ctx, 0), s->ivlen);
			memcpy(s->current_iv_msgid, block + ISAKMP_MESSAGE_ID_O, 4);
		}
		gcry_md_close(md_ctx);
	} else if (info_ex) {
		abort();
	}
	
	if (!info_ex) {
		iv = s->current_iv;
	}

	new_iv = xallocc(s->ivlen);
	gcry_cipher_open(&cry_ctx, s->cry_algo, GCRY_CIPHER_MODE_CBC, 0);
	gcry_cipher_setkey(cry_ctx, s->key, s->keylen);
	gcry_cipher_setiv(cry_ctx, iv, s->ivlen);
	if (!enc) {
		memcpy(new_iv, block + blocklen - s->ivlen, s->ivlen);
		gcry_cipher_decrypt(cry_ctx, block + ISAKMP_PAYLOAD_O, blocklen - ISAKMP_PAYLOAD_O,
			NULL, 0);
		if (!info_ex)
			memcpy(s->current_iv, new_iv, s->ivlen);
	} else {
		gcry_cipher_encrypt(cry_ctx, block + ISAKMP_PAYLOAD_O, blocklen - ISAKMP_PAYLOAD_O,
			NULL, 0);
		if (!info_ex)
			memcpy(s->current_iv, block + blocklen - s->ivlen, s->ivlen);
	}
	gcry_cipher_close(cry_ctx);
	
	free(new_iv);
	if (info_ex)
		free(iv);
	
	return 0;
}

static uint16_t unpack_verify_phase2(struct sa_block *s,
	uint8_t * r_packet,
	size_t r_length, struct isakmp_packet **r_p, const uint8_t * nonce, size_t nonce_size)
{
	struct isakmp_packet *r;
	int reject = 0;

	*r_p = NULL;

	if (r_length < ISAKMP_PAYLOAD_O || ((r_length - ISAKMP_PAYLOAD_O) % s->ivlen != 0)) {
		DEBUG(2, printf("payload to short or not padded: len=%d, min=%d (ivlen=%d)\n",
			r_length, ISAKMP_PAYLOAD_O, s->ivlen));
		return ISAKMP_N_UNEQUAL_PAYLOAD_LENGTHS;
	}

	reject = isakmp_crypt(s, r_packet, r_length, 0);
	if (reject != 0)
		return reject;

	{
		r = parse_isakmp_packet(r_packet, r_length, &reject);
		if (reject != 0)
			return reject;
	}

	/* Verify the basic stuff.  */
	if (r->flags != ISAKMP_FLAG_E)
		return ISAKMP_N_INVALID_FLAGS;

	{
		size_t sz, spos;
		gcry_md_hd_t hm;
		unsigned char *expected_hash;
		struct isakmp_payload *h = r->payload;

		if (h == NULL || h->type != ISAKMP_PAYLOAD_HASH || h->u.hash.length != s->md_len)
			return ISAKMP_N_INVALID_HASH_INFORMATION;

		spos = (ISAKMP_PAYLOAD_O + (r_packet[ISAKMP_PAYLOAD_O + 2] << 8)
			+ r_packet[ISAKMP_PAYLOAD_O + 3]);

		/* Compute the real length based on the payload lengths.  */
		for (sz = spos; r_packet[sz] != 0; sz += r_packet[sz + 2] << 8 | r_packet[sz + 3]) ;
		sz += r_packet[sz + 2] << 8 | r_packet[sz + 3];

		gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
		gcry_md_setkey(hm, s->skeyid_a, s->md_len);
		gcry_md_write(hm, r_packet + ISAKMP_MESSAGE_ID_O, 4);
		if (nonce)
			gcry_md_write(hm, nonce, nonce_size);
		gcry_md_write(hm, r_packet + spos, sz - spos);
		gcry_md_final(hm);
		expected_hash = gcry_md_read(hm, 0);

		if (opt_debug >= 3) {
			printf("hashlen: %lu\n", (unsigned long)s->md_len);
			printf("u.hash.length: %d\n", h->u.hash.length);
			hex_dump("expected_hash", expected_hash, s->md_len);
			hex_dump("h->u.hash.data", h->u.hash.data, s->md_len);
		}

		reject = 0;
		if (memcmp(h->u.hash.data, expected_hash, s->md_len) != 0)
			reject = ISAKMP_N_AUTHENTICATION_FAILED;
		gcry_md_close(hm);
#if 0
		if (reject != 0)
			return reject;
#endif
	}
	*r_p = r;
	return 0;
}

static void
phase2_authpacket(struct sa_block *s, struct isakmp_payload *pl,
	uint8_t exchange_type, uint32_t msgid,
	uint8_t ** p_flat, size_t * p_size,
	uint8_t * nonce_i, int ni_len, uint8_t * nonce_r, int nr_len)
{
	struct isakmp_packet *p;
	uint8_t *pl_flat;
	size_t pl_size;
	gcry_md_hd_t hm;
	uint8_t msgid_sent[4];

	/* Build up the packet.  */
	p = new_isakmp_packet();
	memcpy(p->i_cookie, s->i_cookie, ISAKMP_COOKIE_LENGTH);
	memcpy(p->r_cookie, s->r_cookie, ISAKMP_COOKIE_LENGTH);
	p->flags = ISAKMP_FLAG_E;
	p->isakmp_version = ISAKMP_VERSION;
	p->exchange_type = exchange_type;
	p->message_id = msgid;
	p->payload = new_isakmp_payload(ISAKMP_PAYLOAD_HASH);
	p->payload->next = pl;
	p->payload->u.hash.length = s->md_len;
	p->payload->u.hash.data = xallocc(s->md_len);

	/* Set the MAC.  */
	gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
	gcry_md_setkey(hm, s->skeyid_a, s->md_len);

	if (pl == NULL) {
		DEBUG(3, printf("authing NULL package!\n"));
		gcry_md_write(hm, "" /* \0 */ , 1);
	}

	msgid_sent[0] = msgid >> 24;
	msgid_sent[1] = msgid >> 16;
	msgid_sent[2] = msgid >> 8;
	msgid_sent[3] = msgid;
	gcry_md_write(hm, msgid_sent, sizeof(msgid_sent));

	if (nonce_i != NULL)
		gcry_md_write(hm, nonce_i, ni_len);

	if (nonce_r != NULL)
		gcry_md_write(hm, nonce_r, nr_len);

	if (pl != NULL) {
		flatten_isakmp_payload(pl, &pl_flat, &pl_size);
		gcry_md_write(hm, pl_flat, pl_size);
		memset(pl_flat, 0, pl_size);
		free(pl_flat);
	}

	gcry_md_final(hm);
	memcpy(p->payload->u.hash.data, gcry_md_read(hm, 0), s->md_len);
	gcry_md_close(hm);

	flatten_isakmp_packet(p, p_flat, p_size, s->ivlen);
	free_isakmp_packet(p);
}

static void sendrecv_phase2(struct sa_block *s, struct isakmp_payload *pl,
	uint8_t exchange_type, uint32_t msgid, int sendonly,
	uint8_t ** save_p_flat, size_t * save_p_size,
	uint8_t * nonce_i, int ni_len, uint8_t * nonce_r, int nr_len)
{
	uint8_t *p_flat;
	size_t p_size;
	ssize_t recvlen;

	if ((save_p_flat == NULL) || (*save_p_flat == NULL)) {
		phase2_authpacket(s, pl, exchange_type, msgid, &p_flat, &p_size,
			nonce_i, ni_len, nonce_r, nr_len);
		isakmp_crypt(s, p_flat, p_size, 1);
	} else {
		p_flat = *save_p_flat;
		p_size = *save_p_size;
	}

	recvlen = sendrecv(r_packet, sizeof(r_packet), p_flat, p_size, sendonly);
	if (sendonly == 0)
		r_length = recvlen;
	
	if (save_p_flat == NULL) {
		free(p_flat);
	} else {
		*save_p_flat = p_flat;
		*save_p_size = p_size;
	}
}

static void phase2_fatal(struct sa_block *s, const char *msg, int id)
{
	struct isakmp_payload *pl;
	uint32_t msgid;

	DEBUG(1, printf("\n\n---!!!!!!!!! entering phase2_fatal !!!!!!!!!---\n\n\n"));
	gcry_randomize((uint8_t *) & msgid, sizeof(msgid), GCRY_WEAK_RANDOM);
	pl = new_isakmp_payload(ISAKMP_PAYLOAD_N);
	pl->u.n.doi = ISAKMP_DOI_IPSEC;
	pl->u.n.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
	pl->u.n.type = id;
	sendrecv_phase2(s, pl, ISAKMP_EXCHANGE_INFORMATIONAL, msgid, 1, 0, 0, 0, 0, 0, 0);

	gcry_randomize((uint8_t *) & msgid, sizeof(msgid), GCRY_WEAK_RANDOM);
	pl = new_isakmp_payload(ISAKMP_PAYLOAD_D);
	pl->u.d.doi = ISAKMP_DOI_IPSEC;
	pl->u.d.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
	pl->u.d.spi_length = 2 * ISAKMP_COOKIE_LENGTH;
	pl->u.d.num_spi = 1;
	pl->u.d.spi = xallocc(1 * sizeof(uint8_t *));
	pl->u.d.spi[0] = xallocc(2 * ISAKMP_COOKIE_LENGTH);
	memcpy(pl->u.d.spi[0] + ISAKMP_COOKIE_LENGTH * 0, s->i_cookie, ISAKMP_COOKIE_LENGTH);
	memcpy(pl->u.d.spi[0] + ISAKMP_COOKIE_LENGTH * 1, s->r_cookie, ISAKMP_COOKIE_LENGTH);
	sendrecv_phase2(s, pl, ISAKMP_EXCHANGE_INFORMATIONAL, msgid, 1, 0, 0, 0, 0, 0, 0);

	error(1, 0, msg, isakmp_notify_to_error(id));
}

static uint8_t *gen_keymat(struct sa_block *s,
	uint8_t protocol, uint32_t spi,
	int md_algo, int crypt_algo,
	const uint8_t * dh_shared, size_t dh_size,
	const uint8_t * ni_data, size_t ni_size, const uint8_t * nr_data, size_t nr_size)
{
	gcry_md_hd_t hm;
	uint8_t *block;
	int i;
	int blksz;
	int cnt;

	int md_len = gcry_md_get_algo_dlen(md_algo);
	size_t cry_len;

	gcry_cipher_algo_info(crypt_algo, GCRYCTL_GET_KEYLEN, NULL, &cry_len);
	blksz = md_len + cry_len;
	cnt = (blksz + s->md_len - 1) / s->md_len;
	block = xallocc(cnt * s->md_len);
	DEBUG(3, printf("generating %d bytes keymat (cnt=%d)\n", blksz, cnt));
	if (cnt < 1)
		abort();

	for (i = 0; i < cnt; i++) {
		gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
		gcry_md_setkey(hm, s->skeyid_d, s->md_len);
		if (i != 0)
			gcry_md_write(hm, block + (i - 1) * s->md_len, s->md_len);
		if (dh_shared != NULL)
			gcry_md_write(hm, dh_shared, dh_size);
		gcry_md_write(hm, &protocol, 1);
		gcry_md_write(hm, (uint8_t *) & spi, sizeof(spi));
		gcry_md_write(hm, ni_data, ni_size);
		gcry_md_write(hm, nr_data, nr_size);
		gcry_md_final(hm);
		memcpy(block + i * s->md_len, gcry_md_read(hm, 0), s->md_len);
		gcry_md_close(hm);
	}
	return block;
}

/* * */

struct isakmp_attribute *make_transform_ike(int dh_group, int crypt, int hash, int keylen, int auth)
{
	struct isakmp_attribute *a = NULL;

	a = new_isakmp_attribute(IKE_ATTRIB_LIFE_DURATION, a);
	a->af = isakmp_attr_lots;
	a->u.lots.length = 4;
	a->u.lots.data = xallocc(a->u.lots.length);
	*((uint32_t *) a->u.lots.data) = htonl(2147483);
	a = new_isakmp_attribute_16(IKE_ATTRIB_LIFE_TYPE, IKE_LIFE_TYPE_SECONDS, a);
	a = new_isakmp_attribute_16(IKE_ATTRIB_GROUP_DESC, dh_group, a);
	a = new_isakmp_attribute_16(IKE_ATTRIB_AUTH_METHOD, auth, a);
	a = new_isakmp_attribute_16(IKE_ATTRIB_HASH, hash, a);
	a = new_isakmp_attribute_16(IKE_ATTRIB_ENC, crypt, a);
	if (keylen != 0)
		a = new_isakmp_attribute_16(IKE_ATTRIB_KEY_LENGTH, keylen, a);
	return a;
}

struct isakmp_payload *make_our_sa_ike(void)
{
	struct isakmp_payload *r = new_isakmp_payload(ISAKMP_PAYLOAD_SA);
	struct isakmp_payload *t = NULL, *tn;
	struct isakmp_attribute *a;
	int dh_grp = get_dh_group_ike()->ike_sa_id;
	unsigned int auth, crypt, hash, keylen;
	int i;

	r->u.sa.doi = ISAKMP_DOI_IPSEC;
	r->u.sa.situation = ISAKMP_IPSEC_SIT_IDENTITY_ONLY;
	r->u.sa.proposals = new_isakmp_payload(ISAKMP_PAYLOAD_P);
	r->u.sa.proposals->u.p.prot_id = ISAKMP_IPSEC_PROTO_ISAKMP;
	for (auth = 0; supp_auth[auth].name != NULL; auth++) {
		for (crypt = 0; supp_crypt[crypt].name != NULL; crypt++) {
			if ((supp_crypt[crypt].my_id == GCRY_CIPHER_DES) && (opt_1des == 0))
				continue;
			keylen = supp_crypt[crypt].keylen;
			for (hash = 0; supp_hash[hash].name != NULL; hash++) {
				tn = t;
				t = new_isakmp_payload(ISAKMP_PAYLOAD_T);
				t->u.t.id = ISAKMP_IPSEC_KEY_IKE;
				a = make_transform_ike(dh_grp, supp_crypt[crypt].ike_sa_id,
					supp_hash[hash].ike_sa_id, keylen, supp_auth[auth].ike_sa_id);
				t->u.t.attributes = a;
				t->next = tn;
			}
		}
	}
	for (i = 0, tn = t; tn; tn = tn->next)
		tn->u.t.number = i++;
	r->u.sa.proposals->u.p.transforms = t;
	return r;
}

void do_phase_1(const char *key_id, const char *shared_key, struct sa_block *s)
{
	unsigned char i_nonce[20];
	struct group *dh_grp;
	unsigned char *dh_public;
	unsigned char *returned_hash;
	static const uint8_t xauth_vid[] = XAUTH_VENDOR_ID;
	static const uint8_t unity_vid[] = UNITY_VENDOR_ID;
	static const uint8_t unknown_vid[] = UNKNOWN_VENDOR_ID;
	static const uint8_t natt_vid[] = NATT_VENDOR_ID; /* NAT traversal */
#if 0
	static const uint8_t dpd_vid[] = DPD_VENDOR_ID; /* dead peer detection */
	static const uint8_t my_vid[] = {
		0x35, 0x53, 0x07, 0x6c, 0x4f, 0x65, 0x12, 0x68, 0x02, 0x82, 0xf2, 0x15,
		0x8a, 0xa8, 0xa0, 0x9e
	};
#endif

	struct isakmp_packet *p1;
	int seen_natt_vid = 0, seen_natd = 0, seen_natd_them = 0, seen_natd_us = 0, natd_type = 0;
	unsigned char *natd_us = NULL, *natd_them = NULL;

	DEBUG(2, printf("S4.1\n"));
	gcry_randomize(s->i_cookie, ISAKMP_COOKIE_LENGTH, GCRY_STRONG_RANDOM);
	s->do_pfs = -1;
	if (s->i_cookie[0] == 0)
		s->i_cookie[0] = 1;
	hex_dump("i_cookie", s->i_cookie, ISAKMP_COOKIE_LENGTH);
	gcry_randomize(i_nonce, sizeof(i_nonce), GCRY_STRONG_RANDOM);
	hex_dump("i_nonce", i_nonce, sizeof(i_nonce));
	DEBUG(2, printf("S4.2\n"));
	/* Set up the Diffie-Hellman stuff.  */
	{
		dh_grp = group_get(get_dh_group_ike()->my_id);
		dh_public = xallocc(dh_getlen(dh_grp));
		dh_create_exchange(dh_grp, dh_public);
		hex_dump("dh_public", dh_public, dh_getlen(dh_grp));
	}

	DEBUG(2, printf("S4.3\n"));
	/* Create the first packet.  */
	{
		struct isakmp_payload *l;
		uint8_t *pkt;
		size_t pkt_len;

		p1 = new_isakmp_packet();
		memcpy(p1->i_cookie, s->i_cookie, ISAKMP_COOKIE_LENGTH);
		p1->isakmp_version = ISAKMP_VERSION;
		p1->exchange_type = ISAKMP_EXCHANGE_AGGRESSIVE;
		p1->payload = l = make_our_sa_ike();
		l->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_KE, dh_public, dh_getlen(dh_grp));
		l->next->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_NONCE,
			i_nonce, sizeof(i_nonce));
		l = l->next->next;
		l->next = new_isakmp_payload(ISAKMP_PAYLOAD_ID);
		l = l->next;
		l->u.id.type = ISAKMP_IPSEC_ID_KEY_ID;
		l->u.id.protocol = IPPROTO_UDP;
		l->u.id.port = ntohs(local_port);
		l->u.id.length = strlen(key_id);
		l->u.id.data = xallocc(l->u.id.length);
		memcpy(l->u.id.data, key_id, strlen(key_id));
		l = l->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_VID,
			xauth_vid, sizeof(xauth_vid));
		l = l->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_VID,
			unity_vid, sizeof(unity_vid));
		if (!config[CONFIG_DISABLE_NATT])
			l = l->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_VID,
				natt_vid, sizeof(natt_vid));
#if 0
		l = l->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_VID,
			dpd_vid, sizeof(dpd_vid));
#endif
		flatten_isakmp_packet(p1, &pkt, &pkt_len, 0);

		/* Now, send that packet and receive a new one.  */
		r_length = sendrecv(r_packet, sizeof(r_packet), pkt, pkt_len, 0);
		free(pkt);
	}
	DEBUG(2, printf("S4.4\n"));
	/* Decode the recieved packet.  */
	{
		struct isakmp_packet *r;
		int reject;
		struct isakmp_payload *rp;
		struct isakmp_payload *nonce = NULL;
		struct isakmp_payload *ke = NULL;
		struct isakmp_payload *hash = NULL;
		struct isakmp_payload *idp = NULL;
		int seen_sa = 0, seen_xauth_vid = 0;
		unsigned char *skeyid;
		gcry_md_hd_t skeyid_ctx;

		reject = 0;
		r = parse_isakmp_packet(r_packet, r_length, &reject);

		/* Verify the correctness of the recieved packet.  */
		if (reject == 0 && memcmp(r->i_cookie, s->i_cookie, ISAKMP_COOKIE_LENGTH) != 0)
			reject = ISAKMP_N_INVALID_COOKIE;
		if (reject == 0)
			memcpy(s->r_cookie, r->r_cookie, ISAKMP_COOKIE_LENGTH);
		if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_AGGRESSIVE)
			reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;
		if (reject == 0 && r->flags != 0)
			reject = ISAKMP_N_INVALID_FLAGS;
		if (reject == 0 && r->message_id != 0)
			reject = ISAKMP_N_INVALID_MESSAGE_ID;
		if (reject != 0)
			error(1, 0, "response was invalid [1]: %s", isakmp_notify_to_error(reject));
		for (rp = r->payload; rp && reject == 0; rp = rp->next)
			switch (rp->type) {
			case ISAKMP_PAYLOAD_SA:
				if (reject == 0 && rp->u.sa.doi != ISAKMP_DOI_IPSEC)
					reject = ISAKMP_N_DOI_NOT_SUPPORTED;
				if (reject == 0 &&
					rp->u.sa.situation != ISAKMP_IPSEC_SIT_IDENTITY_ONLY)
					reject = ISAKMP_N_SITUATION_NOT_SUPPORTED;
				if (reject == 0 &&
					(rp->u.sa.proposals == NULL
						|| rp->u.sa.proposals->next != NULL))
					reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
				if (reject == 0 &&
					rp->u.sa.proposals->u.p.prot_id !=
					ISAKMP_IPSEC_PROTO_ISAKMP)
					reject = ISAKMP_N_INVALID_PROTOCOL_ID;
				if (reject == 0 && rp->u.sa.proposals->u.p.spi_size != 0)
					reject = ISAKMP_N_INVALID_SPI;
				if (reject == 0 &&
					(rp->u.sa.proposals->u.p.transforms == NULL
						|| rp->u.sa.proposals->u.p.transforms->next !=
						NULL))
					reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
				if (reject == 0 &&
					(rp->u.sa.proposals->u.p.transforms->u.t.id
						!= ISAKMP_IPSEC_KEY_IKE))
					reject = ISAKMP_N_INVALID_TRANSFORM_ID;
				if (reject == 0) {
					struct isakmp_attribute *a
						=
						rp->u.sa.proposals->u.p.transforms->u.t.attributes;
					int seen_enc = 0, seen_hash = 0, seen_auth = 0;
					int seen_group = 0, seen_keylen = 0;
					for (; a && reject == 0; a = a->next)
						switch (a->type) {
						case IKE_ATTRIB_GROUP_DESC:
							if (a->af == isakmp_attr_16 &&
								a->u.attr_16 ==
								get_dh_group_ike()->ike_sa_id)
								seen_group = 1;
							else
								reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
							break;
						case IKE_ATTRIB_AUTH_METHOD:
							if (a->af == isakmp_attr_16)
								seen_auth = a->u.attr_16;
							else
								reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
							break;
						case IKE_ATTRIB_HASH:
							if (a->af == isakmp_attr_16)
								seen_hash = a->u.attr_16;
							else
								reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
							break;
						case IKE_ATTRIB_ENC:
							if (a->af == isakmp_attr_16)
								seen_enc = a->u.attr_16;
							else
								reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
							break;
						case IKE_ATTRIB_KEY_LENGTH:
							if (a->af == isakmp_attr_16)
								seen_keylen = a->u.attr_16;
							else
								reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
							break;
						case IKE_ATTRIB_LIFE_TYPE:
						case IKE_ATTRIB_LIFE_DURATION:
							break;
						default:
							DEBUG(1, printf
								("unknown attribute %d, arborting..\n",
									a->type));
							reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
							break;
						}
					if (!seen_group || !seen_auth || !seen_hash || !seen_enc)
						reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;

					if (get_algo(SUPP_ALGO_AUTH, SUPP_ALGO_IKE_SA, seen_auth,
							NULL, 0) == NULL)
						reject = ISAKMP_N_NO_PROPOSAL_CHOSEN;
					if (get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IKE_SA, seen_hash,
							NULL, 0) == NULL)
						reject = ISAKMP_N_NO_PROPOSAL_CHOSEN;
					if (get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IKE_SA, seen_enc,
							NULL, seen_keylen) == NULL)
						reject = ISAKMP_N_NO_PROPOSAL_CHOSEN;

					if (reject == 0) {
						seen_sa = 1;
						s->auth_algo = seen_auth;
						s->cry_algo =
							get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IKE_SA,
							seen_enc, NULL, seen_keylen)->my_id;
						s->md_algo =
							get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IKE_SA,
							seen_hash, NULL, 0)->my_id;
						s->md_len = gcry_md_get_algo_dlen(s->md_algo);
						DEBUG(1, printf("IKE SA selected %s-%s-%s\n",
								get_algo(SUPP_ALGO_AUTH,
									SUPP_ALGO_IKE_SA, seen_auth,
									NULL, 0)->name,
								get_algo(SUPP_ALGO_CRYPT,
									SUPP_ALGO_IKE_SA, seen_enc,
									NULL, seen_keylen)->name,
								get_algo(SUPP_ALGO_HASH,
									SUPP_ALGO_IKE_SA, seen_hash,
									NULL, 0)->name));
					}
				}
				break;

			case ISAKMP_PAYLOAD_ID:
				idp = rp;
				break;
			case ISAKMP_PAYLOAD_KE:
				ke = rp;
				break;
			case ISAKMP_PAYLOAD_NONCE:
				nonce = rp;
				break;
			case ISAKMP_PAYLOAD_HASH:
				hash = rp;
				break;
			case ISAKMP_PAYLOAD_VID:
				if (rp->u.vid.length == sizeof(xauth_vid)
					&& memcmp(rp->u.vid.data, xauth_vid,
						sizeof(xauth_vid)) == 0)
					seen_xauth_vid = 1;
				else if (rp->u.vid.length == sizeof(natt_vid)
					&& memcmp(rp->u.vid.data, natt_vid,
						sizeof(natt_vid)) == 0)
					seen_natt_vid = 1;
				break;
			case ISAKMP_PAYLOAD_NAT_D_OLD:
			case ISAKMP_PAYLOAD_NAT_D:
				natd_type = rp->type;
				DEBUG(2, printf("peer is using type %d for NAT-Discovery payloads\n", natd_type));
				if (!seen_sa || !seen_natt_vid) {
					reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
				} else if (config[CONFIG_DISABLE_NATT]) {
					;
				} else if (rp->u.natd.length != s->md_len) {
					reject = ISAKMP_N_PAYLOAD_MALFORMED;
				} else if (seen_natd == 0) {
					gcry_md_hd_t hm;
					natd_us = xallocc(s->md_len);
					natd_them = xallocc(s->md_len);
					memcpy(natd_us, rp->u.natd.data, s->md_len);
					gcry_md_open(&hm, s->md_algo, 0);
					gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
					gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
					gcry_md_write(hm, &((struct sockaddr_in *)dest_addr)->sin_addr,
						sizeof(struct in_addr));
					gcry_md_write(hm, &((struct sockaddr_in *)dest_addr)->sin_port,
						sizeof(uint16_t));
					gcry_md_final(hm);
					memcpy(natd_them, gcry_md_read(hm, 0), s->md_len);
					gcry_md_close(hm);
					seen_natd = 1;
				} else {
					if (memcmp(natd_them, rp->u.natd.data, s->md_len) == 0)
						seen_natd_them = 1;
				}
				break;
			default:
				reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
				break;
			}

		if (reject == 0) {
			gcry_cipher_algo_info(s->cry_algo, GCRYCTL_GET_BLKLEN, NULL, &(s->ivlen));
			gcry_cipher_algo_info(s->cry_algo, GCRYCTL_GET_KEYLEN, NULL, &(s->keylen));
		}

		if (reject == 0 && (ke == NULL || ke->u.ke.length != dh_getlen(dh_grp)))
			reject = ISAKMP_N_INVALID_KEY_INFORMATION;
		if (reject == 0 && nonce == NULL)
			reject = ISAKMP_N_INVALID_HASH_INFORMATION;
		if (reject != 0)
			error(1, 0, "response was invalid [2]: %s", isakmp_notify_to_error(reject));
		if (reject == 0 && idp == NULL)
			reject = ISAKMP_N_INVALID_ID_INFORMATION;
		if (reject == 0 && (hash == NULL || hash->u.hash.length != s->md_len))
			reject = ISAKMP_N_INVALID_HASH_INFORMATION;
		if (reject != 0)
			error(1, 0, "response was invalid [3]: %s", isakmp_notify_to_error(reject));

		/* Generate SKEYID.  */
		{
			gcry_md_open(&skeyid_ctx, s->md_algo, GCRY_MD_FLAG_HMAC);
			gcry_md_setkey(skeyid_ctx, shared_key, strlen(shared_key));
			gcry_md_write(skeyid_ctx, i_nonce, sizeof(i_nonce));
			gcry_md_write(skeyid_ctx, nonce->u.nonce.data, nonce->u.nonce.length);
			gcry_md_final(skeyid_ctx);
			skeyid = gcry_md_read(skeyid_ctx, 0);
			hex_dump("skeyid", skeyid, s->md_len);
		}

		/* Verify the hash.  */
		{
			gcry_md_hd_t hm;
			unsigned char *expected_hash;
			uint8_t *sa_f, *idi_f, *idp_f;
			size_t sa_size, idi_size, idp_size;
			struct isakmp_payload *sa, *idi;

			sa = p1->payload;
			for (idi = sa; idi->type != ISAKMP_PAYLOAD_ID; idi = idi->next) ;
			sa->next = NULL;
			idi->next = NULL;
			idp->next = NULL;
			flatten_isakmp_payload(sa, &sa_f, &sa_size);
			flatten_isakmp_payload(idi, &idi_f, &idi_size);
			flatten_isakmp_payload(idp, &idp_f, &idp_size);

			gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
			gcry_md_setkey(hm, skeyid, s->md_len);
			gcry_md_write(hm, ke->u.ke.data, ke->u.ke.length);
			gcry_md_write(hm, dh_public, dh_getlen(dh_grp));
			gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, sa_f + 4, sa_size - 4);
			gcry_md_write(hm, idp_f + 4, idp_size - 4);
			gcry_md_final(hm);
			expected_hash = gcry_md_read(hm, 0);

			if (memcmp(expected_hash, hash->u.hash.data, s->md_len) != 0) {
				error(1, 0, "hash comparison failed: %s\ncheck group password!",
					isakmp_notify_to_error(ISAKMP_N_AUTHENTICATION_FAILED));
			}
			gcry_md_close(hm);

			gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
			gcry_md_setkey(hm, skeyid, s->md_len);
			gcry_md_write(hm, dh_public, dh_getlen(dh_grp));
			gcry_md_write(hm, ke->u.ke.data, ke->u.ke.length);
			gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, sa_f + 4, sa_size - 4);
			gcry_md_write(hm, idi_f + 4, idi_size - 4);
			gcry_md_final(hm);
			returned_hash = xallocc(s->md_len);
			memcpy(returned_hash, gcry_md_read(hm, 0), s->md_len);
			gcry_md_close(hm);
			hex_dump("returned_hash", returned_hash, s->md_len);

			free(sa_f);
			free(idi);
			free(idp);
		}

		/* Determine all the SKEYID_x keys.  */
		{
			gcry_md_hd_t hm;
			int i;
			static const unsigned char c012[3] = { 0, 1, 2 };
			unsigned char *skeyid_e;
			unsigned char *dh_shared_secret;

			/* Determine the shared secret.  */
			dh_shared_secret = xallocc(dh_getlen(dh_grp));
			dh_create_shared(dh_grp, dh_shared_secret, ke->u.ke.data);
			hex_dump("dh_shared_secret", dh_shared_secret, dh_getlen(dh_grp));

			gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
			gcry_md_setkey(hm, skeyid, s->md_len);
			gcry_md_write(hm, dh_shared_secret, dh_getlen(dh_grp));
			gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, c012 + 0, 1);
			gcry_md_final(hm);
			s->skeyid_d = xallocc(s->md_len);
			memcpy(s->skeyid_d, gcry_md_read(hm, 0), s->md_len);
			gcry_md_close(hm);
			hex_dump("skeyid_d", s->skeyid_d, s->md_len);

			gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
			gcry_md_setkey(hm, skeyid, s->md_len);
			gcry_md_write(hm, s->skeyid_d, s->md_len);
			gcry_md_write(hm, dh_shared_secret, dh_getlen(dh_grp));
			gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, c012 + 1, 1);
			gcry_md_final(hm);
			s->skeyid_a = xallocc(s->md_len);
			memcpy(s->skeyid_a, gcry_md_read(hm, 0), s->md_len);
			gcry_md_close(hm);
			hex_dump("skeyid_a", s->skeyid_a, s->md_len);

			gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
			gcry_md_setkey(hm, skeyid, s->md_len);
			gcry_md_write(hm, s->skeyid_a, s->md_len);
			gcry_md_write(hm, dh_shared_secret, dh_getlen(dh_grp));
			gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
			gcry_md_write(hm, c012 + 2, 1);
			gcry_md_final(hm);
			skeyid_e = xallocc(s->md_len);
			memcpy(skeyid_e, gcry_md_read(hm, 0), s->md_len);
			gcry_md_close(hm);
			hex_dump("skeyid_e", skeyid_e, s->md_len);

			memset(dh_shared_secret, 0, sizeof(dh_shared_secret));

			/* Determine the IKE encryption key.  */
			s->key = xallocc(s->keylen);

			if (s->keylen > s->md_len) {
				for (i = 0; i * s->md_len < s->keylen; i++) {
					gcry_md_open(&hm, s->md_algo, GCRY_MD_FLAG_HMAC);
					gcry_md_setkey(hm, skeyid_e, s->md_len);
					if (i == 0)
						gcry_md_write(hm, "" /* &'\0' */ , 1);
					else
						gcry_md_write(hm, s->key + (i - 1) * s->md_len,
							s->md_len);
					gcry_md_final(hm);
					memcpy(s->key + i * s->md_len, gcry_md_read(hm, 0),
						min(s->md_len, s->keylen - i * s->md_len));
					gcry_md_close(hm);
				}
			} else { /* keylen <= md_len */
				memcpy(s->key, skeyid_e, s->keylen);
			}
			hex_dump("enc-key", s->key, s->keylen);

			memset(skeyid_e, 0, s->md_len);
		}

		/* Determine the initial IV.  */
		{
			gcry_md_hd_t hm;

			assert(s->ivlen <= s->md_len);
			gcry_md_open(&hm, s->md_algo, 0);
			gcry_md_write(hm, dh_public, dh_getlen(dh_grp));
			gcry_md_write(hm, ke->u.ke.data, ke->u.ke.length);
			gcry_md_final(hm);
			s->current_iv = xallocc(s->ivlen);
			memcpy(s->current_iv, gcry_md_read(hm, 0), s->ivlen);
			gcry_md_close(hm);
			hex_dump("current_iv", s->current_iv, s->ivlen);
			memset(s->current_iv_msgid, 0, 4);
		}

		gcry_md_close(skeyid_ctx);
	}

	DEBUG(2, printf("S4.5\n"));
	/* Send final phase 1 packet.  */
	{
		struct isakmp_packet *p2;
		uint8_t *p2kt;
		size_t p2kt_len;
		struct isakmp_payload *pl;

		p2 = new_isakmp_packet();
		memcpy(p2->i_cookie, s->i_cookie, ISAKMP_COOKIE_LENGTH);
		memcpy(p2->r_cookie, s->r_cookie, ISAKMP_COOKIE_LENGTH);
		p2->flags = ISAKMP_FLAG_E;
		p2->isakmp_version = ISAKMP_VERSION;
		p2->exchange_type = ISAKMP_EXCHANGE_AGGRESSIVE;
		p2->payload = new_isakmp_data_payload(ISAKMP_PAYLOAD_HASH,
			returned_hash, s->md_len);
		p2->payload->next = pl = new_isakmp_payload(ISAKMP_PAYLOAD_N);
		pl->u.n.doi = ISAKMP_DOI_IPSEC;
		pl->u.n.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
		pl->u.n.type = ISAKMP_N_IPSEC_INITIAL_CONTACT;
		pl->u.n.spi_length = 2 * ISAKMP_COOKIE_LENGTH;
		pl->u.n.spi = xallocc(2 * ISAKMP_COOKIE_LENGTH);
		memcpy(pl->u.n.spi + ISAKMP_COOKIE_LENGTH * 0, s->i_cookie, ISAKMP_COOKIE_LENGTH);
		memcpy(pl->u.n.spi + ISAKMP_COOKIE_LENGTH * 1, s->r_cookie, ISAKMP_COOKIE_LENGTH);
		pl = pl->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_VID,
			unknown_vid, sizeof(unknown_vid));
		pl = pl->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_VID,
			unity_vid, sizeof(unity_vid));

		/* include NAT traversal discovery payloads */
		if (seen_natt_vid) {
			assert(natd_type != 0);
			pl = pl->next = new_isakmp_data_payload(natd_type,
				natd_them, s->md_len);
			/* this could be repeated fo any known outbound interfaces */
			{
				gcry_md_hd_t hm;
				struct sockaddr_in src_addr;
				src_addr.sin_port=local_port;
				find_local_addr((struct sockaddr_in *)dest_addr, &src_addr);
				gcry_md_open(&hm, s->md_algo, 0);
				gcry_md_write(hm, s->i_cookie, ISAKMP_COOKIE_LENGTH);
				gcry_md_write(hm, s->r_cookie, ISAKMP_COOKIE_LENGTH);
				gcry_md_write(hm, &src_addr.sin_addr, sizeof(struct in_addr));
				gcry_md_write(hm, &local_port, sizeof(uint16_t));
				gcry_md_final(hm);
				pl = pl->next = new_isakmp_data_payload(natd_type,
					gcry_md_read(hm, 0), s->md_len);
				if (seen_natd && memcmp(natd_us, pl->u.ke.data, s->md_len) == 0)
					seen_natd_us = 1;
				gcry_md_close(hm);
			}
			if (seen_natd) {
				free(natd_us);
				free(natd_them);
			}
			/* if there is a NAT, change to port 4500 and select UDP encap */
			if (!seen_natd_us || !seen_natd_them) {
				DEBUG(1, printf("NAT status: this end behind NAT? %s -- remote end behind NAT? %s\n",
					seen_natd_us ? "no" : "YES", seen_natd_them ? "no" : "YES"));
				switch (natd_type) {
					case ISAKMP_PAYLOAD_NAT_D:
						encap_mode = IPSEC_ENCAP_UDP_TUNNEL;
						break;
					case ISAKMP_PAYLOAD_NAT_D_OLD:
						encap_mode = IPSEC_ENCAP_UDP_TUNNEL_OLD;
						break;
					default:
						abort();
				}
				((struct sockaddr_in *)dest_addr)->sin_port = htons(4500);
				if (local_port == htons(500)) {
					close(sockfd);
					sockfd = make_socket(local_port = htons(4500));
				}
			} else {
				DEBUG(1, printf("NAT status: NAT-T VID seen, no NAT device detected\n"));
			}
		} else {
			DEBUG(1, printf("NAT status: no NAT-T VID seen\n"));
		}

		
		flatten_isakmp_packet(p2, &p2kt, &p2kt_len, s->ivlen);
		free_isakmp_packet(p2);
		isakmp_crypt(s, p2kt, p2kt_len, 1);

		s->initial_iv = xallocc(s->ivlen);
		memcpy(s->initial_iv, s->current_iv, s->ivlen);
		hex_dump("initial_iv", s->initial_iv, s->ivlen);

		/* Now, send that packet and receive a new one.  */
		r_length = sendrecv(r_packet, sizeof(r_packet), p2kt, p2kt_len, 0);
		free(p2kt);
	}
	DEBUG(2, printf("S4.6\n"));

	free(returned_hash);
}

static int do_phase2_notice_check(struct sa_block *s, struct isakmp_packet **r_p)
{
	int reject = 0;
	struct isakmp_packet *r;
	
	while (1) {
		reject = unpack_verify_phase2(s, r_packet, r_length, r_p, NULL, 0);
		if (reject == ISAKMP_N_INVALID_COOKIE) {
			r_length = sendrecv(r_packet, sizeof(r_packet), NULL, 0, 0);
			continue;
		}
		r = *r_p;
		
		/* check for notices */
		if (r->exchange_type == ISAKMP_EXCHANGE_INFORMATIONAL &&
			r->payload->next != NULL) {
			if (r->payload->next->type == ISAKMP_PAYLOAD_N) {
				if (r->payload->next->u.n.type == ISAKMP_N_CISCO_LOAD_BALANCE) {
					/* load balancing notice ==> restart with new gw */
					if (r->payload->next->u.n.data_length != 4)
					error(1, 0, "malformed loadbalance target");
					memcpy(&((struct sockaddr_in *)dest_addr)->sin_addr,
						r->payload->next->u.n.data, 4);
					((struct sockaddr_in *)dest_addr)->sin_port = htons(ISAKMP_PORT);
					encap_mode = IPSEC_ENCAP_TUNNEL;
					if (local_port == htons(4500)) {
						close(sockfd);
						sockfd = make_socket(local_port = htons(500));
					}
					DEBUG(2, printf("got cisco loadbalancing notice, diverting to %s\n",
							inet_ntoa(((struct sockaddr_in *)dest_addr)->
								sin_addr)));
					return -1;
				} else if (r->payload->next->u.n.type == ISAKMP_N_IPSEC_RESPONDER_LIFETIME) {
					/* responder liftime notice ==> ignore */
					DEBUG(2, printf("got responder liftime notice, ignoring..\n"));
					r_length = sendrecv(r_packet, sizeof(r_packet), NULL, 0, 0);
					continue;
				} else if (r->payload->next->u.n.type == ISAKMP_N_IPSEC_INITIAL_CONTACT) {
					/* why in hell do we get this?? */
					DEBUG(2, printf("got initial contact notice, ignoring..\n"));
					r_length = sendrecv(r_packet, sizeof(r_packet), NULL, 0, 0);
					continue;
				} else {
					/* whatever */
					printf("received notice of type %s(%d), giving up\n",
						isakmp_notify_to_error(r->payload->next->u.n.type),
						r->payload->next->u.n.type);
					return reject;
				}
			}
			if (r->payload->next->type == ISAKMP_PAYLOAD_D) {
				/* responder liftime notice ==> ignore */
				DEBUG(2, printf("got delete for old connection, ignoring..\n"));
				r_length = sendrecv(r_packet, sizeof(r_packet), NULL, 0, 0);
				continue;
			}
		}
		
		break;
	}
	return reject;
}

static int do_phase_2_xauth(struct sa_block *s)
{
	struct isakmp_packet *r;
	int loopcount;

	DEBUG(2, printf("S5.1\n"));
	/* This can go around for a while.  */
	for (loopcount = 0;; loopcount++) {
		int reject;
		struct isakmp_payload *rp;
		struct isakmp_attribute *a, *ap, *reply_attr;
		char ntop_buf[32];
		int seen_answer = 0;

		DEBUG(2, printf("S5.2\n"));
		
		/* recv and check for notices */
		reject = do_phase2_notice_check(s, &r);
		if (reject == -1)
			return 1;
		
		DEBUG(2, printf("S5.3\n"));
		/* Check the transaction type is OK.  */
		if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_MODECFG_TRANSACTION)
			reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;

		/* After the hash, expect an attribute block.  */
		if (reject == 0
			&& (r->payload->next == NULL
				|| r->payload->next->next != NULL
				|| r->payload->next->type != ISAKMP_PAYLOAD_MODECFG_ATTR))
			reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;

		if (reject == 0 && r->payload->next->u.modecfg.type == ISAKMP_MODECFG_CFG_SET)
			break;
		if (reject == 0 && r->payload->next->u.modecfg.type != ISAKMP_MODECFG_CFG_REQUEST)
			reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;

		if (reject != 0)
			phase2_fatal(s, "expected xauth packet; rejected: %s", reject);

		DEBUG(2, printf("S5.4\n"));
		a = r->payload->next->u.modecfg.attributes;
		/* First, print any messages, and verify that we understand the
		 * conversation.  */
		for (ap = a; ap && seen_answer == 0; ap = ap->next)
			if (ap->type == ISAKMP_XAUTH_ATTRIB_ANSWER)
				seen_answer = 1;

		for (ap = a; ap && reject == 0; ap = ap->next)
			switch (ap->type) {
			case ISAKMP_XAUTH_ATTRIB_TYPE:
				if (ap->af != isakmp_attr_16 || ap->u.attr_16 != 0)
					reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
				break;
			case ISAKMP_XAUTH_ATTRIB_USER_NAME:
			case ISAKMP_XAUTH_ATTRIB_USER_PASSWORD:
			case ISAKMP_XAUTH_ATTRIB_PASSCODE:
			case ISAKMP_XAUTH_ATTRIB_DOMAIN:
			case ISAKMP_XAUTH_ATTRIB_ANSWER:
			case ISAKMP_XAUTH_ATTRIB_CISCOEXT_VENDOR:
				break;
			case ISAKMP_XAUTH_ATTRIB_MESSAGE:
				if (opt_debug || seen_answer || config[CONFIG_XAUTH_INTERACTIVE]) {
					if (ap->af == isakmp_attr_16)
						printf("%c%c\n", ap->u.attr_16 >> 8, ap->u.attr_16);
					else
						printf("%.*s%s", ap->u.lots.length, ap->u.lots.data,
							((ap->u.lots.data
									&& ap->u.lots.data[ap->u.
										lots.length - 1] !=
									'\n')
								? "\n" : ""));
				}
				break;
			default:
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			}
		DEBUG(2, printf("S5.5\n"));
		if (reject != 0)
			phase2_fatal(s, "xauth packet unsupported: %s", reject);

		inet_ntop(dest_addr->sa_family,
			&((struct sockaddr_in *)dest_addr)->sin_addr, ntop_buf, sizeof(ntop_buf));

		/* Collect data from the user.  */
		reply_attr = NULL;
		for (ap = a; ap && reject == 0; ap = ap->next)
			switch (ap->type) {
			case ISAKMP_XAUTH_ATTRIB_DOMAIN:
				{
					struct isakmp_attribute *na;
					na = new_isakmp_attribute(ap->type, reply_attr);
					reply_attr = na;
					na->u.lots.length = strlen(config[CONFIG_DOMAIN]);
					if (na->u.lots.length == 0)
						error(1, 0,
							"server requested domain, but none set (use \"Domain ...\" in config or --domain");
					na->u.lots.data = xallocc(na->u.lots.length);
					memcpy(na->u.lots.data, config[CONFIG_DOMAIN],
						na->u.lots.length);
					break;
				}
			case ISAKMP_XAUTH_ATTRIB_USER_NAME:
				{
					struct isakmp_attribute *na;
					na = new_isakmp_attribute(ap->type, reply_attr);
					reply_attr = na;
					na->u.lots.length = strlen(config[CONFIG_XAUTH_USERNAME]);
					na->u.lots.data = xallocc(na->u.lots.length);
					memcpy(na->u.lots.data, config[CONFIG_XAUTH_USERNAME],
						na->u.lots.length);
					break;
				}
			case ISAKMP_XAUTH_ATTRIB_ANSWER:
			case ISAKMP_XAUTH_ATTRIB_USER_PASSWORD:
			case ISAKMP_XAUTH_ATTRIB_PASSCODE:
				if (seen_answer || config[CONFIG_XAUTH_INTERACTIVE]) {
					char *pass, *prompt = NULL;
					struct isakmp_attribute *na;

					asprintf(&prompt, "%s for VPN %s@%s: ",
						(ap->type == ISAKMP_XAUTH_ATTRIB_ANSWER) ?
						"Answer" :
						(ap->type == ISAKMP_XAUTH_ATTRIB_USER_PASSWORD) ?
						"Password" : "Passcode",
						config[CONFIG_XAUTH_USERNAME], ntop_buf);
					pass = getpass(prompt);
					free(prompt);

					na = new_isakmp_attribute(ap->type, reply_attr);
					reply_attr = na;
					na->u.lots.length = strlen(pass);
					na->u.lots.data = xallocc(na->u.lots.length);
					memcpy(na->u.lots.data, pass, na->u.lots.length);
					memset(pass, 0, na->u.lots.length);
				} else {
					struct isakmp_attribute *na;
					na = new_isakmp_attribute(ap->type, reply_attr);
					reply_attr = na;
					na->u.lots.length = strlen(config[CONFIG_XAUTH_PASSWORD]);
					na->u.lots.data = xallocc(na->u.lots.length);
					memcpy(na->u.lots.data, config[CONFIG_XAUTH_PASSWORD],
						na->u.lots.length);
				}
				break;
			default:
				;
			}

		/* Send the response.  */
		rp = new_isakmp_payload(ISAKMP_PAYLOAD_MODECFG_ATTR);
		rp->u.modecfg.type = ISAKMP_MODECFG_CFG_REPLY;
		rp->u.modecfg.id = r->payload->next->u.modecfg.id;
		rp->u.modecfg.attributes = reply_attr;
		sendrecv_phase2(s, rp, ISAKMP_EXCHANGE_MODECFG_TRANSACTION,
			r->message_id, 0, 0, 0, 0, 0, 0, 0);

		free_isakmp_packet(r);
	}

	DEBUG(2, printf("S5.6\n"));
	{
		/* The final SET should have just one attribute.  */
		int reject = 0;
		struct isakmp_attribute *a = r->payload->next->u.modecfg.attributes;
		uint16_t set_result;

		if (a == NULL
			|| a->type != ISAKMP_XAUTH_ATTRIB_STATUS
			|| a->af != isakmp_attr_16 || a->next != NULL) {
			reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
			phase2_fatal(s, "xauth SET response rejected: %s", reject);
		}
		set_result = a->u.attr_16;

		/* ACK the SET.  */
		r->payload->next->u.modecfg.type = ISAKMP_MODECFG_CFG_ACK;
		sendrecv_phase2(s, r->payload->next, ISAKMP_EXCHANGE_MODECFG_TRANSACTION,
			r->message_id, 1, 0, 0, 0, 0, 0, 0);
		r->payload->next = NULL;
		free_isakmp_packet(r);

		if (set_result == 0)
			error(2, 0, "authentication unsuccessful");
	}
	DEBUG(2, printf("S5.7\n"));
	return 0;
}

static int do_phase_2_config(struct sa_block *s)
{
	struct isakmp_payload *rp;
	struct isakmp_attribute *a;
	struct isakmp_packet *r;
	struct utsname uts;
	uint32_t msgid;
	int reject;
	int seen_address = 0;
	char *strbuf;

	uname(&uts);

	gcry_randomize((uint8_t *) & msgid, sizeof(msgid), GCRY_WEAK_RANDOM);
	if (msgid == 0)
		msgid = 1;

	rp = new_isakmp_payload(ISAKMP_PAYLOAD_MODECFG_ATTR);
	rp->u.modecfg.type = ISAKMP_MODECFG_CFG_REQUEST;
	rp->u.modecfg.id = 20;
	a = NULL;

	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_APPLICATION_VERSION, a);
	a->u.lots.length = strlen(config[CONFIG_VERSION]);
	a->u.lots.data = xallocc(a->u.lots.length);
	memcpy(a->u.lots.data, config[CONFIG_VERSION], a->u.lots.length);

	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_CISCO_DDNS_HOSTNAME, a);
	a->u.lots.length = strlen(uts.nodename);
	a->u.lots.data = xallocc(a->u.lots.length);
	memcpy(a->u.lots.data, uts.nodename, a->u.lots.length);

	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_CISCO_BANNER, a);
	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_CISCO_DO_PFS, a);
	if (opt_udpencap)
		a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_CISCO_UDP_ENCAP_PORT, a);
	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_CISCO_DEF_DOMAIN, a);
	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_NBNS, a);
	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_DNS, a);
	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_NETMASK, a);
	a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_ADDRESS, a);

	rp->u.modecfg.attributes = a;
	sendrecv_phase2(s, rp, ISAKMP_EXCHANGE_MODECFG_TRANSACTION, msgid, 0, 0, 0, 0, 0, 0, 0);

	/* recv and check for notices */
	reject = do_phase2_notice_check(s, &r);
	if (reject == -1)
		return 1;
		
	/* Check the transaction type & message ID are OK.  */
	if (reject == 0 && r->message_id != msgid)
		reject = ISAKMP_N_INVALID_MESSAGE_ID;
	if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_MODECFG_TRANSACTION)
		reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;

	/* After the hash, expect an attribute block.  */
	if (reject == 0
		&& (r->payload->next == NULL
			|| r->payload->next->next != NULL
			|| r->payload->next->type != ISAKMP_PAYLOAD_MODECFG_ATTR
#if 0
			|| r->payload->next->u.modecfg.id != 20
#endif
			|| r->payload->next->u.modecfg.type != ISAKMP_MODECFG_CFG_REPLY))
		reject = ISAKMP_N_PAYLOAD_MALFORMED;

	if (reject != 0)
		phase2_fatal(s, "configuration response rejected: %s", reject);

	unsetenv("CISCO_BANNER");
	unsetenv("CISCO_DEF_DOMAIN");
	unsetenv("INTERNAL_IP4_NBNS");
	unsetenv("INTERNAL_IP4_DNS");
	unsetenv("INTERNAL_IP4_NETMASK");
	unsetenv("INTERNAL_IP4_ADDRESS");

	for (a = r->payload->next->u.modecfg.attributes; a && reject == 0; a = a->next)
		switch (a->type) {
		case ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_ADDRESS:
			if (a->af != isakmp_attr_lots || a->u.lots.length != 4)
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			else {
				addenv_ipv4("INTERNAL_IP4_ADDRESS", a->u.lots.data);
				memcpy(s->our_address, a->u.lots.data, 4);
			}
			seen_address = 1;
			break;

		case ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_NETMASK:
			if (a->af == isakmp_attr_lots && a->u.lots.length == 0) {
				DEBUG(2, printf("ignoring zero length netmask\n"));
				continue;
			}
			if (a->af != isakmp_attr_lots || a->u.lots.length != 4)
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			else
				addenv_ipv4("INTERNAL_IP4_NETMASK", a->u.lots.data);
			break;

		case ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_DNS:
			if (a->af != isakmp_attr_lots || a->u.lots.length != 4)
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			else
				addenv_ipv4("INTERNAL_IP4_DNS", a->u.lots.data);
			break;

		case ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_NBNS:
			if (a->af != isakmp_attr_lots || a->u.lots.length != 4)
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			else
				addenv_ipv4("INTERNAL_IP4_NBNS", a->u.lots.data);
			break;

		case ISAKMP_MODECFG_ATTRIB_CISCO_DEF_DOMAIN:
			if (a->af != isakmp_attr_lots) {
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
				break;
			}
			strbuf = xallocc(a->u.lots.length + 1);
			memcpy(strbuf, a->u.lots.data, a->u.lots.length);
			addenv("CISCO_DEF_DOMAIN", strbuf);
			free(strbuf);
			break;

		case ISAKMP_MODECFG_ATTRIB_CISCO_BANNER:
			if (a->af != isakmp_attr_lots) {
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
				break;
			}
			strbuf = xallocc(a->u.lots.length + 1);
			memcpy(strbuf, a->u.lots.data, a->u.lots.length);
			addenv("CISCO_BANNER", strbuf);
			free(strbuf);
			DEBUG(1, printf("Banner: "));
			DEBUG(1, fwrite(a->u.lots.data, a->u.lots.length, 1, stdout));
			DEBUG(1, printf("\n"));
			break;

		case ISAKMP_MODECFG_ATTRIB_APPLICATION_VERSION:
			DEBUG(2, printf("Remote Application Version: "));
			DEBUG(2, fwrite(a->u.lots.data, a->u.lots.length, 1, stdout));
			DEBUG(2, printf("\n"));
			break;

		case ISAKMP_MODECFG_ATTRIB_CISCO_DO_PFS:
			if (a->af != isakmp_attr_16)
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			else {
				s->do_pfs = a->u.attr_16;
				DEBUG(2, printf("got pfs setting: %d\n", s->do_pfs));
			}
			break;

		case ISAKMP_MODECFG_ATTRIB_CISCO_UDP_ENCAP_PORT:
			if (a->af != isakmp_attr_16)
				reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
			else {
				s->peer_udpencap_port = a->u.attr_16;
				DEBUG(2, printf("got peer udp encapsulation port: %hu\n", s->peer_udpencap_port));
			}
			break;

		default:
			DEBUG(2, printf("unknown attriube %d / 0x%X\n", a->type, a->type));
			break;
		}

	if (reject == 0 && !seen_address)
		reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;

	if (reject != 0)
		phase2_fatal(s, "configuration response rejected: %s", reject);

	DEBUG(1, printf("got address %s\n", getenv("INTERNAL_IP4_ADDRESS")));
	return 0;
}

struct isakmp_attribute *make_transform_ipsec(int dh_group, int hash, int keylen)
{
	struct isakmp_attribute *a = NULL;

	a = new_isakmp_attribute(ISAKMP_IPSEC_ATTRIB_SA_LIFE_DURATION, a);
	a->af = isakmp_attr_lots;
	a->u.lots.length = 4;
	a->u.lots.data = xallocc(a->u.lots.length);
	*((uint32_t *) a->u.lots.data) = htonl(2147483);
	a = new_isakmp_attribute_16(ISAKMP_IPSEC_ATTRIB_SA_LIFE_TYPE, IPSEC_LIFE_SECONDS, a);

	if (dh_group)
		a = new_isakmp_attribute_16(ISAKMP_IPSEC_ATTRIB_GROUP_DESC, dh_group, a);
	a = new_isakmp_attribute_16(ISAKMP_IPSEC_ATTRIB_AUTH_ALG, hash, a);
	a = new_isakmp_attribute_16(ISAKMP_IPSEC_ATTRIB_ENCAP_MODE, encap_mode, a);
	if (keylen != 0)
		a = new_isakmp_attribute_16(ISAKMP_IPSEC_ATTRIB_KEY_LENGTH, keylen, a);

	return a;
}

struct isakmp_payload *make_our_sa_ipsec(struct sa_block *s)
{
	struct isakmp_payload *r = new_isakmp_payload(ISAKMP_PAYLOAD_SA);
	struct isakmp_payload *p = NULL, *pn;
	struct isakmp_attribute *a;
	int dh_grp = get_dh_group_ipsec(s->do_pfs)->ipsec_sa_id;
	unsigned int crypt, hash, keylen;
	int i;

	r = new_isakmp_payload(ISAKMP_PAYLOAD_SA);
	r->u.sa.doi = ISAKMP_DOI_IPSEC;
	r->u.sa.situation = ISAKMP_IPSEC_SIT_IDENTITY_ONLY;
	r->u.sa.proposals = new_isakmp_payload(ISAKMP_PAYLOAD_P);
	r->u.sa.proposals->u.p.spi_size = 4;
	r->u.sa.proposals->u.p.spi = xallocc(4);
	/* The sadb_sa_spi field is already in network order.  */
	memcpy(r->u.sa.proposals->u.p.spi, &s->tous_esp_spi, 4);
	r->u.sa.proposals->u.p.prot_id = ISAKMP_IPSEC_PROTO_IPSEC_ESP;
	for (crypt = 0; supp_crypt[crypt].name != NULL; crypt++) {
		if ((supp_crypt[crypt].my_id == GCRY_CIPHER_DES) && (opt_1des == 0))
			continue;
		keylen = supp_crypt[crypt].keylen;
		for (hash = 0; supp_hash[hash].name != NULL; hash++) {
			pn = p;
			p = new_isakmp_payload(ISAKMP_PAYLOAD_P);
			p->u.p.spi_size = 4;
			p->u.p.spi = xallocc(4);
			/* The sadb_sa_spi field is already in network order.  */
			memcpy(p->u.p.spi, &s->tous_esp_spi, 4);
			p->u.p.prot_id = ISAKMP_IPSEC_PROTO_IPSEC_ESP;
			p->u.p.transforms = new_isakmp_payload(ISAKMP_PAYLOAD_T);
			p->u.p.transforms->u.t.id = supp_crypt[crypt].ipsec_sa_id;
			a = make_transform_ipsec(dh_grp, supp_hash[hash].ipsec_sa_id, keylen);
			p->u.p.transforms->u.t.attributes = a;
			p->next = pn;
		}
	}
	for (i = 0, pn = p; pn; pn = pn->next)
		pn->u.p.number = i++;
	r->u.sa.proposals = p;
	return r;
}

static void setup_link(struct sa_block *s)
{
	struct isakmp_payload *rp, *us, *ke = NULL, *them, *nonce_r = NULL;
	struct isakmp_packet *r;
	struct group *dh_grp = NULL;
	uint32_t msgid;
	int reject;
	uint8_t *p_flat = NULL, *realiv = NULL, realiv_msgid[4];
	size_t p_size = 0;
	uint8_t nonce[20], *dh_public = NULL;
	int ipsec_cry_algo = 0, ipsec_hash_algo = 0, i;

	DEBUG(2, printf("S7.1\n"));
	/* Set up the Diffie-Hellman stuff.  */
	if (get_dh_group_ipsec(s->do_pfs)->my_id) {
		dh_grp = group_get(get_dh_group_ipsec(s->do_pfs)->my_id);
		DEBUG(3, printf("len = %d\n", dh_getlen(dh_grp)));
		dh_public = xallocc(dh_getlen(dh_grp));
		dh_create_exchange(dh_grp, dh_public);
		hex_dump("dh_public", dh_public, dh_getlen(dh_grp));
	}

	gcry_randomize((uint8_t *) & s->tous_esp_spi, sizeof(s->tous_esp_spi), GCRY_WEAK_RANDOM);
	rp = make_our_sa_ipsec(s);
	gcry_randomize((uint8_t *) nonce, sizeof(nonce), GCRY_WEAK_RANDOM);
	rp->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_NONCE, nonce, sizeof(nonce));

	us = new_isakmp_payload(ISAKMP_PAYLOAD_ID);
	us->u.id.type = ISAKMP_IPSEC_ID_IPV4_ADDR;
	us->u.id.length = 4;
	us->u.id.data = xallocc(4);
	memcpy(us->u.id.data, s->our_address, sizeof(struct in_addr));
	them = new_isakmp_payload(ISAKMP_PAYLOAD_ID);
	them->u.id.type = ISAKMP_IPSEC_ID_IPV4_ADDR_SUBNET;
	them->u.id.length = 8;
	them->u.id.data = xallocc(8);
	memset(them->u.id.data, 0, 8);
	us->next = them;

	if (!dh_grp) {
		rp->next->next = us;
	} else {
		rp->next->next = new_isakmp_data_payload(ISAKMP_PAYLOAD_KE,
			dh_public, dh_getlen(dh_grp));
		rp->next->next->next = us;
	}

	gcry_randomize((uint8_t *) & msgid, sizeof(&msgid), GCRY_WEAK_RANDOM);
	if (msgid == 0)
		msgid = 1;

	DEBUG(2, printf("S7.2\n"));
	for (i = 0; i < 4; i++) {
		sendrecv_phase2(s, rp, ISAKMP_EXCHANGE_IKE_QUICK,
			msgid, 0, &p_flat, &p_size, 0, 0, 0, 0);

		if (realiv == NULL) {
			realiv = xallocc(s->ivlen);
			memcpy(realiv, s->current_iv, s->ivlen);
			memcpy(realiv_msgid, s->current_iv_msgid, 4);
		}

		DEBUG(2, printf("S7.3\n"));
		reject = unpack_verify_phase2(s, r_packet, r_length, &r, nonce, sizeof(nonce));

		DEBUG(2, printf("S7.4\n"));
		if (((reject == 0) || (reject == ISAKMP_N_AUTHENTICATION_FAILED))
			&& r->exchange_type == ISAKMP_EXCHANGE_INFORMATIONAL) {
			/* handle notifie responder-lifetime (ignore) */
			/* (broken hash => ignore AUTHENTICATION_FAILED) */
			if (reject == 0 && r->payload->next->type != ISAKMP_PAYLOAD_N)
				reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;

			if (reject == 0
				&& r->payload->next->u.n.type ==
				ISAKMP_N_IPSEC_RESPONDER_LIFETIME) {
				DEBUG(2, printf("ignoring responder-lifetime notify\n"));
				memcpy(s->current_iv, realiv, s->ivlen);
				memcpy(s->current_iv_msgid, realiv_msgid, 4);
				continue;
			}
		}

		/* Check the transaction type & message ID are OK.  */
		if (reject == 0 && r->message_id != msgid)
			reject = ISAKMP_N_INVALID_MESSAGE_ID;

		if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_IKE_QUICK)
			reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;

		/* The SA payload must be second.  */
		if (reject == 0 && r->payload->next->type != ISAKMP_PAYLOAD_SA)
			reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;

		if (p_flat)
			free(p_flat);
		if (realiv)
			free(realiv);
		break;
	}

	DEBUG(2, printf("S7.5\n"));
	if (reject != 0)
		phase2_fatal(s, "quick mode response rejected: %s\ncheck pfs setting", reject);

	DEBUG(2, printf("S7.6\n"));
	for (rp = r->payload->next; rp && reject == 0; rp = rp->next)
		switch (rp->type) {
		case ISAKMP_PAYLOAD_SA:
			if (reject == 0 && rp->u.sa.doi != ISAKMP_DOI_IPSEC)
				reject = ISAKMP_N_DOI_NOT_SUPPORTED;
			if (reject == 0 && rp->u.sa.situation != ISAKMP_IPSEC_SIT_IDENTITY_ONLY)
				reject = ISAKMP_N_SITUATION_NOT_SUPPORTED;
			if (reject == 0 &&
				(rp->u.sa.proposals == NULL || rp->u.sa.proposals->next != NULL))
				reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
			if (reject == 0 &&
				rp->u.sa.proposals->u.p.prot_id != ISAKMP_IPSEC_PROTO_IPSEC_ESP)
				reject = ISAKMP_N_INVALID_PROTOCOL_ID;
			if (reject == 0 && rp->u.sa.proposals->u.p.spi_size != 4)
				reject = ISAKMP_N_INVALID_SPI;
			if (reject == 0 &&
				(rp->u.sa.proposals->u.p.transforms == NULL
					|| rp->u.sa.proposals->u.p.transforms->next != NULL))
				reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
			if (reject == 0) {
				struct isakmp_attribute *a
					= rp->u.sa.proposals->u.p.transforms->u.t.attributes;
				int seen_enc = rp->u.sa.proposals->u.p.transforms->u.t.id;
				int seen_auth = 0, seen_encap = 0, seen_group = 0, seen_keylen = 0;

				memcpy(&s->tothem_esp_spi, rp->u.sa.proposals->u.p.spi, 4);

				for (; a && reject == 0; a = a->next)
					switch (a->type) {
					case ISAKMP_IPSEC_ATTRIB_AUTH_ALG:
						if (a->af == isakmp_attr_16)
							seen_auth = a->u.attr_16;
						else
							reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
						break;
					case ISAKMP_IPSEC_ATTRIB_ENCAP_MODE:
						if (a->af == isakmp_attr_16 &&
							a->u.attr_16 == encap_mode)
							seen_encap = 1;
						else
							reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
						break;
					case ISAKMP_IPSEC_ATTRIB_GROUP_DESC:
						if (dh_grp &&
							a->af == isakmp_attr_16 &&
							a->u.attr_16 ==
							get_dh_group_ipsec(s->do_pfs)->ipsec_sa_id)
							seen_group = 1;
						else
							reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
						break;
					case ISAKMP_IPSEC_ATTRIB_KEY_LENGTH:
						if (a->af == isakmp_attr_16)
							seen_keylen = a->u.attr_16;
						else
							reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
						break;
					case ISAKMP_IPSEC_ATTRIB_SA_LIFE_TYPE:
					case ISAKMP_IPSEC_ATTRIB_SA_LIFE_DURATION:
						break;
					default:
						reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
						break;
					}
				if (reject == 0 && (!seen_auth || !seen_encap ||
						(dh_grp && !seen_group)))
					reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;

				if (reject == 0
					&& get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IPSEC_SA, seen_auth,
						NULL, 0) == NULL)
					reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
				if (reject == 0
					&& get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IPSEC_SA, seen_enc,
						NULL, seen_keylen) == NULL)
					reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;

				if (reject == 0) {
					ipsec_cry_algo =
						get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IPSEC_SA,
						seen_enc, NULL, seen_keylen)->my_id;
					ipsec_hash_algo =
						get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IPSEC_SA,
						seen_auth, NULL, 0)->my_id;
					DEBUG(1, printf("IPSEC SA selected %s-%s\n",
							get_algo(SUPP_ALGO_CRYPT,
								SUPP_ALGO_IPSEC_SA, seen_enc, NULL,
								seen_keylen)->name,
							get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IPSEC_SA,
								seen_auth, NULL, 0)->name));
				}
			}
			break;

		case ISAKMP_PAYLOAD_N:
			break;
		case ISAKMP_PAYLOAD_ID:
			break;
		case ISAKMP_PAYLOAD_KE:
			ke = rp;
			break;
		case ISAKMP_PAYLOAD_NONCE:
			nonce_r = rp;
			break;

		default:
			reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
			break;
		}

	if (reject == 0 && nonce_r == NULL)
		reject = ISAKMP_N_INVALID_HASH_INFORMATION;
	if (reject == 0 && dh_grp && (ke == NULL || ke->u.ke.length != dh_getlen(dh_grp)))
		reject = ISAKMP_N_INVALID_KEY_INFORMATION;
	if (reject != 0)
		phase2_fatal(s, "quick mode response rejected [2]: %s", reject);

	/* send final packet */
	sendrecv_phase2(s, NULL, ISAKMP_EXCHANGE_IKE_QUICK,
		msgid, 1, 0, 0, nonce, sizeof(nonce),
		nonce_r->u.nonce.data, nonce_r->u.nonce.length);

	DEBUG(2, printf("S7.7\n"));
	/* Create the delete payload, now that we have all the information.  */
	{
		struct isakmp_payload *d_isakmp, *d_ipsec;
		uint32_t del_msgid;

		gcry_randomize((uint8_t *) & del_msgid, sizeof(del_msgid), GCRY_WEAK_RANDOM);
		d_isakmp = new_isakmp_payload(ISAKMP_PAYLOAD_D);
		d_isakmp->u.d.doi = ISAKMP_DOI_IPSEC;
		d_isakmp->u.d.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
		d_isakmp->u.d.spi_length = 2 * ISAKMP_COOKIE_LENGTH;
		d_isakmp->u.d.num_spi = 1;
		d_isakmp->u.d.spi = xallocc(1 * sizeof(uint8_t *));
		d_isakmp->u.d.spi[0] = xallocc(2 * ISAKMP_COOKIE_LENGTH);
		memcpy(d_isakmp->u.d.spi[0] + ISAKMP_COOKIE_LENGTH * 0, s->i_cookie,
			ISAKMP_COOKIE_LENGTH);
		memcpy(d_isakmp->u.d.spi[0] + ISAKMP_COOKIE_LENGTH * 1, s->r_cookie,
			ISAKMP_COOKIE_LENGTH);
		d_ipsec = new_isakmp_payload(ISAKMP_PAYLOAD_D);
		d_ipsec->next = d_isakmp;
		d_ipsec->u.d.doi = ISAKMP_DOI_IPSEC;
		d_ipsec->u.d.protocol = ISAKMP_IPSEC_PROTO_IPSEC_ESP;
		d_ipsec->u.d.spi_length = 4;
		d_ipsec->u.d.num_spi = 2;
		d_ipsec->u.d.spi = xallocc(2 * sizeof(uint8_t *));
		d_ipsec->u.d.spi[0] = xallocc(d_ipsec->u.d.spi_length);
		memcpy(d_ipsec->u.d.spi[0], &s->tous_esp_spi, 4);
		d_ipsec->u.d.spi[1] = xallocc(d_ipsec->u.d.spi_length);
		memcpy(d_ipsec->u.d.spi[1], &s->tothem_esp_spi, 4);
		phase2_authpacket(s, d_ipsec, ISAKMP_EXCHANGE_INFORMATIONAL,
			del_msgid, &s->kill_packet, &s->kill_packet_size,
			nonce, sizeof(nonce), nonce_r->u.nonce.data, nonce_r->u.nonce.length);
		isakmp_crypt(s, s->kill_packet, s->kill_packet_size, 1);
	}
	DEBUG(2, printf("S7.8\n"));

	/* Set up the interface here so it's ready when our acknowledgement
	 * arrives.  */
	config_tunnel(s);
	DEBUG(2, printf("S7.9\n"));
	{
		uint8_t *tous_keys, *tothem_keys;
		struct sockaddr_in tothem_dest, tous_dest;
		unsigned char *dh_shared_secret = NULL;
		int tunnelfd = sockfd;

		if (dh_grp) {
			/* Determine the shared secret.  */
			dh_shared_secret = xallocc(dh_getlen(dh_grp));
			dh_create_shared(dh_grp, dh_shared_secret, ke->u.ke.data);
			hex_dump("dh_shared_secret", dh_shared_secret, dh_getlen(dh_grp));
		}
		tous_keys = gen_keymat(s, ISAKMP_IPSEC_PROTO_IPSEC_ESP, s->tous_esp_spi,
			ipsec_hash_algo, ipsec_cry_algo,
			dh_shared_secret, dh_grp ? dh_getlen(dh_grp) : 0,
			nonce, sizeof(nonce), nonce_r->u.nonce.data, nonce_r->u.nonce.length);
		memset(&tothem_dest, 0, sizeof(tothem_dest));
		tothem_dest.sin_family = AF_INET;
		memcpy(&tothem_dest.sin_addr, s->our_address, 4);
		tothem_keys = gen_keymat(s, ISAKMP_IPSEC_PROTO_IPSEC_ESP, s->tothem_esp_spi,
			ipsec_hash_algo, ipsec_cry_algo,
			dh_shared_secret, dh_grp ? dh_getlen(dh_grp) : 0,
			nonce, sizeof(nonce), nonce_r->u.nonce.data, nonce_r->u.nonce.length);
		memcpy(&tous_dest, dest_addr, sizeof(tous_dest));
		if (opt_udpencap && s->peer_udpencap_port) {
			close(tunnelfd);
			tunnelfd = make_socket(htons(opt_udpencapport));
			tous_dest.sin_port = htons(s->peer_udpencap_port);
			encap_mode = IPSEC_ENCAP_UDP_TUNNEL;
		}
		DEBUG(2, printf("S7.10\n"));
		vpnc_doit(s->tous_esp_spi, tous_keys, &tothem_dest,
			s->tothem_esp_spi, tothem_keys, (struct sockaddr_in *)&tous_dest,
			s->tun_fd, ipsec_hash_algo, ipsec_cry_algo,
			s->kill_packet, s->kill_packet_size, dest_addr,
			encap_mode, tunnelfd,
			config[CONFIG_PID_FILE]);
	}
}

int main(int argc, char **argv)
{
	int do_load_balance;
	const uint8_t hex_test[] = { 0, 1, 2, 3 };

	test_pack_unpack();
	gcry_check_version("1.1.90");
	gcry_control(GCRYCTL_INIT_SECMEM, 16384, 0);
	group_init();
	memset(oursa, 0, sizeof(oursa));

	do_config(argc, argv);
	
	hex_dump("hex_test", hex_test, sizeof(hex_test));

	DEBUG(1, printf("vpnc version " VERSION "\n"));
	DEBUG(2, printf("S1\n"));
	dest_addr = init_sockaddr(config[CONFIG_IPSEC_GATEWAY], ISAKMP_PORT);
	DEBUG(2, printf("S2\n"));
	local_port = htons(atoi(config[CONFIG_LOCAL_PORT]));
	sockfd = make_socket(local_port);
	DEBUG(2, printf("S3\n"));
	setup_tunnel(oursa);

	do_load_balance = 0;
	do {
		DEBUG(2, printf("S4\n"));
		do_phase_1(config[CONFIG_IPSEC_ID], config[CONFIG_IPSEC_SECRET], oursa);
		DEBUG(2, printf("S5\n"));
		if (oursa->auth_algo == IKE_AUTH_XAUTHInitPreShared)
			do_load_balance = do_phase_2_xauth(oursa);
		DEBUG(2, printf("S6\n"));
		if (do_load_balance == 0)
			do_load_balance = do_phase_2_config(oursa);
	} while (do_load_balance);
	DEBUG(2, printf("S7\n"));
	setup_link(oursa);

	return 0;
}
