/*
 *  this file is part of Game Categories Lite
 *
 *  Copyright (C) 2011  Bubbletune
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
#include <string.h>
#include "categories_lite.h"
#include "psppaf.h"
#include "gcpatches.h"
#include "pspdefs.h"
#include "config.h"
#include "logger.h"

// change the module name back to GCLite once PRO stops doing weird things with plugins
PSP_MODULE_INFO("Game_Categories_Light", 0x0807, 1, 5);
PSP_NO_CREATE_MAIN_THREAD();

/* Global variables */
int patch_index;
int model;
int game_plug = 0;
int sysconf_plug = 0;

/* Adrenaline (PSP-emu on PS Vita) detection, via Epinephrine's EPI-XmbControl
   module. Adrenaline's "System Storage" (ef0:) is opened with the normal Memory
   Stick game action, not the PSP-Go internal-storage action -- so the ef0
   handling in vshitem.c/gcread.c is gated on this to leave real PSP-Go hardware
   untouched. Detected LAZILY on first use (gc_adrenaline()) rather than at
   vsh_module load: walking the module list during the vsh_module start handler
   (GC Lite must be first in vsh.txt, so it runs at a delicate point) crashed the
   XMB before it built. -1 = not yet detected. */
int g_adrenaline = -1;

int gc_adrenaline(void) {
    if (g_adrenaline < 0) {
        /* kuKernelFindModuleByName fills the kernel's OWN SceModule, which on
           6.61 is larger than our (or the SDK's) truncated SceModule2 typedef.
           Copying it into a right-sized struct overflowed the buffer -- harmless
           on hidexmb's stack copy, but here (static BSS) it smashed adjacent
           globals and corrupted later ef0/EPI operations. Give it generous
           slack so the copy can never overflow. */
        static char epi[1024];
        g_adrenaline = (kuKernelFindModuleByName("EPI-XmbControl", (SceModule2 *)epi) >= 0);
        kprintf("adrenaline (EPI-XmbControl) present: %i\n", g_adrenaline);
    }
    return g_adrenaline;
}

char currfw[5];

//TODO: remove it from here
u32 text_addr_game;
u32 text_size_game;

/* Captured in OnModuleStart's vsh_module branch; used by vshitem.c's
   load_xmbih_shift to probe vshmain for XMBIH's patch signature. */
u32 vsh_text_addr = 0;

static STMOD_HANDLER previous;

int OnModuleStart(SceModule2 *mod) {
    //kprintf(">> %s: loading %s, text_addr: %08X\n", __func__, mod->modname, mod->text_addr);
	if (sce_paf_private_strcmp(mod->modname, "game_plugin_module") == 0) {

	    kprintf("loading %s, text_addr: %08X\n", mod->modname, mod->text_addr);
	    game_plug = 1;

	    //TODO: remove it from here
	    text_addr_game = mod->text_addr;
	    text_size_game = mod->text_size;

		PatchGamePluginForGCread(mod->text_addr);
		if(config.mode == MODE_FOLDER) {
		    PatchSelection(mod->text_addr);
		}
		ClearCaches();

	} else if (sce_paf_private_strcmp(mod->modname, "vsh_module") == 0) {

	    kprintf("loading %s, text_addr: %08X\n", mod->modname, mod->text_addr);
        vsh_text_addr = mod->text_addr;
        PatchVshmain(mod->text_addr);
        PatchVshmainForSysconf(mod->text_addr);
        PatchVshmainForContext(mod->text_addr);

        /* Make sceKernelGetCompiledSdkVersion clear the caches,
           so that we don't have to create a kernel module just
           to be able to clear the caches from user mode.*/

        //6.20: 0xFC114573 [0x00009B0C] - SysMemUserForUser_FC114573
        //6.35:	0xFC114573 [0x000099EC] - SysMemUserForUser_FC114573
        //6.60: 0xFC114573 [0x000098B0] - SysMemUserForUser_FC114573
        kprintf("Patching sceKernelGetCompiledSdkVersion, index: %i\n", patch_index);
        MAKE_JUMP(patches.get_compiled_sdk_version[patch_index], ClearCaches);
        ClearCaches();

	} else if (sce_paf_private_strcmp(mod->modname, "sysconf_plugin_module") == 0) {

	    kprintf("loading %s, text_addr: %08X\n", mod->modname, mod->text_addr);
	    sysconf_plug = 1;
	    PatchSysconf(mod->text_addr);
	    ClearCaches();

	} else if (sce_paf_private_strcmp(mod->modname, "scePaf_Module") == 0) {

	    kprintf("loading %s, text_addr: %08X\n", mod->modname, mod->text_addr);
        PatchPaf(mod->text_addr);
        PatchPafForSysconf(mod->text_addr);
        ClearCaches();

    } else if (sce_paf_private_strcmp(mod->modname, "sceVshCommonGui_Module") == 0) {

        kprintf("loading %s, text_addr: %08X\n", mod->modname, mod->text_addr);
        PatchVshCommonGui(mod->text_addr);
        ClearCaches();
    }

	return previous ? previous(mod) : 0;
}

int module_start(SceSize args UNUSED, void *argp UNUSED) {
#if defined(DEBUG) && GCLITE_LOGGING
    const char *src = "xx0:/category_lite.log";
    static const char build_id[] =
            "Game Categories Lite FOLDER-JAL-FIX-14-NOLOG starting\n";
    char *dest = filebuf;
#endif

    model = kuKernelGetModel();
#if defined(DEBUG) && GCLITE_LOGGING
    while((*dest++ = *src++)) {
        /* copy */
    }
    SET_DEVICENAME(filebuf, model == 4 ? INTERNAL_STORAGE : MEMORY_STICK);
    // paf isn't loaded yet
    kwrite(filebuf, build_id, sizeof(build_id) - 1);
#endif
    // Determine fw group
    u32 devkit = sceKernelDevkitVersion();
    if (devkit == 0x06020010) {
        patch_index = FW_620;
        ResolveNIDs(FW_620);
    } else if (devkit >= 0x06030010 && devkit < 0x06040010) {
        patch_index = FW_630;
    } else if (devkit >= 0x06060010 && devkit < 0x06070010) {
        patch_index = FW_660;
        ResolveNIDs(FW_660);
    } else {
        return 1;
    }

    currfw[0] = ((devkit >> 24) & 0xF) + '0';
    currfw[1] = '.';
    currfw[2] = ((devkit >> 16) & 0xF) + '0';
    currfw[3] = ((devkit >> 8) & 0xF) + '0';
    currfw[4] = 0;

    previous = sctrlHENSetStartModuleHandler(OnModuleStart);
    return 0;
}
