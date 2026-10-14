/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure admission check: exact observed routing, not register replay. */
#ifndef M4_CODEC_CONTRACT_H
#define M4_CODEC_CONTRACT_H
#include <stddef.h>
#include <string.h>
struct codec_prop_expected { const char *path, *name; const unsigned char *bytes; unsigned size; };
#include "codec-contract-data.h"
typedef const void *(*codec_getprop)(void *, const char *, const char *, unsigned *);
static int codec_contract(codec_getprop get, void *ctx)
{
 unsigned i, j, n, matches; const unsigned char *p;
 for (i=0;i<sizeof(codec_expected)/sizeof(codec_expected[0]);i++) {
  const struct codec_prop_expected *e = &codec_expected[i];
  p=get(ctx,e->path,e->name,&n);
  if (!p || n!=e->size || memcmp(p,e->bytes,n)) return 100+i;
 }
 /* Apple direction2 uses register value0x220 only without the glitchless quirk. */
 if (get(ctx,"/arm-io/gpio0","glitchless",&n) ||
     get(ctx,"/arm-io/nub-gpio0","glitchless",&n)) return 210;
 p=get(ctx,"/arm-io/pmgr","devices",&n);
 if (!p || n<48 || n>65536 || n%48) return 200;
 for (i=0;i<sizeof(codec_pmgr_expected)/48;i++) {
  matches=0;
  for (j=0;j<n;j+=48)
   if (!memcmp(p+j+26,codec_pmgr_expected[i]+26,2)) {
    if (memcmp(p+j,codec_pmgr_expected[i],48)) return 201+i;
    matches++;
   }
  if (matches!=1) return 205+i;
 }
 return 0;
}
#endif
