/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TDENGINE_MGMT_H
#define TDENGINE_MGMT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <syslog.h>

#include "sdb.h"
#include "tglobalcfg.h"
#include "thash.h"
#include "tidpool.h"
#include "tlog.h"
#include "tmempool.h"
#include "trpc.h"
#include "tsdb.h"
#include "tskiplist.h"
#include "tsocket.h"
#include "ttime.h"
#include "ttimer.h"
#include "tutil.h"

// internal globals
extern char  version[];
extern void *mgmtTmr;
extern void *mgmtQhandle;
extern void *mgmtTranQhandle;
extern int   mgmtShellConns;
extern int   mgmtDnodeConns;
extern char  mgmtDirectory[];

/*
 * vnode group 状态枚举值
 */
enum _TSDB_VG_STATUS {
  TSDB_VG_STATUS_READY,                  // vgroup 准备就绪
  TSDB_VG_STATUS_IN_PROGRESS,            // vgroup 进行中
  TSDB_VG_STATUS_COMMITLOG_INIT_FAILED,  // vgroup commit日志初始化失败
  TSDB_VG_STATUS_INIT_FAILED,            // vgroup 初始化失败
  TSDB_VG_STATUS_FULL                    // vgroup 状态满
};

/*
 * DB 状态枚举值
 */
enum _TSDB_DB_STATUS { TSDB_DB_STATUS_READY, TSDB_DB_STATUS_DROPPING, TSDB_DB_STATUS_DROP_FROM_SDB };

enum _TSDB_VN_STATUS { TSDB_VN_STATUS_READY, TSDB_VN_STATUS_DROPPING };

/*
 * Dnode对象结构体
 */
typedef struct {
  uint32_t   privateIp;
  int32_t    sid;  //
  uint32_t   moduleStatus;
  int32_t    openVnodes;       // 打开的vnode计数
  int32_t    numOfVnodes;      // dnode的vnode数目
  int32_t    numOfFreeVnodes;  // dnode的空闲vnode数目
  int64_t    createdTime;      // dnode对象创建时间戳
  uint32_t   publicIp;
  int32_t    status;
  uint32_t   lastAccess;
  uint32_t   rebootTimes;      // 重启计数
  uint32_t   lastReboot;       // time stamp for last reboot
  uint16_t   numOfCores;       // from dnode status msg
  uint8_t    alternativeRole;  // from dnode status msg, 0-any, 1-mgmt, 2-dnode
  uint8_t    reserveStatus;
  float      memoryAvailable;  // from dnode status msg
  float      diskAvailable;    // from dnode status msg
  int32_t    bandwidthMb;      // config by user，网络带宽，MB
  int16_t    cpuAvgUsage;      // calc from sys.cpu
  int16_t    memoryAvgUsage;   // calc from sys.mem
  int16_t    diskAvgUsage;     // calc from sys.disk
  int16_t    bandwidthUsage;   // calc from sys.band
  uint32_t   rack;
  uint16_t   idc;
  uint16_t   slot;
  int32_t    customScore;             // config by user
  float      lbScore;                 // calc in balance function
  int16_t    lbState;                 // set in balance function
  int16_t    lastAllocVnode;          // increase while create vnode,vnode创建时的自增序号
  SVnodeLoad vload[TSDB_MAX_VNODES];  // 加载生效的vnode数组
  char       reserved[16];
  char       updateEnd[1];
} SDnodeObj;

typedef struct {
  uint32_t ip;
  uint32_t publicIp;
  int32_t  vnode;
} SVnodeGid;

typedef struct {
  int32_t sid;   //session id
  int32_t vgId;  // vnode group ID
} SMeterGid;

/*
 * 表对象结构体
 */
typedef struct _tab_obj {
  char      meterId[TSDB_METER_ID_LEN + 1];
  uint64_t  uid;
  SMeterGid gid;

  int32_t sversion;  // schema version
  int64_t createdTime;
  int32_t numOfTags;    // for metric
  int32_t numOfMeters;  // for metric
  int32_t numOfColumns;
  int32_t schemaSize;
  short   nextColId;
  char    meterType : 4;
  char    status : 3;
  char    isDirty : 1;  // if the table change tag column 1 value
  char    reserved[15];
  char    updateEnd[1];

  pthread_rwlock_t rwLock;  //线程互斥锁
  tSkipList *      pSkipList; //指针变量，指向tSkipList
  struct _tab_obj *pHead;  // for metric, a link list for all meters created
                           // according to this metric
  char *pTagData;          // TSDB_METER_ID_LEN(metric_name)+
                           // tags_value1/tags_value2/tags_value3
  struct _tab_obj *prev, *next; //prev，next指针变量，分别指向前一个和后一个tab obj对象
  char *           pSql;  // pointer to SQL, for SC, null-terminated string
  char *           pReserve1;
  char *           pReserve2;
  char *           schema;  //指针指向schema的描述
  // SSchema    schema[];
} STabObj;

/*
 * vnode group对象结构体
 */
typedef struct _vg_obj {
  uint32_t        vgId;
  char            dbName[TSDB_DB_NAME_LEN];
  int64_t         createdTime;
  uint64_t        lastCreate;
  uint64_t        lastRemove;
  int32_t         numOfVnodes;
  SVnodeGid       vnodeGid[TSDB_VNODES_SUPPORT];
  int32_t         numOfMeters;
  int32_t         lbIp;
  int32_t         lbTime;
  int8_t          lbState;
  char            reserved[16];
  char            updateEnd[1];
  struct _vg_obj *prev, *next;
  void *          idPool;
  STabObj **      meterList;
} SVgObj;

/*
 * DB对象结构体
 */
typedef struct _db_obj {
  char    name[TSDB_DB_NAME_LEN + 1];     //db name
  int64_t createdTime;        //db 创建时间戳
  SDbCfg  cfg;                //db 配置对象
  int32_t numOfVgroups;
  int32_t numOfTables;
  int32_t numOfMetrics;
  uint8_t vgStatus;
  uint8_t dropStatus;
  char    reserved[16];
  char    updateEnd[1];

  STabObj *       pMetric;  //
  struct _db_obj *prev, *next;
  SVgObj *        pHead;  // empty vgroup first
  SVgObj *        pTail;  // empty vgroup end
  void *          vgTimer;
} SDbObj;

/*
 * 用户对象结构体
 */
typedef struct _user_obj {
  char              user[TSDB_USER_LEN + 1];
  char              pass[TSDB_KEY_LEN];
  char              acct[TSDB_USER_LEN];
  int64_t           createdTime;
  char              superAuth : 1;
  char              writeAuth : 1;
  char              reserved[16];
  char              updateEnd[1];
  struct _user_obj *prev, *next;
} SUserObj;

typedef struct {
  int32_t numOfUsers;
  int32_t numOfDbs;
  int32_t numOfTimeSeries;
  int32_t numOfPointsPerSecond;
  int32_t numOfConns;
  int32_t numOfQueries;
  int32_t numOfStreams;
  int64_t totalStorage;  // Total storage wrtten from this account
  int64_t compStorage;   // Compressed storage on disk
  int64_t queryTime;
  int64_t totalPoints;
  int64_t inblound;
  int64_t outbound;
  TSKEY   sKey;
  char    accessState;  // Checked by mgmt heartbeat message
} SAcctInfo;

/*
 * Acct对象结构体
 */
typedef struct {
  char      user[TSDB_USER_LEN + 1];
  char      pass[TSDB_KEY_LEN];
  SAcctCfg  cfg;
  int32_t   acctId;
  int64_t   createdTime;
  char      reserved[15];
  char      updateEnd[1];
  SAcctInfo acctInfo;

  SDbObj *         pHead;
  SUserObj *       pUser;
  struct _connObj *pConn;
  pthread_mutex_t  mutex;
} SAcctObj;

/*
 * connect对象结构体
 */
typedef struct _connObj {
  SAcctObj *       pAcct; //指针变量，指向Acct对象
  SDbObj *         pDb;   //指针变量，指向DB对象
  SUserObj *       pUser; //指针变量，指向User对象
  char             user[TSDB_USER_LEN];
  char             db[TSDB_METER_ID_LEN];
  uint64_t         stime;               // login time
  char             superAuth : 1;       // super user flag
  char             writeAuth : 1;       // write flag
  char             killConnection : 1;  // kill the connection flag
  uint32_t         queryId;             // query ID to be killed
  uint32_t         streamId;            // stream ID to be killed
  uint32_t         ip;                  // shell IP
  short            port;                // shell port
  void *           thandle;
  SQList *         pQList;  // query list
  SSList *         pSList;  // stream list
  uint64_t         qhandle;
  struct _connObj *prev, *next; //指针变量，指向connect链表上前一个/下一个connect对象
} SConnObj;

typedef struct {
  char spi;
  char encrypt;
  char secret[TSDB_KEY_LEN];
  char cipheringKey[TSDB_KEY_LEN];
} SSecInfo;

typedef struct {
  char     type;
  void *   pNode;
  short    numOfColumns;
  int      rowSize;
  int      numOfRows;
  int      numOfReads;
  short    offset[TSDB_MAX_COLUMNS];
  short    bytes[TSDB_MAX_COLUMNS];
  void *   signature;
  uint16_t payloadLen; /* length of payload*/
  char     payload[];  /* payload for wildcard match in show tables */
} SShowObj;

extern SAcctObj  acctObj;
extern SDnodeObj dnodeObj;

// dnodeInt API
int  mgmtInitDnodeInt();
void mgmtCleanUpDnodeInt();
int  mgmtSendCreateMsgToVnode(STabObj *pMeter, int vnode);
int  mgmtSendRemoveMeterMsgToVnode(STabObj *pMeter, int vnode);
int  mgmtSendVPeersMsg(SVgObj *pVgroup, SDbObj *pDb);
int  mgmtSendFreeVnodeMsg(int vnode);

// shell API
int  mgmtInitShell();
void mgmtCleanUpShell();
int  mgmtRetriveUserAuthInfo(char *user, char *spi, char *encrypt, uint8_t *secret, uint8_t *ckey);

// acct API
int     mgmtAddDbIntoAcct(SAcctObj *pAcct, SDbObj *pDb);
int     mgmtRemoveDbFromAcct(SAcctObj *pAcct, SDbObj *pDb);
int     mgmtAddUserIntoAcct(SAcctObj *pAcct, SUserObj *pUser);
int     mgmtRemoveUserFromAcct(SAcctObj *pAcct, SUserObj *pUser);
int     mgmtAddConnIntoAcct(SConnObj *pConn);
int     mgmtRemoveConnFromAcct(SConnObj *pConn);
void    mgmtCheckAcct();
int64_t mgmtGetAcctStatistic(SAcctObj *pAcct);

// user API
int       mgmtInitUsers();
SUserObj *mgmtGetUser(char *name);
int       mgmtCreateUser(SAcctObj *pAcct, char *name, char *pass);
int       mgmtDropUser(SAcctObj *pAcct, char *name);
int       mgmtUpdateUser(SUserObj *pUser);
int       mgmtGetUserMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int       mgmtRetrieveUsers(SShowObj *pShow, char *data, int rows, SConnObj *pConn);
void      mgmtCleanUpUsers();

// metric API
int mgmtAddMeterIntoMetric(STabObj *pMetric, STabObj *pMeter);
int mgmtRemoveMeterFromMetric(STabObj *pMetric, STabObj *pMeter);
int mgmtGetMetricMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int mgmtRetrieveMetrics(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

// DB API
int     mgmtInitDbs();
int     mgmtUpdateDb(SDbObj *pDb);
SDbObj *mgmtGetDb(char *db);
SDbObj *mgmtGetDbByMeterId(char *db);
int     mgmtCreateDb(SAcctObj *pAcct, SCreateDbMsg *pCreate);
int     mgmtDropDbByName(SAcctObj *pAcct, char *name);
int     mgmtDropDb(SDbObj *pDb);
/* void    mgmtMonitorDbDrop(void *unused); */
void mgmtMonitorDbDrop(void *unused, void *unusedt);
int  mgmtAlterDb(SAcctObj *pAcct, SAlterDbMsg *pAlter);
int  mgmtUseDb(SConnObj *pConn, char *name);
int  mgmtAddVgroupIntoDb(SDbObj *pDb, SVgObj *pVgroup);
int  mgmtAddVgroupIntoDbTail(SDbObj *pDb, SVgObj *pVgroup);
int  mgmtRemoveVgroupFromDb(SDbObj *pDb, SVgObj *pVgroup);
int  mgmtAddMetricIntoDb(SDbObj *pDb, STabObj *pMetric);
int  mgmtRemoveMetricFromDb(SDbObj *pDb, STabObj *pMetric);
int  mgmtMoveVgroupToTail(SDbObj *pDb, SVgObj *pVgroup);
int  mgmtMoveVgroupToHead(SDbObj *pDb, SVgObj *pVgroup);
int  mgmtGetDbMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int  mgmtRetrieveDbs(SShowObj *pShow, char *data, int rows, SConnObj *pConn);
void mgmtCleanUpDbs();

// vGroup API
int       mgmtInitVgroups();
SVgObj *  mgmtGetVgroup(int vgId);
SVgObj *  mgmtCreateVgroup(SDbObj *pDb);
int       mgmtDropVgroup(SDbObj *pDb, SVgObj *pVgroup);
void      mgmtSetVgroupIdPool();
int       mgmtGetVgroupMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int       mgmtRetrieveVgroups(SShowObj *pShow, char *data, int rows, SConnObj *pConn);
void      mgmtCleanUpVgroups();
SAcctObj *mgmtGetVgroupAcct(int vgId);

// meter API
int      mgmtInitMeters();
STabObj *mgmtGetMeter(char *meterId);
STabObj *mgmtGetMeterInfo(char *src, char *tags[]);
int      mgmtRetrieveMetricMeta(void *thandle, char **pStart, STabObj *pMetric, SMetricMetaMsg *pInfo);
int      mgmtCreateMeter(SDbObj *pDb, SCreateTableMsg *pCreate);
int      mgmtDropMeter(SDbObj *pDb, char *meterId, int ignore);
int      mgmtAlterMeter(SDbObj *pDb, SAlterTableMsg *pAlter);
int      mgmtGetMeterMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int      mgmtRetrieveMeters(SShowObj *pShow, char *data, int rows, SConnObj *pConn);
void     mgmtCleanUpMeters();
SSchema *mgmtGetMeterSchema(STabObj *pMeter);  // get schema for a meter

bool mgmtMeterCreateFromMetric(STabObj *pMeterObj);
bool mgmtIsMetric(STabObj *pMeterObj);
bool mgmtIsNormalMeter(STabObj *pMeterObj);

int  mgmtGetDnodeMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int  mgmtRetrieveDnodes(SShowObj *pShow, char *data, int rows, SConnObj *pConn);
void mgmtSetDnodeVgid(int vnode, int vgId);
void mgmtUnSetDnodeVgid(int vnode);
void mgmtSetDnodeMaxVnodes(SDnodeObj *pDnode);

int mgmtGetModuleMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int mgmtRetrieveModules(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

int mgmtGetConfigMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int mgmtRetrieveConfigs(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

int mgmtGetConnsMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int mgmtRetrieveConns(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

int mgmtGetScoresMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int mgmtRetrieveScores(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

int grantGetGrantsMeta(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
int grantRetrieveGrants(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

void mgmtSetModuleInDnode(SDnodeObj *pDnode, int moduleType);
int  mgmtUnSetModuleInDnode(SDnodeObj *pDnode, int moduleType);

extern int (*mgmtGetMetaFp[])(SMeterMeta *pMeta, SShowObj *pShow, SConnObj *pConn);
extern int (*mgmtRetrieveFp[])(SShowObj *pShow, char *data, int rows, SConnObj *pConn);

extern int tsVgUpdateSize;
extern int tsDbUpdateSize;
extern int tsUserUpdateSize;

#ifdef __cplusplus
}
#endif

#endif  // TDENGINE_MGMT_H
