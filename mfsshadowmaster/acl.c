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
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <inttypes.h>

#include "MFSCommunication.h"
#include "md5.h"
#include "acl.h"
#include "datapack.h"
#include "main.h"
#include "cfg.h"

typedef struct _acl {
	uint32_t pleng;
	const uint8_t *path;	// without '/' at the begin and at the end
	uint32_t fromip,toip;
	uint32_t minversion;
	uint8_t passworddigest[16];
	unsigned alldirs:1;
	unsigned needpassword:1;
	unsigned meta:1;
	unsigned rootredefined:1;
//	unsigned old:1;
	uint8_t sesflags;
	uint32_t rootuid;
	uint32_t rootgid;
	uint32_t mapalluid;
	uint32_t mapallgid;
	struct _acl *next;
} acl;

static acl *acl_records;
static char *ExportsFileName;

char* acl_strsep(char **stringp, const char *delim) {
	char *s;
	const char *spanp;
	int c, sc;
	char *tok;

	s = *stringp;
	if (s==NULL) {
		return NULL;
	}
	tok = s;
	while (1) {
		c = *s++;
		spanp = delim;
		do {
			if ((sc=*spanp++)==c) {
				if (c==0) {
					s = NULL;
				} else {
					s[-1] = 0;
				}
				*stringp = s;
				return tok;
			}
		} while (sc!=0);
	}
	return NULL;	// unreachable
}


uint32_t acl_info_size(void) {
	acl *e;
	uint32_t size=0;
	for (e=acl_records ; e ; e=e->next) {
		if (e->meta) {
			size+=35;
		} else {
			size+=35+e->pleng;
		}
	}
	return size;
}

void acl_info_data(uint8_t *buff) {
	acl *e;
	for (e=acl_records ; e ; e=e->next) {
		put32bit(&buff,e->fromip);
		put32bit(&buff,e->toip);
		if (e->meta) {
			put32bit(&buff,1);
			put8bit(&buff,'.');
		} else {
			put32bit(&buff,e->pleng+1);
			put8bit(&buff,'/');
			if (e->pleng>0) {
				memcpy(buff,e->path,e->pleng);
				buff+=e->pleng;
			}
		}
		put32bit(&buff,e->minversion);
		put8bit(&buff,(e->alldirs?1:0)+(e->needpassword?2:0));
		put8bit(&buff,e->sesflags);
		put32bit(&buff,e->rootuid);
		put32bit(&buff,e->rootgid);
		put32bit(&buff,e->mapalluid);
		put32bit(&buff,e->mapallgid);
	}
}

uint8_t acl_check(uint32_t ip,uint32_t version,uint8_t meta,const uint8_t *path,const uint8_t rndcode[32],const uint8_t passcode[16],uint8_t *sesflags,uint32_t *rootuid,uint32_t *rootgid,uint32_t *mapalluid,uint32_t *mapallgid) {
	const uint8_t *p;
	uint32_t pleng,i;
	uint8_t rndstate;
	int ok,nopass;
	md5ctx md5c;
	uint8_t entrydigest[16];
	acl *e,*f;

//	syslog(LOG_NOTICE,"check acl for: %u.%u.%u.%u:%s",(ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF,path);

	if (meta==0) {
		p = path;
		while (*p=='/') {
			p++;
		}
		pleng = 0;
		while (p[pleng]) {
			pleng++;
		}
		while (pleng>0 && p[pleng-1]=='/') {
			pleng--;
		}
	} else {
		p = NULL;
		pleng = 0;
	}
	rndstate=0;
	if (rndcode!=NULL) {
		for (i=0 ; i<32 ; i++) {
			rndstate|=rndcode[i];
		}
	}
	nopass=0;
	f=NULL;
	for (e=acl_records ; e ; e=e->next) {
		ok = 0;
//		syslog(LOG_NOTICE,"entry: network:%u.%u.%u.%u-%u.%u.%u.%u",(e->fromip>>24)&0xFF,(e->fromip>>16)&0xFF,(e->fromip>>8)&0xFF,e->fromip&0xFF,(e->toip>>24)&0xFF,(e->toip>>16)&0xFF,(e->toip>>8)&0xFF,e->toip&0xFF);
		if (ip>=e->fromip && ip<=e->toip && version>=e->minversion && meta==e->meta) {
//			syslog(LOG_NOTICE,"ip and version ok");
			// path check
			if (meta) {	// no path in META
				ok=1;
			} else {
				if (e->pleng==0) {	// root dir
//					syslog(LOG_NOTICE,"rootdir entry (pleng:%u)",pleng);
					if (pleng==0) {
						ok=1;
					} else if (e->alldirs) {
						ok=1;
					}
				} else {
//					syslog(LOG_NOTICE,"entry path: %s (pleng:%u)",e->path,e->pleng);
					if (pleng==e->pleng && memcmp(p,e->path,pleng)==0) {
						ok=1;
					} else if (e->alldirs && pleng>e->pleng && p[e->pleng]=='/' && memcmp(p,e->path,e->pleng)==0) {
						ok=1;
					}
				}
			}
//			if (ok) {
//				syslog(LOG_NOTICE,"path ok");
//			}
			if (ok && e->needpassword) {
				if (rndstate==0 || rndcode==NULL || passcode==NULL) {
					ok=0;
					nopass=1;
				} else {
					md5_init(&md5c);
					md5_update(&md5c,rndcode,16);
					md5_update(&md5c,e->passworddigest,16);
					md5_update(&md5c,rndcode+16,16);
					md5_final(entrydigest,&md5c);
					if (memcmp(entrydigest,passcode,16)!=0) {
						ok=0;
						nopass=1;
					}
				}
			}
		}
		if (ok) {
//			syslog(LOG_NOTICE,"entry accepted");
			if (f==NULL) {
				f=e;
			} else {
				if ((e->sesflags&SESFLAG_READONLY)==0 && (f->sesflags&SESFLAG_READONLY)!=0) {	// prefer rw to ro
					f=e;
				} else if (e->rootuid==0 && f->rootuid!=0) {	// prefer root not restricted to restricted
					f=e;
				} else if ((e->sesflags&SESFLAG_CANCHANGEQUOTA)!=0 && (f->sesflags&SESFLAG_CANCHANGEQUOTA)==0) {	// prefer lines with more privileges
					f=e;
				} else if (e->needpassword==1 && f->needpassword==0) {	// prefer lines with passwords
					f=e;
				} else if (e->pleng > f->pleng) {	// prefer more accurate path
					f=e;
				}
			}
		}
	}
	if (f==NULL) {
		if (nopass) {
			if (rndstate==0 || rndcode==NULL || passcode==NULL) {
				return ERROR_NOPASSWORD;
			} else {
				return ERROR_BADPASSWORD;
			}
		}
		return ERROR_EACCES;
	}
	*sesflags = f->sesflags;
	*rootuid = f->rootuid;
	*rootgid = f->rootgid;
	*mapalluid = f->mapalluid;
	*mapallgid = f->mapallgid;
	return STATUS_OK;
}

void acl_freelist(acl *arec) {
	acl *drec;
	while (arec) {
		drec = arec;
		arec = arec->next;
		if (drec->path) {
			free((uint8_t *)(drec->path));
		}
		free(drec);
	}
}

// format:
// ip[/bits]	path	options
//
// options:
//  readonly
//  maproot=uid[:gid]
//  alldirs
//  md5pass=md5(password)
//  password=password
//  dynamicip
//  ignoregid
//  canchangequota
//
// ip[/bits] can be '*' (same as 0.0.0.0/0)
//
// default:
// *	/	alldirs,maproot=0

int acl_parsenet(char *net,uint32_t *fromip,uint32_t *toip) {
	uint32_t ip,i,octet;
	if (net[0]=='*' && net[1]==0) {
		*fromip = 0;
		*toip = 0xFFFFFFFFU;
		return 0;
	}
	ip=0;
	for (i=0 ; i<4; i++) {
		if (*net>='0' && *net<='9') {
			octet=0;
			while (*net>='0' && *net<='9') {
				octet*=10;
				octet+=*net-'0';
				net++;
				if (octet>255) {
					return -1;
				}
			}
		} else {
			return -1;
		}
		if (i<3) {
			if (*net!='.') {
				return -1;
			}
			net++;
		}
		ip*=256;
		ip+=octet;
	}
	if (*net==0) {
		*fromip = ip;
		*toip = ip;
		return 0;
	}
	if (*net=='/') {	// ip/bits and ip/mask
		*fromip = ip;
		ip=0;
		net++;
		for (i=0 ; i<4; i++) {
			if (*net>='0' && *net<='9') {
				octet=0;
				while (*net>='0' && *net<='9') {
					octet*=10;
					octet+=*net-'0';
					net++;
					if (octet>255) {
						return -1;
					}
				}
			} else {
				return -1;
			}
			if (i==0 && *net==0 && octet<=32) {	// bits -> convert to mask and skip rest of loop
				ip = 0xFFFFFFFF;
				if (octet<32) {
					ip<<=32-octet;
				}
				break;
			}
			if (i<3) {
				if (*net!='.') {
					return -1;
				}
				net++;
			}
			ip*=256;
			ip+=octet;
		}
		if (*net!=0) {
			return -1;
		}
		*fromip &= ip;
		*toip = *fromip | (ip ^ 0xFFFFFFFFU);
		return 0;
	}
	if (*net=='-') {	// ip1-ip2
		*fromip = ip;
		ip=0;
		net++;
		for (i=0 ; i<4; i++) {
			if (*net>='0' && *net<='9') {
				octet=0;
				while (*net>='0' && *net<='9') {
					octet*=10;
					octet+=*net-'0';
					net++;
					if (octet>255) {
						return -1;
					}
				}
			} else {
				return -1;
			}
			if (i<3) {
				if (*net!='.') {
					return -1;
				}
				net++;
			}
			ip*=256;
			ip+=octet;
		}
		if (*net!=0) {
			return -1;
		}
		*toip = ip;
		return 0;
	}
	return -1;
}

int acl_parseversion(char *verstr,uint32_t *version) {
	uint32_t vp;
	if (*verstr<'0' || *verstr>'9') {
		return -1;
	}
	vp=0;
	while (*verstr>='0' && *verstr<='9') {
		vp*=10;
		vp+=*verstr-'0';
		verstr++;
	}
	if (vp>255 || (*verstr!='.' && *verstr)) {
		return -1;
	}
	*version = vp<<16;
	if (*verstr==0) {
		return 0;
	}
	verstr++;
	if (*verstr<'0' || *verstr>'9') {
		return -1;
	}
	vp=0;
	while (*verstr>='0' && *verstr<='9') {
		vp*=10;
		vp+=*verstr-'0';
		verstr++;
	}
	if (vp>255 || (*verstr!='.' && *verstr)) {
		return -1;
	}
	*version += vp<<8;
	if (*verstr==0) {
		return 0;
	}
	verstr++;
	if (*verstr<'0' || *verstr>'9') {
		return -1;
	}
	vp=0;
	while (*verstr>='0' && *verstr<='9') {
		vp*=10;
		vp+=*verstr-'0';
		verstr++;
	}
	if (vp>255 || *verstr) {
		return -1;
	}
	*version += vp;
	return 0;
}

int acl_parseuidgid(char *maproot,uint32_t lineno,uint32_t *ruid,uint32_t *rgid) {
	char *uptr,*gptr,*eptr;
	struct group *grrec;
	struct passwd *pwrec;
	uint32_t uid,gid;
	int gidok;

	uptr = maproot;
	gptr = maproot;
	while (*gptr && *gptr!=':') {
		gptr++;
	}

	if (*gptr==':') {
		*gptr = 0;
		gid = 0;
		eptr = gptr+1;
		while (*eptr>='0' && *eptr<='9') {
			gid*=10;
			gid+=*eptr-'0';
			eptr++;
		}
		if (*eptr!=0) {	// not only digits - treat it as a groupname
			grrec = getgrnam(gptr+1);
			if (grrec==NULL) {
				MFSLOG(LOG_WARNING,"mfsexports/maproot: can't find group named '%s' defined in line: %"PRIu32,gptr+1,lineno);
				return -1;
			}
			gid = grrec->gr_gid;
		}
		gidok = 1;
	} else {
		gidok = 0;
		gid = 0;
		gptr = NULL;
	}

	uid = 0;
	eptr = uptr;
	while (*eptr>='0' && *eptr<='9') {
		uid*=10;
		uid+=*eptr-'0';
		eptr++;
	}
	if (*eptr!=0) {	// not only digits - treat it as a username
		pwrec = getpwnam(uptr);
		if (pwrec==NULL) {
			MFSLOG(LOG_WARNING,"mfsexports/maproot: can't find user named '%s' defined in line: %"PRIu32,uptr,lineno);
			return -1;
		}
		*ruid = pwrec->pw_uid;
		if (gidok==0) {
			*rgid = pwrec->pw_gid;
		} else {
			*rgid = gid;
		}
		return 0;
	} else if (gidok==1) {
		*ruid = uid;
		*rgid = gid;
		return 0;
	} else {
		pwrec = getpwuid(uid);
		if (pwrec==NULL) {
			MFSLOG(LOG_WARNING,"mfsexports/maproot: can't determine gid, because can't find user with uid %"PRIu32" defined in line: %"PRIu32,uid,lineno);
			return -1;
		}
		*ruid = pwrec->pw_uid;
		*rgid = pwrec->pw_gid;
		return 0;
	}
	return -1;
}

int acl_parseoptions(char *opts,uint32_t lineno,acl *arec) {
	char *p;
	int o;
	md5ctx ctx;

	while ((p=acl_strsep(&opts,","))) {
		o=0;
//		syslog(LOG_WARNING,"option: %s",p);
		switch (*p) {
		case 'r':
			if (strcmp(p,"ro")==0) {
				arec->sesflags |= SESFLAG_READONLY;
				o=1;
			} else if (strcmp(p,"readonly")==0) {
				arec->sesflags |= SESFLAG_READONLY;
				o=1;
			} else if (strcmp(p,"rw")==0) {
				arec->sesflags &= ~SESFLAG_READONLY;
				o=1;
			} else if (strcmp(p,"readwrite")==0) {
				arec->sesflags &= ~SESFLAG_READONLY;
				o=1;
			}
			break;
		case 'i':
			if (strcmp(p,"ignoregid")==0) {
				if (arec->meta) {
					MFSLOG(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					arec->sesflags |= SESFLAG_IGNOREGID;
				}
				o=1;
			}
			break;
		case 'a':
			if (strcmp(p,"alldirs")==0) {
				if (arec->meta) {
					MFSLOG(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					arec->alldirs = 1;
				}
				o=1;
			}
			break;
		case 'd':
			if (strcmp(p,"dynamicip")==0) {
				arec->sesflags |= SESFLAG_DYNAMICIP;
				o=1;
			}
			break;
		case 'c':
			if (strcmp(p,"canchangequota")==0) {
				if (arec->meta) {
					MFSLOG(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					arec->sesflags |= SESFLAG_CANCHANGEQUOTA;
				}
				o=1;
			}
			break;
		case 'm':
			if (strncmp(p,"maproot=",8)==0) {
				o=1;
				if (arec->meta) {
					MFSLOG(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					if (acl_parseuidgid(p+8,lineno,&arec->rootuid,&arec->rootgid)<0) {
						return -1;
					}
					arec->rootredefined = 1;
				}
			} else if (strncmp(p,"mapall=",7)==0) {
				o=1;
				if (arec->meta) {
					MFSLOG(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					if (acl_parseuidgid(p+7,lineno,&arec->mapalluid,&arec->mapallgid)<0) {
						return -1;
					}
					arec->sesflags |= SESFLAG_MAPALL;
				}
			} else if (strncmp(p,"md5pass=",8)==0) {
				char *ptr = p+8;
				uint32_t i=0;
				o=1;
				while ((*ptr>='0' && *ptr<='9') || (*ptr>='a' && *ptr<='f') || (*ptr>='A' && *ptr<='F')) {
					ptr++;
					i++;
				}
				if (*ptr==0 && i==32) {
					ptr = p+8;
					for (i=0 ; i<16 ; i++) {
						if (*ptr>='0' && *ptr<='9') {
							arec->passworddigest[i]=(*ptr-'0')<<4;
						} else if (*ptr>='a' && *ptr<='f') {
							arec->passworddigest[i]=(*ptr-'a'+10)<<4;
						} else {
							arec->passworddigest[i]=(*ptr-'A'+10)<<4;
						}
						ptr++;
						if (*ptr>='0' && *ptr<='9') {
							arec->passworddigest[i]+=(*ptr-'0');
						} else if (*ptr>='a' && *ptr<='f') {
							arec->passworddigest[i]+=(*ptr-'a'+10);
						} else {
							arec->passworddigest[i]+=(*ptr-'A'+10);
						}
						ptr++;
					}
					arec->needpassword=1;
				} else {
					MFSLOG(LOG_WARNING,"mfsexports: incorrect md5pass definition (%s) in line: %"PRIu32,p,lineno);
					return -1;
				}
			} else if (strncmp(p,"minversion=",11)==0) {
				o=1;
				if (acl_parseversion(p+11,&arec->minversion)<0) {
					MFSLOG(LOG_WARNING,"mfsexports: incorrect minversion definition (%s) in line: %"PRIu32,p,lineno);
					return -1;
				}
			}
			break;
		case 'p':
			if (strncmp(p,"password=",9)==0) {
				md5_init(&ctx);
				md5_update(&ctx,(uint8_t*)(p+9),strlen(p+9));
				md5_final(arec->passworddigest,&ctx);
				arec->needpassword=1;
				o=1;
			}
			break;
		}
		if (o==0) {
			MFSLOG(LOG_WARNING,"mfsexports: unknown option '%s' in line: %"PRIu32" (ignored)",p,lineno);
		}
	}
	return 0;
}

int acl_parseline(char *line,uint32_t lineno,acl *arec) {
	char *net,*path;
	char *p;
	uint32_t pleng;

	arec->pleng = 0;
	arec->path = NULL;
	arec->fromip = 0;
	arec->toip = 0;
	arec->minversion = 0;
	arec->alldirs = 0;
	arec->needpassword = 0;
	arec->meta = 0;
	arec->rootredefined = 0;
	arec->sesflags = SESFLAG_READONLY;
	arec->rootuid = 999;
	arec->rootgid = 999;
	arec->mapalluid = 999;
	arec->mapallgid = 999;
	arec->next = NULL;

	p = line;
	while (*p==' ' || *p=='\t') {
		p++;
	}
	net = p;
	while (*p && *p!=' ' && *p!='\t') {
		p++;
	}
	if (*p==0) {
		MFSLOG(LOG_WARNING,"mfsexports: incomplete definition in line: %"PRIu32,lineno);
		return -1;
	}
	*p=0;
	p++;
	if (acl_parsenet(net,&arec->fromip,&arec->toip)<0) {
		MFSLOG(LOG_WARNING,"mfsexports: incorrect ip/network definition in line: %"PRIu32,lineno);
		return -1;
	}

	while (*p==' ' || *p=='\t') {
		p++;
	}
	if (p[0]=='.' && (p[1]==0 || p[1]==' ' || p[1]=='\t')) {
		path = NULL;
		pleng = 0;
		arec->rootuid = 0;
		arec->rootgid = 0;
		arec->meta = 1;
		p++;
	} else {
		while (*p=='/') {
			p++;
		}
		path = p;
		pleng = 0;
		while (*p && *p!=' ' && *p!='\t') {
			p++;
			pleng++;
		}
		while (pleng>0 && path[pleng-1]=='/') {
			pleng--;
		}
	}
	if (*p==0) {
		// no options - use defaults
		arec->pleng = pleng;
		if (pleng>0) {
			arec->path = malloc(pleng+1);
			memcpy((uint8_t*)(arec->path),path,pleng);
			((uint8_t*)(arec->path))[pleng]=0;
		} else {
			arec->path = NULL;
		}

		return 0;
	}
	while (*p==' ' || *p=='\t') {
		p++;
	}

	if (acl_parseoptions(p,lineno,arec)<0) {
		return -1;
	}

	if ((arec->sesflags&SESFLAG_MAPALL) && (arec->rootredefined==0)) {
		arec->rootuid = arec->mapalluid;
		arec->rootgid = arec->mapallgid;
	}

	arec->pleng = pleng;
	if (pleng>0) {
		arec->path = malloc(pleng+1);
		memcpy((uint8_t*)(arec->path),path,pleng);
		((uint8_t*)(arec->path))[pleng]=0;
	} else {
		arec->path = NULL;
	}

	return 0;
}

void acl_loadexports() {
	FILE *fd;
	char linebuff[10000];
	uint32_t s,lineno;
	acl *newexports,**netail,*arec;

	fd = fopen(ExportsFileName,"r");
	if (fd==NULL) {
/*
		if (errno==ENOENT) {
			if (acl_records==NULL) {
				syslog(LOG_WARNING,"no mfsexports file - using default");
				acl_records = malloc(sizeof(acl));
				acl_records->pleng = 0;
				acl_records->path = NULL;
				acl_records->fromip = 0;
				acl_records->toip = 0xFFFFFFFFU;
				acl_records->alldirs = 1;
				acl_records->needpassword = 0;
				acl_records->meta = 0;
				acl_records->sesflags = 0;
				acl_records->rootuid = 0;
				acl_records->rootgid = 0;
				acl_records->mapalluid = 0;
				acl_records->mapallgid = 0;
				acl_records->next = NULL;
			} else {
				syslog(LOG_WARNING,"no mfsexports file - using previous settings");
			}
		} else {
			syslog(LOG_WARNING,"can't open mfsexports file: %m");
		}
*/
		if (errno==ENOENT) {
			if (acl_records) {
				MFSLOG(LOG_WARNING,"mfsexports configuration file (%s) not found - exports not changed",ExportsFileName);
			} else {
				MFSLOG(LOG_WARNING,"mfsexports configuration file (%s) not found - no exports !!!",ExportsFileName);
			}
			if (msgfd) {
				fprintf(msgfd,"mfsexports configuration file (%s) not found - please create one (you can copy %s.dist to get a base configuration)\n",ExportsFileName,ExportsFileName);
			}
		} else {
			if (acl_records) {
				MFSLOG(LOG_WARNING,"can't open mfsexports configuration file (%s): %m - exports not changed",ExportsFileName);
			} else {
				MFSLOG(LOG_WARNING,"can't open mfsexports configuration file (%s): %m - no exports !!!",ExportsFileName);
			}
			if (msgfd) {
				fprintf(msgfd,"can't open mfsexports configuration file (%s)\n",ExportsFileName);
			}
		}
		return;
	}
	newexports = NULL;
	netail = &newexports;
	lineno = 1;
	arec = malloc(sizeof(acl));
	while (fgets(linebuff,10000,fd)) {
		if (linebuff[0]!='#') {
			linebuff[9999]=0;
			s=strlen(linebuff);
			while (s>0 && (linebuff[s-1]=='\r' || linebuff[s-1]=='\n' || linebuff[s-1]=='\t' || linebuff[s-1]==' ')) {
				s--;
			}
			linebuff[s]=0;
			if (acl_parseline(linebuff,lineno,arec)>=0) {
				*netail = arec;
				netail = &(arec->next);
				arec = malloc(sizeof(acl));
			}
		}
		lineno++;
	}
	free(arec);
	if (ferror(fd)) {
		fclose(fd);
		MFSLOG(LOG_WARNING,"error reading mfsexports file - exports not changed");
		acl_freelist(newexports);
		if (msgfd) {
			fprintf(msgfd,"error reading mfsexports file - using defaults\n");
		}
		return;
	}
	fclose(fd);
	acl_freelist(acl_records);
	acl_records = newexports;
	MFSLOG(LOG_NOTICE,"exports file has been loaded");
	if (msgfd) {
		fprintf(msgfd,"exports file has been loaded\n");
	}
}

void acl_reloadexports(void) {
	acl_loadexports(NULL);
}

int acl_init() {
	ExportsFileName = cfg_getstr("EXPORTS_FILENAME",ETC_PATH "/mfsexports.cfg");
	acl_records = NULL;
	acl_loadexports();
	if (acl_records==NULL) {
		fprintf(msgfd,"no exports defined !!!\n");
		return -1;
	}
	main_reloadregister(acl_reloadexports);
	return 0;
}
