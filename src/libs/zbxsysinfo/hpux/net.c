/*
** Zabbix
** Copyright (C) 2001-2013 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include <sys/unistd.h>
#include <sys/stropts.h>
#include <sys/dlpi.h>
#include <sys/dlpi_ext.h>
#include <sys/mib.h>

#include "common.h"
#include "sysinfo.h"
#include "zbxjson.h"

#define PPA(n) (*(dl_hp_ppa_info_t *)(buf_ctl + ack.dl_offset + n*sizeof(dl_hp_ppa_info_t)))

char	buf_ctl[1024];

/* Low Level Discovery needs a way to get the list of network interfaces available */
/* on the monitored system. HP-UX versions starting from 11.31 have if_nameindex() */
/* available in libc, older versions have it in libipv6 which we do not want to    */
/* depend on. So for older versions we use different code to get that list.        */
/* More information:                                                               */
/* h20000.www2.hp.com/bc/docs/support/SupportManual/c02258083/c02258083.pdf        */
struct strbuf	ctlbuf = {
	1024,
	0,
	buf_ctl
};

#if HPUX_VERSION < 1131

#define ZBX_IF_SEP	','

void	add_if_name(char **if_list, size_t *if_list_alloc, size_t *if_list_offset, const char *name)
{
	if (FAIL == str_in_list(*if_list, name, ZBX_IF_SEP))
	{
		if ('\0' != **if_list)
			zbx_chrcpy_alloc(if_list, if_list_alloc, if_list_offset, ZBX_IF_SEP);

		zbx_strcpy_alloc(if_list, if_list_alloc, if_list_offset, name);
	}
}

int	get_if_names(char **if_list, size_t *if_list_alloc, size_t *if_list_offset)
{
	int			s, ifreq_size, numifs, i, family = AF_INET;
	struct sockaddr		*from;
	size_t			fromlen;
	u_char			*buffer = NULL;
	struct ifconf		ifc;
	struct ifreq		*ifr;
	struct if_laddrconf	lifc;
	struct if_laddrreq	*lifr;

	if (-1 == (s = socket(family, SOCK_DGRAM, 0)))
		return FAIL;

	ifc.ifc_buf = 0;
	ifc.ifc_len = 0;

	if (0 == ioctl(s, SIOCGIFCONF, (caddr_t)&ifc) && 0 != ifc.ifc_len)
		ifreq_size = 2 * ifc.ifc_len;
	else
		ifreq_size = 2 * 512;

	buffer = zbx_malloc(buffer, ifreq_size);
	memset(buffer, 0, ifreq_size);

	ifc.ifc_buf = (caddr_t)buffer;
	ifc.ifc_len = ifreq_size;

	if (-1 == ioctl(s, SIOCGIFCONF, &ifc))
		goto next;

	/* check all IPv4 interfaces */
	ifr = (struct ifreq *)ifc.ifc_req;
	while ((u_char *)ifr < (u_char *)(buffer + ifc.ifc_len))
	{
		from = &ifr->ifr_addr;

		if (AF_INET6 != from->sa_family && AF_INET != from->sa_family)
			continue;

		add_if_name(if_list, if_list_alloc, if_list_offset, ifr->ifr_name);

#ifdef _SOCKADDR_LEN
		ifr = (struct ifreq *)((char *)ifr + sizeof(*ifr) + (from->sa_len > sizeof(*from) ? from->sa_len - sizeof(*from) : 0));
#else
		ifr++;
#endif
	}
next:
	zbx_free(buffer);
	close(s);

#if defined (SIOCGLIFCONF)
	family = AF_INET6;

	if (-1 == (s = socket(family, SOCK_DGRAM, 0)))
		return FAIL;

	i = ioctl(s, SIOCGLIFNUM, (char *)&numifs);
	if (0 == numifs)
	{
		close(s);
		return SUCCEED;
	}

	lifc.iflc_len = numifs * sizeof(struct if_laddrreq);
	lifc.iflc_buf = zbx_malloc(NULL, lifc.iflc_len);
	buffer = (u_char *)lifc.iflc_buf;

	if (-1 == ioctl(s, SIOCGLIFCONF, &lifc))
		goto end;

	/* check all IPv6 interfaces */
	for (lifr = lifc.iflc_req; '\0' != *lifr->iflr_name; lifr++)
	{
		from = (struct sockaddr *)&lifr->iflr_addr;

		if (AF_INET6 != from->sa_family && AF_INET != from->sa_family)
			continue;

		add_if_name(if_list, if_list_alloc, if_list_offset, lifr->iflr_name);
	}
end:
	zbx_free(buffer);
	close(s);
#endif
	return SUCCEED;
}
#endif	/* HPUX_VERSION < 1131 */

int	NET_IF_DISCOVERY(AGENT_REQUEST *request, AGENT_RESULT *result)
{
#if HPUX_VERSION < 1131
	char			*if_list = NULL, *if_name_end;
	size_t			if_list_alloc = 64, if_list_offset = 0;
#else
	struct if_nameindex	*ni;
	int			i;
#endif
	struct zbx_json		j;
	char			*if_name;

	zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);

	zbx_json_addarray(&j, ZBX_PROTO_TAG_DATA);

#if HPUX_VERSION < 1131
	if_list = zbx_malloc(if_list, if_list_alloc);
	*if_list = '\0';

	if (FAIL == get_if_names(&if_list, &if_list_alloc, &if_list_offset))
		return SYSINFO_RET_FAIL;

	if_name = if_list;

	while (NULL != if_name)
	{
		if (NULL != (if_name_end = strchr(if_name, ZBX_IF_SEP)))
			*if_name_end = '\0';
#else
	for (ni = if_nameindex(), i = 0; 0 != ni[i].if_index; i++)
	{
		if_name = ni[i].if_name;
#endif
		zbx_json_addobject(&j, NULL);
		zbx_json_addstring(&j, "{#IFNAME}", if_name, ZBX_JSON_TYPE_STRING);
		zbx_json_close(&j);
#if HPUX_VERSION < 1131

		if (NULL != if_name_end)
		{
			*if_name_end = ZBX_IF_SEP;
			if_name = if_name_end + 1;
		}
		else
			if_name = NULL;
#endif
	}

#if HPUX_VERSION < 1131
	zbx_free(if_list);
#else
	if_freenameindex(ni);
#endif
	zbx_json_close(&j);

	SET_STR_RESULT(result, strdup(j.buffer));

	zbx_json_free(&j);

	return SYSINFO_RET_OK;
}

/* Attaches to a PPA via an already open stream to DLPI provider. */
static int	dlpi_attach(int fd, int ppa)
{
	dl_attach_req_t		attach_req;
	int			ret, flags = RS_HIPRI;

	attach_req.dl_primitive = DL_ATTACH_REQ;
	attach_req.dl_ppa = ppa;

	ctlbuf.len = sizeof(attach_req);
	ctlbuf.buf = (char *)&attach_req;

	if (0 > putmsg(fd, &ctlbuf, NULL, flags)) {
		perror("dlpi_attach: putmsg");
		return 0;
	}

	ctlbuf.buf = buf_ctl;
	ctlbuf.maxlen = 1024;

	if (0 > getmsg(fd, &ctlbuf, NULL, &flags)) {
		perror("dlpi_attach: getmsg");
		return 0;
	}

	ret = *(int *)buf_ctl;

	if (ret != DL_OK_ACK)
		return 0;

	/* Succesfully attached to a PPA. */
	return 1;
}

/* Detaches from a PPA via an already open stream to DLPI provider. */
static int	dlpi_detach(int fd)
{
	dl_detach_req_t		detach_req;
	int			ret, flags = RS_HIPRI;

	detach_req.dl_primitive = DL_DETACH_REQ;

	ctlbuf.len = sizeof(detach_req);
	ctlbuf.buf = (char *)&detach_req;

	if (0 > putmsg(fd, &ctlbuf, NULL, flags)) {
		perror("dlpi_detach: putmsg");
		return 0;
	}

	ctlbuf.buf = buf_ctl;
	ctlbuf.maxlen = 1024;

	if (0 > getmsg(fd, &ctlbuf, NULL, &flags)) {
		perror("dlpi_detach: getmsg");
		return 0;
	}

	ret = *(int *)buf_ctl;

	if (ret != DL_OK_ACK)
		return 0;

	/* Succesfully detached. */
	return 1;
}

static int	dlpi_get_stats(int fd, Ext_mib_t *mib)
{
	dl_get_statistics_req_t		stat_req;
	dl_get_statistics_ack_t		stat_msg;
	int				ret, flags = RS_HIPRI;

	stat_req.dl_primitive = DL_GET_STATISTICS_REQ;

	ctlbuf.len = sizeof(stat_req);
	ctlbuf.buf = (char *)&stat_req;

	if (0 > putmsg(fd, &ctlbuf, NULL, flags)) {
		perror("dlpi_get_stats: putmsg");
		return 0;
	}

	ctlbuf.buf = buf_ctl;
	ctlbuf.maxlen = 1024;

	if (0 > getmsg(fd, &ctlbuf, NULL, &flags)) {
		perror("dlpi_get_stats: getmsg");
		return 0;
	}

	ret = *(int *)buf_ctl;

	if (ret != DL_GET_STATISTICS_ACK)
		return 0;

	stat_msg = *(dl_get_statistics_ack_t *)buf_ctl;

	memcpy (mib, (Ext_mib_t *)(buf_ctl + stat_msg.dl_stat_offset), sizeof(Ext_mib_t));
	return 1;
}

static int	get_net_stat(Ext_mib_t *mib, char *if_name)
{
	int	fd;
	int	ppa;

	if (-1 == (fd = open("/dev/dlpi", O_RDWR)))
		return 0;

	if (0 == get_ppa(fd, if_name, &ppa)){
		close(fd);
		return 0;
	}

	if (0 == dlpi_attach(fd, ppa))
		return 0;

	if (0 == dlpi_get_stats(fd, mib))
		return 0;

	dlpi_detach(fd);

	close(fd);

	return 1;
}

int get_ppa(int fd, char *if_name, int *ppa)
{
	dl_hp_ppa_req_t		ppa_req;
	dl_hp_ppa_ack_t		ack;
	int			i, offset, ret, flags = RS_HIPRI;
	char			*buf;

	ppa_req.dl_primitive = DL_HP_PPA_REQ;

	ctlbuf.len = sizeof(ppa_req);
	ctlbuf.buf = (char *)&ppa_req;

	if (putmsg(fd, &ctlbuf, NULL, flags) < 0) {
		printf("error: putmsg\n");
		return 0;
	}

	ctlbuf.buf = buf_ctl;
	ctlbuf.maxlen = 1024;

	if (getmsg(fd, &ctlbuf, NULL, &flags) < 0) {
		printf("error: getmsg\n");
		return 0;
	}

	ret = *(int *)buf_ctl;

	if (ret != DL_HP_PPA_ACK)
		return 0;

	ack = *(dl_hp_ppa_ack_t *)buf_ctl;

	buf=malloc(strlen(if_name)+1);
	for (i = 0; i < ack.dl_count; i++) {
		zbx_snprintf(buf, strlen(if_name)+1, "%s%d", PPA(i).dl_module_id_1, PPA(i).dl_ppa);
		if(0 == strcmp(if_name, buf)){
			*ppa = PPA(i).dl_ppa;
			free(buf);
			return 1;
		}
	}

	free(buf);
	return 0;
}

int	NET_IF_IN(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	char		*if_name, *mode;
	Ext_mib_t	mib;

	if (2 < request->nparam)
		return SYSINFO_RET_FAIL;

	if_name = get_rparam(request, 0);
	mode = get_rparam(request, 1);

	if (0 == get_net_stat(&mib, if_name))
		return SYSINFO_RET_FAIL;

	if (NULL == mode || '\0' == *mode || 0 == strcmp(mode, "bytes"))
		SET_UI64_RESULT(result, mib.mib_if.ifInOctets);
	else if (0 == strcmp(mode, "packets"))
		SET_UI64_RESULT(result, mib.mib_if.ifInUcastPkts + mib.mib_if.ifInNUcastPkts);
	else if (0 == strcmp(mode, "errors"))
		SET_UI64_RESULT(result, mib.mib_if.ifInErrors);
	else if (0 == strcmp(mode, "dropped"))
		SET_UI64_RESULT(result, mib.mib_if.ifInDiscards);
	else
		return SYSINFO_RET_FAIL;

	return SYSINFO_RET_OK;
}

int	NET_IF_OUT(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	char		*if_name, *mode;
	Ext_mib_t	mib;

	if (2 < request->nparam)
		return SYSINFO_RET_FAIL;

	if_name = get_rparam(request, 0);
	mode = get_rparam(request, 1);

	if (0 == get_net_stat(&mib, if_name))
		return SYSINFO_RET_FAIL;

	if (NULL == mode || '\0' == *mode || 0 == strcmp(mode, "bytes"))
		SET_UI64_RESULT(result, mib.mib_if.ifOutOctets);
	else if (0 == strcmp(mode, "packets"))
		SET_UI64_RESULT(result, mib.mib_if.ifOutUcastPkts + mib.mib_if.ifOutNUcastPkts);
	else if (0 == strcmp(mode, "errors"))
		SET_UI64_RESULT(result, mib.mib_if.ifOutErrors);
	else if (0 == strcmp(mode, "dropped"))
		SET_UI64_RESULT(result, mib.mib_if.ifOutDiscards);
	else
		return SYSINFO_RET_FAIL;

	return SYSINFO_RET_OK;
}

int	NET_IF_TOTAL(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	char		*if_name, *mode;
	Ext_mib_t	mib;

	if (2 < request->nparam)
		return SYSINFO_RET_FAIL;

	if_name = get_rparam(request, 0);
	mode = get_rparam(request, 1);

	if (0 == get_net_stat(&mib, if_name))
		return SYSINFO_RET_FAIL;

	if (NULL == mode || '\0' == *mode || 0 == strcmp(mode, "bytes"))
		SET_UI64_RESULT(result, mib.mib_if.ifInOctets + mib.mib_if.ifOutOctets);
	else if (0 == strcmp(mode, "packets"))
		SET_UI64_RESULT(result, mib.mib_if.ifInUcastPkts + mib.mib_if.ifInNUcastPkts + mib.mib_if.ifOutUcastPkts + mib.mib_if.ifOutNUcastPkts);
	else if (0 == strcmp(mode, "errors"))
		SET_UI64_RESULT(result, mib.mib_if.ifInErrors + mib.mib_if.ifOutErrors);
	else if (0 == strcmp(mode, "dropped"))
		SET_UI64_RESULT(result, mib.mib_if.ifInDiscards + mib.mib_if.ifOutDiscards);
	else
		return SYSINFO_RET_FAIL;

	return SYSINFO_RET_OK;
}
