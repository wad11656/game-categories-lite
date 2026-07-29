/*
 *  this file is part of Game Categories Lite
 *
 *  Copyright (C) 2009  Bubbletune
 *  Copyright (C) 2011  Codestation
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

#include <pspsdk.h>
#include <pspkernel.h>
#include "categories_lite.h"
#include "stub_funcs.h"
#include "psppaf.h"
#include "vshitem.h"
#include "utils.h"
#include "gcread.h"
#include "config.h"
#include "logger.h"

extern int game_plug;

static int (* OnXmbPush)(void *arg0, void *arg1) = NULL;
static int (* OnXmbContextMenu)(void *arg0, void *arg1) = NULL;
//static int (* OnMenuListScrollIn)(void *arg0, void *arg1) = NULL;

static SceVshItem *original_item[2] = { NULL, NULL };
/* Adrenaline: EPI-XmbControl injects its own fake ef0 "msg_em" item for System
   Storage (reusing the firmware item's id), and that is the object the XMB
   actually displays -- distinct from GetBackupVshItem's res that we normally
   attach to. Capture the live item passed through GetBackupVshItem so we can
   attach the category menu to the object that's really on screen. */
static SceVshItem *live_item[2] = { NULL, NULL };
static SceContextItem *original_context[2] = { NULL, NULL };
static int context_preinstalled[2] = { 0, 0 };
SceContextItem *context_items[2] = { NULL, NULL };

int context_gamecats = 0;
static int context_just_opened = 0;
static void *xmb_arg0, *xmb_arg1;

static int context_action_arg[2];

int PatchExecuteActionForContext(int *action, int *action_arg) {
    int location;
    int uncategorized;

    kprintf("called, action: %i, action_arg: %i\n", *action, *action_arg);

    if (*action == GAME_ACTION && (*action_arg == 100 || *action_arg == 1000)) {
        location = get_location(*action_arg);
        global_pos = location;

        context_gamecats = 1;
        kprintf("ctx-diag: loc=%i orig_item=%08X ctx_items=%08X xmb_arg0=%08X xmb_arg1=%08X\n",
                location, (u32)original_item[location], (u32)context_items[location],
                (u32)xmb_arg0, (u32)xmb_arg1);
        if (original_item[location]) {
            kprintf("ctx-diag: orig id=%i action_arg=%i text=[%s] ctxptr=%08X\n",
                    original_item[location]->id, original_item[location]->action_arg,
                    original_item[location]->text, (u32)original_item[location]->context);
        }
        if (context_items[location]) {
            kprintf("ctx-diag: ctx[0] text=[%s] action=%i action_arg=%i\n",
                    context_items[location][0].text, context_items[location][0].action,
                    context_items[location][0].action_arg);
        }
        original_item[location]->context = context_items[location];
        /* Adrenaline: also attach to EPI's live fake item -- that's the one the
           XMB actually renders the context menu for. Gated so real PSP-Go, where
           res IS the live item, is unchanged. */
        if (gc_adrenaline() && live_item[location] && live_item[location] != original_item[location]) {
            kprintf("ctx-diag: also attaching to live item %08X\n", (u32)live_item[location]);
            live_item[location]->context = context_items[location];
        }
        kprintf("calling OnXmbContextMenu\n");
        {
            int context_result = OnXmbContextMenu(xmb_arg0, xmb_arg1);
            (void)context_result;
            kprintf("SYSCTX-DIRECT-8: context result=%i displayed=%i\n",
                    context_result, !context_gamecats);
            if (location == INTERNAL_STORAGE && context_gamecats) {
                int replay_result = ReplayGameContextDisplay(xmb_arg0);
                (void)replay_result;
                kprintf("SYSCTX-DIRECT-8: replay result=%i displayed=%i\n",
                        replay_result, !context_gamecats);
            }
        }
        return 2;
    } else if (*action == PSPMS_CONTEXT_SENTINEL || *action == PSPGO_CONTEXT_SENTINEL || *action == PSPMS_CONTEXT_SENTINEL+1 || *action == PSPGO_CONTEXT_SENTINEL+1) {
        location = get_location(*action);
        global_pos = location;
        if(game_plug) {
            if (*action_arg != last_action_arg[location]) {
                kprintf("marking game_plugin for unload, %i != %i\n", *action_arg, last_action_arg[location]);
                unload = 1;
            }
        }
        kprintf("location: %i, uncategorized: %i\n", location, config.uncategorized);
        if(!location && (config.uncategorized & ONLY_MS)) {
            uncategorized = 1;
        } else if(location && (config.uncategorized & ONLY_IE)) {
            uncategorized = 1;
        } else {
            uncategorized = 0;
        }

        if (!(uncategorized && (*action == PSPMS_CONTEXT_SENTINEL+1 || *action == PSPGO_CONTEXT_SENTINEL+1))) {
            // set category
            Category *p = (Category *)sce_paf_private_strtoul(context_items[location][*action_arg].text+4, NULL, 16);
            sce_paf_private_strncpy(category, &p->name, sizeof(category));
            kprintf("selected category: %s\n", category);
        } else {
            category[0] = '\0';
        }

        config.selection = *action_arg;

        save_config();

        return location;
    }

    return -1;
}

int PatchAddVshItemForContext(void *arg, int topitem, SceVshItem *item, int location) {
    u32 index = 0;
    int uncategorized;
    SceContextItem *base_context = item->context;

    kprintf("called, name: %s, location: %i\n", item->text, location);
    if(!location && (config.uncategorized & ONLY_MS)) {
        uncategorized = 1;
    } else if(location && (config.uncategorized & ONLY_IE)) {
        uncategorized = 1;
    } else  {
        uncategorized = 0;
    }
    Category *p = NULL;

    context_action_arg[location] = item->context->action_arg;

    item->action_arg = location ? 1000 : 100;

    //TODO: mark location in original context
    item->play_sound = 0;
    int malloc_size = (CountCategories(cat_list, location) + uncategorized) * sizeof(SceContextItem) + 1;
    context_items[location] = sce_paf_private_malloc(malloc_size);
    sce_paf_private_memset(context_items[location], 0, malloc_size);

    while ((p = GetNextCategory(cat_list, p, location))) {

        if(p->location == MEMORY_STICK) {
            sce_paf_private_snprintf(context_items[location][index].text, 48, "gcv_%08X", (u32)p);
        } else {
            sce_paf_private_snprintf(context_items[location][index].text, 48, "gcw_%08X", (u32)p);
        }
        kprintf("creating %s\n", context_items[location][index].text);
        context_items[location][index].play_sound = 1;
        context_items[location][index].action = !location ? PSPMS_CONTEXT_SENTINEL : PSPGO_CONTEXT_SENTINEL;
        context_items[location][index].action_arg = index;
        index++;
    }

    if(uncategorized) {
        kprintf("creating uncategorized context\n");
        sce_paf_private_strcpy(context_items[location][index].text, "gc4");
        context_items[location][index].play_sound = 1;
        context_items[location][index].action = (!location ? PSPMS_CONTEXT_SENTINEL : PSPGO_CONTEXT_SENTINEL) + 1;
        context_items[location][index].action_arg = index;
        index++;
    }

    if (config.selection > (index-1)) {
        config.selection = index-1;
    }

    /* Epinephrine injects System Storage by copying the Memory Stick item and
       then passing the copy to AddVshItem. vshmain makes another private copy
       while registering it; changing EPI's heap item later is therefore too
       late. Install GC's context before AddVshItem so every downstream copy is
       created with the category menu. */
    context_preinstalled[location] = 0;
    if (location == INTERNAL_STORAGE && gc_adrenaline()) {
        original_context[location] = base_context;
        item->context = context_items[location];
        context_preinstalled[location] = 1;
        kprintf("SYSCTX-PREATTACH-1: preinstalled ef0 context item=%08X ctx=%08X\n",
                (u32)item, (u32)item->context);
        sceKernelDcacheWritebackAll();
    }

    return AddVshItem(arg, topitem, item);
}

//int OnMenuListScrollInPatched(void *arg0, void *arg1) {
//    kprintf("called, opened: %i, sel: %i\n", context_just_opened, config.selection);
//    if (context_just_opened)  {
//        context_just_opened = 0;
//        scePafSetSelectedItem(arg0, config.selection);
//    }
//
//    return OnMenuListScrollIn(arg0, arg1);
//}

int OnXmbPushPatched(void *arg0, void *arg1) {
    kprintf("called\n");
    xmb_arg0 = arg0;
    xmb_arg1 = arg1;
    return OnXmbPush(arg0, arg1);
}

int OnXmbContextMenuPatched(void *arg0, void *arg1) {
    kprintf("called, global_pos: %i\n", global_pos);
    context_gamecats = 0;
    if (original_item[global_pos] && !context_preinstalled[global_pos]) {
        original_item[global_pos]->context = original_context[global_pos];
    }
    sceKernelDcacheWritebackAll();
    return OnXmbContextMenu(arg0, arg1);
}

void PatchGetBackupVshItemForContext(SceVshItem *item, SceVshItem *res) {
    kprintf("id: %i, action_arg: %i\n", item->id, item->action_arg);
    int location = get_location(item->action_arg);
    if(location != INVALID && item->id == vsh_id[location]) {
        global_pos = location;
        kprintf("restoring original content, loc: %i, live=%08X res=%08X preinstalled=%i\n",
                location, (u32)item, (u32)res, context_preinstalled[location]);
        original_item[location] = res;
        live_item[location] = item;
        if (!context_preinstalled[location]) {
            original_context[location] = item->context;
        } else if (res) {
            /* Cover the firmware backup as well; the registered row already
               inherited this pointer during AddVshItem. */
            res->context = context_items[location];
        }
        context_just_opened = 1;
    }
}

void PatchVshmainForContext(u32 text_addr) {
    OnXmbPush = redir2stub(text_addr+patches.OnXmbPush[patch_index], xmb_push_stub, OnXmbPushPatched);
    OnXmbContextMenu = redir2stub(text_addr+patches.OnXmbContextMenu[patch_index], xmb_context_stub, OnXmbContextMenuPatched);
    //OnMenuListScrollIn = redir2stub(text_addr+patches.OnMenuListScrollIn[patch_index], menu_scroll_stub, OnMenuListScrollInPatched);
}
