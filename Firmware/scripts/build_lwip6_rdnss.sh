#!/usr/bin/env bash
# Rebuild custom_libs/liblwip6-1460-feat.a with RDNSS (RFC 8106) and the
# native lwIP mDNS responder enabled.
#
# Why: the prebuilt lwIP IPv6 variant in the ESP8266 Arduino framework is
# compiled with LWIP_ND6_RDNSS_MAX_DNS_SERVERS=0, so a device on an
# IPv6-only (SLAAC) network never learns a DNS resolver from Router
# Advertisements, and with LWIP_MDNS_RESPONDER=0, so the dual-stack mDNS
# responder (the replacement for LEAmDNS, whose IPv6 support is
# nonfunctional) is compiled out. This script rebuilds the single
# liblwip6-1460-feat.a variant with both enabled, using the framework's
# own lwip2 builder and patch set, against lwIP STABLE-2_1_3 (the
# upstream the framework documents). Everything happens in a scratch
# directory; the installed framework package is never modified. The
# result is vendored in custom_libs/ and linked via "-L custom_libs" in
# platformio.ini, which precedes the framework library path so the
# linker prefers it.
#
# ABI WARNING: LWIP_NUM_NETIF_CLIENT_DATA changes the size of struct
# netif. The firmware build MUST compile every translation unit with the
# same value (-D LWIP_NUM_NETIF_CLIENT_DATA=1 in platformio.ini, scoped
# with -L custom_libs) or netif field accesses in the app read shifted
# offsets - silent heap corruption, no build error. MEMP_NUM_SYS_TIMEOUT
# is memp.c-internal (library only), no firmware-side define needed.
#
# Verification: nd6.o in the rebuilt archive references dns_setserver
# (U symbol) and the archive contains mdns.o with mdns_resp_add_netif;
# the stock archive has neither.
#
# Usage: scripts/build_lwip6_rdnss.sh
set -euo pipefail

FRAMEWORK="${FRAMEWORK:-$HOME/.platformio/packages/framework-arduinoespressif8266}"
TOOLCHAIN="${TOOLCHAIN:-$HOME/.platformio/packages/toolchain-xtensa}"
LWIP_REF="${LWIP_REF:-STABLE-2_1_3_RELEASE}"
SCRATCH="$(mktemp -d /tmp/lwip2-rdnss.XXXXXX)"
OUT="$(cd "$(dirname "$0")/.." && pwd)/custom_libs"

echo "scratch:   $SCRATCH"
echo "framework: $FRAMEWORK"
echo "toolchain: $TOOLCHAIN"

cp -r "$FRAMEWORK/tools/sdk" "$SCRATCH/sdk"
chmod +x "$SCRATCH/sdk/lwip2/builder/makefiles/"* 2>/dev/null || true

git clone --depth 1 --branch "$LWIP_REF" \
    https://github.com/lwip-tcpip/lwip.git "$SCRATCH/sdk/lwip2/builder/lwip2-src"

LWIPOPTS="$SCRATCH/sdk/lwip2/builder/glue-lwip/arduino/lwipopts.h"

# Knob 1: enable RDNSS (DNS servers from Router Advertisements).
sed -i 's|#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS  0 .*|#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS  2 // RDNSS rebuild (RFC 8106)|' \
    "$LWIPOPTS"
grep -q "RDNSS_MAX_DNS_SERVERS  2" "$LWIPOPTS"

# Knob 2: netif client-data slot for the mDNS responder's per-netif state.
# At 0, netif_alloc_client_data_id() is compiled out and mdns can't attach.
# Grows struct netif - see ABI WARNING in the header.
sed -i 's|#define LWIP_NUM_NETIF_CLIENT_DATA      0|#define LWIP_NUM_NETIF_CLIENT_DATA      1 // mDNS responder netif state|' \
    "$LWIPOPTS"
grep -q "LWIP_NUM_NETIF_CLIENT_DATA      1" "$LWIPOPTS"

# Knob 3: two extra sys_timeout slots for the mDNS probe/announce timers.
sed -i 's|#define MEMP_NUM_SYS_TIMEOUT            LWIP_NUM_SYS_TIMEOUT_INTERNAL|#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 2) // mDNS probe/announce timers|' \
    "$LWIPOPTS"
grep -q "LWIP_NUM_SYS_TIMEOUT_INTERNAL + 2" "$LWIPOPTS"

# Knob 4: the mDNS responder itself. No line exists in lwipopts.h (the
# default lives in mdns_opts.h), so insert one just inside the include
# guard rather than replace.
sed -i '/^#define MYLWIPOPTS_H/a #define LWIP_MDNS_RESPONDER             1 // native dual-stack mDNS responder' \
    "$LWIPOPTS"
grep -q "define LWIP_MDNS_RESPONDER             1" "$LWIPOPTS"

# Build the single variant the IPv6 firmware links (parameters mirror the
# v6/1460/feat case of the builder's Makefile.arduino loop). The builder
# applies its patches/ set to lwip2-src automatically.
make -C "$SCRATCH/sdk/lwip2/builder" -f makefiles/Makefile.build-lwip2 \
    target=arduino DEFINE_TARGET=ARDUINO SDK=../.. \
    LWIP_ESP=glue-esp/lwip-1.4-arduino/include \
    LWIP_LIB=liblwip6-1460-feat.a \
    LWIP_LIB_RELEASE=../../lib/liblwip6-1460-feat.a \
    LWIP_INCLUDES_RELEASE=../include \
    TOOLS="$TOOLCHAIN/bin/xtensa-lx106-elf-" \
    TCP_MSS=1460 LWIP_FEATURES=1 LWIP_IPV6=1 \
    BUILD=build-1460-feat-v6 install

# Verify the RDNSS and mDNS code made it in before vendoring. Capture nm
# output to a file first: with pipefail, "nm | grep -q" can abort the
# script when grep -q exits at first match and nm dies on SIGPIPE.
"$TOOLCHAIN/bin/xtensa-lx106-elf-nm" "$SCRATCH/sdk/lib/liblwip6-1460-feat.a" \
    > "$SCRATCH/nm.txt"
grep -q dns_setserver "$SCRATCH/nm.txt"
grep -q "T mdns_resp_add_netif" "$SCRATCH/nm.txt"
mkdir -p "$OUT"
cp "$SCRATCH/sdk/lib/liblwip6-1460-feat.a" "$OUT/"
( cd "$OUT" && sha256sum liblwip6-1460-feat.a > liblwip6-1460-feat.a.sha256 )

echo "vendored: $OUT/liblwip6-1460-feat.a"
echo "scratch left at $SCRATCH (rm -rf when satisfied)"
