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

#ifndef MFSMAXFILES
#define MFSMAXFILES 5000
#endif

#if defined(HAVE_MLOCKALL) && defined(RLIMIT_MEMLOCK) && defined(MCL_CURRENT) && defined(MCL_FUTURE)
#define MFS_USE_MEMLOCK
#endif

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/time.h>
#ifdef MFS_USE_MEMLOCK
#include <sys/mman.h>
#endif
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/resource.h>
#include <grp.h>
#include <pwd.h>

#define STR_AUX(x) #x
#define STR(x) STR_AUX(x)

#include "cfg.h"
#include "main.h"
#include "init.h"

#define RM_RESTART 0
#define RM_START 1
#define RM_STOP 2
#define RM_RELOAD 3

typedef struct deentry {
	void (*fun)(void);
	struct deentry *next;
} deentry;

static deentry *dehead=NULL;

typedef struct weentry {
	void (*fun)(void);
	struct weentry *next;
} weentry;

static weentry *wehead=NULL;


typedef struct ceentry {
	int (*fun)(void);
	struct ceentry *next;
} ceentry;

static ceentry *cehead=NULL;


typedef struct rlentry {
	void (*fun)(void);
	struct rlentry *next;
} rlentry;

static rlentry *rlhead=NULL;


typedef struct epollentry {
	void (*desc)(int);
	void (*serve)(int ,int ,struct epoll_event *);
	struct epollentry *next;
} epollentry;

static epollentry *epollhead=NULL;


typedef struct eloopentry {
	void (*fun)(void);
	struct eloopentry *next;
} eloopentry;

static eloopentry *eloophead=NULL;


typedef struct timeentry {
	time_t nextevent;
	uint32_t seconds;
	int mode;
//	int offset;
	void (*fun)(void);
	struct timeentry *next;
} timeentry;

static timeentry *timehead=NULL;

static int now;
static uint64_t usecnow;
//static int alcnt=0;

static int terminate=0;
static int sigchld=0;
static int reload=0;
static int epoll_fd;

//to avoid epoll&fork problem

/* interface */

void main_destructregister (void (*fun)(void)) {
	deentry *aux=(deentry*)malloc(sizeof(deentry));
	aux->fun = fun;
	aux->next = dehead;
	dehead = aux;
}

void main_canexitregister (int (*fun)(void)) {
	ceentry *aux=(ceentry*)malloc(sizeof(ceentry));
	aux->fun = fun;
	aux->next = cehead;
	cehead = aux;
}

void main_wantexitregister (void (*fun)(void)) {
	weentry *aux=(weentry*)malloc(sizeof(weentry));
	aux->fun = fun;
	aux->next = wehead;
	wehead = aux;
}

void main_reloadregister (void (*fun)(void)) {
	rlentry *aux=(rlentry*)malloc(sizeof(rlentry));
	aux->fun = fun;
	aux->next = rlhead;
	rlhead = aux;
}

void main_epollregister (void (*desc)(int),void (*serve)(int ,int ,struct epoll_event *)) {
	epollentry *aux=(epollentry*)malloc(sizeof(epollentry));
	aux->desc = desc;
	aux->serve = serve;
	aux->next = epollhead;
	epollhead = aux;
}

void main_eachloopregister (void (*fun)(void)) {
	eloopentry *aux=(eloopentry*)malloc(sizeof(eloopentry));
	aux->fun = fun;
	aux->next = eloophead;
	eloophead = aux;
}

void main_timeregister (int mode,uint32_t seconds,uint32_t offset,void (*fun)(void)) {
	timeentry *aux;
	if (seconds==0 || offset>=seconds) return;
	aux = (timeentry*)malloc(sizeof(timeentry));
	aux->nextevent = ((now / seconds) * seconds) + offset;
	while (aux->nextevent<now) {
		aux->nextevent+=seconds;
	}
	aux->seconds = seconds;
	aux->mode = mode;
	aux->fun = fun;
	aux->next = timehead;
	timehead = aux;
}

/* internal */


int canexit() {
	ceentry *aux;
	for (aux = cehead ; aux!=NULL ; aux=aux->next ) {
		if (aux->fun()==0) {
			return 0;
		}
	}
	return 1;
}

void termhandle(int signo) {
	(void)signo;
	terminate=1;
}

void reloadhandle(int signo) {
	(void)signo;
	reload=1;
}

void sigchldhandle(int signo) {
	(void)signo;
	sigchld=1;
}

int main_time() {
	return now;
}

uint64_t main_utime() {
	return usecnow;
}

void destruct() {
	deentry *deit;
	for (deit = dehead ; deit!=NULL ; deit=deit->next ) {
		deit->fun();
	}
}

void mainloop() {
	struct timeval tv;
	epollentry *epollit;
	eloopentry *eloopit;
	timeentry *timeit;
	ceentry *ceit;
	weentry *weit;
	rlentry *rlit;
	struct epoll_event pdesc[MFSMAXFILES];
	uint32_t ndesc;
	int fd_num;
	int count;
	serventry *weptr;
	epollentry *matoml_entry = NULL,*matocs_entry = NULL ;
	epollentry *matocu_entry = NULL,*masterconn_entry = NULL,*matosla_entry = NULL;

	/* 
	  * as the sequence decide the head the role, you should consider carefull the 
	  * init func which call the main_epollregister
	  */
	epollit = epollhead;
	matocu_entry = epollit;
	if (epollit->next != NULL) {
		epollit = epollit->next;
		matocs_entry = epollit;
	}
	if (epollit->next != NULL) {
		epollit = epollit->next;
		matoml_entry = epollit;
	}
	if (epollit->next != NULL) {
		epollit = epollit->next;
		masterconn_entry = epollit;
	}	
	if(epollit->next != NULL) {
		epollit = epollit->next;
		matosla_entry = epollit;		
	}	

	epoll_fd = epoll_create(MFSMAXFILES);
	if(epoll_fd <= 0) {
		syslog(LOG_ERR,"epoll create failed");
		terminate = 3;
	}	
	while (terminate!=3) {
		tv.tv_sec=0;
		tv.tv_usec=300000;
		ndesc=0;
		for (epollit = epollhead ; epollit != NULL ; epollit = epollit->next) {
			epollit->desc(epoll_fd);
		}
		fd_num = epoll_wait(epoll_fd,pdesc,MFSMAXFILES,50);
		gettimeofday(&tv,NULL);
		usecnow = tv.tv_sec;
		usecnow *= 1000000;
		usecnow += tv.tv_usec;
		now = tv.tv_sec;
		if (fd_num<0) {
			if (errno==EAGAIN) {
				syslog(LOG_WARNING,"epoll returned EAGAIN");
				usleep(100000);
				continue;
			}
			if (errno!=EINTR) {
				syslog(LOG_WARNING,"epoll error: %m");
				break;
			}
		} else {
			for(count = 0;count < fd_num;count++)
			{
				weptr = (serventry*)pdesc[count].data.ptr;
		//		syslog(LOG_ERR,"connection %d,sock %d",weptr->connection,weptr->sock);
				if(weptr->connection==0) { 
					matocu_entry->serve(epoll_fd,count,pdesc);
				} else if (weptr->connection==1) {
					matocs_entry->serve(epoll_fd,count,pdesc);
				} else if (weptr->connection==2) {
					matoml_entry->serve(epoll_fd,count,pdesc);
				} else if (weptr->connection==3) {
					masterconn_entry->serve(epoll_fd,count,pdesc);				
				} else if (weptr->connection==4) {
					matosla_entry->serve(epoll_fd,count,pdesc);						
				} else {
					syslog(LOG_ERR,"unrecognized connection %d",weptr->connection);
				}
			}
		}
		for (eloopit = eloophead ; eloopit != NULL ; eloopit = eloopit->next) {
			eloopit->fun();
		}
		for (timeit = timehead ; timeit != NULL ; timeit = timeit->next) {
			if (timeit->mode==TIMEMODE_RUNALL) {
				while (now>=timeit->nextevent) {
					timeit->nextevent += timeit->seconds;
					timeit->fun();
				}
			} else if (timeit->mode==TIMEMODE_RUNONCE) {
				if (now>=timeit->nextevent) {
					while (now>=timeit->nextevent) {
						timeit->nextevent += timeit->seconds;
					}
					timeit->fun();
				}
			} else { /* timeit->mode == TIMEMODE_SKIP */
				if (now>=timeit->nextevent) {
					if (now==timeit->nextevent) {
						timeit->fun();
					}
					while (now>=timeit->nextevent) {
						timeit->nextevent += timeit->seconds;
					}
				}
			}
		}
		if (terminate==0 && reload) {
			for (rlit = rlhead ; rlit!=NULL ; rlit=rlit->next ) {
				rlit->fun();
			}
			reload=0;
		}
		if (terminate==1) {
			for (weit = wehead ; weit!=NULL ; weit=weit->next ) {
				weit->fun();
			}
			terminate=2;
		}
		if (terminate==2) {
			fd_num = 1;
			for (ceit = cehead ; ceit!=NULL && fd_num ; ceit=ceit->next ) {
				if (ceit->fun()==0) {
					fd_num=0;
				}
			}
			if (fd_num) {
				terminate=3;
			}
		}
	}
	close(epoll_fd);
}

int initialize() {
	uint32_t i;
	int ok;
	ok = 1;
	now = time(NULL);
	for (i=0 ; (long int)(RunTab[i].fn)!=0 && ok ; i++) {
		if (RunTab[i].fn(msgfd)<0) {
			syslog(LOG_ERR,"init: %s failed !!!",RunTab[i].name);
			fprintf(msgfd,"init: %s failed !!!\n",RunTab[i].name);
			ok=0;
		}
	}
	return ok;
}

static int termsignal[]={
	SIGTERM,
	-1};

static int reloadsignal[]={
	SIGHUP,
	-1};

static int ignoresignal[]={
	SIGINT,
	SIGQUIT,
#ifdef SIGPIPE
	SIGPIPE,
#endif
#ifdef SIGTSTP
	SIGTSTP,
#endif
#ifdef SIGTTIN
	SIGTTIN,
#endif
#ifdef SIGTTOU
	SIGTTOU,
#endif
#ifdef SIGINFO
	SIGINFO,
#endif
	-1};

void set_signal_handlers(void) {
	struct sigaction sa;
	uint32_t i;

#ifdef SA_RESTART
	sa.sa_flags = SA_RESTART;
#else
	sa.sa_flags = 0;
#endif
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = termhandle;
	for (i=0 ; termsignal[i]>0 ; i++) {
		sigaction(termsignal[i],&sa,(struct sigaction *)0);
	}
	sa.sa_handler = reloadhandle;
	for (i=0 ; reloadsignal[i]>0 ; i++) {
		sigaction(reloadsignal[i],&sa,(struct sigaction *)0);
	}
	sa.sa_handler = SIG_IGN;
	for (i=0 ; ignoresignal[i]>0 ; i++) {
		sigaction(ignoresignal[i],&sa,(struct sigaction *)0);
	}
}

static inline const char* errno_to_str() {
#ifdef HAVE_STRERROR_R
	static char errorbuff[500];
# ifdef STRERROR_R_CHAR_P
	return strerror_r(errno,errorbuff,500);
# else
	strerror_r(errno,errorbuff,500);
	return errorbuff;
# endif
#else
	snprintf(errorbuff,500,"(errno:%d)",errno);
	errorbuff[499]=0;
	return errorbuff;
#endif
}

void changeugid(void) {
	char *wuser;
	char *wgroup;
	uid_t wrk_uid;
	gid_t wrk_gid;
	int gidok;

	if (geteuid()==0) {
		wuser = cfg_getstr("WORKING_USER",DEFAULT_USER);
		wgroup = cfg_getstr("WORKING_GROUP",DEFAULT_GROUP);

		gidok = 0;
		wrk_gid = -1;
		if (wgroup[0]=='#') {
			wrk_gid = strtol(wgroup+1,NULL,10);
			gidok = 1;
		} else if (wgroup[0]) {
			struct group *gr;
			gr = getgrnam(wgroup);
			if (gr==NULL) {
				fprintf(stderr,"%s: no such group !!!\n",wgroup);
				syslog(LOG_WARNING,"%s: no such group !!!",wgroup);
				exit(1);
			} else {
				wrk_gid = gr->gr_gid;
				gidok = 1;
			}
		}

		if (wuser[0]=='#') {
			struct passwd *pw;
			wrk_uid = strtol(wuser+1,NULL,10);
			if (gidok==0) {
				pw = getpwuid(wrk_uid);
				if (pw==NULL) {
					fprintf(stderr,"%s: no such user id - can't obtain group id\n",wuser+1);
					syslog(LOG_ERR,"%s: no such user id - can't obtain group id",wuser+1);
					exit(1);
				}
				wrk_gid = pw->pw_gid;
			}
		} else {
			struct passwd *pw;
			pw = getpwnam(wuser);
			if (pw==NULL) {
				fprintf(stderr,"%s: no such user !!!\n",wuser);
				syslog(LOG_ERR,"%s: no such user !!!",wuser);
				exit(1);
			}
			wrk_uid = pw->pw_uid;
			if (gidok==0) {
				wrk_gid = pw->pw_gid;
			}
		}
		free(wuser);
		free(wgroup);

		setgid(wrk_gid);
		syslog(LOG_NOTICE,"set gid to %d",(int)wrk_gid);
		setuid(wrk_uid);
		syslog(LOG_NOTICE,"set uid to %d",(int)wrk_uid);
	}
}

pid_t mylock(int fd) {
	struct flock fl;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = getpid();
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	for (;;) {
		if (fcntl(fd,F_SETLK,&fl)>=0) {	// lock set
			return 0;	// ok
		}
		if (errno!=EAGAIN) {	// error other than "already locked"
			return -1;	// error
		}
		if (fcntl(fd,F_GETLK,&fl)<0) {	// get lock owner
			return -1;	// error getting lock
		}
		if (fl.l_type!=F_UNLCK) {	// found lock
			return fl.l_pid;	// return lock owner
		}
	}
	return -1;	// pro forma
}

int wdlock(uint8_t runmode,uint32_t timeout) {
	pid_t ownerpid;
	pid_t newownerpid;
	int lfp;
	uint32_t l;

	lfp = open("." STR(APPNAME) ".lock",O_WRONLY|O_CREAT,0666);
	if (lfp<0) {
		syslog(LOG_ERR,"can't create lockfile in working directory: %m");
		fprintf(msgfd,"can't create lockfile in working directory: %s\n",errno_to_str());
		return -1;
	}
	ownerpid = mylock(lfp);
	if (ownerpid<0) {
		syslog(LOG_ERR,"fcntl error: %m");
		fprintf(msgfd,"fcntl error: %s\n",errno_to_str());
		return -1;
	}
	if (ownerpid>0) {
		if (runmode==RM_START) {
			fprintf(msgfd,"can't start: lockfile is already locked by another process\n");
			return -1;
		}
		if (runmode==RM_RELOAD) {
			if (kill(ownerpid,SIGHUP)<0) {
				syslog(LOG_WARNING,"can't send reload signal to lock owner (error: %m)");
				fprintf(msgfd,"can't send reload signal to lock owner (error: %s)\n",errno_to_str());
				return -1;
			}
			fprintf(msgfd,"reload signal has beed sent\n");
			return 0;
		}
		fprintf(msgfd,"sending SIGTERM to lock owner (pid:%ld)\n",(long int)ownerpid);
		if (kill(ownerpid,SIGTERM)<0) {
			syslog(LOG_WARNING,"can't kill lock owner (error: %m)");
			fprintf(msgfd,"can't kill lock owner (error: %s)\n",errno_to_str());
			return -1;
		}
		l=0;
		fprintf(msgfd,"waiting for termination ... ");
		fflush(msgfd);
		do {
			newownerpid = mylock(lfp);
			if (newownerpid<0) {
				syslog(LOG_ERR,"fcntl error: %m");
				fprintf(msgfd,"fcntl error: %s\n",errno_to_str());
				return -1;
			}
			if (newownerpid>0) {
				l++;
				if (l>=timeout) {
					syslog(LOG_ERR,"about %"PRIu32" seconds passed and lockfile is still locked - giving up",l);
					fprintf(msgfd,"give up\n");
					return -1;
				}
				if (l%10==0) {
					syslog(LOG_WARNING,"about %"PRIu32" seconds passed and lock still exists",l);
					fprintf(msgfd,"%"PRIu32"s ",l);
					fflush(msgfd);
				}
				if (newownerpid!=ownerpid) {
					fprintf(msgfd,"\nnew lock owner detected\n");
					fprintf(msgfd,"sending SIGTERM to lock owner (pid:%ld) ... ",(long int)newownerpid);
					fflush(msgfd);
					if (kill(newownerpid,SIGTERM)<0) {
						syslog(LOG_WARNING,"can't kill lock owner (error: %m)");
						fprintf(msgfd,"can't kill lock owner (error: %s)\n",errno_to_str());
						return -1;
					}
					ownerpid = newownerpid;
				}
			}
			sleep(1);
		} while (newownerpid!=0);
		fprintf(msgfd,"terminated\n");
		return 0;
	}
	if (runmode==RM_START || runmode==RM_RESTART) {
		fprintf(msgfd,"lockfile created and locked\n");
	} else if (runmode==RM_STOP) {
		fprintf(msgfd,"can't find process to terminate\n");
	} else if (runmode==RM_RELOAD) {
		fprintf(msgfd,"can't find process to send reload signal\n");
	}
	return 0;
}

int check_old_locks(uint8_t runmode,uint32_t timeout) {
	int lfp;
	char str[13];
	uint32_t l;
	pid_t ptk;
	char *lockfname;

	lockfname = cfg_getstr("LOCK_FILE",RUN_PATH "/" STR(APPNAME) ".lock");
	lfp=open(lockfname,O_RDWR);
	if (lfp<0) {
		if (errno==ENOENT) {    // no old lock file
			free(lockfname);
			return 0;	// ok
		}
		syslog(LOG_ERR,"open %s error: %m",lockfname);
		fprintf(msgfd,"open %s error: %s\n",lockfname,errno_to_str());
		free(lockfname);
		return -1;
	}
	if (lockf(lfp,F_TLOCK,0)<0) {
		if (errno!=EAGAIN) {
			syslog(LOG_ERR,"lock %s error: %m",lockfname);
			fprintf(msgfd,"lock %s error: %s\n",lockfname,errno_to_str());
			free(lockfname);
			return -1;
		}
		if (runmode==RM_START) {
			syslog(LOG_ERR,"old lockfile is locked - can't start");
			fprintf(msgfd,"old lockfile is locked - can't start\n");
			free(lockfname);
			return -1;
		}
		if (runmode==RM_STOP || runmode==RM_RESTART) {
			fprintf(msgfd,"old lockfile found - trying to kill previous instance using data from old lockfile\n");
		} else if (runmode==RM_RELOAD) {
			fprintf(msgfd,"old lockfile found - sending reload signal using data from old lockfile\n");
		}
		l=read(lfp,str,13);
		if (l==0 || l>=13) {
			syslog(LOG_ERR,"wrong pid in old lockfile %s",lockfname);
			fprintf(msgfd,"wrong pid in old lockfile %s\n",lockfname);
			free(lockfname);
			return -1;
		}
		str[l]=0;
		ptk = strtol(str,NULL,10);
		if (runmode==RM_RELOAD) {
			if (kill(ptk,SIGHUP)<0) {
				syslog(LOG_WARNING,"can't send reload signal (error: %m)");
				fprintf(msgfd,"can't send reload signal (error: %s)\n",errno_to_str());
				free(lockfname);
				return -1;
			}
			fprintf(msgfd,"reload signal has beed sent\n");
			return 0;
		}
		fprintf(msgfd,"sending SIGTERM to previous instance (pid:%ld)\n",(long int)ptk);
		if (kill(ptk,SIGTERM)<0) {
			syslog(LOG_WARNING,"can't kill previous process (error: %m)");
			fprintf(msgfd,"can't kill previous process (error: %s)\n",errno_to_str());
			free(lockfname);
			return -1;
		}
		l=0;
		fprintf(msgfd,"waiting for termination ...\n");
		while (lockf(lfp,F_TLOCK,0)<0) {
			if (errno!=EAGAIN) {
				syslog(LOG_ERR,"lock %s error: %m",lockfname);
				fprintf(msgfd,"lock %s error: %s\n",lockfname,errno_to_str());
				free(lockfname);
				return -1;
			}
			sleep(1);
			l++;
			if (l>=timeout) {
				syslog(LOG_ERR,"about %"PRIu32" seconds passed and old lockfile is still locked - giving up",l);
				fprintf(msgfd,"about %"PRIu32" seconds passed and old lockfile is still locked - giving up\n",l);
				free(lockfname);
				return -1;
			}
			if (l%10==0) {
				syslog(LOG_WARNING,"about %"PRIu32" seconds passed and old lockfile is still locked",l);
				fprintf(msgfd,"about %"PRIu32" seconds passed and old lockfile is still locked\n",l);
			}
		}
		fprintf(msgfd,"terminated\n");
	} else {
		fprintf(msgfd,"found unlocked old lockfile\n");
		if (runmode==RM_RELOAD) {
			fprintf(msgfd,"can't obtain process id using old lockfile\n");
			return 0;
		}
	}
	fprintf(msgfd,"removing old lockfile\n");
	close(lfp);
	unlink(lockfname);
	free(lockfname);
	return 0;
}

void remove_old_wdlock(void) {
	unlink(".lock_" STR(APPNAME));
}

FILE* makedaemon() {
	int nd,f;
#ifndef HAVE_DUP2
	int dd;
#endif
	f = fork();
	if (f<0) {
		syslog(LOG_ERR,"first fork error: %m");
		exit(1);
	}	
	if (f>0) {	
		printf("Starting daemon ...\n");
		exit(0);
	}
/*
	ssize_t r;
	uint8_t pipebuff[1000];
	int piped[2];

	if (pipe(piped)<0) {
		fprintf(stderr,"pipe error\n");
		exit(1);
	}
	if (f>0) {
		close(piped[1]);
		printf("Starting daemon ...\n");
		while ((r=read(piped[0],pipebuff,1000))) {
			if (r>0) {
				fwrite(pipebuff,1,r,stderr);
			} else {
				fprintf(stderr,"Error reading pipe: %s\n",errno_to_str());
				exit(1);
			}
		}
		exit(0);
	}
*/
	setsid();
	setpgid(0,getpid());
	f = fork();
	if (f<0) {
		syslog(LOG_ERR,"second fork error: %m");
/*
		if (write(piped[1],"fork error\n",11)!=11) {
			syslog(LOG_ERR,"pipe write error: %m");
		}
		close(piped[1]);
*/
		exit(1);
	}
	if (f>0) {
		exit(0);
	}
	set_signal_handlers();
	//msgfd = fdopen(piped[1],"w");
	//setvbuf(msgfd,(char *)NULL,_IOLBF,0);

#ifdef HAVE_DUP2
	if ((nd = open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(nd, STDIN_FILENO);
		dup2(nd, STDOUT_FILENO);
		dup2(nd, STDERR_FILENO);
		if (nd!=STDIN_FILENO && nd!=STDOUT_FILENO && nd!=STDERR_FILENO) {
			close (nd);
		}
	} else {
		syslog(LOG_ERR,"can't open /dev/null (error: %m)");
		fprintf(msgfd,"can't open /dev/null (error: %s)\n",errno_to_str());
		exit(1);
	}
#else
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	nd = open("/dev/null", O_RDWR, 0);
	if (nd!=STDIN_FILENO) {
		fprintf(msgfd,"unexpected descriptor number (got: %d,expected: %d)\n",nd,STDIN_FILENO);
		exit(1);
	}
	dd = dup(nd);
	if (dd!=STDOUT_FILENO) {
		fprintf(msgfd,"unexpected descriptor number (got: %d,expected: %d)\n",dd,STDOUT_FILENO);
		exit(1);
	}
	dd = dup(nd);
	if (dd!=STDERR_FILENO) {
		fprintf(msgfd,"unexpected descriptor number (got: %d,expected: %d)\n",dd,STDERR_FILENO);
		exit(1);
	}
#endif
	return msgfd;
}

/*
void createpath(const char *filename, FILE *msgfd) {
	char pathbuff[1024];
	const char *src = filename;
	char *dst = pathbuff;
	if (*src=='/') *dst++=*src++;

	while (*src) {
		while (*src!='/' && *src) {
			*dst++=*src++;
		}
		if (*src=='/') {
			*dst='\0';
			if (mkdir(pathbuff,(mode_t)0777)<0) {
				if (errno!=EEXIST) {
					syslog(LOG_NOTICE,"creating directory %s: %m",pathbuff);
					fprintf(msgfd,"error creating directory %s\n",pathbuff);
				}
			} else {
				syslog(LOG_NOTICE,"directory %s has been created",pathbuff);
				fprintf(msgfd,"directory %s has been created\n",pathbuff);
			}
			*dst++=*src++;
		}
	}
}
*/

void usage(const char *appname) {
	printf(
"usage: %s [-vdu] [-t locktimeout] [-c cfgfile] [start|stop|restart|reload]\n"
"\n"
"-v : print version number and exit\n"
"-d : run in foreground\n"
"-u : log undefined config variables\n"
"-t locktimeout : how long wait for lockfile\n"
"-c cfgfile : use given config file\n"
	,appname);
	exit(1);
}

#ifndef UNITTEST
int main(int argc,char **argv) {
	char *logappname;
//	char *lockfname;
	char *wrkdir;
	char *cfgfile;
	char *appname;
	int ch;
	uint8_t runmode;
	int rundaemon,logundefined;
#ifdef MFS_USE_MEMLOCK
	int lockmemory;
#endif
	int32_t nicelevel;
	uint32_t locktimeout;
	struct rlimit rls;

	cfgfile=strdup(ETC_PATH "/" STR(APPNAME) ".cfg");
	locktimeout=60;
	rundaemon=1;
	runmode = RM_RESTART;
	logundefined=0;
	appname = argv[0];

	while ((ch = getopt(argc, argv, "uvdfsc:t:h?")) != -1) {
		switch(ch) {
			case 'v':
				printf("version: %u.%u.%u r%u\n",VERSMAJ,VERSMID,VERSMIN,SVNVER);
				return 0;
			case 'd':
				rundaemon=0;
				break;
			case 'f':
				runmode=RM_START;
				break;
			case 's':
				runmode=RM_STOP;
				break;
			case 't':
				locktimeout=strtoul(optarg,NULL,10);
				break;
			case 'c':
				free(cfgfile);
				cfgfile = strdup(optarg);
				break;
			case 'u':
				logundefined=1;
				break;
			default:
				usage(appname);
				return 1;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc==1) {
		if (strcasecmp(argv[0],"start")==0) {
			runmode = RM_START;
		} else if (strcasecmp(argv[0],"stop")==0) {
			runmode = RM_STOP;
		} else if (strcasecmp(argv[0],"restart")==0) {
			runmode = RM_RESTART;
		} else if (strcasecmp(argv[0],"reload")==0) {
			runmode = RM_RELOAD;
		} else {
			usage(appname);
			return 1;
		}
	} else if (argc!=0) {
		usage(appname);
		return 1;
	}

	if (cfg_load(cfgfile,logundefined)==0) {
		fprintf(stderr,"can't load config file: %s - using defaults\n",cfgfile);
	}
	free(cfgfile);

	logappname = cfg_getstr("SYSLOG_IDENT",STR(APPNAME));

	if (rundaemon) {
		if (logappname[0]) {
			openlog(logappname, LOG_PID | LOG_NDELAY , LOG_DAEMON);
		} else {
			openlog(STR(APPNAME), LOG_PID | LOG_NDELAY , LOG_DAEMON);
		}
	} else {
#if defined(LOG_PERROR)
		if (logappname[0]) {
			openlog(logappname, LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_USER);
		} else {
			openlog(STR(APPNAME), LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_USER);
		}
#else
		if (logappname[0]) {
			openlog(logappname, LOG_PID | LOG_NDELAY, LOG_USER);
		} else {
			openlog(STR(APPNAME), LOG_PID | LOG_NDELAY, LOG_USER);
		}
#endif
	}

	rls.rlim_cur = MFSMAXFILES;
	rls.rlim_max = MFSMAXFILES;
	if (setrlimit(RLIMIT_NOFILE,&rls)<0) {
		syslog(LOG_NOTICE,"can't change open files limit to %u",MFSMAXFILES);
	}
#ifdef MFS_USE_MEMLOCK
	lockmemory = cfg_getnum("LOCK_MEMORY",0);
	if (lockmemory) {
		rls.rlim_cur = RLIM_INFINITY;
		rls.rlim_max = RLIM_INFINITY;
		setrlimit(RLIMIT_MEMLOCK,&rls);
	}
#endif
	nicelevel = cfg_getint32("NICE_LEVEL",-19);
	setpriority(PRIO_PROCESS,getpid(),nicelevel);

	changeugid();

	wrkdir = cfg_getstr("DATA_PATH",DATA_PATH);
	fprintf(stderr,"working directory: %s\n",wrkdir);

	if (chdir(wrkdir)<0) {
		fprintf(stderr,"can't set working directory to %s\n",wrkdir);
		syslog(LOG_ERR,"can't set working directory to %s",wrkdir);
		return 1;
	}
	free(wrkdir);

	if(runmode == RM_START || runmode == RM_RESTART) {
		logfile = cfg_getstr("LOG_PATH", "");
        struct stat buf;        

		if(strcmp(logfile, "") == 0) {
			fprintf(stderr, "no log path , use the stderr which you should redirect it\n");
			msgfd = fdopen(dup(STDERR_FILENO),"a");
		} else {
			fprintf(stderr, "log path:%s\n", logfile);
			msgfd = fopen(logfile,"a");                   
            stat(logfile,  &buf);
            logsize = buf.st_size;
		}		
	} else {
		msgfd = fdopen(dup(STDERR_FILENO),"a");
	}

	if(msgfd == NULL) {
		fprintf(stderr, "open msgfd failed errno:%d\n", errno);
		fflush(stderr);
		return 1;
	}
	
	setvbuf(msgfd,(char *)NULL,_IOLBF,0);

	if ((runmode==RM_START || runmode==RM_RESTART) && rundaemon) {
		//msgfd = makedaemon();
		makedaemon();
	} else {
		if (runmode==RM_START || runmode==RM_RESTART) {
			set_signal_handlers();
		}
	}

	umask(027);

	/* for upgrading from previous versions of MFS */
	if (check_old_locks(runmode,locktimeout)<0) {
		return 1;
	}

	if (wdlock(runmode,locktimeout)<0) {
		return 1;
	}

	remove_old_wdlock();

	if (runmode==RM_STOP || runmode==RM_RELOAD) {
		return 0;
	}

#ifdef MFS_USE_MEMLOCK
	if (lockmemory) {
		if (mlockall(MCL_CURRENT|MCL_FUTURE)==0) {
			syslog(LOG_NOTICE,"process memory was successfully locked in RAM");
			fprintf(msgfd,"process memory was successfully locked in RAM\n");
		}
	}
#endif
	fprintf(msgfd,"initializing %s modules ...\n",logappname);

	if (initialize()) {
		if (getrlimit(RLIMIT_NOFILE,&rls)==0) {
			syslog(LOG_NOTICE,"open files limit: %lu",(unsigned long)(rls.rlim_cur));
		}
		fprintf(msgfd,"%s daemon initialized properly\n",logappname);
		//fclose(msgfd);
		mainloop();
	} else {
		fprintf(msgfd,"error occured during initialization - exiting\n");
		fclose(msgfd);
	}
	free(logappname);
	destruct();
	closelog();
	return 0;
}
#endif
