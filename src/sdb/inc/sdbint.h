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

#ifndef _sdbint_header_
#define _sdbint_header_

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hashint.h"
#include "hashstr.h"
#include "sdb.h"
#include "tchecksum.h"
#include "tlog.h"
#include "trpc.h"
#include "tutil.h"

#define sdbError(...)                            \
  if (sdbDebugFlag & DEBUG_ERROR) {              \
    tprintf("ERROR MND-SDB ", 255, __VA_ARGS__); \
  }
#define sdbWarn(...)                                      \
  if (sdbDebugFlag & DEBUG_WARN) {                        \
    tprintf("WARN  MND-SDB ", sdbDebugFlag, __VA_ARGS__); \
  }
#define sdbTrace(...)                               \
  if (sdbDebugFlag & DEBUG_TRACE) {                 \
    tprintf("MND-SDB ", sdbDebugFlag, __VA_ARGS__); \
  }
#define sdbPrint(...) \
  { tprintf("MND-SDB ", 255, __VA_ARGS__); }

#define sdbLError(...) taosLogError(__VA_ARGS__) sdbError(__VA_ARGS__)
#define sdbLWarn(...) taosLogWarn(__VA_ARGS__) sdbWarn(__VA_ARGS__)
#define sdbLPrint(...) taosLogPrint(__VA_ARGS__) sdbPrint(__VA_ARGS__)

#define SDB_MAX_PEERS 4
#define SDB_DELIMITER 0xFFF00F00
#define SDB_ENDCOMMIT 0xAFFFAAAF

typedef struct {
  uint64_t swVersion;
  int16_t  sdbFileVersion;
  char     reserved[6];
  TSCKSUM  checkSum;
} SSdbHeader;

typedef struct {
  char type;
  // short  rowSize;
  char *row;
} SSdbUpdate;

typedef struct {
  char     numOfTables;
  uint64_t version[];
} SSdbSync;

/*
 * sdbTable对象（系统数据库表）结构体
 * 系统数据库表对象：db,meters,user,vgroups
 */
typedef struct {
  SSdbHeader header;
  int        maxRows; //系统数据表最大行
  int        dbId;
  int32_t    maxRowSize;  //系统数据表最大行存储大小
  char       name[TSDB_DB_NAME_LEN];    //系统数据表名称，db，meters，user，vgroup
  char       fn[128];   //系统数据表的目录和文件名，如：dir/db.db ; dir/user.db;
  int        keyType;   //key类型
  uint32_t   autoIndex;
  int64_t    numOfRows; //行计数
  int64_t    id;
  int64_t    size;    //系统数据表文件字节大小
  void *     iHandle;   //指针变量，指向keyType对应的index索引初始化函数，函数返回的Hash对象
  int        fd;  //打开系统数据表文件的文件句柄 
  void *(*appTool)(char, void *, char *, int, int *);   //函数指针，指向系统数据表的处理函数，函数返回指针
  pthread_mutex_t mutex;    //线程互斥锁
  SSdbUpdate *    update;
  int             numOfUpdates;
  int             updatePos;
} SSdbTable;

/*
 * 系统数据表的行元数据结构体
 */
typedef struct {
  int64_t id;
  int64_t offset;
  int     rowSize;  //行存储字节
  void *  row;      //系统数据库表的一行
} SRowMeta;

typedef struct {
  int32_t delimiter;
  int32_t rowSize;
  int64_t id;
  char    data[];
} SRowHead;

typedef struct {
  char *          buffer;
  char *          offset;
  int             trans;
  int             bufferSize;
  pthread_mutex_t qmutex;
} STranQueue;

typedef struct {
  char     status;
  char     role;
  char     numOfMnodes;
  uint64_t dbVersion;
  uint32_t numOfDnodes;
  uint32_t publicIp;
} SMnodeStatus;

typedef struct {
  char     dbId;
  char     type;
  uint64_t version;
  short    dataLen;
  char     data[];
} SForwardMsg;

extern SSdbTable *tableList[];
extern int        sdbMaxPeers;
extern int        sdbDebugFlag;
extern int        sdbNumOfTables;
extern int64_t    sdbVersion;

int sdbForwardDbReqToPeer(SSdbTable *pTable, char type, char *data, int dataLen);
int sdbRetrieveRows(int fd, SSdbTable *pTable, uint64_t version);
void sdbResetTable(SSdbTable *pTable);
extern const int16_t sdbFileVersion;

#endif
