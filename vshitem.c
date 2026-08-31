/*
 *  this file is part of Game Categories Lite
 *
 *  Copyright (C) 2009, Bubbletune
 *  Copyright (C) 2011, Codestation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pspkernel.h>
#include "categories_lite.h"
#include "psppaf.h"
#include "pspdefs.h"
#include "stub_funcs.h"
#include "utils.h"
#include "multims.h"
#include "context.h"
#include "gcread.h"
#include "config.h"
#include "filter.h"
#include "logger.h"
#include "language.h"
#include "utils.h"

#define GAME_ACTION 0x0F

extern int game_plug;
extern int model;
extern int context_mode;

extern int sysconf_hint_mode;
extern unsigned long long sysconf_hint_time;

/* Captured in main.c's OnModuleStart vsh_module branch. */
extern u32 vsh_text_addr;
extern u32 vsh_text_size;

/* Set in main.c when the "XMBIH" module loads (GC Lite is first in vsh.txt, so
   we see it before the XMB builds). Used instead of probing vsh memory. */
extern int g_xmbih_present;

/* Shared scratch buffer (used for transient UTF-8/wide conversion across
   vshitem.c / sysconf.c / mode.c). Old GCC defaults (-fcommon) merged the
   same-named globals in each TU into one; modern GCC (>=10) requires
   explicit linkage. This is now the single definition; sysconf.c and mode.c
   declare it `extern`. The XMB calls these formatters serially, so a single
   transient buffer is safe (and matches the original behaviour). */
char user_buffer[256];

int unload = 0;
int lang_id = 1;
int global_pos = 0;

Category *cat_list[2] = { NULL, NULL };

/* Category-index cache validity per location (see AddVshItemPatched).
 * Invalidated by InvalidateCategoryCache() on config resets. */
int cat_cache_valid[2] = { 0, 0 };

void InvalidateCategoryCache(void) {
    cat_cache_valid[MEMORY_STICK] = 0;
    cat_cache_valid[INTERNAL_STORAGE] = 0;
}

static const char* GC_PREFIX = "gc";

static const char* GC_SYSCONF_MODE = "gc0";
static const char* GC_SYSCONF_MODE_SUB = "gcs0";
static const char* GC_SYSCONF_PREFIX = "gc1";
static const char* GC_SYSCONF_PREFIX_SUB = "gcs1";
static const char* GC_SYSCONF_SHOW = "gc2";
static const char* GC_SYSCONF_SHOW_SUB = "gcs2";
static const char* GC_SYSCONF_SORT = "gc3";
static const char* GC_SYSCONF_SORT_SUB = "gcs3";

static const char* GC_UNCATEGORIZED_MS = "gc4";
static const char* GC_UNCATEGORIZED_INTERNAL = "gc5";
static const char* GC_CATEGORY_PREFIX_MS = "gcv_";
static const char* GC_CATEGORY_PREFIX_INTERNAL = "gcw_";

#define FAKE_REGION_LATIN_AMERICA   6
#define FAKE_REGION_HONGKONG        8
#define FAKE_REGION_TAIWAN          9
#define FAKE_REGION_RUSSIA         10
#define FAKE_REGION_CHINA          11
#define FAKE_REGION_DEBUG_TYPE_I   12
#define SE_CONFIG_EX_NID           0x8E426F09
#define XMBIH_COUNT_PATCH_OFFSET   0x20890
#define MIPS_OPCODE_JAL            0x03

typedef struct {
    u32 magic;
    s16 iso_cache_size;
    s16 iso_cache_num;
    u8 iso_cache;
    u8 iso_cache_partition;
    u8 umdseek;
    u8 umdspeed;
    u8 cpubus_clock;
    u8 disable_pause;
    u8 hidedlc;
    u8 umdregion;
    u8 vshregion;
    u8 usbdevice;
    u8 usbcharge;
    u8 hidemac;
    u8 noanalog;
    u8 qaflags;
    u8 launcher_mode;
    u8 hidepics;
    u8 usbdevice_rdonly;
    u8 skiplogos;
    u8 noumd;
    u8 hibblock;
    u8 oldplugin;
    u8 msspeed;
    u8 noled;
    u8 wpa2;
    u8 force_high_memory;
    u8 custom_update;
} SEConfig;

typedef SEConfig *(*GetSEConfigExFunc)(SEConfig *config, int size);

/* Imported DIRECTLY by NID (see imports.S) rather than resolved through
   sctrlHENFindFunction: Adrenaline/Epinephrine <=7 export sctrlSEGetConfigEx but
   NOT the resolver, so calling the resolver crashes there -- wherever it is
   called from, which is why priming it at module_start only made the crash
   happen earlier (hardware, 2026-08-29). XMB Item Hider hit this first and fixed
   it the same way. Declared with the real signature so no function-type cast is
   needed. */
SEConfig *sctrlSEGetConfigEx(SEConfig *config, int size);

int vsh_id[2] = { -1, -1 };
int vsh_action_arg[2] = { -1, -1 };
int last_action_arg[2] = { GAME_ACTION, GAME_ACTION };

int (*UnloadModule)(int skip) = NULL;
int (*ExecuteAction)(int action, int action_arg) = NULL;
int (*AddVshItem)(void *arg, int topitem, SceVshItem *item) = NULL;
wchar_t* (*scePafGetText)(void *arg, const char *name) = NULL;
SceVshItem *(*GetBackupVshItem)(int topitem, u32 unk, SceVshItem *item) = NULL;
int (*sceVshCommonGuiDisplayContext_func)(void *arg, char *page, char *plane, int width, char *mlist, void *temp1, void *temp2) = NULL;

/* The Games column normally lives at topitem==5, but if XMB Item Hider has
   fully-hidden one or more categories LEFT of Games (Extras/Photo/Music/Video),
   XMBIH shifts Game's topitem down by that count. To stay in sync we read
   xmbih.ini directly (single source of truth) on the first get_item_location()
   call -- by then both plugins have finished loading, so the file reflects the
   current boot's config. Missing/unreadable ini means "no shift", preserving
   the original behaviour when XMBIH isn't installed.

   Extras (index 1) is counted when HIDE_ALL_EXTRAS=2 is active in XMBIH.
   In wad11656's XMB Item Hider fork, that path already relocates the ARK CFW
   items before hiding Extras, so the Game shift is real and must be matched
   here. Fake-region Extras hiding is handled separately because it does not
   live in xmbih.ini. */
static int xmbih_game_topitem = 5;
static int xmbih_shift_loaded = 0;
static int fake_region_loaded = 0;
static int fake_region_hides_extras = 0;

/* Return 1 if `key` appears at a line start in buf (after optional whitespace,
   not in a comment/section) with value exactly the single char `val` followed
   by end-of-token. Used to read [Global] flags out of xmbih.ini. */
static int ini_key_is(const char *buf, int n, const char *key, char val) {
    int keylen = sce_paf_private_strlen(key);
    int i;

    for (i = 0; i + keylen + 2 < n; i++) {
        int j, p;

        if (i > 0 && buf[i - 1] != '\n' && buf[i - 1] != '\r')
            continue;
        j = i;
        while (j < n && (buf[j] == ' ' || buf[j] == '\t'))
            j++;
        if (buf[j] == '#' || buf[j] == ';' || buf[j] == '[')
            continue;
        if (sce_paf_private_strncmp(buf + j, key, keylen) != 0)
            continue;
        /* the char after the key name must be ws or '=' so we don't match a
           longer key that happens to start with this one */
        p = j + keylen;
        if (p < n && buf[p] != ' ' && buf[p] != '\t' && buf[p] != '=')
            continue;
        while (p < n && (buf[p] == ' ' || buf[p] == '\t'))
            p++;
        if (p >= n || buf[p] != '=')
            continue;
        p++;
        while (p < n && (buf[p] == ' ' || buf[p] == '\t'))
            p++;
        if (p < n && buf[p] == val &&
            (p + 1 >= n ||
             buf[p + 1] == '\r' || buf[p + 1] == '\n' ||
             buf[p + 1] == ' '  || buf[p + 1] == '\t' ||
             buf[p + 1] == '#'  || buf[p + 1] == ';'))
            return 1;
        return 0;   /* key found but value didn't match */
    }
    return 0;
}

/* Count how many pre-Game top categories XMBIH hides this boot, by reading the
   relevant [Global] flags from xmbih.ini. Only matches keys at line start so
   comments/other sections can't false-positive. */
static int count_pregame_hides_in_ini(void) {
    static char buf[4096];      /* file-scope-static avoids stack pressure */
    const char *path;
    SceUID fd;
    int n, shift = 0;

    /* Try the model-preferred device first, then the other one. XMBIH's ini
       lives wherever xmbih.prx was loaded from, which we cannot see -- we can
       only guess from the model. A wrong guess used to fail SILENTLY (open<0 ->
       "shift 0"), which looks identical to "XMBIH isn't hiding anything": GC
       Lite simply stops shifting Game with nothing else wrong. A PSP Go running
       plugins from ms0:, or any model that reports unexpectedly, hit exactly
       that. Reading a second path costs one failed open on the normal route. */
    path = (model == 4) ? "ef0:/SEPLUGINS/xmbih.ini"
                        : "ms0:/SEPLUGINS/xmbih.ini";
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        path = (model == 4) ? "ms0:/SEPLUGINS/xmbih.ini"
                            : "ef0:/SEPLUGINS/xmbih.ini";
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    }
    if (fd < 0) {
        kprintf("xmbih ini: NOT FOUND on either device (model=%i)\n", model);
        return 0;
    }
    kprintf("xmbih ini: opened %s\n", path);
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;

    /* Pre-Game categories fully hidden via XMBIH's HIDE_ALL_*=2 mechanism. */
    shift += ini_key_is(buf, n, "HIDE_ALL_PHOTO", '2');
    shift += ini_key_is(buf, n, "HIDE_ALL_MUSIC", '2');
    shift += ini_key_is(buf, n, "HIDE_ALL_VIDEO", '2');

    /* Extras (index 1) deliberately does NOT count. XMBIH refuses to hide that
       column: module_start downgrades HIDE_ALL_EXTRAS=2 to 1 (`if (set[1] == 2)
       set[1] = 1;`) and top_category_requested_hidden(1) returns 0
       unconditionally, so Extras never contributes to XMBIH's
       adjust_topitem_for_hidden_categories(). xmbih.ini says so in its own
       header: "Extras and Settings can't be fully hidden with 2 - they fall back
       to 1."

       Counting it here made GC Lite look one column LEFT of where Game actually
       is, which silently disables everything (context menu, categories) with no
       other symptom. Hardware, ARK-4 PSP Go 2026-08-29: probe active=1, ini read
       fine, shift=2 -> want_game=3, while the real Game items arrived at
       topitem=4. */

    return shift;
}

static int xmbih_is_active(void) {
    u32 instr;

    /* Read the location XMBIH patches (its top-category count patch) and ask
       whether a JAL is sitting there. This is the original detection, restored:
       no file, no handshake, no dependency on which VSH plugin loads first --
       which is what the xmbih.state scheme cost us (it broke Game detection on
       ARK-5 when the plugins loaded in the other order, and left a stray file on
       the Memory Stick whenever XMBIH ran without GC Lite to consume it).

       It was removed on the theory that a raw vsh read faults where vshmain is
       sized differently. Bounds-check it against the module's real text size and
       that concern is answered directly, without inventing a second channel.
       (Hardware since showed Adrenaline 7 runs the same 6.61 vshmain, where this
       offset is valid anyway -- the crash that prompted the removal was the late
       sctrlSEGetConfigEx call fixed in the same commit.) */
    if (!vsh_text_addr)
        return 0;
    /* Enforce the bound ONLY when the size is actually known. Not every loader
       populates SceModule2.text_size for vsh_module, and treating "0" as "too
       small" turns this probe into a silent "XMBIH not active" -- which is
       exactly "GC Lite stops shifting Game" with no other symptom. The original
       probe did this read unguarded and was fine on ARK for years, so an unknown
       size must fall back to that behaviour rather than fail closed. */
    if (vsh_text_size && vsh_text_size < XMBIH_COUNT_PATCH_OFFSET + 4) {
        kprintf("xmbih probe: size %08X too small for off %08X -> inactive\n",
                vsh_text_size, (u32)XMBIH_COUNT_PATCH_OFFSET);
        return 0;
    }
    instr = *(u32 *)(vsh_text_addr + XMBIH_COUNT_PATCH_OFFSET);
    /* Decisive line: vsh base, reported size, and the word actually sitting at
       XMBIH's patch site. A JAL (top 6 bits == 3) means XMBIH patched. */
    kprintf("xmbih probe: vsh=%08X size=%08X instr=%08X op=%i active=%i\n",
            vsh_text_addr, vsh_text_size, instr, (int)((instr >> 26) & 0x3F),
            (int)(((instr >> 26) & 0x3F) == MIPS_OPCODE_JAL));
    return ((instr >> 26) & 0x3F) == MIPS_OPCODE_JAL;
}

static int is_ark_custom_item(const char *text) {
    return sce_paf_private_strcmp(text, "xmbmsgtop_sysconf_configuration") == 0 ||
           sce_paf_private_strcmp(text, "xmbmsgtop_sysconf_plugins") == 0 ||
           sce_paf_private_strcmp(text, "xmbmsgtop_custom_launcher") == 0 ||
           sce_paf_private_strcmp(text, "xmbmsgtop_custom_app") == 0 ||
           sce_paf_private_strcmp(text, "xmbmsgtop_150_reboot") == 0;
}

static int fake_region_value_hides_extras(int vshregion) {
    return vshregion == FAKE_REGION_LATIN_AMERICA ||
           vshregion == FAKE_REGION_HONGKONG ||
           vshregion == FAKE_REGION_TAIWAN ||
           vshregion == FAKE_REGION_RUSSIA ||
           vshregion == FAKE_REGION_CHINA ||
           vshregion == FAKE_REGION_DEBUG_TYPE_I;
}

static int extras_hidden_by_fake_region(void) {
    if (!fake_region_loaded) {
        GetSEConfigExFunc get_se_config_ex = NULL;

        fake_region_loaded = 1;

        /* Direct, not via sctrlHENFindFunction -- that resolver is absent on
           Adrenaline/Epinephrine <=7 and calling it crashes there. */
        get_se_config_ex = sctrlSEGetConfigEx;
        if (get_se_config_ex) {
            /* OVERSIZED BUFFER, and success is ret == 0.

               The real SEConfig is LARGER than the 36 bytes this header
               declares, so the CFW writes past a plain `SEConfig se_config;`
               and smashes our stack -- a delayed crash, not a fault at the call.
               That is the ARK-5 UMD-hover crash: disabling this call entirely
               fixed it while the return value was identical (off14 reads 0
               there), so the damage was the write, not the result.
               It stayed hidden until today because the old code path only
               reached this call when g_xmbih_present was set, which never
               happens on ARK-5 (XMBIH loads before GC Lite, so our handler never
               sees it) -- CL-HEAD therefore never made the call and never
               crashed, at the cost of never detecting the shift either.

               Also: this returns int, 0 = success. The header types it as
               returning SEConfig*, so the old truthiness test treated success as
               failure and the region was never read at all. Measured on PRO:
               sizes 36..80 return 0 with the struct populated, 128 returns -2,
               and vshregion sits at BYTE OFFSET 14, not the 16 this struct
               computes. XMB Item Hider carries the identical fix. */
            static u32 cfgbuf[64];
            unsigned char *cfgb = (unsigned char *)cfgbuf;
            int cfgret;

            sce_paf_private_memset(cfgbuf, 0, sizeof(cfgbuf));
            cfgret = (int)(long)get_se_config_ex((SEConfig *)cfgbuf,
                sizeof(SEConfig));
            kprintf("SEConfig ret=%i off14=%i off16=%i\n",
                    cfgret, cfgb[14], cfgb[16]);
            if (cfgret == 0)
                fake_region_hides_extras =
                    fake_region_value_hides_extras(cfgb[14]);
        }
    }

    return fake_region_hides_extras;
}

/* Prime the fake-region detection from a SAFE early context (main.c module_start)
   -- BEFORE the vsh-build phase where load_xmbih_shift runs. At that later point,
   sctrlHENFindFunction / GetSEConfigEx fault on Adrenaline/Epinephrine <=7 (same
   early-boot fragility that killed the old vsh probe). extras_hidden_by_fake_region
   caches (fake_region_loaded), so the later call in load_xmbih_shift is just a
   flag read. Safe no-op on firmwares where it already worked. */
void gc_prime_xmbih_detection(void) {
    (void)extras_hidden_by_fake_region();
}



static void load_xmbih_shift(void) {
    int shift;

    xmbih_shift_loaded = 1;
    shift = 0;

    /* Derive the shift from xmbih.ini, gated on XMBIH actually having patched
       vshmain (probe above). This is what shipped on ARK for years. When XMBIH
       is absent or inert, skip the ini read and the SEConfig lookup entirely --
       both fault at this early boot point on Adrenaline <=7 and there is nothing
       to sync anyway. */
    if (xmbih_is_active()) {
        /* No fake-region term: XMBIH does not count Extras either (main.c
           top_category_requested_hidden case 1). Counting it on both sides was
           tried 2026-08-30 and disproven -- it did not fix the PRO crash and
           only moved Game a column. These two must always agree. */
        shift = count_pregame_hides_in_ini();
    }
    if (shift > 0 && shift <= 4)
        xmbih_game_topitem = 5 - shift;
    kprintf("xmbih fallback: active=%i shift=%i game_topitem=%i\n",
            xmbih_is_active(), shift, xmbih_game_topitem);
}

int get_item_location(int topitem, SceVshItem *item) {
    /*
     * 0: sysconf
     * 1: extra (digital comics)
     * 2: pictures
     * 3: music
     * 4: videos
     * 5: games  (or shifted left by XMBIH if pre-Game categories are hidden)
     * 6: network
     * 7: store
     */
    if (!xmbih_shift_loaded)
        load_xmbih_shift();

    if(topitem == xmbih_game_topitem) {
        if(sce_paf_private_strcmp(item->text, "msgshare_ms") == 0 ||
                sce_paf_private_strcmp(item->text, "gc4") == 0) {
            return MEMORY_STICK;
        } else if(sce_paf_private_strcmp(item->text, "msg_em") == 0 ||
                sce_paf_private_strcmp(item->text, "gc5") == 0) {
            return INTERNAL_STORAGE;
        }
    }
    return -1;
}

SceVshItem *GetBackupVshItemPatched(u32 unk, int topitem, SceVshItem *item) {
    SceVshItem *ret;
    kprintf("item: %s, topitem: %i, id: %i\n", item->text, topitem, item->id);
    SceVshItem *res = GetBackupVshItem(unk, topitem, item);
    if(config.mode == MODE_MULTI_MS) {
        if ((ret = PatchGetBackupVshItemForMultiMs(item, res))) {
            return ret;
        }
    } else if(config.mode == MODE_CONTEXT_MENU){
        PatchGetBackupVshItemForContext(item, res);
    }
    return res;
}

int AddVshItemPatched(void *arg, int topitem, SceVshItem *item) {
    int location;

    if (!xmbih_shift_loaded)
        load_xmbih_shift();

    /* The number CL actually receives, vs the one it is looking for. If these
       never line up for the Game items, that mismatch is the whole bug. */
    kprintf("add: topitem=%i want_game=%i text=%s\n",
            topitem, xmbih_game_topitem, item->text);

    if (topitem == 1 && is_ark_custom_item(item->text) &&
            extras_hidden_by_fake_region()) {
        topitem = xmbih_game_topitem;
    }

    if((location = get_item_location(topitem, item)) >= 0) {
        load_config();
        load_filter();
        lang_id = get_registry_value("/CONFIG/SYSTEM/XMB", "language");
        LoadLanguage(lang_id, model == 4 ? INTERNAL_STORAGE : MEMORY_STICK);
        kprintf("got %s, location: %i, id: %i\n", item->text, location, item->id);
        category[0] = '\0';

        /* Detect only when the System Storage row arrives. If GC is loaded
           before EPI-XmbControl, probing on the earlier Memory Stick row can
           cache a false negative before EPI injects ef0. The injected msg_em
           callback itself proves EPI has reached its AddVshItem path. */
        if (location == INTERNAL_STORAGE) {
            gc_adrenaline();
        }

        if (vsh_items[location]) {
            sce_paf_private_free(vsh_items[location]);
            vsh_items[location] = NULL;
        }
        if(context_items[location]) {
            sce_paf_private_free(context_items[location]);
            context_items[location] = NULL;
        }

        if(config.mode != MODE_FOLDER) {
            /* Reuse the category index across re-adds. Re-indexing walks
             * every folder under /PSP/GAME with several IO probes each,
             * which takes seconds on a large stick -- and the XMB re-adds
             * the Memory Stick item on every sleep/wake resume, making the
             * category entries appear seconds after the rest of the column.
             * The cache is invalidated on config resets (save_config's fake
             * MS reinsertion path); a VSH reset naturally starts cold. A
             * stick swapped while the PSP sleeps keeps the old index until
             * the next VSH reset or config change. */
            if (!cat_cache_valid[location]) {
                ClearCategories(cat_list, location);
                IndexCategories(cat_list, "xxx:/PSP/GAME", location);
                cat_cache_valid[location] = 1;
            }
        }

        // make a backup of the id and action_arg
        if(vsh_id[location] < 0 || vsh_action_arg[location] < 0) {
            vsh_id[location] = item->id;
            vsh_action_arg[location] = item->action_arg;
        } else {
            item->id = vsh_id[location];
            item->action_arg = vsh_action_arg[location];
            item->play_sound = 1;
        }
        global_pos = location;
        kprintf("saved: id: %i, action: %i\n", vsh_id[location], vsh_action_arg[location]);
        last_action_arg[location] = GAME_ACTION;

        /* Restore in case it was changed by MultiMs */
        const char *msg = location == MEMORY_STICK ? "msgshare_ms" : "msg_em";
        sce_paf_private_strcpy(item->text, msg);

        if(config.mode == MODE_MULTI_MS) {
            return PatchAddVshItemForMultiMs(arg, topitem, item, location);
        } else if(config.mode == MODE_CONTEXT_MENU) {
            return PatchAddVshItemForContext(arg, topitem, item, location);
        }
    }
    return AddVshItem(arg, topitem, item);
}

/* Which firmware game action_arg to hand the game_plugin for `location`.
   On Adrenaline, System Storage (ef0) opens with the Memory Stick game action
   (the PSP-Go internal-storage action is a no-op there -- the game_plugin loads
   but never browses); GC's own ef0: path-rewriting in gcread.c then redirects
   the reads to ef0. On real PSP-Go, g_adrenaline is 0 so the native arg is used
   and behaviour is unchanged. */
static int game_action_arg_for(int location) {
    if (location == INTERNAL_STORAGE && gc_adrenaline()) {
        /* Only substitute once the Memory Stick action has been captured.
           vsh_action_arg[] starts { -1, -1 }, so this is a guard against
           handing ExecuteAction a -1, not the cause of any known crash --
           hardware logs showed both slots populated (ms=2 ef=9) in the
           System Storage crash, so this branch did not fire there. Kept
           because -1 would be a real problem if it ever did. */
        if (vsh_action_arg[MEMORY_STICK] >= 0) {
            return vsh_action_arg[MEMORY_STICK];
        }
    }
    return vsh_action_arg[location];
}

int ExecuteActionPatched(int action, int action_arg) {
    int location;
    kprintf("action: %i, action_arg: %i\n", action, action_arg);
    if(config.mode == MODE_MULTI_MS) {
        location = PatchExecuteActionForMultiMs(&action, &action_arg);
        if(location >= 0) {
            last_action_arg[location] = action_arg;
            action_arg = game_action_arg_for(location);
        }
    } else if(config.mode == MODE_CONTEXT_MENU) {
        location = PatchExecuteActionForContext(&action, &action_arg);
        if(location == 2) {
            return 0;
        } else if(location >= 0) {
            last_action_arg[location] = action_arg;
            action_arg = game_action_arg_for(location);

            // simulate MS selection
            action = GAME_ACTION;
        }
    } else if(config.mode == MODE_FOLDER && action == GAME_ACTION) {
        /* Folder mode leaves the firmware storage rows intact, so unlike the
           other two modes it has no synthetic action from which to update
           global_pos. On Adrenaline, Epinephrine translates BOTH rows to the
           normal Memory Stick action before this chained hook runs; the actual
           ms0/ef0 base supplied by game_plugin is therefore the only reliable
           discriminator and ReturnBasePathPatched selects it. Native PSP-Go
           retains distinct action arguments, so handle those here. */
        if(gc_adrenaline()) {
            kprintf("FOLDER-DRIVE-11: action=%i; deferring location to base path\n",
                    action_arg);
        } else if(action_arg == vsh_action_arg[MEMORY_STICK]) {
            global_pos = MEMORY_STICK;
            category[0] = '\0';
            kprintf("FOLDER-DRIVE-11: selected Memory Stick action=%i\n",
                    action_arg);
        } else if(action_arg == vsh_action_arg[INTERNAL_STORAGE]) {
            global_pos = INTERNAL_STORAGE;
            category[0] = '\0';
            kprintf("FOLDER-DRIVE-11: selected System Storage action=%i\n",
                    action_arg);
        }
    }
    kprintf("sending action: %i, action_arg: %i (ms_arg=%i ef_arg=%i adr=%i)\n",
            action, action_arg, vsh_action_arg[MEMORY_STICK],
            vsh_action_arg[INTERNAL_STORAGE], gc_adrenaline());
    return ExecuteAction(action, action_arg);
}

int UnloadModulePatched(int skip) {
    if (unload) {
        skip = -1;
        game_plug = 0;
        unload = 0;
    }
    return UnloadModule(skip);
}

wchar_t* scePafGetTextPatched(void *arg, char *name) {
    if (name && sce_paf_private_strncmp(name, GC_PREFIX, 2) == 0) {
        kprintf("match name: %s\n", name);
        //TODO: optimize this code
        // sysconf 1
        if (sce_paf_private_strcmp(name, GC_SYSCONF_MODE) == 0) {
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_mode);
            return (wchar_t *) user_buffer;
        // sysconf 2
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_PREFIX) == 0) {
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_prefix);
            return (wchar_t *) user_buffer;
        // sysconf 3
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_SHOW) == 0) {
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_show);
            return (wchar_t *) user_buffer;
            // sysconf 4
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_SORT) == 0) {
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_sort);
            return (wchar_t *) user_buffer;
            // sysconf subtitle 1
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_MODE_SUB) == 0) {
            sysconf_hint_mode = 1;
            sysconf_hint_time = sceKernelGetSystemTimeWide();
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_mode_sub);
            return (wchar_t *) user_buffer;
        // sysconf subtitle 2
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_PREFIX_SUB) == 0) {
            sysconf_hint_mode = 2;
            sysconf_hint_time = sceKernelGetSystemTimeWide();
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_prefix_sub);
            return (wchar_t *) user_buffer;
        // sysconf subtitle 3
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_SHOW_SUB) == 0) {
            sysconf_hint_mode = 3;
            sysconf_hint_time = sceKernelGetSystemTimeWide();
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_show_sub);
            return (wchar_t *) user_buffer;
            // sysconf subtitle 4
        } else if (sce_paf_private_strcmp(name, GC_SYSCONF_SORT_SUB) == 0) {
            sysconf_hint_mode = 4;
            sysconf_hint_time = sceKernelGetSystemTimeWide();
            gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.msg_sort_sub);
            return (wchar_t *) user_buffer;
            // Memory Stick
        } else if (sce_paf_private_strncmp(name, GC_CATEGORY_PREFIX_MS, 4) == 0) {
            Category *p = (Category *) sce_paf_private_strtoul(name + 4, NULL, 16);
            if(config.catsort) {
                gc_utf8_to_unicode((wchar_t *) user_buffer, &p->name+2);
            } else {
                gc_utf8_to_unicode((wchar_t *) user_buffer, &p->name);
            }
            fix_text_padding((wchar_t *) user_buffer, scePafGetText(arg, "msgshare_ms"), 'M', 0x2122);
            return (wchar_t *) user_buffer;
        } else if (sce_paf_private_strcmp(name, GC_UNCATEGORIZED_MS) == 0) {
            gc_utf8_to_unicode((wchar_t *) user_buffer, lang_container.msg_uncategorized);
            fix_text_padding((wchar_t *) user_buffer, scePafGetText(arg, "msgshare_ms"), 'M', 0x2122);
            return (wchar_t *) user_buffer;
        // Internal Storage
        } else if (sce_paf_private_strncmp(name, GC_CATEGORY_PREFIX_INTERNAL, 4) == 0) {
            Category *p = (Category *) sce_paf_private_strtoul(name + 4, NULL, 16);
            if(config.catsort) {
                gc_utf8_to_unicode((wchar_t *) user_buffer, &p->name+2);
            } else {
                gc_utf8_to_unicode((wchar_t *) user_buffer, &p->name);
            }
            fix_text_padding((wchar_t *) user_buffer, scePafGetText(arg, "msg_em"), 'M', 0x2122);
            return (wchar_t *) user_buffer;
        } else if (sce_paf_private_strcmp(name, GC_UNCATEGORIZED_INTERNAL) == 0) {
            gc_utf8_to_unicode((wchar_t *) user_buffer, lang_container.msg_uncategorized);
            fix_text_padding((wchar_t *) user_buffer, scePafGetText(arg, "msg_em"), 'M', 0x2122);
            return (wchar_t *) user_buffer;
        }
    // By category (folder mode)
    } else if (name && sce_paf_private_strcmp(name, "msg_by_category") == 0) {
        gc_utf8_to_unicode((wchar_t *)user_buffer, lang_container.by_category);
        return (wchar_t *) user_buffer;
    }
    return scePafGetText(arg, name);
}


static void *game_context_arg = NULL;
static char *game_context_page = NULL;
static char *game_context_plane = NULL;
static char *game_context_mlist = NULL;
static void *game_context_temp1 = NULL;
static void *game_context_temp2 = NULL;
static int game_context_template_valid = 0;

/* Adrenaline 8 runs the stock 6.61 vsh_module, so the Game-column context menu
   is displayed by a fixed callsite in vshmain. Disassembled (vshmain.prx exec
   LOAD segment, file offset 0xA0):

       1cb6c:  3c120000   lui   s2, %hi(g_context_object)
       1cb70:  8e446188   lw    a0, %lo(g_context_object)(s2)
       1cb74:  3c110004   lui   s1, %hi(page)
       1cb78:  3c060004   lui   a2, %hi(plane)
       1cb7c:  3c080004   lui   t0, %hi(mlist)
       1cb80:  2625429c   addiu a1, s1, %lo(page)    -> "J"  (rodata +0x4429C)
       1cb84:  24c647c4   addiu a2, a2, %lo(plane)   -> "M"  (rodata +0x447C4)
       1cb88:  250847c8   addiu t0, t0, %lo(mlist)   -> "NE" (rodata +0x447C8)
       1cb8c:  00004821   move  t1, zero             -> temp1 = NULL
       1cb90:  0c00fefa   jal   sceVshCommonGuiDisplayContext
       1cb94:  00005021   move  t2, zero             -> temp2 = NULL

   The crucial part is `lw a0`: the context object is read from a GLOBAL, it is
   not at any fixed distance from the XMB object. The old
   ADRENALINE_CONTEXT_OBJECT_DELTA (0xA070) was the gap between two unrelated
   heap allocations as they happened to land on one boot, which is why deriving
   from it produced a wild pointer and took Epinephrine down.

   We do not need to reimplement the relocation to find that global. By the time
   we run, the loader has already relocated this code in memory, so the %hi/%lo
   immediates at these instructions hold the real, post-ASLR values. Read them
   back and we get exactly the operands vshmain itself would pass -- correct on
   any base, on any boot, with nothing about heap layout assumed.

   Only the callsite offset is hardcoded, which is what this codebase does
   everywhere (see patches.CommonGuiDisplayContextOffset), and 6.61's vshmain is
   a fixed binary. The 8-instruction opcode signature below is verified before
   any of it is trusted, so a wrong offset bails instead of crashing. */
#define ADRENALINE_CTX_CALLSITE         0x1CB6C

/* Retained purely as a cross-check on the callsite: these rodata offsets were
   confirmed against a real captured template on hardware. */
#define ADRENALINE_CONTEXT_PAGE_OFFSET  0x4429C
#define ADRENALINE_CONTEXT_PLANE_OFFSET 0x447C4
#define ADRENALINE_CONTEXT_MLIST_OFFSET 0x447C8

/* Top halfword of each instruction: opcode + register fields. Relocation only
   ever rewrites the low 16 bits, so this is a stable signature at runtime. */
static const u16 adrenaline_ctx_sig[8] = {
    0x3C12, /* +0x00  lui   s2, ...      */
    0x8E44, /* +0x04  lw    a0, ...(s2)  */
    0x3C11, /* +0x08  lui   s1, ...      */
    0x3C06, /* +0x0C  lui   a2, ...      */
    0x3C08, /* +0x10  lui   t0, ...      */
    0x2625, /* +0x14  addiu a1, s1, ...  */
    0x24C6, /* +0x18  addiu a2, a2, ...  */
    0x2508, /* +0x1C  addiu t0, t0, ...  */
};

#define CTX_INSN(a)   (*(volatile u32 *)(u32)(a))
#define CTX_HI(a)     ((CTX_INSN(a) & 0xFFFF) << 16)
#define CTX_LO(a)     ((int)(short)(CTX_INSN(a) & 0xFFFF))

/* Rebuild the display template from the relocated callsite. Returns 0 on
   success. Adrenaline/6.61 only; every caller gates on that already. */
static int DeriveGameContextTemplate(void **out_arg, char **out_page,
                                     char **out_plane, char **out_mlist) {
    u32 site = vsh_text_addr + ADRENALINE_CTX_CALLSITE;
    u32 obj_global;
    char *page, *plane, *mlist;
    void *arg;
    int i;

    for (i = 0; i < 8; i++) {
        if ((u16)(CTX_INSN(site + (i * 4)) >> 16) != adrenaline_ctx_sig[i]) {
            kprintf("SYSCTX-DIRECT-8: callsite signature mismatch at +%i: %08X want %04X\n",
                    i * 4, (u32)CTX_INSN(site + (i * 4)), adrenaline_ctx_sig[i]);
            return -1;
        }
    }

    obj_global = CTX_HI(site + 0x00) + CTX_LO(site + 0x04);
    page  = (char *)(CTX_HI(site + 0x08) + CTX_LO(site + 0x14));
    plane = (char *)(CTX_HI(site + 0x0C) + CTX_LO(site + 0x18));
    mlist = (char *)(CTX_HI(site + 0x10) + CTX_LO(site + 0x1C));

    /* The rodata pointers must land where hardware said they do. If they do
       not, we are reading the wrong callsite and must not proceed. */
    if (page  != (char *)(vsh_text_addr + ADRENALINE_CONTEXT_PAGE_OFFSET) ||
        plane != (char *)(vsh_text_addr + ADRENALINE_CONTEXT_PLANE_OFFSET) ||
        mlist != (char *)(vsh_text_addr + ADRENALINE_CONTEXT_MLIST_OFFSET)) {
        kprintf("SYSCTX-DIRECT-8: rodata cross-check failed page=%08X plane=%08X mlist=%08X\n",
                (u32)page, (u32)plane, (u32)mlist);
        return -1;
    }

    arg = *(void **)obj_global;
    if (!arg) {
        kprintf("SYSCTX-DIRECT-8: context object global @%08X is NULL\n", obj_global);
        return -1;
    }

    kprintf("SYSCTX-DIRECT-8: derived from callsite: global=%08X arg=%08X\n",
            obj_global, (u32)arg);
    *out_arg = arg; *out_page = page; *out_plane = plane; *out_mlist = mlist;
    return 0;
}

int sceVshCommonGuiDisplayContextPatched(void *arg, char *page, char *plane, int width, char *mlist, void *temp1, void *temp2) {
    int gamecats = context_gamecats;

    kprintf("DisplayContext: page=[%s] plane=[%s] width=%i gamecats=%i global_pos=%i\n",
            page ? page : "(null)", plane ? plane : "(null)", width, context_gamecats, global_pos);
    kprintf("DisplayContextArgs: arg=%08X page=%08X plane=%08X mlist=%08X temp1=%08X temp2=%08X\n",
            (u32)arg, (u32)page, (u32)plane, (u32)mlist, (u32)temp1, (u32)temp2);
    if (gamecats && global_pos == MEMORY_STICK) {
        game_context_arg = arg;
        game_context_page = page;
        game_context_plane = plane;
        game_context_mlist = mlist;
        game_context_temp1 = temp1;
        game_context_temp2 = temp2;
        game_context_template_valid = 1;
        kprintf("SYSCTX-DIRECT-8: captured display template\n");
    }
    if (context_gamecats || (context_mode > 0 && lang_width[lang_id])) {
        width = 1;
        context_gamecats = 0;
    }
    return sceVshCommonGuiDisplayContext_func(arg, page, plane, width, mlist, temp1, temp2);
}

int ReplayGameContextDisplay(void *xmb_context_arg) {
    /* Deliberately unused: the display template is now taken from vshmain's own
       relocated callsite, so nothing is inferred from the XMB object's heap
       address any more. Kept in the signature for the caller in context.c. */
    (void)xmb_context_arg;

    void *arg = game_context_arg;
    char *page = game_context_page;
    char *plane = game_context_plane;
    char *mlist = game_context_mlist;
    void *temp1 = game_context_temp1;
    void *temp2 = game_context_temp2;

    if (!game_context_template_valid) {
        if (!gc_adrenaline() || patch_index != FW_660 || !vsh_text_addr) {
            kprintf("SYSCTX-DIRECT-8: cannot derive display template adrenaline=%i fw=%i vsh=%08X\n",
                    gc_adrenaline(), patch_index, vsh_text_addr);
            return -1;
        }

        /* No Memory Stick context has been shown yet, so there is nothing
           captured to reuse. Rebuild the template from vshmain's own relocated
           callsite instead of guessing the object pointer from a heap delta.
           This is the path that used to take Epinephrine down when System
           Storage was opened first. */
        if (DeriveGameContextTemplate(&arg, &page, &plane, &mlist) < 0) {
            return -1;
        }
        temp1 = NULL;   /* move t1, zero at the real callsite */
        temp2 = NULL;   /* move t2, zero at the real callsite */
        kprintf("SYSCTX-DIRECT-8: derived display template from vshmain callsite\n");
    } else {
        kprintf("SYSCTX-DIRECT-8: using captured Memory Stick display template\n");
        /* Cross-validate: deriving from the callsite should reproduce the
           captured object exactly. If these ever disagree the log says so
           before it can matter. */
        {
            void *d_arg; char *d_page; char *d_plane; char *d_mlist;
            if (gc_adrenaline() && patch_index == FW_660 && vsh_text_addr &&
                    DeriveGameContextTemplate(&d_arg, &d_page, &d_plane, &d_mlist) == 0) {
                kprintf("SYSCTX-DIRECT-8: derive-vs-capture arg %08X vs %08X %s\n",
                        (u32)d_arg, (u32)arg,
                        (d_arg == arg) ? "MATCH" : "MISMATCH");
            }
        }
    }

    kprintf("SYSCTX-DIRECT-8: replaying arg=%08X page=%08X plane=%08X mlist=%08X\n",
            (u32)arg, (u32)page, (u32)plane, (u32)mlist);
    return sceVshCommonGuiDisplayContextPatched(
            arg, page, plane, 1, mlist, temp1, temp2);
}

void PatchVshmain(u32 text_addr) {
    AddVshItem = redir2stub(text_addr+patches.AddVshItemOffset[patch_index], add_vsh_item_stub, AddVshItemPatched);
    GetBackupVshItem = redir_call(text_addr+patches.GetBackupVshItem[patch_index], GetBackupVshItemPatched);
    ExecuteAction = redir2stub(text_addr+patches.ExecuteActionOffset[patch_index], execute_action_stub, ExecuteActionPatched);
    UnloadModule = redir2stub(text_addr+patches.UnloadModuleOffset[patch_index], unload_module_stub, UnloadModulePatched);
}

void PatchPaf(u32 text_addr) {
    //sysconf called scePafGetText from offset: 0x052AC
    scePafGetText = redir2stub(text_addr+patches.scePafGetTextOffset[patch_index], paf_get_text_stub, scePafGetTextPatched);
}

void PatchVshCommonGui(u32 text_addr) {
    sceVshCommonGuiDisplayContext_func = redir2stub(text_addr+patches.CommonGuiDisplayContextOffset[patch_index], display_context_stub, sceVshCommonGuiDisplayContextPatched);
}
