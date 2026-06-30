/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_config.c - Platform-specific nameserver discovery
 *
 * POSIX:  parses /etc/resolv.conf for "nameserver" lines.
 * Windows: uses GetNetworkParams() from iphlpapi.h.
 * Fallback: "8.8.8.8".
 */

#include "dns_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <iphlpapi.h>
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#endif

/* ───────────────────── POSIX: /etc/resolv.conf ───────────────────── */

#ifndef _WIN32
static int parse_resolv_conf(char out[][46], int max) {
  FILE *f = fopen("/etc/resolv.conf", "r");
  if (!f) return 0;

  char line[256];
  int  n = 0;
  while (n < max && fgets(line, sizeof(line), f)) {
    /* skip comments and blanks */
    char *p = line;
    while (*p == ' ' || *p == '\t')
      ++p;
    if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0') continue;

    /* match "nameserver <ip>" */
    if (strncmp(p, "nameserver", 10) != 0) continue;
    p += 10;
    while (*p == ' ' || *p == '\t')
      ++p;

    /* extract token */
    char ip[46];
    int  i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && i < (int)sizeof(ip) - 1) {
      ip[i++] = *p++;
    }
    ip[i] = '\0';
    if (i == 0) continue;

    /* validate by attempting inet_pton (IPv4 or IPv6) */
    struct in_addr  a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, ip, &a4) == 1 || inet_pton(AF_INET6, ip, &a6) == 1) {
      strncpy(out[n], ip, 45);
      out[n][45] = '\0';
      ++n;
    }
  }
  fclose(f);
  return n;
}
#endif /* !_WIN32 */

/* ───────────────────── Windows: GetNetworkParams ───────────────────── */

#ifdef _WIN32
static int parse_windows(char out[][46], int max) {
  ULONG len = 0;
  if (GetNetworkParams(NULL, &len) != ERROR_BUFFER_OVERFLOW) return 0;

  FIXED_INFO *info = (FIXED_INFO *)malloc(len);
  if (!info) return 0;

  int n = 0;
  if (GetNetworkParams(info, &len) == NO_ERROR) {
    for (IP_ADDR_STRING *s = &info->DnsServerList; s && n < max; s = s->Next) {
      strncpy(out[n], s->IpAddress.String, 45);
      out[n][45] = '\0';
      /* basic validation: must contain a digit */
      if (strpbrk(out[n], "0123456789")) ++n;
    }
  }
  free(info);
  return n;
}
#endif /* _WIN32 */

/* ───────────────────── Public API ───────────────────── */

int dns_config_load_nameservers(char out[][46], int max) {
  if (!out || max <= 0) return 0;

  int n = 0;
#ifdef _WIN32
  n = parse_windows(out, max);
#else
  n = parse_resolv_conf(out, max);
#endif

  if (n <= 0) {
    /* fallback */
    strncpy(out[0], "8.8.8.8", 45);
    out[0][45] = '\0';
    n          = 1;
  }
  return n;
}
