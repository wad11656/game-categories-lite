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

    path = (model == 4) ? "ef0:/SEPLUGINS/xmbih.ini"
                        : "ms0:/SEPLUGINS/xmbih.ini";
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;

    /* Pre-Game categories fully hidden via XMBIH's HIDE_ALL_*=2 mechanism. */
    shift += ini_key_is(buf, n, "HIDE_ALL_PHOTO", '2');
    shift += ini_key_is(buf, n, "HIDE_ALL_MUSIC", '2');
    shift += ini_key_is(buf, n, "HIDE_ALL_VIDEO", '2');

    /* Extras (index 1) is also pre-Game when XMBIH fully hides it. */
    if (ini_key_is(buf, n, "HIDE_ALL_EXTRAS", '2'))
        shift++;

    return shift;
}

static int xmbih_is_active(void) {
    u32 instr;
    if (!vsh_text_addr)
        return 0;
    instr = *(u32 *)(vsh_text_addr + XMBIH_COUNT_PATCH_OFFSET);
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
    return vshregion == FAKE_REGION_RUSSIA ||
           vshregion == FAKE_REGION_CHINA ||
           vshregion == FAKE_REGION_DEBUG_TYPE_I;
}

static int extras_hidden_by_fake_region(void) {
    if (!fake_region_loaded) {
        GetSEConfigExFunc get_se_config_ex = NULL;

        fake_region_loaded = 1;

        get_se_config_ex = (GetSEConfigExFunc)sctrlHENFindFunction(
            "SystemCtrlForUser", "SystemCtrlForUser", SE_CONFIG_EX_NID);
        if (get_se_config_ex) {
            SEConfig se_config;
            sce_paf_private_memset(&se_config, 0, sizeof(se_config));
            if (get_se_config_ex(&se_config, sizeof(se_config))) {
                fake_region_hides_extras =
                    fake_region_value_hides_extras(se_config.vshregion);
            }
        }
    }

    return fake_region_hides_extras;
}

static void load_xmbih_shift(void) {
    int shift;

    xmbih_shift_loaded = 1;
    shift = 0;

    if (xmbih_is_active())
        shift = count_pregame_hides_in_ini();

    if (extras_hidden_by_fake_region())
        shift++;
    if (shift > 0 && shift <= 4)
        xmbih_game_topitem = 5 - shift;
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

        if (vsh_items[location]) {
            sce_paf_private_free(vsh_items[location]);
            vsh_items[location] = NULL;
        }
        if(context_items[location]) {
            sce_paf_private_free(context_items[location]);
            context_items[location] = NULL;
        }

        if(config.mode != MODE_FOLDER) {
            ClearCategories(cat_list, location);
            IndexCategories(cat_list, "xxx:/PSP/GAME", location);
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

int ExecuteActionPatched(int action, int action_arg) {
    int location;
    kprintf("action: %i, action_arg: %i\n", action, action_arg);
    if(config.mode == MODE_MULTI_MS) {
        location = PatchExecuteActionForMultiMs(&action, &action_arg);
        if(location >= 0) {
            last_action_arg[location] = action_arg;
            action_arg = vsh_action_arg[location];
        }
    } else if(config.mode == MODE_CONTEXT_MENU) {
        location = PatchExecuteActionForContext(&action, &action_arg);
        if(location == 2) {
            return 0;
        } else if(location >= 0) {
            last_action_arg[location] = action_arg;
            action_arg = vsh_action_arg[location];

            // simulate MS selection
            action = GAME_ACTION;
        }
    }
    kprintf("sending action: %i, action_arg: %i\n", action, action_arg);
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


int sceVshCommonGuiDisplayContextPatched(void *arg, char *page, char *plane, int width, char *mlist, void *temp1, void *temp2) {
    if (context_gamecats || (context_mode > 0 && lang_width[lang_id])) {
        width = 1;
        context_gamecats = 0;
    }
    return sceVshCommonGuiDisplayContext_func(arg, page, plane, width, mlist, temp1, temp2);
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
