/*
 * if2_version.h -- the EBOOT this build lifted, and where the title hooks' addresses are in it.
 *
 * Every guest address the title code uses (functions it calls or hooks, globals it reads or patches, the
 * instructions it checks) is found in the EBOOT being lifted by tools/if2_resolve.py (the lift step), from code
 * patterns (config/if2_sigs.json), and comes from the header it writes, generated/if2_resolved.h:
 *
 *   IF2_R(NAME)     the value (an address; for a few items a displacement)
 *   IF2_OK(NAME)    1 when it was found in this EBOOT
 *   IF2_F(NAME)     the lifted function func_XXXXXXXX (if2_unresolved_fn when not found)
 *   IF2_HAVE(FEAT)  1 when every item the feature needs was found; else the feature stays off at run time and
 *                   the game keeps its own behaviour (if2_feature_off says so once in the log)
 *
 * The item names, what each one is and the two US builds' values (BCUS98125 disc EBOOT 1.00 and update 1.04,
 * which the resolver must reproduce exactly on those EBOOTs) are in tools/if2_items.py. The addresses in the
 * comments of the title code are the US builds' (mostly the disc EBOOT's, some 1.04's).
 *
 * IF2_EBOOT_V104 is the layout family the resolver saw (1.04's code, or the disc EBOOT's), IF2_TITLE_ID the
 * disc's edition (from PARAM.SFO), IF2_EBOOT_KNOWN 1 on the two US builds.
 *
 * The host tests (tests/test_if2_*.cpp) include title sources without a lift: they get the US disc EBOOT's
 * values from tests/if2_resolved_test.h.
 */
#pragma once

#if defined(__has_include)
#if __has_include("if2_resolved.h")
#include "if2_resolved.h"
#else
#include "../tests/if2_resolved_test.h"
#endif
#else
#include "if2_resolved.h"
#endif

#ifndef IF2_RESOLVED
#error "if2_resolved.h is missing: run the lift step (tools/relift.sh runs tools/if2_resolve.py)"
#endif

#define IF2_R(n)    IF2_R_##n
#define IF2_OK(n)   IF2_OK_##n
#define IF2_F(n)    IF2_FN_##n
#define IF2_HAVE(f) IF2_HAVE_##f

/* A log string that names the US builds' own addresses: kept as it was on those, generic elsewhere. */
#if IF2_EBOOT_KNOWN
#define IF2_KNOWN_STR(s100, s104, other) (IF2_EBOOT_V104 ? (s104) : (s100))
#else
#define IF2_KNOWN_STR(s100, s104, other) (other)
#endif

#include <cstdio>

struct ppu_context;
/* Stands in for a lifted function that was not found in this EBOOT; never called (its feature is off). */
inline void if2_unresolved_fn(struct ppu_context*) {}
/* A title feature that is off on this EBOOT because the resolver did not find what it needs: one log line, from
 * the feature's install (which runs once). */
inline void if2_feature_off(const char* feature, const char* why)
{
    fprintf(stderr, "[if2] %s: off -- not found in this EBOOT (%s): the game's own behaviour (%s)\n", feature, why,
            IF2_EBOOT_NAME);
    fflush(stderr);
}
