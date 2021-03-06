/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA.

   This file is part of MooseFS.

   MooseFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   MooseFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MooseFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>

#include "acl.h"
#include "datacachemgr.h"
#include "matomlserv.h"
#include "matocsserv.h"
#include "matocuserv.h"
#include "filesystem.h"
#include "random.h"
#include "changelog.h"
#include "chartsdata.h"
#include "masterconn.h"
#include "main.h"

#define STR_AUX(x) #x
#define STR(x) STR_AUX(x)
const char id[]="@(#) version: " STR(VERSMAJ) "." STR(VERSMID) "." STR(VERSMIN) ", written by Jakub Kruszona-Zawadzki";

/* Run Tab */
typedef int (*runfn)();
struct {
	runfn fn;
	char *name;
} RunTab[]={
	{rndinit,"random generator"},
	{dcm_init,"data cache manager"}, // has to be before 'fs_init' and 'matocuserv_networkinit'
	{matocuserv_sessionsinit,"load stored sessions"}, // has to be before 'fs_init'
	{acl_init,"access control list"},
	{fs_init,"file system manager"},
	{chartsdata_init,"charts module"},
	{changelog_init,"change log"},
	{masterconn_init,"connection with master"},
	{matomlserv_init,"communication with metalogger"},
	{matocsserv_init,"communication with chunkserver"},
	{matocuserv_networkinit,"communication with clients"},
	{(runfn)0,"****"}
};
