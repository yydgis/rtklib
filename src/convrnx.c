/*------------------------------------------------------------------------------
* convrnx.c : rinex translator for rtcm and receiver raw data log
*
*          Copyright (C) 2009-2020 by T.TAKASU, All rights reserved.
*
* version : $Revision: 1.2 $ $Date: 2008/07/17 21:48:06 $
* history : 2009/04/10 1.0  new
*           2009/06/02 1.1  support glonass
*           2009/12/18 1.2  add check return of init_rtcm()/init_raw()
*           2010/07/15 1.3  support wildcard expansion of input file
*                           support rinex 3.00
*                           support rinex as input format
*                           support output of geo navigation message
*                           support rtcm antenna and receiver info
*                           changed api:
*                               convrnx()
*           2011/05/27 1.4  support GW10, Javad, LEX receiver
*                           support lex message conversion
*                           change api convrnx()
*           2012/10/18 1.5  support multiple codes in a frequency
*           2012/10/29 1.6  fix bug on scanning obs types
*                           support output of compass navigation data
*                           add supported obs types for rinex input
*           2013/03/11 1.7  support binex and rinex 3.02
*                           add approx position in rinex obs header if blank
*           2014/05/24 1.8  support beidou B1
*           2014/08/26 1.9  support input format rt17
*           2015/05/24 1.10 fix bug on setting antenna delta in rtcm2opt()
*           2016/07/04 1.11 support IRNSS
*           2016/10/10 1.12 support event output by staid change in rtcm
*                           support separated navigation files for ver.3
*           2017/06/06 1.13 fix bug on array overflow in set_obstype() and
*                           scan_obstype()
*           2018/10/10 1.14 add trace of half-cycle ambiguity status
*                           fix bug on missing navigation data
*           2020/11/30 1.15 force scanning receiver log for obs-types (2-pass)
*                           delete scanobs in RINEX options (rnxopt_t)
*                           add phase shift option (phshift) in rnxopt_t
*                           sort obs-types by freq-index and code priority
*                           add test obs-types supportted by RINEX versions
*                           support receiver/antenna info in raw data
*                           fix bug on writing BDS/IRN nav header in closefile()
*                           fix bug on screening time in screent_ttol()
*                           fix bug on screening QZS L1S messages as SBAS
*                           use integer types in stdint.h
*-----------------------------------------------------------------------------*/
#include "rtklib.h"

#define NOUTFILE       10       /* number of output files */
#define NSATSYS         7       /* number of satellite systems */
#define TSTARTMARGIN    60.0    /* time margin for file name replacement */

#define EVENT_STARTMOVE 2       /* rinex event start moving antenna */
#define EVENT_NEWSITE   3       /* rinex event new site occupation */
#define EVENT_HEADER    4       /* rinex event header info follows */
#define EVENT_EXTERNAL  5       /* rinex event external event */

/* type definitions ----------------------------------------------------------*/

typedef struct stas_tag {       /* station list type */
    int staid;                  /* station IS */
    gtime_t ts,te;              /* first and last observation time */
    sta_t  sta;                 /* station parameters */
    struct stas_tag *next;      /* next list */
} stas_t;

typedef struct halfc_tag {      /* half-cycle ambiguity list type */
    gtime_t ts,te;              /* first and last observation time */
    uint8_t stat;               /* half-cycle ambiguity status */
    struct halfc_tag *next;     /* next list */
} halfc_t;

typedef struct {                /* stream file type */
    int format;                 /* stream format (STRFMT_???) */
    int staid;                  /* station ID */
    int ephsat,ephset;          /* satelite and set of input ephemeris */
    gtime_t time;               /* current time */
    gtime_t tstart;             /* start time */
    obs_t  *obs;                /* pointer to input observation data */
    nav_t  *nav;                /* pointer to input navigation data */
    sta_t  *sta;                /* pointer to input station parameters */
    rtcm_t rtcm;                /* input RTCM data */
    rtcm_t out;                 /* output RTCM data */
    raw_t  raw;                 /* input receiver raw data */
    rnxctr_t rnx;               /* input RINEX control data */
    stas_t *stas;               /* station list */
    uint8_t slips [MAXSAT][NFREQ+NEXOBS]; /* cycle slip flag cache */
    halfc_t *halfc[MAXSAT][NFREQ+NEXOBS]; /* half-cycle ambiguity list */
    FILE   *fp;                 /* output file pointer */
} strfile_t;

/* global variables ----------------------------------------------------------*/
static const int navsys[]={     /* system codes */
    SYS_GPS,SYS_GLO,SYS_GAL,SYS_QZS,SYS_SBS,SYS_CMP,SYS_IRN,0
};
static const char vercode[][MAXCODE]={ /* supported obs-type by RINEX version */
  /* 0........1.........2.........3.........4.........5.........6........          */
  /* 11111111111112222222222555777666666688822663331155599991555677788444     CODE */
  /* CPWYMNSLEABXZCDSLXPWYMNIQXIQXABCXZSLIQXIQIQIQXIQABCABCXDDPZEDPZDPABX          */
    "00000000...0.0000000000000..........................................", /* GPS */
    "00...........0....0..........44.4..........222...................444", /* GLO */
    "0........0000..........0000000000...000.............................", /* GAL */
    "2.....22...22..222.....222......2422....................4444........", /* QZS */
    "0......................000..........................................", /* SBS */
    ".4...4...4.4.....1.......41114..1.....41111............444..44444...", /* BDS */
    ".........................3......................3333333............."  /* IRN */
};
/* convert RINEX obs-type ver.3 -> ver.2 -------------------------------------*/
static void convcode(int rnxver, int sys, char *type)
{
    if (rnxver>=212&&(sys==SYS_GPS||sys==SYS_QZS||sys==SYS_SBS)&&
        !strcmp(type+1,"1C")) { /* L1C/A */
        strcpy(type+1,"A");
    }
    else if (rnxver>=212&&(sys==SYS_GPS||sys==SYS_QZS)&&
             (!strcmp(type+1,"1S")||!strcmp(type+1,"1L")||
              !strcmp(type+1,"1X"))) { /* L1C */
        strcpy(type+1,"B");
    }
    else if (rnxver>=212&&(sys==SYS_GPS||sys==SYS_QZS)&&
             (!strcmp(type+1,"2S")||!strcmp(type+1,"2L")||
              !strcmp(type+1,"2X"))) { /* L2C */
        strcpy(type+1,"C");
    }
    else if (rnxver>=212&&sys==SYS_GLO&&!strcmp(type+1,"1C")) { /* L1C/A */
        strcpy(type+1,"A");
    }
    else if (rnxver>=212&&sys==SYS_GLO&&!strcmp(type+1,"2C")) { /* L2C/A */
        strcpy(type+1,"D");
    }
    else if (sys==SYS_CMP&&(!strcmp(type+1,"2I")||!strcmp(type+1,"2Q")||
             !strcmp(type+1,"2X"))) { /* B1_2 */
        strcpy(type+1,"2");
    }
    else if (!strcmp(type,"C1P")||!strcmp(type,"C1W")||!strcmp(type,"C1Y")||
             !strcmp(type,"C1N")) { /* L1P,P(Y) */
        strcpy(type,"P1");
    }
    else if (!strcmp(type,"C2P")||!strcmp(type,"C2W")||!strcmp(type,"C2Y")||
             !strcmp(type,"C2N")||!strcmp(type,"C2D")) { /* L2P,P(Y) */
        strcpy(type,"P2");
    }
    else {
        type[2]='\0';
    }
}
/* generate stream file ------------------------------------------------------*/
static strfile_t *gen_strfile(int format, const char *opt)
{
    strfile_t *str;
    gtime_t time0={0};
    int i,j;
    
    trace(3,"init_strfile:\n");
    
    if (!(str=(strfile_t *)calloc(sizeof(strfile_t),1))) return NULL;
    
    str->format=format;
    str->staid=-1;
    str->ephsat=str->ephset=0;
    str->time=str->tstart=time0;
    /* output rtcm */
    if (!init_rtcm(&str->out)) {
        showmsg("init rtcm error");
        return 0;
    }
    if (format==STRFMT_RTCM2||format==STRFMT_RTCM3) {
        if (!init_rtcm(&str->rtcm)) {
            showmsg("init rtcm error");
            return 0;
        }
        str->rtcm.time=time0;
        str->obs=&str->rtcm.obs;
        str->nav=&str->rtcm.nav; 
        str->sta=&str->rtcm.sta; 
        strcpy(str->rtcm.opt,opt);
    }
    else if (format<=MAXRCVFMT) {
        if (!init_raw(&str->raw,format)) {
            showmsg("init raw error");
            return 0;
        }
        str->raw.time=time0;
        str->obs=&str->raw.obs;
        str->nav=&str->raw.nav;
        str->sta=&str->raw.sta;
        strcpy(str->raw.opt,opt);
    }
    else if (format==STRFMT_RINEX) {
        if (!init_rnxctr(&str->rnx)) {
            showmsg("init rnx error");
            return 0;
        }
        str->rnx.time=time0;
        str->obs=&str->rnx.obs;
        str->nav=&str->rnx.nav;
        str->sta=&str->rnx.sta;
        strcpy(str->rnx.opt,opt);
    }
    else return 0;

    str->stas=NULL;
    for (i=0;i<MAXSAT;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
        str->slips[i][j]=0;
        str->halfc[i][j]=NULL;
    }
    str->fp=NULL;
    return str;
}
/* free stream file ----------------------------------------------------------*/
static void free_strfile(strfile_t *str)
{
    stas_t *sp,*sp_next;
    halfc_t *hp,*hp_next;
    int i,j;

    trace(3,"free_strfile:\n");
    /* output rtcm */
    free_rtcm(&str->out);

    if (str->format==STRFMT_RTCM2||str->format==STRFMT_RTCM3) {
        free_rtcm(&str->rtcm);
    }
    else if (str->format<=MAXRCVFMT) {
        free_raw(&str->raw);
    }
    else if (str->format==STRFMT_RINEX) {
        free_rnxctr(&str->rnx);
    }
    for (sp=str->stas;sp;sp=sp_next) {
        sp_next=sp->next;
        free(sp);
    }
    for (i=0;i<MAXSAT;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
        for (hp=str->halfc[i][j];hp;hp=hp_next) {
            hp_next=hp->next;
            free(hp);
        }
    }
    free(str);
}
/* input stream file ---------------------------------------------------------*/
static int input_strfile(strfile_t *str)
{
    int type=0;
    
    trace(4,"input_strfile:\n");
    
    if (str->format==STRFMT_RTCM2) {
        if ((type=input_rtcm2f(&str->rtcm,str->fp))>=1) {
            str->time=str->rtcm.time;
            str->ephsat=str->rtcm.ephsat;
            str->ephset=str->rtcm.ephset;
            str->staid=str->rtcm.staid;
        }
    }
    else if (str->format==STRFMT_RTCM3) {
        if ((type=input_rtcm3f(&str->rtcm,str->fp))>=1) {
            str->time=str->rtcm.time;
            str->ephsat=str->rtcm.ephsat;
            str->ephset=str->rtcm.ephset;
            str->staid=str->rtcm.staid;
        }
    }
    else if (str->format<=MAXRCVFMT) {
        if ((type=input_rawf(&str->raw,str->format,str->fp))>=1) {
            str->time=str->raw.time;
            str->ephsat=str->raw.ephsat;
            str->ephset=str->raw.ephset;
            str->staid=0;
        }
    }
    else if (str->format==STRFMT_RINEX) {
        if ((type=input_rnxctr(&str->rnx,str->fp))>=1) {
            str->time=str->rnx.time;
            str->ephsat=str->rnx.ephsat;
            str->ephset=str->rnx.ephset;
            str->staid=0;
        }
    }
    if (!str->tstart.time&&str->time.time) {
        str->tstart=str->time;
    }
    trace(4,"input_strfile: time=%s type=%d\n",time_str(str->time,3),type);
    return type;
}
/* open stream file ----------------------------------------------------------*/
static int open_strfile(strfile_t *str, const char *file)
{
    trace(3,"open_strfile: file=%s\n",file);
    
    if (str->format==STRFMT_RTCM2||str->format==STRFMT_RTCM3) {
        if (!(str->fp=fopen(file,"rb"))) {
            showmsg("rtcm open error: %s",file);
            return 0;
        }
        str->rtcm.time=str->time;
    }
    else if (str->format<=MAXRCVFMT) {
        if (!(str->fp=fopen(file,"rb"))) {
            showmsg("log open error: %s",file);
            return 0;
        }
        str->raw.time=str->time;
    }
    else if (str->format==STRFMT_RINEX) {
        if (!(str->fp=fopen(file,"r"))) {
            showmsg("rinex open error: %s",file);
            return 0;
        }
        /* open rinex control */
        if (!open_rnxctr(&str->rnx,str->fp)) {
            showmsg("no rinex file: %s",file);
            fclose(str->fp);
            return 0;
        }
        str->rnx.time=str->time;
    }
    return 1;
}
/* close stream file ---------------------------------------------------------*/
static void close_strfile(strfile_t *str)
{
    trace(3,"close_strfile:\n");
    
    if (str->format==STRFMT_RTCM2||str->format==STRFMT_RTCM3) {
        if (str->fp) fclose(str->fp);
    }
    else if (str->format<=MAXRCVFMT) {
        if (str->fp) fclose(str->fp);
    }
    else if (str->format==STRFMT_RINEX) {
        if (str->fp) fclose(str->fp);
    }
}
/* set format and files in RINEX options comments ----------------------------*/
static void setopt_file(int format, char **paths, int n, const int *mask,
                        rnxopt_t *opt)
{
    int i,j;

    for (i=0;i<MAXCOMMENT;i++) {
        if (!*opt->comment[i]) break;
    }
    if (i<MAXCOMMENT) {
        sprintf(opt->comment[i++],"format: %.55s",formatstrs[format]);
    }
    for (j=0;j<n&&i<MAXCOMMENT;j++) {
        if (!mask[j]) continue;
        sprintf(opt->comment[i++],"log: %.58s",paths[j]);
    }
}
/* unset RINEX options comments ----------------------------------------------*/
static void unsetopt_file(rnxopt_t *opt)
{
    int i,brk=0;

    for (i=MAXCOMMENT-1;i>=0&&!brk;i--) {
        if (!*opt->comment[i]) continue;
        if (!strncmp(opt->comment[i],"format: ",8)) brk=1;
        *opt->comment[i]='\0';
    }
}
/* sort obs-types ------------------------------------------------------------*/
static void sort_obstype(uint8_t *codes, uint8_t *types, int n, int sys)
{
    uint8_t tmp;
    int i,j,idx1,idx2,pri1,pri2;
    
    for (i=0;i<n-1;i++) for (j=i+1;j<n;j++) {
       idx1=code2idx(navsys[sys],codes[i]);
       idx2=code2idx(navsys[sys],codes[j]);
       pri1=getcodepri(navsys[sys],codes[i],"");
       pri2=getcodepri(navsys[sys],codes[j],"");
       if (idx1<idx2||(idx1==idx2&&pri1>=pri2)) continue;
       tmp=codes[i]; codes[i]=codes[j]; codes[j]=tmp;
       tmp=types[i]; types[i]=types[j]; types[j]=tmp;
    }
}
/* set obs-types in RINEX options --------------------------------------------*/
static void setopt_obstype(const uint8_t *codes, const uint8_t *types, int sys,
                           rnxopt_t *opt)
{
    const char type_str[]="CLDS";
    char type[16],*id,ver;
    int i,j,k,idx;
    
    trace(3,"setopt_obstype: sys=%d\n",sys);
    
    opt->nobs[sys]=0;
    
    if (!(navsys[sys]&opt->navsys)) return;
    
    for (i=0;codes[i];i++) {
        if (!(id=code2obs(codes[i]))||(idx=code2idx(navsys[sys],codes[i]))<0) {
            continue;
        }
        if (!(opt->freqtype&(1<<idx))||opt->mask[sys][codes[i]-1]=='0') {
            continue;
        }
        if (opt->rnxver>=300) {
            ver=vercode[sys][codes[i]-1];
            if (ver<'0'||ver>'0'+opt->rnxver-300) {
                trace(2,"unsupported obs type: rnxver=%.2f sys=%d code=%s\n",
                      opt->rnxver/100.0,sys,code2obs(codes[i]));
                continue;
            }
        }
        for (j=0;j<4;j++) {
            if (!(opt->obstype&(1<<j))) continue;
            if (types&&!(types[i]&(1<<j))) continue;
            
            /* obs-types in ver.3 */
            sprintf(type,"%c%s",type_str[j],id);
            if (type[0]=='C'&&type[2]=='N') continue; /* codeless */
            
            if (opt->rnxver<=299) { /* ver.2 */
                
                /* ver.3 -> ver.2 */
                convcode(opt->rnxver,navsys[sys],type);
                
                /* check duplicated obs-type */
                for (k=0;k<opt->nobs[0];k++) {
                    if (!strcmp(opt->tobs[0][k],type)) break;
                }
                if (k>=opt->nobs[0]&&opt->nobs[0]<MAXOBSTYPE) {
                    strcpy(opt->tobs[0][opt->nobs[0]++],type);
                }
            }
            else if (opt->nobs[sys]<MAXOBSTYPE) { /* ver.3 */
                strcpy(opt->tobs[sys][opt->nobs[sys]++],type);
            }
        }
    }
}
/* set phase shift in RINEX options (RINEX 3.04 A23) -------------------------*/
static void setopt_phshift(rnxopt_t *opt)
{
    uint8_t code;
    int i,j;
    
    for (i=0;i<NSATSYS;i++) for (j=0;j<opt->nobs[i];j++) {
        if (opt->tobs[i][j][0]!='L') continue;
        code=obs2code(opt->tobs[i][j]+1);

        if (navsys[i]==SYS_GPS) {
            if (code==CODE_L1S||code==CODE_L1L||code==CODE_L1X||code==CODE_L1P||
                code==CODE_L1W||code==CODE_L1N) {
                opt->shift[i][j]=0.25; /* +1/4 cyc */
            }
            else if (code==CODE_L2C||code==CODE_L2S||code==CODE_L2L||
                     code==CODE_L2X||code==CODE_L5Q) {
                opt->shift[i][j]=-0.25; /* -1/4 cyc */
            }
        }
        else if (navsys[i]==SYS_GLO) {
            if (code==CODE_L1P||code==CODE_L2P||code==CODE_L3Q) {
                opt->shift[i][j]=0.25; /* +1/4 cyc */
            }
        }
        else if (navsys[i]==SYS_GAL) {
            if (code==CODE_L1C) {
                opt->shift[i][j]=0.5; /* +1/2 cyc */
            }
            else if (code==CODE_L5Q||code==CODE_L7Q||code==CODE_L8Q) {
                opt->shift[i][j]=-0.25; /* -1/4 cyc */
            }
            else if (code==CODE_L6C) {
                opt->shift[i][j]=-0.5; /* -1/2 cyc */
            }
        }
        else if (navsys[i]==SYS_QZS) {
            if (code==CODE_L1S||code==CODE_L1L||code==CODE_L1X) {
                opt->shift[i][j]=0.25; /* +1/4 cyc */
            }
            else if (code==CODE_L5Q||code==CODE_L5P) {
                opt->shift[i][j]=-0.25; /* -1/4 cyc */
            }
        }
        else if (navsys[i]==SYS_CMP) {
            if (code==CODE_L2P||code==CODE_L7Q||code==CODE_L6Q) {
                opt->shift[i][j]=-0.25; /* -1/4 cyc */
            }
            else if (code==CODE_L1P||code==CODE_L5P||code==CODE_L7P) {
                opt->shift[i][j]=0.25; /* +1/4 cyc */
            }
        }
    }
}
/* set station ID list to RINEX options comments -----------------------------*/
static void setopt_sta_list(const strfile_t *str, rnxopt_t *opt)
{
    const stas_t *p;
    char s1[32],s2[32];
    int i,n=0;

    for (p=str->stas;p;p=p->next) {
        n++;
    }
    if (n<=1) return;

    for (i=0;i<MAXCOMMENT;i++) {
        if (!*opt->comment[i]) break;
    }
    sprintf(opt->comment[i++],"%5s  %22s  %22s","STAID","TIME OF FIRST OBS",
            "TIME OF LAST OBS");
    
    for (p=str->stas,n--;p&&n>=0;p=p->next,n--) {
        if (i+n>=MAXCOMMENT) continue;
        time2str(p->ts,s1,2);
        time2str(p->te,s2,2);
        sprintf(opt->comment[i+n]," %04d  %s  %s",p->staid,s1,s2);
    }
}
/* set station info in RINEX options -----------------------------------------*/
static void setopt_sta(const strfile_t *str, rnxopt_t *opt)
{
    const stas_t *p;
    const sta_t *sta;
    double pos[3],enu[3];
    
    trace(3,"setopt_sta:\n");
    
    /* search first station in station list */
    for (p=str->stas;p;p=p->next) {
        if (!p->next) break;
        if (opt->ts.time&&timediff(p->next->te,opt->ts)<0.0) break;
    }
    if (p) {
        sta=&p->sta;
        setopt_sta_list(str,opt);
    }
    else {
        sta=str->sta;
    }
    /* marker name and number */
    if (!*opt->marker&&!*opt->markerno) {
        strcpy(opt->marker  ,sta->name  );
        strcpy(opt->markerno,sta->marker);
    }
    /* receiver and antenna info */
    if (!*opt->rec[0]&&!*opt->rec[1]&&!*opt->rec[2]) {
        strcpy(opt->rec[0],sta->recsno);
        strcpy(opt->rec[1],sta->rectype);
        strcpy(opt->rec[2],sta->recver);
    }
    if (!*opt->ant[0]&&!*opt->ant[1]&&!*opt->ant[2]) {
        strcpy(opt->ant[0],sta->antsno);
        strcpy(opt->ant[1],sta->antdes);
        if (sta->antsetup) {
            sprintf(opt->ant[2],"%d",sta->antsetup);
        }
        else *opt->ant[2]='\0';
    }
    /* antenna approx position */
    if (!opt->autopos&&norm(sta->pos,3)>0.0) {
        matcpy(opt->apppos,sta->pos,3,1);
    }
    /* antenna delta */
    if (norm(opt->antdel,3)>0.0) {
        ;
    }
    else if (norm(sta->del,3)>0.0) {
        if (!sta->deltype&&norm(sta->del,3)>0.0) { /* enu */
            opt->antdel[0]=sta->del[2]; /* h */
            opt->antdel[1]=sta->del[0]; /* e */
            opt->antdel[2]=sta->del[1]; /* n */
        }
        else if (norm(sta->pos,3)>0.0) { /* xyz */
            ecef2pos(sta->pos,pos);
            ecef2enu(pos,sta->del,enu);
            opt->antdel[0]=enu[2]; /* h */
            opt->antdel[1]=enu[0]; /* e */
            opt->antdel[2]=enu[1]; /* n */
        }
    }
    else {
        opt->antdel[0]=sta->hgt;
        opt->antdel[1]=0.0;
        opt->antdel[2]=0.0;
    }
}
/* update station list -------------------------------------------------------*/
static void update_stas(strfile_t *str)
{
    stas_t *p;
    
    if (!str->stas||str->stas->staid!=str->staid) { /* station ID changed */
        if (!(p=(stas_t *)calloc(sizeof(stas_t),1))) return;
        p->staid=str->staid;
        p->ts=p->te=str->time;
        p->next=str->stas;
        str->stas=p;
   }
   else {
        str->stas->te=str->time;
   }
}
/* update station info in station list ---------------------------------------*/
static void update_stainf(strfile_t *str)
{
    if (str->stas&&str->stas->staid==str->staid) {
        str->stas->sta=*str->sta;
    }
}
/* dump station list ---------------------------------------------------------*/
static void dump_stas(const strfile_t *str)
{
#if 1 /* for debug */
    stas_t *p;
    double pos[3];
    char s1[32],s2[32];

    trace(2,"# STATION LIST\n");
    trace(2,"# %17s %19s %5s %6s %16s %16s %12s %13s %9s %2s %6s %6s %6s\n",
          "TIME","STAID","MARKER","ANTENNA","RECEIVER","LATITUDE","LONGITUDE",
          "HIGHT","DT","DEL1","DEL2","DEL3");
    
    for (p=str->stas;p;p=p->next) {
        time2str(p->ts,s1,0);
        time2str(p->te,s2,0);
        ecef2pos(p->sta.pos,pos);
        trace(2,"%s %s  %04d %-6.6s %-16.16s %-16.16s %12.8f %13.8f %9.3f %2d "
              "%6.3f %6.3f %6.3f\n",s1,s2,p->staid,p->sta.name,p->sta.antdes,
              p->sta.rectype,pos[0]*R2D,pos[1]*R2D,pos[2],p->sta.deltype,
              p->sta.del[0],p->sta.del[1],p->sta.del[2]);
    }
#endif
}
/* add half-cycle ambiguity list ---------------------------------------------*/
static int add_halfc(strfile_t *str, int sat, int idx, gtime_t time)
{
    halfc_t *p;
    
    if (!(p=(halfc_t *)calloc(sizeof(halfc_t),1))) return 0;
    p->ts=p->te=time;
    p->stat=0;
    p->next=str->halfc[sat-1][idx];
    str->halfc[sat-1][idx]=p;
    return 1;
}
/* update half-cycle ambiguity -----------------------------------------------*/
static void update_halfc(strfile_t *str, obsd_t *obs)
{
    int i,sat=obs->sat;
    
    for (i=0;i<NFREQ+NEXOBS;i++) {
        if (obs->L[i]==0.0) continue;
        
        if (!str->halfc[sat-1][i]) {
            if (!add_halfc(str,sat,i,obs->time)) continue;
        }
        if (obs->LLI[i]&LLI_SLIP) {
            str->halfc[sat-1][i]->stat=0;
        }
        if (obs->LLI[i]&LLI_HALFC) { /* halfcyc unknown */
            if (str->halfc[sat-1][i]->stat==0) {
                str->halfc[sat-1][i]->ts=obs->time;
            }
            str->halfc[sat-1][i]->te=obs->time;
            str->halfc[sat-1][i]->stat=1; /* unresolved */
        }
        else if (str->halfc[sat-1][i]->stat==1) { /* halfcyc unknown -> known */
            if (obs->LLI[i]&LLI_HALFA) {
                str->halfc[sat-1][i]->stat=2; /* resolved with added */
            }
            else if (obs->LLI[i]&LLI_HALFS) {
                str->halfc[sat-1][i]->stat=3; /* resolved with subtracted */
            }
            else {
                str->halfc[sat-1][i]->stat=4; /* resolved with none */
            }
            if (!add_halfc(str,sat,i,obs->time)) continue;
        }
    }
}
/* dump half-cycle ambiguity list --------------------------------------------*/
static void dump_halfc(const strfile_t *str)
{
#if 0 /* for debug */
    halfc_t *p;
    char s0[32],s1[32],s2[32],*stats[]={"ADD","SUB","NON"};
    int i,j;
    
    trace(2,"# HALF-CYCLE AMBIGUITY CORRECTIONS\n");
    trace(2,"# %20s %22s %4s %3s %3s\n","START","END","SAT","FRQ","COR");
    
    for (i=0;i<MAXSAT;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
        for (p=str->halfc[i][j];p;p=p->next) {
            if (p->stat<=1) continue;
            satno2id(i+1,s0);
            time2str(p->ts,s1,2);
            time2str(p->te,s2,2);
            trace(2,"%s %s %4s %3d %3s\n",s1,s2,s0,j+1,stats[p->stat-2]);
        }
    }
#endif
}
/* resolve half-cycle ambiguity ----------------------------------------------*/
static void resolve_halfc(const strfile_t *str, obsd_t *data, int n)
{
    halfc_t *p;
    int i,j,sat;
    
    for (i=0;i<n;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
        sat=data[i].sat;
        
        for (p=str->halfc[sat-1][j];p;p=p->next) {
            if (p->stat<=1) continue;
            if (timediff(data[i].time,p->ts)<-DTTOL||
                timediff(data[i].time,p->te)> DTTOL) continue;
            
            if (p->stat==3) {
                data[i].L[j]+=0.5;
            }
            else if (p->stat==4) {
                data[i].L[j]-=0.5;
            }
            data[i].LLI[j]&=~LLI_HALFC;
        }
        data[i].LLI[j]&=~(LLI_HALFA|LLI_HALFS);
    }
}
/* scan input files ----------------------------------------------------------*/
static int scan_file(char **files, int nf, rnxopt_t *opt, strfile_t *str,
                     int *mask)
{
    eph_t  eph0 ={0,-1,-1};
    geph_t geph0={0,-1};
    seph_t seph0={0};
    uint8_t codes[NSATSYS][33]={{0}};
    uint8_t types[NSATSYS][33]={{0}};
    char msg[128];
    int i,j,k,l,m,c=0,type,sys,prn,abort=0,n[NSATSYS]={0};
    
    trace(3,"scan_file: nf=%d\n",nf);
    
    for (m=0;m<nf&&!abort;m++) {
        
        if (!open_strfile(str,files[m])) {
            continue;
        }
        while ((type=input_strfile(str))>=-1) {
            if (opt->ts.time&&timediff(str->time,opt->ts)<-opt->ttol) continue;
            if (opt->te.time&&timediff(str->time,opt->te)>-opt->ttol) break;
            mask[m]=1; /* update file mask */
            
            if (type==1) { /* observation data */
                for (i=0;i<str->obs->n;i++) {
                    sys=satsys(str->obs->data[i].sat,NULL);
                    if (!(sys&opt->navsys)) continue;
                    for (l=0;navsys[l];l++) if (navsys[l]==sys) break;
                    if (!navsys[l]) continue;
                    
                    /* update obs-types */
                    for (j=0;j<NFREQ+NEXOBS;j++) {
                        if (!str->obs->data[i].code[j]) continue;
                        
                        for (k=0;k<n[l];k++) {
                            if (codes[l][k]==str->obs->data[i].code[j]) break;
                        }
                        if (k>=n[l]&&n[l]<32) {
                            codes[l][n[l]++]=str->obs->data[i].code[j];
                        }
                        if (k<n[l]) {
                            if (str->obs->data[i].P[j]!=0.0) types[l][k]|=1;
                            if (str->obs->data[i].L[j]!=0.0) types[l][k]|=2;
                            if (str->obs->data[i].D[j]!=0.0) types[l][k]|=4;
                            if (str->obs->data[i].SNR[j]!=0) types[l][k]|=8;
                        }
                    }
                    /* update half-cycle ambiguity list */
                    if (opt->halfcyc) {
                        update_halfc(str,str->obs->data+i);
                    }
                }
                /* update station list */
                update_stas(str);
            }
            else if (type==5) { /* station info */
                /* update station info */
                update_stainf(str);
            }
            if (++c%11) continue;
            
            sprintf(msg,"scanning: %s %s%s%s%s%s%s%s",time_str(str->time,0),
                    n[0]?"G":"",n[1]?"R":"",n[2]?"E":"",n[3]?"J":"",
                    n[4]?"S":"",n[5]?"C":"",n[6]?"I":"");
            if ((abort=showmsg(msg))) break;
        }
        close_strfile(str);
    }
    showmsg("");
    
    if (abort) {
        trace(2,"aborted in scan\n");
        return 0;
    }
    for (i=0;i<NSATSYS;i++) for (j=0;j<n[i];j++) {
        trace(2,"scan_file: sys=%d code=%s type=%d\n",i,code2obs(codes[i][j]),
              types[i][j]);
    }
    /* sort and set obs-types in RINEX options */
    for (i=0;i<NSATSYS;i++) {
        sort_obstype(codes[i],types[i],n[i],i);
        setopt_obstype(codes[i],types[i],i,opt);
        
        for (j=0;j<n[i];j++) {
            trace(3,"scan_file: sys=%d code=%s\n",i,code2obs(codes[i][j]));
        }
    }
    /* set station info in RINEX options */
    setopt_sta(str,opt);
     
    /* set phase shifts in RINEX options */
    if (opt->phshift) {
        setopt_phshift(opt);
    }
    /* set GLONASS FCN and clear ephemeris */
    for (i=0;i<str->nav->n;i++) {
        str->nav->eph[i]=eph0;
    }
    for (i=0;i<str->nav->ng;i++) {
        if (satsys(str->nav->geph[i].sat,&prn)!=SYS_GLO) continue;
        str->nav->glo_fcn[prn-1]=str->nav->geph[i].frq+8;
        str->nav->geph[i]=geph0;
    }
    for (i=0;i<str->nav->ns;i++) {
        str->nav->seph[i]=seph0;
    }
    dump_stas(str);
    dump_halfc(str);
    return 1;
}
/* write RINEX header --------------------------------------------------------*/
static void write_header(FILE **ofp, int idx, const rnxopt_t *opt,
                         const nav_t *nav)
{
    switch (idx) {
        case 0: outrnxobsh (ofp[0],opt,nav); break;
        case 1: outrnxnavh (ofp[1],opt,nav); break;
        case 2: outrnxgnavh(ofp[2],opt,nav); break;
        case 3: outrnxhnavh(ofp[3],opt,nav); break;
        case 4: outrnxqnavh(ofp[4],opt,nav); break;
        case 5: outrnxlnavh(ofp[5],opt,nav); break;
        case 6: outrnxcnavh(ofp[6],opt,nav); break;
        case 7: outrnxinavh(ofp[7],opt,nav); break;
    }
}
/* open output files ---------------------------------------------------------*/
static int openfile(FILE **ofp, char *files[], const char *file,
                    const rnxopt_t *opt, const nav_t *nav)
{
    char path[1024];
    int i;
    
    trace(3,"openfile:\n");
    
    for (i=0;i<NOUTFILE;i++) {
        
        if (!*files[i]) continue;
        
        strcpy(path,files[i]);
        
        /* check overwrite input file and modify output file */
        if (!strcmp(path,file)) strcat(path,"_");
        
        /* create directory if not exist */
        createdir(path);
        
        if (!(ofp[i]=fopen(path,(i+1)==NOUTFILE?"wb":"w"))) {
            showmsg("file open error: %s",path);
            for (i--;i>=0;i--) if (ofp[i]) fclose(ofp[i]);
            return 0;
        }
        /* write RINEX header */
        write_header(ofp,i,opt,nav);
    }
    return 1;
}
/* close output files --------------------------------------------------------*/
static void closefile(FILE **ofp, const rnxopt_t *opt, nav_t *nav)
{
    int i;
    
    trace(3,"closefile:\n");
    
    for (i=0;i<NOUTFILE;i++) {
        
        if (!ofp[i]) continue;
        
        /* rewrite RINEX header */
        rewind(ofp[i]);
        write_header(ofp,i,opt,nav);
        
        fclose(ofp[i]);
    }
}
/* output RINEX event --------------------------------------------------------*/
static void outrnxevent(FILE *fp, const rnxopt_t *opt, gtime_t time, int event,
                        const stas_t *stas, int staid)
{
    const stas_t *p=NULL,*q;
    double ep[6],pos[3],enu[3],del[3];

    trace(3,"outrnxevent: event=%d\n",event);
    
    if (event==EVENT_STARTMOVE) {
        fprintf(fp,"%*s%d%3d\n",(opt->rnxver>=300)?31:28,"",event,2);
        fprintf(fp,"%-60s%-20s\n","EVENT: START MOVING ANTENNA","COMMENT");
        fprintf(fp,"%-60s%-20s\n",opt->marker,"MARKER NAME");
    }
    else if (event==EVENT_NEWSITE) {
        for (q=stas;q;q=q->next) {
            if (q->staid==staid&&timediff(time,q->te)<=0.0) p=q;
        }
        fprintf(fp,"%*s%d%3d\n",(opt->rnxver>=300)?31:28,"",event,6);
        fprintf(fp,"%-60s%-20s\n","EVENT: NEW SITE OCCUPATION","COMMENT");
        if (!p) {
            fprintf(fp,"%04d%56s%-20s\n",staid,"","MARKER NAME");
            return;
        }
        fprintf(fp,"%-60s%-20s\n",p->sta.name,"MARKER NAME");
        fprintf(fp,"%-20.20s%-20.20s%-20.20s%-20s\n",p->sta.recsno,
                p->sta.rectype,p->sta.recver,"REC # / TYPE / VERS");
        fprintf(fp,"%-20.20s%-20.20s%-20.20s%-20s\n",p->sta.antsno,
                p->sta.antdes,"","ANT # / TYPE");
        fprintf(fp,"%14.4f%14.4f%14.4f%-18s%-20s\n",p->sta.pos[0],
                p->sta.pos[1],p->sta.pos[2],"","APPROX POSITION XYZ");
        
        /* antenna delta */
        if (norm(p->sta.del,3)>0.0) {
            if (!p->sta.deltype&&norm(p->sta.del,3)>0.0) { /* enu */
                del[0]=p->sta.del[2]; /* h */
                del[1]=p->sta.del[0]; /* e */
                del[2]=p->sta.del[1]; /* n */
            }
            else if (norm(p->sta.pos,3)>0.0) { /* xyz */
                ecef2pos(p->sta.pos,pos);
                ecef2enu(pos,p->sta.del,enu);
                del[0]=enu[2]; /* h */
                del[1]=enu[0]; /* e */
                del[2]=enu[1]; /* n */
            }
        }
        else {
            del[0]=p->sta.hgt;
            del[1]=del[2]=0.0;
        }
        fprintf(fp,"%14.4f%14.4f%14.4f%-18s%-20s\n",del[0],del[1],del[2],"",
                "ANTENNA: DELTA H/E/N");
    }
    else if (event==EVENT_EXTERNAL) {
        time2epoch(time,ep);
        fprintf(fp,"%s %02d %02.0f %02.0f %02.0f %02.0f %010.7f  %d%3d\n",
                (opt->rnxver>=300)?">":"",
                (opt->rnxver>=300)?(int)ep[0]:(int)ep[0]%100,
                ep[1],ep[2],ep[3],ep[4],ep[5],event,1);
        fprintf(fp,"%-60s%-20s\n","EXTERNAL EVENT","COMMENT");
    }
}
/* save cycle slips ----------------------------------------------------------*/
static void save_slips(strfile_t *str, obsd_t *data, int n)
{
    int i,j;
    
    for (i=0;i<n;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
        if (data[i].LLI[j]&LLI_SLIP) str->slips[data[i].sat-1][j]=1;
    }
}
/* restore cycle slips -------------------------------------------------------*/
static void rest_slips(strfile_t *str, obsd_t *data, int n)
{
    int i,j;
    
    for (i=0;i<n;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
        if (data[i].L[j]!=0.0&&str->slips[data[i].sat-1][j]) {
            data[i].LLI[j]|=LLI_SLIP;
            str->slips[data[i].sat-1][j]=0;
        }
    }
}
/* screen time with time tolerance -------------------------------------------*/
static int screent_ttol(gtime_t time, gtime_t ts, gtime_t te, double tint,
                        double ttol)
{
    if (ttol<=0.0) ttol=DTTOL;
    
    return (tint<=0.0||fmod(time2gpst(time,NULL)+ttol,tint)<=ttol*2.0)&&
           (ts.time==0||timediff(time,ts)>=-ttol)&&
           (te.time==0||timediff(time,te)<  ttol);
}

/* used for code smooth */
typedef struct
{
    uint8_t n;
    obsd_t data[MAXOBS];
    uint16_t numofL1[MAXOBS];
    uint16_t numofL2[MAXOBS];
    uint16_t numofL5[MAXOBS];
}epoch_t;

static epoch_t gEpoch = { 0 };

static char sys2char(int sys)
{
    if (sys == SYS_GPS) return 'G';
    else if (sys == SYS_GLO) return 'R';
    else if (sys == SYS_GAL) return 'E';
    else if (sys == SYS_CMP) return 'C';
    else if (sys == SYS_QZS) return 'J';
    else if (sys == SYS_SBS) return 'S';
    else return ' ';
}

#ifndef WAVE_GPS_L1
#define WAVE_GPS_L1 (CLIGHT/FREQ1)
#endif

static int smooth_obs_data(obs_t *obs, nav_t *nav)
{
	int prn = 0, wk = 0;
    uint8_t i = 0, j = 0, f = 0, sys = 0, idx = 0, isCS_L1 = 0, isCS_L2 = 0, isCS_L5 = 0, ms_jump_int = 0;
    epoch_t* epoch = &gEpoch;
    obsd_t* obst = obs->data + i;
    obsd_t* obsf = epoch->data + j;
    double dt = 0, ws = 0, L1_cur = 0, L1_pre = 0, L2_cur = 0, L2_pre = 0, L5_cur = 0, L5_pre = 0, waveL1 = 0, waveL2 = 0, waveL5 = 0, frq = 0, wave = 0;
	double ion_diff = 0, dL1 = 0, dL2 = 0, dL5 = 0;
	double f1_fL1 = 0, f2_fL1 = 0, f5_fL1 = 0, f_fL1 = 0;
	double beta1 = 0, beta2 = 0, beta5 = 0, beta = 0;
	double ion_cur = 0, ion_pre = 0, dion = 0;
	double predictP = 0, alpha = 0, ms_jump = 0;
    if (obs->n == 0) return 0;
    for (j = 0, obsf = epoch->data + j; j < epoch->n; ++j, ++obsf)
    {
        if (obsf->sat > 0)
        {
            dt = timediff(obst->time, obsf->time);
            if (fabs(dt) > 60.0)
            {
                memset(obsf, 0, sizeof(obsd_t));
            }
        }
    }
    for (; i < obs->n; ++i, ++obst)
    {
        for (j = 0, obsf = epoch->data + j; j < epoch->n; ++j, ++obsf)
        {
            if (obsf->sat == obst->sat) break;
        }
        if (j == epoch->n)
        {
            /* new data */
            for (j = 0, obsf = epoch->data + j; j < MAXOBS; ++j, ++obsf)
            {
                if (obsf->sat == 0) break;
            }
            if (j < MAXOBS)
            {
                epoch->data[j] = *obst;
                memset(epoch->numofL1, 0, sizeof(epoch->numofL1));
                memset(epoch->numofL2, 0, sizeof(epoch->numofL2));
                memset(epoch->numofL5, 0, sizeof(epoch->numofL5));
                if (j < epoch->n)
                {
                    f = 0;
                }
                else
                {
                    ++epoch->n;
                }
            }
            else
            {
                f = 0;
            }
        }
        else
        {
            prn = 0;
            sys = satsys(obst->sat, &prn);
            wk = 0;
            ws = time2gpst(obst->time, &wk);
            L1_cur = 0;
            L2_cur = 0;
            L5_cur = 0;
            L1_pre = 0;
            L2_pre = 0;
            L5_pre = 0;
            waveL1 = 0;
            waveL2 = 0;
            waveL5 = 0;
            for (f = 0; f < (NFREQ + NEXOBS); ++f)
            {
                if (obst->code[f] == 0) continue;
                prn = 0;
                idx = code2idx(sys, obst->code[f]);
                frq = sat2freq(obst->sat, obst->code[f], nav);
                if (frq < 0.01) continue;

                if (idx == 0)
                {
                    L1_cur = obst->L[f];
                    L1_pre = obsf->L[f];
                    waveL1 = CLIGHT / frq;
                }
                else if (idx == 1)
                {
                    L2_cur = obst->L[f];
                    L2_pre = obsf->L[f];
                    waveL2 = CLIGHT / frq;
                }
                else if (idx == 2)
                {
                    L5_cur = obst->L[f];
                    L5_pre = obsf->L[f];
                    waveL5 = CLIGHT / frq;
                }
            }
            isCS_L1 = 1;
            isCS_L2 = 1;
            isCS_L5 = 1;
            ion_diff = 0;
            dL1 = 0;
            dL2 = 0;
            dL5 = 0;
            f1_fL1 = waveL1 / WAVE_GPS_L1;
            f2_fL1 = waveL2 / WAVE_GPS_L1;
            f5_fL1 = waveL5 / WAVE_GPS_L1;
            beta1 = f1_fL1 * f1_fL1;
            beta2 = f2_fL1 * f2_fL1;
            beta5 = f5_fL1 * f5_fL1;
            if (waveL1 > 0.01 && waveL2 > 0.01 && fabs(L1_cur) > 0.001 && fabs(L1_pre) > 0.001 && fabs(L2_cur) > 0.001 && fabs(L2_pre) > 0.001)
            {
                /* L1 + L2 */
                ion_cur = waveL1 * L1_cur - waveL2 * L2_cur;
                ion_pre = waveL1 * L1_pre - waveL2 * L2_pre;
                dion = (ion_cur - ion_pre) / (beta1 - beta2);
                /*printf("%10.3f,%c%2i,%3i,%10.4f,L1+L2\n", ws, sys2char(sys), prn, obst->sat, dion);*/
                if (fabs(dion) > 0.05)
                {
                    isCS_L1 = 1;
                    isCS_L2 = 1;
                }
                else
                {
                    ion_diff = dion;
                    dL1 = waveL1 * L1_cur - waveL1 * L1_pre;
                    dL2 = waveL2 * L2_cur - waveL2 * L2_pre;
                    isCS_L1 = 0;
                    isCS_L2 = 0;
                }
            }
            if (waveL1 > 0.01 && waveL5 > 0.01 && fabs(L1_cur) > 0.001 && fabs(L1_pre) > 0.001 && fabs(L5_cur) > 0.001 && fabs(L5_pre) > 0.001)
            {
                /* L1 + L5 */
                ion_cur = waveL1 * L1_cur - waveL5 * L5_cur;
                ion_pre = waveL1 * L1_pre - waveL5 * L5_pre;
                dion = (ion_cur - ion_pre) / (beta1 - beta5);
                /*printf("%10.3f,%c%2i,%3i,%10.4f,%10.4f,%10.4f,L1+L5\n", ws, sys2char(sys), prn, obst->sat, dion, ion_cur, ion_pre);*/
                if (fabs(dion) > 0.05)
                {
                    isCS_L1 = 1;
                    isCS_L5 = 1;
                }
                else
                {
                    ion_diff = dion;
                    dL1 = waveL1 * L1_cur - waveL1 * L1_pre;
                    dL5 = waveL5 * L5_cur - waveL5 * L5_pre;
                    isCS_L1 = 0;
                    isCS_L5 = 0;
                }
            }
            if (isCS_L1)
            {
                epoch->numofL1[j] = 0;
            }
            else
            {
                if (epoch->numofL1[j] == 0)
                    epoch->numofL1[j] = 2;
                else
                    ++epoch->numofL1[j];
            }
            if (isCS_L2)
            {
                epoch->numofL2[j] = 0;
            }
            else
            {
                if (epoch->numofL2[j] == 0)
                    epoch->numofL2[j] = 2;
                else
                    ++epoch->numofL2[j];
            }
            if (isCS_L5)
            {
                epoch->numofL5[j] = 0;
            }
            else
            {
                if (epoch->numofL5[j] == 0)
                    epoch->numofL5[j] = 2;
                else
                    ++epoch->numofL5[j];
            }
            if (!isCS_L1 || !isCS_L2 || !isCS_L5)
            {
                /* code smooth */
                for (f = 0; f < (NFREQ + NEXOBS); ++f)
                {
                    if (obst->code[f] == 0) continue;
                    prn = 0;
                    idx = code2idx(sys, obst->code[f]);
                    frq = sat2freq(obst->sat, obst->code[f], nav);
                    if (frq < 0.01) continue;
                    wave = CLIGHT / frq;
                    f_fL1 = wave / WAVE_GPS_L1;
                    beta = f_fL1 * f_fL1;
                    if (fabs(obst->P[f]) > 0.001 && fabs(obsf->P[f]) > 0.001)
                    {
                        predictP = obsf->P[f];
                        alpha = 0;
                        if (!isCS_L5)
                        {
                            predictP += dL5 + beta5 * ion_diff + beta * ion_diff;
                            alpha = 1.0 / epoch->numofL5[j];
                        }
                        else if (!isCS_L2)
                        {
                            predictP += dL2 + beta2 * ion_diff + beta * ion_diff;
                            alpha = 1.0 / epoch->numofL2[j];
                        }
                        else if (!isCS_L1)
                        {
                            predictP += dL1 + beta1 * ion_diff + beta * ion_diff;
                            alpha = 1.0 / epoch->numofL1[j];
                        }
                        else
                            continue;
                        ms_jump = (predictP - obst->P[f])*1000.0/CLIGHT;
                        ms_jump_int = (int)floor(ms_jump + 0.5);
                        if (ms_jump_int != 0)
                        {
                            predictP -= ms_jump_int * CLIGHT / 1000.0;
                        }
                        obst->P[f] = obst->P[f] * alpha + (1.0 - alpha) * predictP;
                    }
                }
            }
            *obsf = *obst;
            /*printf("%10.3f,%c%2i,%3i,%i,%i\n", ws, sys2char(sys), prn, obst->sat, epoch->numofL1[j], epoch->numofL5[j]);*/

        }
    }
    return epoch->n;
}
/* write rtcm3 msm to stream -------------------------------------------------*/
static void write_rtcm3_msm(rtcm_t *out, int msg, int sync, FILE *fOUT)
{
    obsd_t *data,buff[MAXOBS];
    int i,j,n,ns,sys,nobs,code,nsat=0,nsig=0,nmsg,mask[MAXCODE]={0};
    
    if      (1071<=msg&&msg<=1077) sys=SYS_GPS;
    else if (1081<=msg&&msg<=1087) sys=SYS_GLO;
    else if (1091<=msg&&msg<=1097) sys=SYS_GAL;
    else if (1101<=msg&&msg<=1107) sys=SYS_SBS;
    else if (1111<=msg&&msg<=1117) sys=SYS_QZS;
    else if (1121<=msg&&msg<=1127) sys=SYS_CMP;
    else return;
    
    data=out->obs.data;
    nobs=out->obs.n;
    
    /* count number of satellites and signals */
    for (i=0;i<nobs&&i<MAXOBS;i++) {
        if (satsys(data[i].sat,NULL)!=sys) continue;
        nsat++;
        for (j=0;j<NFREQ+NEXOBS;j++) {
            if (!(code=data[i].code[j])||mask[code-1]) continue;
            mask[code-1]=1;
            nsig++;
        }
    }
    if (nsig<=0||nsig>64) return;
    
    /* pack data to multiple messages if nsat x nsig > 64 */
    ns=64/nsig;         /* max number of sats in a message */
    nmsg=(nsat-1)/ns+1; /* number of messages */
    
    out->obs.data=buff;
    
    for (i=j=0;i<nmsg;i++) {
        for (n=0;n<ns&&j<nobs&&j<MAXOBS;j++) {
            if (satsys(data[j].sat,NULL)!=sys) continue;
            out->obs.data[n++]=data[j];
        }
        out->obs.n=n;
        
        if (gen_rtcm3(out,msg,0,i<nmsg-1?1:sync)&& out->nbyte > 0 && fOUT != NULL) {
            fwrite(out->buff, out->nbyte, sizeof(uint8_t), fOUT);
        }
    }
    out->obs.data=data;
    out->obs.n=nobs;
}
/* convert observation data --------------------------------------------------*/
static void convobs(FILE **ofp, rnxopt_t *opt, strfile_t *str, int *n,
                    gtime_t *tend, int *staid)
{
    gtime_t time;
    int i,j;
    int ng = 0, nr = 0, ne = 0, ns = 0, nj = 0, nc = 0, wk = 0;
    obsd_t* obsd = str->obs->data + 0;
    int sys = 0, prn = 0;

    trace(3,"convobs :\n");
    
    if (!ofp[0]||str->obs->n<=0) return;
    
    time=str->obs->data[0].time;
    
    /* avoid duplicated data by multiple files handover */
    if (tend->time&&timediff(time,*tend)<opt->ttol) return;
    *tend=time;
    /* code smooth */
    if (opt->csmooth)
    {
        smooth_obs_data(str->obs, str->nav);
    }
#if 0
    double *rs=mat(6,str->obs->n),*dts=mat(2,str->obs->n),*var=mat(1,str->obs->n),*azel_=zeros(2,str->obs->n),*resp=mat(1,str->obs->n);
    int svh[MAXOBS]={0};
    /* satellite positons, velocities and clocks */
    satposs(time,str->obs->data,str->obs->n,str->nav,EPHOPT_BRDC,rs,dts,var,svh);
#ifdef _WIN32
    static FILE* fOBS = NULL;
    if (!fOBS&&str->obs->n>0)
    {
        char buffer[255] = { 0 };
        double ep[6] = { 0 };
        time2epoch(time, ep);
        sprintf(buffer, "%04i-%02i-%02i-%02i-%02i-%02i-obs.csv", (int)ep[0], (int)ep[1], (int)ep[2], (int)ep[3], (int)ep[4], (int)ep[5]);
        fOBS = fopen(buffer, "w");
    }
    if (fOBS)
    {
        for (i = 0; i < str->obs->n; ++i)
        {
            const obsd_t* obsd = str->obs->data + i;
            int wk = 0;
            double ws = time2gpst(obsd->time, &wk);
            int prn = 0;
            int sys = satsys(obsd->sat, &prn);
            double* cur_rs = rs + i * 6;
            double* cur_dts = dts + i * 2;
            for (int f = 0; f < (NFREQ + NEXOBS); ++f)
            {
                if (obsd->code[f] == 0) continue;
                double frq = sat2freq(obsd->sat, obsd->code[f], str->nav);
                fprintf(fOBS, "%i,%12.4f,%3i,%3i,%3i,%3i,%14.4f,%14.4f,%10.4f,%7.2f,%14.4f,%14.4f,%14.4f,%10.4f,%10.4f,%10.4f,%14.4f,%10.4f,%f,%i\n"
                    , obsd->rcv
                    , ws /* 1 */
                    , obsd->sat, sys, prn, obsd->code[f] /* 2,3,4,5 */
                    , obsd->P[f], obsd->L[f], obsd->D[f], obsd->SNR[f] * SNR_UNIT /* 6,7,8,9 */
                    , cur_rs[0], cur_rs[1], cur_rs[2], cur_rs[3], cur_rs[4], cur_rs[5] /* 10,11,12,13,14,15 */
                    , cur_dts[0] * CLIGHT, cur_dts[1] * CLIGHT
                    , frq
					, code2idx(sys,obsd->code[f])
                );
            }
        }
        fflush(fOBS);
    }
#endif    

    free(rs); free(dts); free(var); free(azel_); free(resp);
#endif
    /* save cycle slips */
    save_slips(str,str->obs->data,str->obs->n);
    
    if (!screent_ttol(time,opt->ts,opt->te,opt->tint,opt->ttol)) return;
    
    /* restore cycle slips */
    rest_slips(str,str->obs->data,str->obs->n);
    
    if (str->staid!=*staid) { /* station ID changed */
        
        if (*staid>=0) { /* output RINEX event */
            outrnxevent(ofp[0],opt,str->time,EVENT_NEWSITE,str->stas,str->staid);
        }
        *staid=str->staid;

        /* set cycle slips */
        for (i=0;i<str->obs->n;i++) for (j=0;j<NFREQ+NEXOBS;j++) {
            if (str->obs->data[i].L[j]!=0.0) {
                str->obs->data[i].LLI[j]|=LLI_SLIP;
            }
        }
    }
    /* resolve half-cycle ambiguity */
    if (opt->halfcyc) {
        resolve_halfc(str,str->obs->data,str->obs->n);
    }
    /* output RINEX observation data */
    outrnxobsb(ofp[0],opt,str->obs->data,str->obs->n,0);
    
    /* output rtcm */
    if (opt->outmsm > 0 && opt->outmsm <= 7)
    {
        str->out.obs.n = 0;
        str->out.time = str->out.time_s = str->time;
        memset(str->out.buff, 0, sizeof(str->out.buff));
        str->out.nbyte = str->out.len = str->out.nbit = 0;
        for (i = 0, obsd = str->obs->data + i; i < str->obs->n; ++i, ++obsd)
        {
            sys = satsys(obsd->sat, &prn);
            if (sys == SYS_GPS)
                ++ng;
            else if (sys == SYS_GLO)
                ++nr;
            else if (sys == SYS_GAL)
                ++ne;
            else if (sys == SYS_SBS)
                ++ns;
            else if (sys == SYS_QZS)
                ++nj;
            else if (sys == SYS_CMP)
                ++nc;
            else
                continue;
            str->out.obs.data[str->out.obs.n++] = *obsd;
        }
        if (ng > 0) write_rtcm3_msm(&str->out, 1070 + opt->outmsm, (nr + ne + ns + nj + nc) > 0, ofp[9]); /* GPS */
        if (nr > 0) write_rtcm3_msm(&str->out, 1080 + opt->outmsm, (ne + ns + nj + nc) > 0, ofp[9]);/* GLO */
        if (ne > 0) write_rtcm3_msm(&str->out, 1090 + opt->outmsm, (ns + nj + nc) > 0, ofp[9]); /* GAL */
        if (ns > 0) write_rtcm3_msm(&str->out, 1100 + opt->outmsm, (nj + nc) > 0, ofp[9]); /* SBAS */
        if (nj > 0) write_rtcm3_msm(&str->out, 1110 + opt->outmsm, nc > 0, ofp[9]); /* QZS */
        if (nc > 0) write_rtcm3_msm(&str->out, 1120 + opt->outmsm, 0, ofp[9]); /* CMP */

        if (ng > 0 || nr > 0 || ne > 0 || ns > 0 || nj > 0 || nc > 0) n[9]++;
    }

    if (opt->tstart.time==0) opt->tstart=time;
    opt->tend=time;
    
    n[0]++;
}
/* convert navigattion data --------------------------------------------------*/
static void convnav(FILE **ofp, rnxopt_t *opt, strfile_t *str, int *n)
{
    gtime_t ts;
    double dtoe;
    int sat,set,sys,prn,sep_nav=(opt->rnxver<=299||opt->sep_nav);
    
    trace(3,"convnav :\n");
    
    sat=str->ephsat;
    set=str->ephset;
    sys=satsys(sat,&prn);

    /* output rtcm */
    if (opt->outmsm > 0)
    {
        memset(str->out.buff, 0, sizeof(str->out.buff));
        str->out.nbyte = str->out.len = str->out.nbit = 0;
        str->out.ephsat = str->ephsat;
        str->out.ephset = str->ephset;
        if (sys == SYS_GPS)
        {
            str->out.nav.eph[str->out.ephsat - 1] = str->nav->eph[str->ephsat - 1];
            gen_rtcm3(&str->out, 1019, 0, 0);
            if (str->out.nbyte > 0 && ofp[9] != NULL)
            {
                fwrite(str->out.buff, str->out.nbyte, sizeof(char), ofp[9]);
                n[9]++;
            }
        }
        else if (sys == SYS_GLO)
        {
            str->out.nav.geph[prn - 1] = str->nav->geph[prn - 1];
            gen_rtcm3(&str->out, 1020, 0, 0);
            if (str->out.nbyte > 0 && ofp[9] != NULL)
            {
                fwrite(str->out.buff, str->out.nbyte, sizeof(char), ofp[9]);
                n[9]++;
            }
        }
        else if (sys == SYS_GAL)
        {
            if (str->ephset)
            {
                str->out.nav.eph[str->out.ephsat - 1 + MAXSAT] = str->nav->eph[str->ephsat - 1 + MAXSAT];
                gen_rtcm3(&str->out, 1045, 0, 0);
            }
            else
            {
                str->out.nav.eph[str->out.ephsat - 1] = str->nav->eph[str->ephsat - 1];
                gen_rtcm3(&str->out, 1046, 0, 0);
            }
            if (str->out.nbyte > 0 && ofp[9] != NULL)
            {
                fwrite(str->out.buff, str->out.nbyte, sizeof(char), ofp[9]);
                n[9]++;
            }
        }
        else if (sys == SYS_SBS)
        {
            sys = sys;
        }
        else if (sys == SYS_QZS)
        {
            str->out.nav.eph[str->out.ephsat - 1] = str->nav->eph[str->ephsat - 1];
            gen_rtcm3(&str->out, 1044, 0, 0);
            if (str->out.nbyte > 0 && ofp[9] != NULL)
            {
                fwrite(str->out.buff, str->out.nbyte, sizeof(char), ofp[9]);
                n[9]++;
            }
        }
        else if (sys == SYS_CMP)
        {
            str->out.nav.eph[str->out.ephsat - 1] = str->nav->eph[str->ephsat - 1];
            gen_rtcm3(&str->out, 1042, 0, 0);
            if (str->out.nbyte > 0 && ofp[9] != NULL)
            {
                fwrite(str->out.buff, str->out.nbyte, sizeof(char), ofp[9]);
                n[9]++;
            }
        }
    }

    if (!(sys&opt->navsys)||opt->exsats[sat-1]) return;
    
    switch (sys) {
        case SYS_GLO: dtoe=MAXDTOE_GLO; break;
        case SYS_GAL: dtoe=MAXDTOE_GAL; break;
        case SYS_QZS: dtoe=MAXDTOE_QZS; break;
        case SYS_CMP: dtoe=MAXDTOE_CMP; break;
        case SYS_IRN: dtoe=MAXDTOE_IRN; break;
        case SYS_SBS: dtoe=MAXDTOE_SBS; break;
        default     : dtoe=MAXDTOE    ; break;
    }
    ts=opt->ts;
    if (ts.time!=0) ts=timeadd(ts,-dtoe);
    if (!screent(str->time,ts,opt->te,0.0)) return;
    
    if (sys==SYS_GPS) {
        if (ofp[1]) {
            outrnxnavb(ofp[1],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[1]++;
        }
    }
    else if (sys==SYS_GLO) {
        if (ofp[1]&&!sep_nav) {
            outrnxgnavb(ofp[1],opt,str->nav->geph+prn-1);
            n[1]++;
        }
        else if (ofp[2]&&sep_nav) {
            outrnxgnavb(ofp[2],opt,str->nav->geph+prn-1);
            n[2]++;
        }
    }
    else if (sys==SYS_SBS) {
        if (ofp[1]&&!sep_nav) {
            outrnxhnavb(ofp[1],opt,str->nav->seph+prn-MINPRNSBS);
            n[1]++;
        }
        else if (ofp[3]&&sep_nav) {
            outrnxhnavb(ofp[3],opt,str->nav->seph+prn-MINPRNSBS);
            n[3]++;
        }
    }
    else if (sys==SYS_QZS) {
        if (ofp[1]&&!sep_nav) {
            outrnxnavb(ofp[1],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[1]++;
        }
        else if (ofp[4]&&sep_nav) {
            outrnxnavb(ofp[4],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[4]++;
        }
    }
    else if (sys==SYS_GAL) {
        if (ofp[1]&&!sep_nav) {
            outrnxnavb(ofp[1],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[1]++;
        }
        else if (ofp[5]&&sep_nav) {
            outrnxnavb(ofp[5],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[5]++;
        }
    }
    else if (sys==SYS_CMP) {
        if (ofp[1]&&!sep_nav) {
            outrnxnavb(ofp[1],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[1]++;
        }
        else if (ofp[6]&&sep_nav) {
            outrnxnavb(ofp[6],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[6]++;
        }
    }
    else if (sys==SYS_IRN) {
        if (ofp[1]&&!sep_nav) {
            outrnxnavb(ofp[1],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[1]++;
        }
        else if (ofp[7]&&sep_nav) {
            outrnxnavb(ofp[7],opt,str->nav->eph+sat-1+MAXSAT*set);
            n[7]++;
        }
    }
}
/* convert SBAS message ------------------------------------------------------*/
static void convsbs(FILE **ofp, rnxopt_t *opt, strfile_t *str, int *n,
                    gtime_t *tend)
{
    gtime_t time;
    int prn,sat,sys,sep_nav=opt->rnxver<=299||opt->sep_nav;
    
    trace(3,"convsbs :\n");
    
    time=gpst2time(str->raw.sbsmsg.week,str->raw.sbsmsg.tow);

    if (!screent(time,opt->ts,opt->te,0.0)) return;
    
    /* avoid duplicated data by multiple files handover */
    if (tend->time&&timediff(time,*tend)<opt->ttol) return;
    *tend=time;

    prn=str->raw.sbsmsg.prn;
    if (MINPRNSBS<=prn&&prn<=MAXPRNSBS) {
        sys=SYS_SBS;
    }
    else if (MINPRNQZS_S<=prn&&prn<=MAXPRNQZS_S) {
        sys=SYS_QZS;
        prn+=10;
    }
    else {
        trace(2,"sbas message satellite error: prn=%d\n",prn);
        return;
    }
    if (!(sat=satno(sys,prn))||opt->exsats[sat-1]==1) return;
    
    /* output SBAS message log */
    if (ofp[NOUTFILE-2]) {
        sbsoutmsg(ofp[NOUTFILE-2],&str->raw.sbsmsg);
        n[NOUTFILE-2]++;
    }
    /* output SBAS ephemeris */
    if ((opt->navsys&SYS_SBS)&&sbsupdatecorr(&str->raw.sbsmsg,str->nav)==9) {
        
        if (ofp[1]&&!sep_nav) {
            outrnxhnavb(ofp[1],opt,str->nav->seph+prn-MINPRNSBS);
            n[1]++;
        }
        else if (ofp[3]&&sep_nav) {
            outrnxhnavb(ofp[3],opt,str->nav->seph+prn-MINPRNSBS);
            n[3]++;
        }
    }
}
/* set approx position in RINEX options --------------------------------------*/
static void setopt_apppos(strfile_t *str, rnxopt_t *opt)
{
    prcopt_t prcopt=prcopt_default;
    sol_t sol={{0}};
    char msg[128];
    
    prcopt.navsys=opt->navsys;
    
    /* point positioning with last obs data */
    if (!pntpos(str->obs->data,str->obs->n,str->nav,&prcopt,&sol,NULL,NULL,
                msg)) {
        trace(2,"point position error (%s)\n",msg);
        return;
    }
    matcpy(opt->apppos,sol.rr,3,1);
}
/* show conversion status ----------------------------------------------------*/
static int showstat(int sess, gtime_t ts, gtime_t te, int *n)
{
    const char type[]="ONGHQLCISE";
    char msg[1024]="",*p=msg,s[64];
    int i;
    
    if (sess>0) {
        p+=sprintf(p,"(%d) ",sess);
    }
    if (ts.time!=0) {
        time2str(ts,s,0);
        p+=sprintf(p,"%s",s);
    }
    if (te.time!=0&&timediff(te,ts)>0.9) {
        time2str(te,s,0);
        p+=sprintf(p,"-%s",s+5);
    }
    p+=sprintf(p,": ");
    
    for (i=0;i<NOUTFILE+1;i++) {
        if (n[i]==0) continue;
        p+=sprintf(p,"%c=%d%s",type[i],n[i],i<NOUTFILE?" ":"");
    }
    return showmsg(msg);
}
/* RINEX converter for single-session ----------------------------------------*/
static int convrnx_s(int sess, int format, rnxopt_t *opt, const char *file,
                     char **ofile)
{
    FILE *ofp[NOUTFILE]={NULL};
    strfile_t *str;
    gtime_t tend[3]={{0}};
    int i,j,nf,type,n[NOUTFILE+1]={0},mask[MAXEXFILE]={0},staid=-1,abort=0;
    char path[1024],*paths[NOUTFILE],s[NOUTFILE][1024];
    char *epath[MAXEXFILE]={0},*staname=*opt->staid?opt->staid:"0000";
    
    trace(3,"convrnx_s: sess=%d format=%d file=%s ofile=%s %s %s %s %s %s %s "
          "%s %s\n",sess,format,file,ofile[0],ofile[1],ofile[2],ofile[3],
          ofile[4],ofile[5],ofile[6],ofile[7],ofile[8]);
    
    /* replace keywords in input file */
    if (reppath(file,path,opt->ts,staname,"")<0) {
        showmsg("no time for input file: %s",file);
        return 0;
    }
    /* expand wild-cards in input file */
    for (i=0;i<MAXEXFILE;i++) {
        if (!(epath[i]=(char *)malloc(1024))) {
            for (i=0;i<MAXEXFILE;i++) free(epath[i]);
            return 0;
        }
    }
    if ((nf=expath(path,epath,MAXEXFILE))<=0) {
        showmsg("no input file: %s",path);
        return 0;
    }
    if (!(str=gen_strfile(format,opt->rcvopt))) {
        for (i=0;i<MAXEXFILE;i++) free(epath[i]);
        return 0;
    }
    if (format==STRFMT_RTCM2||format==STRFMT_RTCM3||format==STRFMT_RT17) {
        str->time=opt->trtcm;
    }
    else if (opt->ts.time) {
        str->time=timeadd(opt->ts,-1.0);
    }
    /* set GLONASS FCN in RINEX options */
    for (i=0;i<MAXPRNGLO;i++) {
        str->nav->glo_fcn[i]=opt->glofcn[i]; /* FCN+8 */
    }
    /* scan input files */
    if (!scan_file(epath,nf,opt,str,mask)) {
        for (i=0;i<MAXEXFILE;i++) free(epath[i]);
        free_strfile(str);
        return 0;
    }
    /* set format and file in RINEX options comments */
    setopt_file(format,epath,nf,mask,opt);
    
    /* replace keywords in output file */
    for (i=0;i<NOUTFILE;i++) {
        paths[i]=s[i];
        if (reppath(ofile[i],paths[i],opt->ts.time?opt->ts:str->tstart,
                    staname,"")<0) {
            showmsg("no time for output path: %s",ofile[i]);
            for (i=0;i<MAXEXFILE;i++) free(epath[i]);
            free_strfile(str);
            return 0;
        }
    }
    /* open output files */
    if (!openfile(ofp,paths,path,opt,str->nav)) {
        for (i=0;i<MAXEXFILE;i++) free(epath[i]);
        free_strfile(str);
        return 0;
    }
    str->time=str->tstart;
    for (i=0;i<nf&&!abort;i++) {
        if (!mask[i]) continue;
        
        /* open stream file */
        if (!open_strfile(str,epath[i])) continue;
        
        /* input message */
        for (j=0;(type=input_strfile(str))>=-1;j++) {
            
            if (!(j%11)&&(abort=showstat(sess,str->time,str->time,n))) break;
            
            if (opt->ts.time&&timediff(str->time,opt->ts)<-opt->ttol) continue;
            if (opt->te.time&&timediff(str->time,opt->te)>-opt->ttol) break;
            
            /* convert message */
            switch (type) {
                case  1: convobs(ofp,opt,str,n,tend,&staid); break;
                case  2: convnav(ofp,opt,str,n); break;
                case  3: convsbs(ofp,opt,str,n,tend+1); break;
                case  5: {
                    if (opt->outmsm > 0 && opt->outmsm <= 7)
                    {
                        memset(str->out.buff, 0, sizeof(str->out.buff));
                        str->out.nbyte = str->out.len = str->out.nbit = 0;
                        str->out.sta = *str->sta;
                        gen_rtcm3(&str->out, 1005, 0, 0);
                        if (str->out.nbyte > 0 && ofp[9] != NULL)
                        {
                            fwrite(str->out.buff, str->out.nbyte, sizeof(uint8_t), ofp[9]);
                            n[9]++;
                        }
                    }
                }
                case -1: n[NOUTFILE]++; break; /* error */
            }
            /* set approx position in rinex option */
            if (type==1&&!opt->autopos&&norm(opt->apppos,3)<=0.0) {
                setopt_apppos(str,opt);
            }
        }
        /* close stream file */
        close_strfile(str);
    }
    /* close output files */
    closefile(ofp,opt,str->nav);
    
    /* remove empty output files */
    for (i=0;i<NOUTFILE;i++) {
        if (ofp[i]&&n[i]<=0) remove(ofile[i]);
    }
    showstat(sess,opt->tstart,opt->tend,n);
    
    /* unset RINEX options comments */
    unsetopt_file(opt);
    
    free_strfile(str);
    for (i=0;i<MAXEXFILE;i++) free(epath[i]);
    
    return abort?-1:1;
}
/* RINEX converter -------------------------------------------------------------
* convert receiver log file to RINEX obs/nav, SBAS log files
* args   : int    format I      receiver raw format (STRFMT_???)
*          rnxopt_t *opt IO     RINEX options (see below)
*          char   *file  I      RTCM, receiver raw or RINEX file
*                               (wild-cards (*) are expanded)
*          char   **ofile IO    output files
*                               ofile[0] RINEX OBS file   ("": no output)
*                               ofile[1] RINEX NAV file   ("": no output)
*                               ofile[2] RINEX GNAV file  ("": no output)
*                               ofile[3] RINEX HNAV file  ("": no output)
*                               ofile[4] RINEX QNAV file  ("": no output)
*                               ofile[5] RINEX LNAV file  ("": no output)
*                               ofile[6] RINEX CNAV file  ("": no output)
*                               ofile[7] RINEX INAV file  ("": no output)
*                               ofile[8] SBAS log file    ("": no output)
*                               ofile[9] rtcm log file    ("": no output)
* return : status (1:ok,0:error,-1:abort)
* notes  : the following members of opt are replaced by information in last
*          converted RINEX: opt->tstart, opt->tend, opt->obstype, opt->nobs
*          keywords in ofile[] are replaced by first observation date/time and
*          station ID (%r)
*          the order of wild-card expanded files must be in-order by time
*-----------------------------------------------------------------------------*/
extern int convrnx(int format, rnxopt_t *opt, const char *file, char **ofile)
{
    gtime_t t0={0};
    rnxopt_t opt_=*opt;
    double tu,ts;
    int i,week,stat=1,sys_GRS=SYS_GPS|SYS_GLO|SYS_SBS;
    
    trace(3,"convrnx: format=%d file=%s ofile=%s %s %s %s %s %s %s %s %s %s\n",
          format,file,ofile[0],ofile[1],ofile[2],ofile[3],ofile[4],ofile[5],
          ofile[6],ofile[7],ofile[8],ofile[9]);
    
    showmsg("");
    
    /* disable systems according to RINEX version */
    if      (opt->rnxver<=210) opt_.navsys&=sys_GRS;
    else if (opt->rnxver<=211) opt_.navsys&=sys_GRS|SYS_GAL;
    else if (opt->rnxver<=212) opt_.navsys&=sys_GRS|SYS_GAL|SYS_CMP;
    else if (opt->rnxver<=300) opt_.navsys&=sys_GRS|SYS_GAL;
    else if (opt->rnxver<=301) opt_.navsys&=sys_GRS|SYS_GAL|SYS_CMP;
    else if (opt->rnxver<=302) opt_.navsys&=sys_GRS|SYS_GAL|SYS_CMP|SYS_QZS;
    
    /* disable frequency according to RINEX version */
    if (opt->rnxver<=210) opt_.freqtype&=0x3;
    
    if (opt->ts.time==0||opt->te.time==0||opt->tunit<=0.0) {
        
        /* single session */
        opt_.tstart=opt_.tend=t0;
        stat=convrnx_s(0,format,&opt_,file,ofile);
    }
    else if (timediff(opt->ts,opt->te)<0.0) {
        
        /* multiple session */
        tu=opt->tunit<86400.0?opt->tunit:86400.0;
        ts=tu*(int)floor(time2gpst(opt->ts,&week)/tu);
        
        for (i=0;;i++) { /* for each session */
            opt_.ts=gpst2time(week,ts+i*tu);
            opt_.te=timeadd(opt_.ts,tu);
            if (opt->trtcm.time) {
                opt_.trtcm=timeadd(opt->trtcm,timediff(opt_.ts,opt->ts));
            }
            if (timediff(opt_.ts,opt->te)>-opt->ttol) break;
            
            if (timediff(opt_.ts,opt->ts)<0.0) opt_.ts=opt->ts;
            if (timediff(opt_.te,opt->te)>0.0) opt_.te=opt->te;
            opt_.tstart=opt_.tend=t0;
            if ((stat=convrnx_s(i+1,format,&opt_,file,ofile))<0) break;
        }
    }
    else {
        showmsg("no period");
        return 0;
    }
    /* output start and end time */
    opt->tstart=opt_.tstart; opt->tend=opt_.tend;
    
    return stat;
}
