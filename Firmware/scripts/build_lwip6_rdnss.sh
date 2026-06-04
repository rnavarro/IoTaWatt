#!/usr/bin/env bash
# Rebuild custom_libs/liblwip6-1460-feat.a with RDNSS enabled (RFC 8106).
#
# Why: the prebuilt lwIP IPv6 variant in the ESP8266 Arduino framework is
# compiled with LWIP_ND6_RDNSS_MAX_DNS_SERVERS=0, so a device on an
# IPv6-only (SLAAC) network never learns a DNS resolver from Router
# Advertisements. This script rebuilds the single liblwip6-1460-feat.a
# variant with the knob set to 2, using the framework's own lwip2 builder
# and patch set, against lwIP STABLE-2_1_3 (the upstream the framework
# documents). Everything happens in a scratch directory; the installed
# framework package is never modified. The result is vendored in
# custom_libs/ and linked via "-L custom_libs" in platformio.ini, which
# precedes the framework library path so the linker prefers it.
#
# Verification: nd6.o in the rebuilt archive references dns_setserver
# (U symbol); the stock archive's nd6.o does not.
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

# The one knob change: enable RDNSS (DNS servers from Router Advertisements).
sed -i 's|#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS  0 .*|#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS  2 // RDNSS rebuild (RFC 8106)|' \
    "$SCRATCH/sdk/lwip2/builder/glue-lwip/arduino/lwipopts.h"
grep -q "RDNSS_MAX_DNS_SERVERS  2" "$SCRATCH/sdk/lwip2/builder/glue-lwip/arduino/lwipopts.h"

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

# Verify the RDNSS code made it in before vendoring.
"$TOOLCHAIN/bin/xtensa-lx106-elf-nm" "$SCRATCH/sdk/lib/liblwip6-1460-feat.a" \
    | grep -q dns_setserver
mkdir -p "$OUT"
cp "$SCRATCH/sdk/lib/liblwip6-1460-feat.a" "$OUT/"
( cd "$OUT" && sha256sum liblwip6-1460-feat.a > liblwip6-1460-feat.a.sha256 )

echo "vendored: $OUT/liblwip6-1460-feat.a"
echo "scratch left at $SCRATCH (rm -rf when satisfied)"
