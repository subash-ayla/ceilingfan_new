/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __COMMAND_TABLE_H__
#define __COMMAND_TABLE_H__

/*
 * Mapping from config tokens to short NVS strings.
 * This should be included in only one file.
 *
 * These values must not change for forward/backward compatibility.
 */
struct libapp_token {
	const char *short_name;
	const char *name;
};

/*
 * Consider these issues:
 *
 * Adding new values here could break compatibility if it changes the
 * encoding of existing config items that couldn't be shortened before.
 *
 * Some of these don't need to be shortened.
 *
 * If config paths are added in ADA but not here, and the result cannot
 * be shortened, it could cause un ndetected problem.
 *
 * Some of these entries are never used as config path components.
 *
 * Please keep these sorted by the short names.
 */
static const struct libapp_token libapp_tokens[] = {
	{ "Ac",	 "acc" },
	{ "Ad",	 "addr" },
	{ "An",	 "ant" },
	{ "Ap",	 "ap_mode" },
	{ "At",	 "awake_time" },
	{ "Au",	 "auto" },
	{ "Ba",	 "bars" },
	{ "Bi",	 "bssid" },
	{ "Bt",	 "bt" },
	{ "Ca",	 "ca" },
	{ "Cd",	 "connected" },
	{ "Ce",	 "cert" },
	{ "Ch",	 "chan" },
	{ "Ck",	 "clock" },
	{ "Cl",	 "client" },
	{ "Cp",	 "complete" },
	{ "Cr",	 "char" },
	{ "Ct",	 "count" },
	{ "Cu",	 "current" },
	{ "Da",	 "dst_active" },
	{ "Dc",	 "dst_change" },
	{ "Df",	 "default" },
	{ "Dh",	 "dhcp" },
	{ "Dn",	 "dns" },
	{ "Ds",	 "dev_id" },
	{ "Dt",	 "data" },
	{ "Dv",	 "dst_valid" },
	{ "Eb",	 "en_bind" },
	{ "En",	 "enable" },
	{ "Er",	 "error" },
	{ "Et",	 "eth" },
	{ "Fl",	 "file" },
	{ "Gi",	 "gif" },
	{ "Gp",	 "gpio" },
	{ "Gw",	 "gw" },
	{ "Hi",	 "hidden" },
	{ "Hn",	 "hostname" },
	{ "Ho",	 "host" },
	{ "Hs",	 "hist" },
	{ "Ht",	 "http" },
	{ "Hw",	 "hw" },
	{ "Id",	 "id" },
	{ "In",	 "intr" },
	{ "Ip",	 "ip" },
	{ "Iv",	 "interval" },
	{ "Kp",	 "private_key" },
	{ "Ky",	 "key" },
	{ "La",	 "lan" },
	{ "Lg",	 "log" },
	{ "Li",	 "listen" },
	{ "Ln",	 "link" },
	{ "Lo",	 "locale" },
	{ "Ma",	 "mac_addr" },
	{ "Md",	 "mode" },
	{ "Me",	 "metric" },
	{ "Mf",	 "mfi" },
	{ "Mi",	 "min" },
	{ "Mk",	 "mask" },
	{ "Ml",	 "model" },
	{ "Mm",	 "mfg_model" },
	{ "Mn",	 "mfg_mode" },
	{ "Mo",	 "mod" },
	{ "Mp",	 "max_perf" },
	{ "Ms",	 "mfg_serial" },
	{ "Nm",	 "name" },
	{ "Nn",	 "n" },
	{ "No",	 "none" },
	{ "Nt",	 "notify" },
	{ "Om",	 "oem" },
	{ "Pf",	 "profile" },
	{ "Pi",	 "poll_interval" },
	{ "Po",	 "port" },
	{ "Pp",	 "prop" },
	{ "Pr",	 "pri" },
	{ "Pw",	 "power" },
	{ "Rd",	 "ready" },
	{ "Rg",	 "region" },
	{ "Ri",	 "rssi" },
	{ "Rs",	 "reset" },
	{ "Rt",	 "rtc_src" },
	{ "Ru",	 "reg" },
	{ "S1",	 "save_on_ap_connect" },
	{ "S2",	 "sim" },
	{ "S3",	 "spi" },
	{ "Sa",	 "save_on_server_connect" },
	{ "Sb",	 "standby" },
	{ "Sc",	 "scan" },
	{ "Sd",	 "speed" },
	{ "Se",	 "security" },
	{ "Sh",	 "sched" },
	{ "Si",	 "setup_ios_app" },
	{ "Sl",	 "ssl" },
	{ "Sm",	 "setup_mode" },
	{ "Sn",	 "serial" },
	{ "So",	 "source" },
	{ "Sp",	 "standby_powered" },
	{ "Sr",	 "server" },
	{ "Ss",	 "ssid" },
	{ "St",	 "status" },
	{ "Su",	 "unconf_powered" },
	{ "Sx",	 "start" },
	{ "Sy",	 "sys" },
	{ "Sz",	 "snapshot" },
	{ "Tc",	 "tcp" },
	{ "Te",	 "test" },
	{ "Ti",	 "time" },
	{ "Tl",	 "time_limit" },
	{ "Tv",	 "timezone_valid" },
	{ "Ty",	 "type" },
	{ "Tz",	 "timezone" },
	{ "Ua",	 "uart" },
	{ "Us",	 "user" },
	{ "Vl",	 "value" },
	{ "Vs",	 "version" },
	{ "W3",	 "WPA3_Personal" },
	{ "Wa",	 "WPA" },
	{ "We",	 "WEP" },
	{ "Wi",	 "wifi" },
	{ "Wp",	 "WPA2_Personal" },
	{ "Ws",	 "WPS" },
};

#endif /* __COMMAND_TABLE_H__ */
