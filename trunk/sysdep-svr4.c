/*  
    VTun - Virtual Tunnel over TCP/IP network.

    Copyright (C) 1998-2000  Maxim Krasnyansky <max_mk@yahoo.com>

    VTun has been derived from VPPP package by Maxim Krasnyansky. 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <stropts.h>
#include <net/if.h>
#include <net/if_tun.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

static int ip_fd = -1, muxid;

/* 
 * Allocate TUN device, returns opened fd. 
 * Stores dev name in the first arg(must be large enough).
 */
int tun_open(char *dev)
{
	int tun_fd, if_fd, ppa = -1;
	struct ifreq ifr;
	char *ptr;

	if (*dev) {
		ptr = dev;
		while (*ptr && !isdigit((int)*ptr))
			ptr++;
		ppa = atoi(ptr);
	}

	if ((ip_fd = open("/dev/udp", O_RDWR, 0)) < 0) {
		syslog(LOG_ERR, "Can't open /dev/ip");
		return -1;
	}

	if ((tun_fd = open("/dev/tun", O_RDWR, 0)) < 0) {
		syslog(LOG_ERR, "Can't open /dev/tun");
		return -1;
	}

	/* Assign a new PPA and get its unit number. */
	if ((ppa = ioctl(tun_fd, TUNNEWPPA, ppa)) < 0) {
		syslog(LOG_ERR, "Can't assign new interface");
		return -1;
	}

	if ((if_fd = open("/dev/tun", O_RDWR, 0)) < 0) {
		syslog(LOG_ERR, "Can't open /dev/tun (2)");
		return -1;
	}
	if (ioctl(if_fd, I_PUSH, "ip") < 0) {
		syslog(LOG_ERR, "Can't push IP module");
		return -1;
	}

	/* Assign ppa according to the unit number returned by tun device */
	if (ioctl(if_fd, IF_UNITSEL, (char *)&ppa) < 0 && errno != EEXIST) {
		syslog(LOG_ERR, "Can't set PPA %d", ppa);
		return -1;
	}
	if ((muxid = ioctl(ip_fd, I_PLINK, if_fd)) < 0) {
		syslog(LOG_ERR, "Can't link TUN device to IP");
		return -1;
	}
	close(if_fd);

	snprintf(dev, IFNAMSIZ, "tun%d", ppa);

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, dev);
	ifr.ifr_ip_muxid = muxid;

	if (ioctl(ip_fd, SIOCSIFMUXID, &ifr) < 0) {
		ioctl(ip_fd, I_PUNLINK, muxid);
		syslog(LOG_ERR, "Can't set multiplexor id");
		return -1;
	}

	return tun_fd;
}

/* 
 * Close TUN device. 
 */
int tun_close(int fd, char *dev)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, dev);
	if (ioctl(ip_fd, SIOCGIFFLAGS, &ifr) < 0) {
		syslog(LOG_ERR, "Can't get iface flags");
		return 0;
	}
#if 0
	if (ioctl(ip_fd, SIOCGIFMUXID, &ifr) < 0) {
		syslog(LOG_ERR, "Can't get multiplexor id");
		return 0;
	}
	muxid = ifr.ifr_ip_muxid;
#endif

	if (ioctl(ip_fd, I_PUNLINK, muxid) < 0) {
		syslog(LOG_ERR, "Can't unlink interface");
		return 0;
	}

	close(ip_fd);
	close(fd);
	return 0;
}

int tun_write(int fd, unsigned char *buf, int len)
{
	struct strbuf sbuf;
	sbuf.len = len;
	sbuf.buf = buf;
	return putmsg(fd, NULL, &sbuf, 0) >= 0 ? sbuf.len : -1;
}

int tun_read(int fd, unsigned char *buf, int len)
{
	struct strbuf sbuf;
	int f = 0;

	sbuf.maxlen = len;
	sbuf.buf = buf;
	return getmsg(fd, NULL, &sbuf, &f) >= 0 ? sbuf.len : -1;
}

/***********************************************************************/
/* other support functions */

int vasprintf(char **strp, const char *fmt, va_list ap)
{
	int ret;
	char *strbuf;

	ret = vsnprintf(NULL, 0, fmt, ap);
	strbuf = (char *)malloc(ret + 1);
	if (strbuf == NULL) {
		errno = ENOMEM;
		ret = -1;
	}
	vsnprintf(strbuf, ret + 1, fmt, ap);
	*strp = strbuf;
	return ret;
}

int asprintf(char **strp, const char *fmt, ...)
{
	int ret;
	va_list ap;

	va_start(ap, fmt);
	ret = vasprintf(strp, fmt, ap);
	va_end(ap);

	return ret;
}

void error(int status, int errornum, const char *fmt, ...)
{
	char *buf2;
	va_list ap;

	va_start(ap, fmt);
	vasprintf(&buf2, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s", buf2);
	if (errornum)
		fprintf(stderr, ": %s\n", strerror(errornum));
	free(buf2);

	if (status)
		exit(status);
}

int getline(char **line, size_t * length, FILE * stream)
{
	char tmpline[512];
	size_t len;

	fgets(tmpline, sizeof(tmpline), stream);
	len = strlen(tmpline);
	if (feof(stream))
		return -1;
	if (*line == NULL) {
		*line = malloc(len + 1);
		*length = len + 1;
	}
	if (*length < len + 1) {
		*line = realloc(*line, len + 1);
		*length = len + 1;
	}
	if (*line == NULL)
		return -1;
	memcpy(*line, tmpline, len + 1);
	return len;
}

extern char **environ;

int unsetenv(const char *name)
{
	int i, len;

	len = strlen(name);
	for (i = 0; environ[i]; i++)
		if (!strncmp(name, environ[i], len))
			if (environ[i][len] == '=')
				break;

	for (; environ[i] && environ[i + 1]; i++)
		environ[i] = environ[i + 1];
	
	return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
	int ret;
	char *newenv;

	if (overwrite == 0)
		if (getenv(name) != NULL)
			return 0;

	newenv = malloc(strlen(name) + 1 + strlen(value) + 1);
	if (newenv == NULL)
		return -1;

	*newenv = '\0';
	strcat(newenv, name);
	strcat(newenv, "=");
	strcat(newenv, value);

	ret = putenv(newenv);
	if (ret == -1)
		free(newenv);

	return ret;
}
