/*
 * Copyright (C) 2000, 2001 H�kan Hjort
 * Copyright (C) 2001 Rich Wareham <richwareham@users.sourceforge.net>
 * 
 * This file is part of libdvdnav, a DVD navigation library. It is modified
 * from a file originally part of the Ogle DVD player.
 * 
 * libdvdnav is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * libdvdnav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id$
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>

#include "decoder.h"
#include "vmcmd.h"
#include "vm.h"

/* Local prototypes */

static void saveRSMinfo(vm_t *vm,int cellN, int blockN);
static int set_PGN(vm_t *vm);
static link_t play_PGC(vm_t *vm);
static link_t play_PGC_post(vm_t *vm);
static link_t play_PG(vm_t *vm);
static link_t play_Cell(vm_t *vm);
static link_t play_Cell_post(vm_t *vm);
static link_t process_command(vm_t *vm,link_t link_values);

static void ifoOpenNewVTSI(vm_t *vm,dvd_reader_t *dvd, int vtsN);
static pgcit_t* get_PGCIT(vm_t *vm);
static int get_video_aspect(vm_t *vm);

/* Can only be called when in VTS_DOMAIN */
static int get_TT(vm_t *vm,int tt);
static int get_VTS_TT(vm_t *vm,int vtsN, int vts_ttn);
static int get_VTS_PTT(vm_t *vm,int vtsN, int vts_ttn, int part);

static int get_MENU(vm_t *vm,int menu); /*  VTSM & VMGM */
static int get_FP_PGC(vm_t *vm); /*  FP */

/* Called in any domain */
static int get_ID(vm_t *vm,int id);
static int get_PGC(vm_t *vm,int pgcN);
static int get_PGCN(vm_t *vm);

/* Initialisation */

vm_t* vm_new_vm() {
  vm_t *vm = (vm_t*)calloc(sizeof(vm_t), sizeof(char));

  return vm;
}

static void vm_print_current_domain_state(vm_t *vm) {
  switch((vm->state).domain) {
    case VTS_DOMAIN:
      fprintf(stderr, "Video Title Domain: -\n");
      break;

    case VTSM_DOMAIN:
      fprintf(stderr, "Video Title Menu Domain: -\n");
      break;

    case VMGM_DOMAIN:
      fprintf(stderr, "Video Manager Menu Domain: -\n");
      break;

    case FP_DOMAIN: 
      fprintf(stderr, "First Play Domain: -\n");
      break;

    default:
      fprintf(stderr, "Unknown Domain: -\n");
      break;
  }
  fprintf(stderr, "VTS:%d PG:%u CELL:%u BLOCK:%u VTS_TTN:%u TTN:%u TT_PGCN:%u\n", 
                   (vm->state).vtsN,
                   (vm->state).pgN,
                   (vm->state).cellN,
                   (vm->state).blockN,
                   (vm->state).VTS_TTN_REG,
                   (vm->state).TTN_REG,
                   (vm->state).TT_PGCN_REG);
}

void vm_stop(vm_t *vm) {
  if(!vm)
   return;

  if(vm->vmgi) {
    ifoClose(vm->vmgi);
    vm->vmgi=NULL;
  }

  if(vm->vtsi) {
    ifoClose(vm->vtsi);
    vm->vmgi=NULL;
  }
  
  if(vm->dvd) {
    DVDClose(vm->dvd);
    vm->dvd=NULL;
  }
}

void vm_free_vm(vm_t *vm) {
  if(vm) {
    vm_stop(vm);
    free(vm);
  }
}

/* IFO Access */

ifo_handle_t *vm_get_vmgi(vm_t *vm) {
  if(!vm)
   return NULL;
  
  return vm->vmgi;
}

ifo_handle_t *vm_get_vtsi(vm_t *vm) {
  if(!vm)
   return NULL;
  
  return vm->vtsi;
}

/* Reader Access */

dvd_reader_t *vm_get_dvd_reader(vm_t *vm) {
  if(!vm)
   return NULL;
  
  return vm->dvd;
}

int vm_reset(vm_t *vm, char *dvdroot) /*  , register_t regs) */ { 
  /*  Setup State */
  memset((vm->state).registers.SPRM, 0, sizeof(uint16_t)*24);
  memset((vm->state).registers.GPRM, 0, sizeof((vm->state).registers.GPRM));
  memset((vm->state).registers.GPRM_mode, 0, sizeof((vm->state).registers.GPRM_mode));
  (vm->state).registers.SPRM[0] = ('e'<<8)|'n'; /*  Player Menu Languange code */
  (vm->state).AST_REG = 15; /*  15 why? */
  (vm->state).SPST_REG = 62; /*  62 why? */
  (vm->state).AGL_REG = 1;
  (vm->state).TTN_REG = 1;
  (vm->state).VTS_TTN_REG = 1;
  /* (vm->state).TT_PGCN_REG = 0 */
  (vm->state).PTTN_REG = 1;
  (vm->state).HL_BTNN_REG = 1 << 10;

  (vm->state).PTL_REG = 15; /*  Parental Level */
  (vm->state).registers.SPRM[12] = ('U'<<8)|'S'; /*  Parental Management Country Code */
  (vm->state).registers.SPRM[16] = ('e'<<8)|'n'; /*  Initial Language Code for Audio */
  (vm->state).registers.SPRM[18] = ('e'<<8)|'n'; /*  Initial Language Code for Spu */
  /*  Player Regional Code Mask. 
   *  bit0 = Region 1
   *  bit1 = Region 2
   */
  (vm->state).registers.SPRM[20] = 0x1; /*  Player Regional Code Mask. Region free! */
  (vm->state).registers.SPRM[14] = 0x100; /* Try Pan&Scan */
   
  (vm->state).pgN = 0;
  (vm->state).cellN = 0;

  (vm->state).domain = FP_DOMAIN;
  (vm->state).rsm_vtsN = 0;
  (vm->state).rsm_cellN = 0;
  (vm->state).rsm_blockN = 0;
  
  (vm->state).vtsN = -1;
  
  if (vm->dvd && dvdroot) {
    // a new dvd device has been requested
    vm_stop(vm);
  }
  if (!vm->dvd) {
    vm->dvd = DVDOpen(dvdroot);
    if(!vm->dvd) {
      fprintf(stderr, "vm: faild to open/read the DVD\n");
      return -1;
    }

    vm->vmgi = ifoOpenVMGI(vm->dvd);
    if(!vm->vmgi) {
      fprintf(stderr, "vm: faild to read VIDEO_TS.IFO\n");
      return -1;
    }
    if(!ifoRead_FP_PGC(vm->vmgi)) {
      fprintf(stderr, "vm: ifoRead_FP_PGC failed\n");
      return -1;
    }
    if(!ifoRead_TT_SRPT(vm->vmgi)) {
      fprintf(stderr, "vm: ifoRead_TT_SRPT failed\n");
      return -1;
    }
    if(!ifoRead_PGCI_UT(vm->vmgi)) {
      fprintf(stderr, "vm: ifoRead_PGCI_UT failed\n");
      return -1;
    }
    if(!ifoRead_PTL_MAIT(vm->vmgi)) {
      fprintf(stderr, "vm: ifoRead_PTL_MAIT failed\n");
      ; /*  return -1; Not really used for now.. */
    }
    if(!ifoRead_VTS_ATRT(vm->vmgi)) {
      fprintf(stderr, "vm: ifoRead_VTS_ATRT failed\n");
      ; /*  return -1; Not really used for now.. */
    }
    if(!ifoRead_VOBU_ADMAP(vm->vmgi)) {
      fprintf(stderr, "vm: ifoRead_VOBU_ADMAP vgmi failed\n");
      ; /*  return -1; Not really used for now.. */
    }
    /* ifoRead_TXTDT_MGI(vmgi); Not implemented yet */
  }
  else fprintf(stderr, "vm: reset\n");

  return 0;
}

/*  FIXME TODO XXX $$$ Handle error condition too... */
int vm_start(vm_t *vm)
{
  link_t link_values;

  /*  Set pgc to FP(First Play) pgc */
  get_FP_PGC(vm);
  link_values = play_PGC(vm); 
  link_values = process_command(vm,link_values);
  assert(link_values.command == PlayThis);
  (vm->state).blockN = link_values.data1;

  return 0; /* ?? */
}

int vm_start_title(vm_t *vm, int tt) {
  link_t link_values;

  get_TT(vm, tt);
  link_values = play_PGC(vm); 
  link_values = process_command(vm, link_values);
  assert(link_values.command == PlayThis);
  (vm->state).blockN = link_values.data1;

  return 0; /* ?? */
}

int vm_jump_prog(vm_t *vm, int pr) {
  link_t link_values;

  (vm->state).pgN = pr; /*  ?? */

  get_PGC(vm, get_PGCN(vm));
  link_values = play_PG(vm); 
  link_values = process_command(vm, link_values);
  assert(link_values.command == PlayThis);
  (vm->state).blockN = link_values.data1;
  
  return 0; /* ?? */
}

int vm_eval_cmd(vm_t *vm, vm_cmd_t *cmd)
{
  link_t link_values;
  
  if(vmEval_CMD(cmd, 1, &(vm->state).registers, &link_values)) {
    link_values = process_command(vm, link_values);
    assert(link_values.command == PlayThis);
    (vm->state).blockN = link_values.data1;
    return 1; /*  Something changed, Jump */
  } else {
    return 0; /*  It updated some state thats all... */
  }
}

int vm_get_next_cell(vm_t *vm)
{
  link_t link_values;
  link_values = play_Cell_post(vm);
  link_values = process_command(vm,link_values);
  assert(link_values.command == PlayThis);
  (vm->state).blockN = link_values.data1;
  
  return 0; /*  ?? */
}

int vm_top_pg(vm_t *vm)
{
  link_t link_values;
  link_values = play_PG(vm);
  link_values = process_command(vm,link_values);
  assert(link_values.command == PlayThis);
  (vm->state).blockN = link_values.data1;
  
  return 1; /*  Jump */
}

int vm_go_up(vm_t *vm)
{
  link_t link_values;
 
  if(get_PGC(vm, (vm->state).pgc->goup_pgc_nr))
   assert(0);

  link_values = play_PGC(vm);
  link_values = process_command(vm,link_values);
  assert(link_values.command == PlayThis);
  (vm->state).blockN = link_values.data1;
  
  return 1; /*  Jump */
}

int vm_next_pg(vm_t *vm)
{
  /*  Do we need to get a updated pgN value first? */
  (vm->state).pgN += 1; 
  return vm_top_pg(vm);
}

int vm_prev_pg(vm_t *vm)
{
  /*  Do we need to get a updated pgN value first? */
  (vm->state).pgN -= 1;
  if((vm->state).pgN == 0) {
    /*  Check for previous PGCN ?  */
    (vm->state).pgN = 1;
    /*  return 0; */
  }
  return vm_top_pg(vm);
}


static domain_t menuid2domain(DVDMenuID_t menuid)
{
  domain_t result = VTSM_DOMAIN; /*  Really shouldn't have to.. */
  
  switch(menuid) {
  case DVD_MENU_Title:
    result = VMGM_DOMAIN;
    break;
  case DVD_MENU_Root:
  case DVD_MENU_Subpicture:
  case DVD_MENU_Audio:
  case DVD_MENU_Angle:
  case DVD_MENU_Part:
    result = VTSM_DOMAIN;
    break;
  }
  
  return result;
}

int vm_menu_call(vm_t *vm, DVDMenuID_t menuid, int block)
{
  domain_t old_domain;
  link_t link_values;
  
  /* Should check if we are allowed/can acces this menu */
  
  
  /* FIXME XXX $$$ How much state needs to be restored 
   * when we fail to find a menu? */
  
  old_domain = (vm->state).domain;
  
  switch((vm->state).domain) {
  case VTS_DOMAIN:
    saveRSMinfo(vm, 0, block);
    /* FALL THROUGH */
  case VTSM_DOMAIN:
  case VMGM_DOMAIN:
    (vm->state).domain = menuid2domain(menuid);
    if(get_PGCIT(vm) != NULL && get_MENU(vm, menuid) != -1) {
      link_values = play_PGC(vm);
      link_values = process_command(vm, link_values);
      assert(link_values.command == PlayThis);
      (vm->state).blockN = link_values.data1;
      return 1; /*  Jump */
    } else {
      (vm->state).domain = old_domain;
    }
    break;
  case FP_DOMAIN: /* FIXME XXX $$$ What should we do here? */
    break;
  }
  
  return 0;
}


int vm_resume(vm_t *vm)
{
  int i;
  link_t link_values;
  
  /*  Check and see if there is any rsm info!! */
  if((vm->state).rsm_vtsN == 0) {
    return 0;
  }
  
  (vm->state).domain = VTS_DOMAIN;
  ifoOpenNewVTSI(vm, vm->dvd, (vm->state).rsm_vtsN);
  get_PGC(vm, (vm->state).rsm_pgcN);
  
  /* These should never be set in SystemSpace and/or MenuSpace */ 
  /*  (vm->state).TTN_REG = (vm->state).rsm_tt; */
  /*  (vm->state).TT_PGCN_REG = (vm->state).rsm_pgcN; */
  /*  (vm->state).HL_BTNN_REG = (vm->state).rsm_btnn; */
  for(i = 0; i < 5; i++) {
    (vm->state).registers.SPRM[4 + i] = (vm->state).rsm_regs[i];
  }

  if((vm->state).rsm_cellN == 0) {
    assert((vm->state).cellN); /*  Checking if this ever happens */
    (vm->state).pgN = 1;
    link_values = play_PG(vm);
    link_values = process_command(vm, link_values);
    assert(link_values.command == PlayThis);
    (vm->state).blockN = link_values.data1;
  } else { 
    (vm->state).cellN = (vm->state).rsm_cellN;
    (vm->state).blockN = (vm->state).rsm_blockN;
    /* (vm->state).pgN = ?? does this gets the righ value in play_Cell, no! */
    if(set_PGN(vm)) {
      /* Were at or past the end of the PGC, should not happen for a RSM */
      assert(0);
      play_PGC_post(vm);
    }
  }
  
  return 1; /*  Jump */
}

/**
 * Return the substream id for 'logical' audio stream audioN.
 *  0 <= audioN < 8
 */
int vm_get_audio_stream(vm_t *vm, int audioN)
{
  int streamN = -1;
  fprintf(stderr,"dvdnav:vm.c:get_audio_stream audioN=%d\n",audioN); 
  if((vm->state).domain == VTSM_DOMAIN 
     || (vm->state).domain == VMGM_DOMAIN
     || (vm->state).domain == FP_DOMAIN) {
    audioN = 0;
  }
  
  if(audioN < 8) {
    /* Is there any contol info for this logical stream */ 
    if((vm->state).pgc->audio_control[audioN] & (1<<15)) {
      streamN = ((vm->state).pgc->audio_control[audioN] >> 8) & 0x07;  
    }
  }
  
  if((vm->state).domain == VTSM_DOMAIN 
     || (vm->state).domain == VMGM_DOMAIN
     || (vm->state).domain == FP_DOMAIN) {
    if(streamN == -1)
      streamN = 0;
  }
  
  /* Should also check in vtsi/vmgi status that what kind of stream
   * it is (ac3/lpcm/dts/sdds...) to find the right (sub)stream id */
  return streamN;
}

/**
 * Return the substream id for 'logical' subpicture stream subpN.
 * 0 <= subpN < 32
 */
int vm_get_subp_stream(vm_t *vm, int subpN)
{
  int streamN = -1;
  int source_aspect = get_video_aspect(vm);
  
  if((vm->state).domain == VTSM_DOMAIN 
     || (vm->state).domain == VMGM_DOMAIN
     || (vm->state).domain == FP_DOMAIN) {
    subpN = 0;
  }
  
  if(subpN < 32) { /* a valid logical stream */
    /* Is this logical stream present */ 
    if((vm->state).pgc->subp_control[subpN] & (1<<31)) {
      if(source_aspect == 0) /* 4:3 */	     
	streamN = ((vm->state).pgc->subp_control[subpN] >> 24) & 0x1f;  
      if(source_aspect == 3) /* 16:9 */
	streamN = ((vm->state).pgc->subp_control[subpN] >> 16) & 0x1f;
    }
  }
  
  /* Paranoia.. if no stream select 0 anyway */
/* I am not paranoid */
/* if((vm->state).domain == VTSM_DOMAIN 
     || (vm->state).domain == VMGM_DOMAIN
     || (vm->state).domain == FP_DOMAIN) {
    if(streamN == -1)
      streamN = 0;
  }
*/
  /* Should also check in vtsi/vmgi status that what kind of stream it is. */
  return streamN;
}

int vm_get_subp_active_stream(vm_t *vm)
{
  int subpN;
  int streamN;
  subpN = (vm->state).SPST_REG & ~0x40;
  streamN = vm_get_subp_stream(vm, subpN);
  
  /* If no such stream, then select the first one that exists. */
  if(streamN == -1) {
    for(subpN = 0; subpN < 32; subpN++) {
      if((vm->state).pgc->subp_control[subpN] & (1<<31)) {
      
        streamN = vm_get_subp_stream(vm, subpN);
        break;
      }
    }
  } 
  
  /* We should instead send the on/off status to the spudecoder / mixer */
  /* If we are in the title domain see if the spu mixing is on */
  if((vm->state).domain == VTS_DOMAIN && !((vm->state).SPST_REG & 0x40)) { 
     /* Bit 7 set means hide, and only let Forced display show */
     return (streamN | 0x80); 
  } else {
    return streamN;
  }
}

int vm_get_audio_active_stream(vm_t *vm)
{
  int audioN;
  int streamN;
  audioN = (vm->state).AST_REG ;
  streamN = vm_get_audio_stream(vm, audioN);
  
  /* If no such stream, then select the first one that exists. */
  if(streamN == -1) {
    for(audioN = 0; audioN < 8; audioN++) {
      if((vm->state).pgc->audio_control[audioN] & (1<<15)) {
        streamN = vm_get_audio_stream(vm, audioN);
        break;
      }
    }
  } 
  
  return streamN;
}


void vm_get_angle_info(vm_t *vm, int *num_avail, int *current)
{
  *num_avail = 1;
  *current = 1;
  
  if((vm->state).domain == VTS_DOMAIN) {
    /*  TTN_REG does not allways point to the correct title.. */
    title_info_t *title;
    if((vm->state).TTN_REG > vm->vmgi->tt_srpt->nr_of_srpts)
      return;
    title = &vm->vmgi->tt_srpt->title[(vm->state).TTN_REG - 1];
    if(title->title_set_nr != (vm->state).vtsN || 
       title->vts_ttn != (vm->state).VTS_TTN_REG)
      return; 
    *num_avail = title->nr_of_angles;
    *current = (vm->state).AGL_REG;
    if(*current > *num_avail) /*  Is this really a good idea? */
      *current = *num_avail; 
  }
}


void vm_get_audio_info(vm_t *vm, int *num_avail, int *current)
{
  if((vm->state).domain == VTS_DOMAIN) {
    *num_avail = vm->vtsi->vtsi_mat->nr_of_vts_audio_streams;
    *current = (vm->state).AST_REG;
  } else if((vm->state).domain == VTSM_DOMAIN) {
    *num_avail = vm->vtsi->vtsi_mat->nr_of_vtsm_audio_streams; /*  1 */
    *current = 1;
  } else if((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN) {
    *num_avail = vm->vmgi->vmgi_mat->nr_of_vmgm_audio_streams; /*  1 */
    *current = 1;
  }
}

void vm_get_subp_info(vm_t *vm, int *num_avail, int *current)
{
  if((vm->state).domain == VTS_DOMAIN) {
    *num_avail = vm->vtsi->vtsi_mat->nr_of_vts_subp_streams;
    *current = (vm->state).SPST_REG;
  } else if((vm->state).domain == VTSM_DOMAIN) {
    *num_avail = vm->vtsi->vtsi_mat->nr_of_vtsm_subp_streams; /*  1 */
    *current = 0x41;
  } else if((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN) {
    *num_avail = vm->vmgi->vmgi_mat->nr_of_vmgm_subp_streams; /*  1 */
    *current = 0x41;
  }
}

subp_attr_t vm_get_subp_attr(vm_t *vm, int streamN)
{
  subp_attr_t attr;
  
  if((vm->state).domain == VTS_DOMAIN) {
    attr = vm->vtsi->vtsi_mat->vts_subp_attr[streamN];
  } else if((vm->state).domain == VTSM_DOMAIN) {
    attr = vm->vtsi->vtsi_mat->vtsm_subp_attr;
  } else if((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN) {
    attr = vm->vmgi->vmgi_mat->vmgm_subp_attr;
  }
  return attr;
}

audio_attr_t vm_get_audio_attr(vm_t *vm, int streamN)
{
  audio_attr_t attr;
  
  if((vm->state).domain == VTS_DOMAIN) {
    attr = vm->vtsi->vtsi_mat->vts_audio_attr[streamN];
  } else if((vm->state).domain == VTSM_DOMAIN) {
    attr = vm->vtsi->vtsi_mat->vtsm_audio_attr;
  } else if((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN) {
    attr = vm->vmgi->vmgi_mat->vmgm_audio_attr;
  }
  return attr;
}

video_attr_t vm_get_video_attr(vm_t *vm)
{
  video_attr_t attr;
  
  if((vm->state).domain == VTS_DOMAIN) {
    attr = vm->vtsi->vtsi_mat->vts_video_attr;
  } else if((vm->state).domain == VTSM_DOMAIN) {
    attr = vm->vtsi->vtsi_mat->vtsm_video_attr;
  } else if((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN) {
    attr = vm->vmgi->vmgi_mat->vmgm_video_attr;
  }
  return attr;
}

void vm_get_video_res(vm_t *vm, int *width, int *height)
{
  video_attr_t attr;
  
  attr = vm_get_video_attr(vm);
  
  if(attr.video_format != 0) 
    *height = 576;
  else
    *height = 480;
  switch(attr.picture_size) {
  case 0:
    *width = 720;
    break;
  case 1:
    *width = 704;
    break;
  case 2:
    *width = 352;
    break;
  case 3:
    *width = 352;
    *height /= 2;
    break;
  }
}

/*  Must be called before domain is changed (get_PGCN()) */
static void saveRSMinfo(vm_t *vm, int cellN, int blockN)
{
  int i;
  
  if(cellN != 0) {
    (vm->state).rsm_cellN = cellN;
    (vm->state).rsm_blockN = 0;
  } else {
    (vm->state).rsm_cellN = (vm->state).cellN;
    (vm->state).rsm_blockN = blockN;
  }
  (vm->state).rsm_vtsN = (vm->state).vtsN;
  (vm->state).rsm_pgcN = get_PGCN(vm);
  
  /* assert((vm->state).rsm_pgcN == (vm->state).TT_PGCN_REG); // for VTS_DOMAIN */
  
  for(i = 0; i < 5; i++) {
    (vm->state).rsm_regs[i] = (vm->state).registers.SPRM[4 + i];
  }
}



/* Figure out the correct pgN from the cell and update (vm->state). */ 
static int set_PGN(vm_t *vm) {
  int new_pgN = 0;
  
  while(new_pgN < (vm->state).pgc->nr_of_programs 
	&& (vm->state).cellN >= (vm->state).pgc->program_map[new_pgN])
    new_pgN++;
  
  if(new_pgN == (vm->state).pgc->nr_of_programs) /* We are at the last program */
    if((vm->state).cellN > (vm->state).pgc->nr_of_cells)
      return 1; /* We are past the last cell */
  
  (vm->state).pgN = new_pgN;
  
  if((vm->state).domain == VTS_DOMAIN) {
    playback_type_t *pb_ty;
    if((vm->state).TTN_REG > vm->vmgi->tt_srpt->nr_of_srpts)
      return 0; /*  ?? */
    pb_ty = &vm->vmgi->tt_srpt->title[(vm->state).TTN_REG - 1].pb_ty;
    if(pb_ty->multi_or_random_pgc_title == /* One_Sequential_PGC_Title */ 0) {
#if 0 /* TTN_REG can't be trusted to have a correct value here... */
      vts_ptt_srpt_t *ptt_srpt = vtsi->vts_ptt_srpt;
      assert((vm->state).VTS_TTN_REG <= ptt_srpt->nr_of_srpts);
      assert(get_PGCN() == ptt_srpt->title[(vm->state).VTS_TTN_REG - 1].ptt[0].pgcn);
      assert(1 == ptt_srpt->title[(vm->state).VTS_TTN_REG - 1].ptt[0].pgn);
#endif
      (vm->state).PTTN_REG = (vm->state).pgN;
    }
  }
  
  return 0;
}

static link_t play_PGC(vm_t *vm) 
{    
  link_t link_values;
  
  fprintf(stderr, "vm: play_PGC:");
  if((vm->state).domain != FP_DOMAIN)
    fprintf(stderr, " (vm->state).pgcN (%i)\n", get_PGCN(vm));
  else
    fprintf(stderr, " first_play_pgc\n");

  /*  This must be set before the pre-commands are executed because they */
  /*  might contain a CallSS that will save resume state */
  (vm->state).pgN = 1;
  (vm->state).cellN = 0;

  /* eval -> updates the state and returns either 
     - some kind of jump (Jump(TT/SS/VTS_TTN/CallSS/link C/PG/PGC/PTTN)
     - just play video i.e first PG
       (This is what happens if you fall of the end of the pre_cmds)
     - or a error (are there more cases?) */
  if((vm->state).pgc->command_tbl && (vm->state).pgc->command_tbl->nr_of_pre) {
    if(vmEval_CMD((vm->state).pgc->command_tbl->pre_cmds, 
		  (vm->state).pgc->command_tbl->nr_of_pre, 
		  &(vm->state).registers, &link_values)) {
      /*  link_values contains the 'jump' return value */
      return link_values;
    } else {
      fprintf(stderr, "PGC pre commands didn't do a Jump, Link or Call\n");
    }
  }
  return play_PG(vm);
}  


static link_t play_PG(vm_t *vm)
{
  fprintf(stderr, "play_PG: (vm->state).pgN (%i)\n", (vm->state).pgN);
  
  assert((vm->state).pgN > 0);
  if((vm->state).pgN > (vm->state).pgc->nr_of_programs) {
    fprintf(stderr, "(vm->state).pgN (%i) == pgc->nr_of_programs + 1 (%i)\n", 
	    (vm->state).pgN, (vm->state).pgc->nr_of_programs + 1);
    assert((vm->state).pgN == (vm->state).pgc->nr_of_programs + 1);
    return play_PGC_post(vm);
  }
  
  (vm->state).cellN = (vm->state).pgc->program_map[(vm->state).pgN - 1];
  
  return play_Cell(vm);
}


static link_t play_Cell(vm_t *vm)
{
  fprintf(stderr, "play_Cell: (vm->state).cellN (%i)\n", (vm->state).cellN);
  
  assert((vm->state).cellN > 0);
  if((vm->state).cellN > (vm->state).pgc->nr_of_cells) {
    fprintf(stderr, "(vm->state).cellN (%i) == pgc->nr_of_cells + 1 (%i)\n", 
	    (vm->state).cellN, (vm->state).pgc->nr_of_cells + 1);
    assert((vm->state).cellN == (vm->state).pgc->nr_of_cells + 1); 
    return play_PGC_post(vm);
  }
  

  /* Multi angle/Interleaved */
  switch((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_mode) {
  case 0: /*  Normal */
    assert((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type == 0);
    break;
  case 1: /*  The first cell in the block */
    switch((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type) {
    case 0: /*  Not part of a block */
      assert(0);
    case 1: /*  Angle block */
      /* Loop and check each cell instead? So we don't get outsid the block. */
      (vm->state).cellN += (vm->state).AGL_REG - 1;
      assert((vm->state).cellN <= (vm->state).pgc->nr_of_cells);
      assert((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_mode != 0);
      assert((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type == 1);
      break;
    case 2: /*  ?? */
    case 3: /*  ?? */
    default:
      fprintf(stderr, "Invalid? Cell block_mode (%d), block_type (%d)\n",
	      (vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_mode,
	      (vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type);
    }
    break;
  case 2: /*  Cell in the block */
  case 3: /*  Last cell in the block */
  /*  These might perhaps happen for RSM or LinkC commands? */
  default:
    fprintf(stderr, "Cell is in block but did not enter at first cell!\n");
  }
  
  /* Updates (vm->state).pgN and PTTN_REG */
  if(set_PGN(vm)) {
    /* Should not happen */
    link_t tmp = {LinkTailPGC, /* No Button */ 0, 0, 0};
    assert(0);
    return tmp;
  }
  
  {
    link_t tmp = {PlayThis, /* Block in Cell */ 0, 0, 0};
    return tmp;
  }

}

static link_t play_Cell_post(vm_t *vm)
{
  cell_playback_t *cell;
  
  fprintf(stderr, "play_Cell_post: (vm->state).cellN (%i)\n", (vm->state).cellN);
  
  cell = &(vm->state).pgc->cell_playback[(vm->state).cellN - 1];
  
  /* Still time is already taken care of before we get called. */
  
  /* Deal with a Cell command, if any */
  if(cell->cell_cmd_nr != 0) {
    link_t link_values;
    
    assert((vm->state).pgc->command_tbl != NULL);
    assert((vm->state).pgc->command_tbl->nr_of_cell >= cell->cell_cmd_nr);
    fprintf(stderr, "Cell command pressent, executing\n");
    if(vmEval_CMD(&(vm->state).pgc->command_tbl->cell_cmds[cell->cell_cmd_nr - 1], 1,
		  &(vm->state).registers, &link_values)) {
      return link_values;
    } else {
       fprintf(stderr, "Cell command didn't do a Jump, Link or Call\n");
      /*  Error ?? goto tail? goto next PG? or what? just continue? */
    }
  }
  
  
  /* Where to continue after playing the cell... */
  /* Multi angle/Interleaved */
  switch((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_mode) {
  case 0: /*  Normal */
    assert((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type == 0);
    (vm->state).cellN++;
    break;
  case 1: /*  The first cell in the block */
  case 2: /*  A cell in the block */
  case 3: /*  The last cell in the block */
  default:
    switch((vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type) {
    case 0: /*  Not part of a block */
      assert(0);
    case 1: /*  Angle block */
      /* Skip the 'other' angles */
      (vm->state).cellN++;
      while((vm->state).cellN <= (vm->state).pgc->nr_of_cells 
	    && (vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_mode >= 2) {
	(vm->state).cellN++;
      }
      break;
    case 2: /*  ?? */
    case 3: /*  ?? */
    default:
      fprintf(stderr, "Invalid? Cell block_mode (%d), block_type (%d)\n",
	      (vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_mode,
	      (vm->state).pgc->cell_playback[(vm->state).cellN - 1].block_type);
    }
    break;
  }
  
  
  /* Figure out the correct pgN for the new cell */ 
  if(set_PGN(vm)) {
    fprintf(stderr, "last cell in this PGC\n");
    return play_PGC_post(vm);
  }

  return play_Cell(vm);
}


static link_t play_PGC_post(vm_t *vm)
{
  link_t link_values;

  fprintf(stderr, "play_PGC_post:\n");
  
  assert((vm->state).pgc->still_time == 0); /*  FIXME $$$ */

  /* eval -> updates the state and returns either 
     - some kind of jump (Jump(TT/SS/VTS_TTN/CallSS/link C/PG/PGC/PTTN)
     - or a error (are there more cases?)
     - if you got to the end of the post_cmds, then what ?? */
  if((vm->state).pgc->command_tbl &&
     vmEval_CMD((vm->state).pgc->command_tbl->post_cmds,
		(vm->state).pgc->command_tbl->nr_of_post, 
		&(vm->state).registers, &link_values)) {
    return link_values;
  }
  
  /*  Or perhaps handle it here? */
  {
    link_t link_next_pgc = {LinkNextPGC, 0, 0, 0};
    fprintf(stderr, "** Fell of the end of the pgc, continuing in NextPGC\n");
    assert((vm->state).pgc->next_pgc_nr != 0);
    /* Should end up in the STOP_DOMAIN if next_pgc is 0. */
    return link_next_pgc;
  }
}


static link_t process_command(vm_t *vm, link_t link_values)
{
  /* FIXME $$$ Move this to a separate function? */
  vm->badness_counter++;
  if (vm->badness_counter > 1) fprintf(stderr, "**** process_command re-entered %d*****\n",vm->badness_counter);
  while(link_values.command != PlayThis) {
    
    vmPrint_LINK(link_values);
    
    fprintf(stderr, "Link values %i %i %i %i\n", link_values.command, 
	    link_values.data1, link_values.data2, link_values.data3);
     
    fprintf(stderr, "Before:");
    vm_print_current_domain_state(vm);
    
    switch(link_values.command) {
    case LinkNoLink:
      /* No Link */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      fprintf(stderr, "libdvdnav: FIXME: in trouble...LinkNoLink - CRASHING!!!\n");
      assert(0);
      
    case LinkTopC:
      /* Link to Top?? Cell. What is TopC */
      /* BUTTON number:data1 */
      fprintf(stderr, "libdvdnav: FIXME: LinkTopC. What is LinkTopC?\n");
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      link_values = play_Cell(vm);
      break;
    case LinkNextC:
      /* Link to Next Cell */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      (vm->state).cellN += 1; /* if cellN becomes > nr_of_cells? it is handled in play_Cell() */
      link_values = play_Cell(vm);
      break;
    case LinkPrevC:
      /* Link to Previous Cell */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      (vm->state).cellN -= 1; /*  If cellN becomes < 1? it is handled in play_Cell() */
      link_values = play_Cell(vm);
      break;
      
    case LinkTopPG:
      /* Link to Top Program */
      /* BUTTON number:data1 */
      fprintf(stderr, "libdvdnav: FIXME: LinkTopPG. What is LinkTopPG?\n");
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      /*  Does pgN always contain the current value? */
      link_values = play_PG(vm);
      break;
    case LinkNextPG:
      /* Link to Next Program */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      /*  Does pgN always contain the current value? */
      (vm->state).pgN += 1; /*  FIXME: What if pgN becomes > pgc.nr_of_programs? */
      link_values = play_PG(vm);
      break;
    case LinkPrevPG:
      /* Link to Previous Program */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      /*  Does pgN always contain the current value? */
      assert((vm->state).pgN > 1);
      (vm->state).pgN -= 1; /*  FIXME: What if pgN becomes < 1? */
      link_values = play_PG(vm);
      break;
      
    case LinkTopPGC:
      /* Link to Top Program Chain */
      /* BUTTON number:data1 */
      fprintf(stderr, "libdvdnav: FIXME: LinkTopPGC. What is LinkTopPGC?\n");
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      link_values = play_PGC(vm);
      break;
    case LinkNextPGC:
      /* Link to Next Program Chain */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      assert((vm->state).pgc->next_pgc_nr != 0);
      if(get_PGC(vm, (vm->state).pgc->next_pgc_nr))
	assert(0);
      link_values = play_PGC(vm);
      break;
    case LinkPrevPGC:
      /* Link to Previous Program Chain */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      assert((vm->state).pgc->prev_pgc_nr != 0);
      if(get_PGC(vm, (vm->state).pgc->prev_pgc_nr))
	assert(0);
      link_values = play_PGC(vm);
      break;
    case LinkGoUpPGC:
      /* Link to GoUp??? Program Chain */
      /* BUTTON number:data1 */
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      assert((vm->state).pgc->goup_pgc_nr != 0);
      if(get_PGC(vm, (vm->state).pgc->goup_pgc_nr))
	assert(0);
      link_values = play_PGC(vm);
      break;
    case LinkTailPGC:
      /* Link to Tail??? Program Chain */
      /* BUTTON number:data1 */
      fprintf(stderr, "libdvdnav: FIXME: LinkTailPGC. What is LinkTailPGC?\n");
      if(link_values.data1 != 0)
	(vm->state).HL_BTNN_REG = link_values.data1 << 10;
      link_values = play_PGC_post(vm);
      break;
      
    case LinkRSM:
      {
	/* Link to Resume */
	int i;
	/*  Check and see if there is any rsm info!! */
	(vm->state).domain = VTS_DOMAIN;
	ifoOpenNewVTSI(vm, vm->dvd, (vm->state).rsm_vtsN);
	get_PGC(vm, (vm->state).rsm_pgcN);
	
	/* These should never be set in SystemSpace and/or MenuSpace */ 
	/* (vm->state).TTN_REG = rsm_tt; ?? */
	/* (vm->state).TT_PGCN_REG = (vm->state).rsm_pgcN; ?? */
	for(i = 0; i < 5; i++) {
	  (vm->state).registers.SPRM[4 + i] = (vm->state).rsm_regs[i];
	}
	
	if(link_values.data1 != 0)
	  (vm->state).HL_BTNN_REG = link_values.data1 << 10;
	
	if((vm->state).rsm_cellN == 0) {
	  assert((vm->state).cellN); /*  Checking if this ever happens */
	  /* assert( time/block/vobu is 0 ); */
	  (vm->state).pgN = 1;
	  link_values = play_PG(vm);
	} else { 
	  /* assert( time/block/vobu is _not_ 0 ); */
	  /* play_Cell_at_time */
	  /* (vm->state).pgN = ?? this gets the righ value in play_Cell */
	  (vm->state).cellN = (vm->state).rsm_cellN;
	  link_values.command = PlayThis;
	  link_values.data1 = (vm->state).rsm_blockN;
	  if(set_PGN(vm)) {
	    /* Were at the end of the PGC, should not happen for a RSM */
	    assert(0);
	    link_values.command = LinkTailPGC;
	    link_values.data1 = 0;  /* No button */
	  }
	}
      }
      break;
    case LinkPGCN:
      /* Link to Program Chain Number:data1 */
      if(get_PGC(vm, link_values.data1))
	assert(0);
      link_values = play_PGC(vm);
      break;
    case LinkPTTN:
      /* Link to Part of this Title Number:data1 */
      /* BUTTON number:data2 */
      assert((vm->state).domain == VTS_DOMAIN);
      if(link_values.data2 != 0)
	(vm->state).HL_BTNN_REG = link_values.data2 << 10;
      if(get_VTS_PTT(vm, (vm->state).vtsN, (vm->state).VTS_TTN_REG, link_values.data1) == -1)
	assert(0);
      link_values = play_PG(vm);
      break;
    case LinkPGN:
      /* Link to Program Number:data1 */
      /* BUTTON number:data2 */
      if(link_values.data2 != 0)
	(vm->state).HL_BTNN_REG = link_values.data2 << 10;
      /* Update any other state, PTTN perhaps? */
      (vm->state).pgN = link_values.data1;
      link_values = play_PG(vm);
      break;
    case LinkCN:
      /* Link to Cell Number:data1 */
      /* BUTTON number:data2 */
      if(link_values.data2 != 0)
	(vm->state).HL_BTNN_REG = link_values.data2 << 10;
      /* Update any other state, pgN, PTTN perhaps? */
      (vm->state).cellN = link_values.data1;
      link_values = play_Cell(vm);
      break;
      
    case Exit:
      fprintf(stderr, "libdvdnav: FIXME:in trouble...Link Exit - CRASHING!!!\n");
      assert(0); /*  What should we do here?? */
      
    case JumpTT:
      /* Jump to VTS Title Domain */
      /* Only allowed from the First Play domain(PGC) */
      /* or the Video Manager domain (VMG) */
      //fprintf(stderr,"****** JumpTT is Broken, please fix me!!! ****\n");
      assert((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN); /* ?? */
      if(get_TT(vm,link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case JumpVTS_TT:
      /* Jump to Title:data1 in same VTS Title Domain */
      /* Only allowed from the VTS Menu Domain(VTSM) */
      /* or the Video Title Set Domain(VTS) */
      assert((vm->state).domain == VTSM_DOMAIN || (vm->state).domain == VTS_DOMAIN); /* ?? */
      fprintf(stderr, "libdvdnav: FIXME: Should be able to use get_VTS_PTT here.\n"); 
      if(get_VTS_TT(vm,(vm->state).vtsN, link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case JumpVTS_PTT:
      /* Jump to Part:data2 of Title:data1 in same VTS Title Domain */
      /* Only allowed from the VTS Menu Domain(VTSM) */
      /* or the Video Title Set Domain(VTS) */
      assert((vm->state).domain == VTSM_DOMAIN || (vm->state).domain == VTS_DOMAIN); /* ?? */
      if(get_VTS_PTT(vm,(vm->state).vtsN, link_values.data1, link_values.data2) == -1)
	assert(0);
      link_values = play_PG(vm);
      break;
      
    case JumpSS_FP:
      /* Jump to First Play Domain */
      /* Only allowed from the VTS Menu Domain(VTSM) */
      /* or the Video Manager domain (VMG) */
      assert((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == VTSM_DOMAIN); /* ?? */
      get_FP_PGC(vm);
      link_values = play_PGC(vm);
      break;
    case JumpSS_VMGM_MENU:
      /* Jump to Video Manger domain - Title Menu:data1 or any PGC in VMG */
      /* Allowed from anywhere except the VTS Title domain */
      assert((vm->state).domain == VMGM_DOMAIN || 
	     (vm->state).domain == VTSM_DOMAIN || 
	     (vm->state).domain == FP_DOMAIN); /* ?? */
      (vm->state).domain = VMGM_DOMAIN;
      if(get_MENU(vm,link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case JumpSS_VTSM:
      /* Jump to a menu in Video Title domain, */
      /* or to a Menu is the current VTS */
      /* FIXME: This goes badly wrong for some DVDs. */
      /* FIXME: Keep in touch with ogle people regarding what to do here */
      /* ifoOpenNewVTSI:data1 */
      /* VTS_TTN_REG:data2 */
      /* get_MENU:data3 */ 
      fprintf(stderr, "dvdnav: BUG TRACKING *******************************************************************\n");
      fprintf(stderr, "dvdnav:              If you see this message, please report these values to the dvd-devel mailing list.\n");
      fprintf(stderr, "    data1=%u data2=%u data3=%u\n", 
                link_values.data1,
                link_values.data2,
                link_values.data3);
      fprintf(stderr, "dvdnav: *******************************************************************\n");

      if(link_values.data1 !=0) {
	assert((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == FP_DOMAIN); /* ?? */
	(vm->state).domain = VTSM_DOMAIN;
	ifoOpenNewVTSI(vm, vm->dvd, link_values.data1);  /*  Also sets (vm->state).vtsN */
      } else {
	/*  This happens on 'The Fifth Element' region 2. */
	assert((vm->state).domain == VTSM_DOMAIN);
      }
      /*  I don't know what title is supposed to be used for. */
      /*  Alien or Aliens has this != 1, I think. */
      /* assert(link_values.data2 == 1); */
      (vm->state).VTS_TTN_REG = link_values.data2;
      if(get_MENU(vm, link_values.data3) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case JumpSS_VMGM_PGC:
      /* get_PGC:data1 */
      assert((vm->state).domain == VMGM_DOMAIN ||
	     (vm->state).domain == VTSM_DOMAIN ||
	     (vm->state).domain == FP_DOMAIN); /* ?? */
      (vm->state).domain = VMGM_DOMAIN;
      if(get_PGC(vm,link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
      
    case CallSS_FP:
      /* saveRSMinfo:data1 */
      assert((vm->state).domain == VTS_DOMAIN); /* ??    */
      /*  Must be called before domain is changed */
      saveRSMinfo(vm, link_values.data1, /* We dont have block info */ 0);
      get_FP_PGC(vm);
      link_values = play_PGC(vm);
      break;
    case CallSS_VMGM_MENU:
      /* get_MENU:data1 */ 
      /* saveRSMinfo:data2 */
      assert((vm->state).domain == VTS_DOMAIN); /* ??    */
      /*  Must be called before domain is changed */
      saveRSMinfo(vm,link_values.data2, /* We dont have block info */ 0);      
      (vm->state).domain = VMGM_DOMAIN;
      if(get_MENU(vm,link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case CallSS_VTSM:
      /* get_MENU:data1 */ 
      /* saveRSMinfo:data2 */
      assert((vm->state).domain == VTS_DOMAIN); /* ??    */
      /*  Must be called before domain is changed */
      saveRSMinfo(vm,link_values.data2, /* We dont have block info */ 0);
      (vm->state).domain = VTSM_DOMAIN;
      if(get_MENU(vm,link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case CallSS_VMGM_PGC:
      /* get_PGC:data1 */
      /* saveRSMinfo:data2 */
      assert((vm->state).domain == VTS_DOMAIN); /* ??    */
      /*  Must be called before domain is changed */
      saveRSMinfo(vm,link_values.data2, /* We dont have block info */ 0);
      (vm->state).domain = VMGM_DOMAIN;
      if(get_PGC(vm,link_values.data1) == -1)
	assert(0);
      link_values = play_PGC(vm);
      break;
    case PlayThis:
      /* Should never happen. */
      assert(0);
      break;
    }
  fprintf(stderr, "After:");
  vm_print_current_domain_state(vm);
    
  }
  vm->badness_counter--;
  return link_values;
}

static int get_TT(vm_t *vm, int tt)
{  
  //fprintf(stderr,"****** get_TT is Broken, please fix me!!! ****\n");
  assert(tt <= vm->vmgi->tt_srpt->nr_of_srpts);
  
  (vm->state).TTN_REG = tt;
   
  return get_VTS_TT(vm, vm->vmgi->tt_srpt->title[tt - 1].title_set_nr,
		    vm->vmgi->tt_srpt->title[tt - 1].vts_ttn);
}


static int get_VTS_TT(vm_t *vm, int vtsN, int vts_ttn)
{
  int pgcN;
  //fprintf(stderr,"****** get_VTS_TT is Broken, please fix me!!! ****\n");
  fprintf(stderr,"title_set_nr=%d\n", vtsN);
  fprintf(stderr,"vts_ttn=%d\n", vts_ttn);
  
  (vm->state).domain = VTS_DOMAIN;
  if(vtsN != (vm->state).vtsN) {
    fprintf(stderr,"****** opening new VTSI ****\n");
    ifoOpenNewVTSI(vm, vm->dvd, vtsN); /*  Also sets (vm->state).vtsN */
    fprintf(stderr,"****** opened VTSI ****\n");
  }
  
  pgcN = get_ID(vm, vts_ttn); /*  This might return -1 */
  assert(pgcN != -1);

  /* (vm->state).TTN_REG = ?? Must search tt_srpt for a matching entry...   */
  (vm->state).VTS_TTN_REG = vts_ttn;
  /* Any other registers? */
  
  return get_PGC(vm, pgcN);
}


static int get_VTS_PTT(vm_t *vm, int vtsN, int /* is this really */ vts_ttn, int part)
{
  int pgcN, pgN;
  
  (vm->state).domain = VTS_DOMAIN;
  if(vtsN != (vm->state).vtsN)
    ifoOpenNewVTSI(vm, vm->dvd, vtsN); /*  Also sets (vm->state).vtsN */
  
  assert(vts_ttn <= vm->vtsi->vts_ptt_srpt->nr_of_srpts);
  assert(part <= vm->vtsi->vts_ptt_srpt->title[vts_ttn - 1].nr_of_ptts);
  
  pgcN = vm->vtsi->vts_ptt_srpt->title[vts_ttn - 1].ptt[part - 1].pgcn;
  pgN = vm->vtsi->vts_ptt_srpt->title[vts_ttn - 1].ptt[part - 1].pgn;
  
  /* (vm->state).TTN_REG = ?? Must search tt_srpt for a matchhing entry... */
  (vm->state).VTS_TTN_REG = vts_ttn;
  /* Any other registers? */
  
  (vm->state).pgN = pgN; /*  ?? */
  
  return get_PGC(vm, pgcN);
}



static int get_FP_PGC(vm_t *vm)
{  
  (vm->state).domain = FP_DOMAIN;

  (vm->state).pgc = vm->vmgi->first_play_pgc;
  
  return 0;
}


static int get_MENU(vm_t *vm, int menu)
{
  assert((vm->state).domain == VMGM_DOMAIN || (vm->state).domain == VTSM_DOMAIN);
  return get_PGC(vm, get_ID(vm, menu));
}

static int get_ID(vm_t *vm, int id)
{
  int pgcN, i;
  pgcit_t *pgcit;
  
  /* Relies on state to get the correct pgcit. */
  pgcit = get_PGCIT(vm);
  assert(pgcit != NULL);
  
  /* Get menu/title */
  for(i = 0; i < pgcit->nr_of_pgci_srp; i++) {
    if((pgcit->pgci_srp[i].entry_id & 0x7f) == id) {
      assert((pgcit->pgci_srp[i].entry_id & 0x80) == 0x80);
      pgcN = i + 1;
      return pgcN;
    }
  }
  fprintf(stderr, "** No such id/menu (%d) entry PGC\n", id);
  return -1; /*  error */
}



static int get_PGC(vm_t *vm, int pgcN)
{
  /* FIXME: Keep this up to date with the ogle people */
  pgcit_t *pgcit;
  
  pgcit = get_PGCIT(vm);
  
  assert(pgcit != NULL); /*  ?? Make this return -1 instead */
  if(pgcN < 1 || pgcN > pgcit->nr_of_pgci_srp) {
/*    if(pgcit->nr_of_pgci_srp != 1)  */
     return -1; /* error */
/*   pgcN = 1; */
  }
  
  /* (vm->state).pgcN = pgcN; */
  (vm->state).pgc = pgcit->pgci_srp[pgcN - 1].pgc;
  
  if((vm->state).domain == VTS_DOMAIN)
    (vm->state).TT_PGCN_REG = pgcN;

  return 0;
}

static int get_PGCN(vm_t *vm)
{
  pgcit_t *pgcit;
  int pgcN = 1;

  pgcit = get_PGCIT(vm);
  
  assert(pgcit != NULL);
  
  while(pgcN <= pgcit->nr_of_pgci_srp) {
    if(pgcit->pgci_srp[pgcN - 1].pgc == (vm->state).pgc)
      return pgcN;
    pgcN++;
  }
  
  return -1; /*  error */
}

static int get_video_aspect(vm_t *vm)
{
  int aspect = 0;
  
  if((vm->state).domain == VTS_DOMAIN) {
    aspect = vm->vtsi->vtsi_mat->vts_video_attr.display_aspect_ratio;  
  } else if((vm->state).domain == VTSM_DOMAIN) {
    aspect = vm->vtsi->vtsi_mat->vtsm_video_attr.display_aspect_ratio;
  } else if((vm->state).domain == VMGM_DOMAIN) {
    aspect = vm->vmgi->vmgi_mat->vmgm_video_attr.display_aspect_ratio;
  }
  fprintf(stderr, "dvdnav:get_video_aspect:aspect=%d\n",aspect);
  assert(aspect == 0 || aspect == 3);
  (vm->state).registers.SPRM[14] &= ~(0x3 << 10);
  (vm->state).registers.SPRM[14] |= aspect << 10;
  
  return aspect;
}

static void ifoOpenNewVTSI(vm_t *vm, dvd_reader_t *dvd, int vtsN) 
{
  if((vm->state).vtsN == vtsN) {
    return; /*  We alread have it */
  }
  
  if(vm->vtsi != NULL)
    ifoClose(vm->vtsi);
  
  vm->vtsi = ifoOpenVTSI(dvd, vtsN);
  if(vm->vtsi == NULL) {
    fprintf(stderr, "ifoOpenVTSI failed - CRASHING!!!\n");
    assert(0);
  }
  if(!ifoRead_VTS_PTT_SRPT(vm->vtsi)) {
    fprintf(stderr, "ifoRead_VTS_PTT_SRPT failed - CRASHING!!!\n");
    assert(0);
  }
  if(!ifoRead_PGCIT(vm->vtsi)) {
    fprintf(stderr, "ifoRead_PGCIT failed - CRASHING!!!\n");
    assert(0);
  }
  if(!ifoRead_PGCI_UT(vm->vtsi)) {
    fprintf(stderr, "ifoRead_PGCI_UT failed - CRASHING!!!\n");
    assert(0);
  }
  if(!ifoRead_VOBU_ADMAP(vm->vtsi)) {
    fprintf(stderr, "ifoRead_VOBU_ADMAP vtsi failed - CRASHING\n");
    assert(0);
  }
  if(!ifoRead_TITLE_VOBU_ADMAP(vm->vtsi)) {
    fprintf(stderr, "ifoRead_TITLE_VOBU_ADMAP vtsi failed - CRASHING\n");
    assert(0);
  }
  (vm->state).vtsN = vtsN;
}

static pgcit_t* get_MENU_PGCIT(vm_t *vm, ifo_handle_t *h, uint16_t lang)
{
  int i;
  
  if(h == NULL || h->pgci_ut == NULL) {
    fprintf(stderr, "*** pgci_ut handle is NULL ***\n");
    return NULL; /*  error? */
  }
  
  i = 0;
  while(i < h->pgci_ut->nr_of_lus
	&& h->pgci_ut->lu[i].lang_code != lang)
    i++;
  if(i == h->pgci_ut->nr_of_lus) {
    fprintf(stderr, "Language '%c%c' not found, using '%c%c' instead\n",
	    (char)(lang >> 8), (char)(lang & 0xff),
 	    (char)(h->pgci_ut->lu[0].lang_code >> 8),
	    (char)(h->pgci_ut->lu[0].lang_code & 0xff));
    i = 0; /*  error? */
  }
  
  return h->pgci_ut->lu[i].pgcit;
}

/* Uses state to decide what to return */
static pgcit_t* get_PGCIT(vm_t *vm) {
  pgcit_t *pgcit;
  
  if((vm->state).domain == VTS_DOMAIN) {
    pgcit = vm->vtsi->vts_pgcit;
  } else if((vm->state).domain == VTSM_DOMAIN) {
    pgcit = get_MENU_PGCIT(vm, vm->vtsi, (vm->state).registers.SPRM[0]);
  } else if((vm->state).domain == VMGM_DOMAIN) {
    pgcit = get_MENU_PGCIT(vm, vm->vmgi, (vm->state).registers.SPRM[0]);
  } else {
    pgcit = NULL;    /* Should never hapen */
  }
  
  return pgcit;
}

/*
 * $Log$
 * Revision 1.6  2002/04/09 15:19:07  jcdutton
 * Added some debug info, to hopefully help in tracking bugs in libdvdnav.
 *
 * Revision 1.5  2002/04/07 19:35:54  jcdutton
 * Added some comments into the code.
 *
 * Revision 1.4  2002/04/06 18:31:50  jcdutton
 * Some cleaning up.
 * changed exit(1) to assert(0) so they actually get seen by the user so that it helps developers more.
 *
 * Revision 1.3  2002/04/02 18:22:27  richwareham
 * Added reset patch from Kees Cook <kees@outflux.net>
 *
 * Revision 1.2  2002/04/01 18:56:28  richwareham
 * Added initial example programs directory and make sure all debug/error output goes to stderr.
 *
 * Revision 1.1.1.1  2002/03/12 19:45:55  richwareham
 * Initial import
 *
 * Revision 1.18  2002/01/22 16:56:49  jcdutton
 * Fix clut after seeking.
 * Add a few virtual machine debug messages, to help diagnose problems with "Deep Purple - Total Abandon" DVD as I don't have the DVD itvm.
 * Fix a few debug messages, so they do not say FIXME.
 * Move the FIXME debug messages to comments in the code.
 *
 * Revision 1.17  2002/01/21 01:16:30  jcdutton
 * Added some debug messages, to hopefully get info from users.
 *
 * Revision 1.16  2002/01/20 21:40:46  jcdutton
 * Start to fix some assert failures.
 *
 * Revision 1.15  2002/01/19 20:24:38  jcdutton
 * Just some FIXME notes added.
 *
 * Revision 1.14  2002/01/13 22:17:57  jcdutton
 * Change logging.
 *
 *
 */
