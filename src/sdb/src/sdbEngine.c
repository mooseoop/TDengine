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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sdb.h"
#include "sdbint.h"
#include "tutil.h"

//宏定义
#define abs(x) (((x) < 0) ? -(x) : (x))

extern char   version[];
const int16_t sdbFileVersion = 0;

/*
 * 函数指针，根据keyType调用不同索引初始化（index）函数
 * maxRows：最大行数
 * dataSize：占用字节数
 * 返回：指针，指向对应类型的hash obj对象
 */
void *(*sdbInitIndexFp[])(int maxRows, int dataSize) = {sdbOpenStrHash, sdbOpenIntHash, sdbOpenIntHash};

/*
 * 函数指针，根据不同keyType调用不同函数
 * 函数实现系统数据表增加索引功能
 */
void *(*sdbAddIndexFp[])(void *handle, void *key, void *data) = {sdbAddStrHash, sdbAddIntHash, sdbAddIntHash};

void (*sdbDeleteIndexFp[])(void *handle, void *key) = {sdbDeleteStrHash, sdbDeleteIntHash, sdbDeleteIntHash};

/*
 * 函数指针，根据keyType检索不同的指针函数
 * 函数实现系统数据表根据key检索到索引
 * *handle：指针，指向系统数据表Hash对象
 * *key：指针，系统数据表的key信息
 * 返回：指针，指向不同系统数据表的对象数据
 */
void *(*sdbGetIndexFp[])(void *handle, void *key) = {sdbGetStrHashData, sdbGetIntHashData, sdbGetIntHashData};

void (*sdbCleanUpIndexFp[])(void *handle) = {
    sdbCloseStrHash, sdbCloseIntHash, sdbCloseIntHash,
};

void *(*sdbFetchRowFp[])(void *handle, void *ptr, void **ppRow) = {
    sdbFetchStrHashData, sdbFetchIntHashData, sdbFetchIntHashData,
};

SSdbTable *tableList[20];
int        sdbNumOfTables;    //系统数据表计数
int64_t    sdbVersion;  //系统数据表版本，每次变化，版本都变化

void sdbFinishCommit(void *handle) {
  SSdbTable *pTable = (SSdbTable *)handle;
  uint32_t   sdbEcommit = SDB_ENDCOMMIT;

  off_t offset = lseek(pTable->fd, 0, SEEK_END);
  assert(offset == pTable->size);
  twrite(pTable->fd, &sdbEcommit, sizeof(sdbEcommit));
  pTable->size += sizeof(sdbEcommit);
}

/*
 * 打开系统数据表文件
 */
int sdbOpenSdbFile(SSdbTable *pTable) {
  struct stat fstat, ofstat;
  uint64_t    size;
  char *      dirc = NULL;
  char *      basec = NULL;
  union {
    char     cversion[64];
    uint64_t iversion;
  } swVersion;

  memcpy(swVersion.cversion, version, sizeof(uint64_t));

  // check sdb.db and .sdb.db status
  char fn[128] = "\0";
  dirc = strdup(pTable->fn);
  basec = strdup(pTable->fn);
  sprintf(fn, "%s/.%s", dirname(dirc), basename(basec));
  tfree(dirc);
  tfree(basec);
  if (stat(fn, &ofstat) == 0) {  // .sdb.db file exists
    if (stat(pTable->fn, &fstat) == 0) {
      remove(fn);
    } else {
      remove(pTable->fn);
      rename(fn, pTable->fn);
    }
  }

  pTable->fd = open(pTable->fn, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO); //打开系统数据表文件
  if (pTable->fd < 0) {
    sdbError("failed to open file:%s", pTable->fn);
    return -1;
  }

  pTable->size = 0;
  stat(pTable->fn, &fstat);
  size = sizeof(pTable->header);

  if (fstat.st_size == 0) {
    pTable->header.swVersion = swVersion.iversion;
    pTable->header.sdbFileVersion = sdbFileVersion;
    if (taosCalcChecksumAppend(0, (uint8_t *)(&pTable->header), size) < 0) {
      sdbError("failed to get file header checksum, file: %s", pTable->fn);
      tclose(pTable->fd);
      return -1;
    }
    twrite(pTable->fd, &(pTable->header), size);
    pTable->size += size;
    sdbFinishCommit(pTable);
  } else {
    uint32_t sdbEcommit = 0;
    off_t    offset = lseek(pTable->fd, -(sizeof(sdbEcommit)), SEEK_END);
    while (offset > 0) {
      read(pTable->fd, &sdbEcommit, sizeof(sdbEcommit));
      if (sdbEcommit == SDB_ENDCOMMIT) {
        ftruncate(pTable->fd, offset + sizeof(sdbEcommit));
        break;
      }
      offset = lseek(pTable->fd, -(sizeof(sdbEcommit) + 1), SEEK_CUR);
    }
    lseek(pTable->fd, 0, SEEK_SET);

    ssize_t tsize = read(pTable->fd, &(pTable->header), size);
    if (tsize < size) {
      sdbError("failed to read sdb file header, file: %s", pTable->fn);
      tclose(pTable->fd);
      return -1;
    }

    if (pTable->header.swVersion != swVersion.iversion) {
      sdbWarn("sdb file %s version not match software version", pTable->fn);
    }

    if (!taosCheckChecksumWhole((uint8_t *)(&pTable->header), size)) {
      sdbError("sdb file header is broken since checksum mismatch, file: %s", pTable->fn);
      tclose(pTable->fd);
      return -1;
    }

    pTable->size += size;
    // skip end commit symbol
    lseek(pTable->fd, sizeof(sdbEcommit), SEEK_CUR);
    pTable->size += sizeof(sdbEcommit);
  }

  pTable->numOfRows = 0;

  return pTable->fd;
}

// TODO: Change here
void sdbAddIntoUpdateList(SSdbTable *pTable, char type, char *row) {
  pTable->numOfUpdates++;
  pTable->updatePos = pTable->numOfUpdates % pTable->maxRows;

  if (pTable->update[pTable->updatePos].type == SDB_TYPE_DELETE)
    (*(pTable->appTool))(SDB_TYPE_DESTROY, pTable->update[pTable->updatePos].row, NULL, 0, NULL);

  pTable->update[pTable->updatePos].type = type;
  pTable->update[pTable->updatePos].row = row;
}

/*
 * 从系统数据表文件初始化系统数据表
 */
int sdbInitTableByFile(SSdbTable *pTable) {
  SRowMeta rowMeta;
  int      numOfDels = 0;
  int      bytes = 0;
  int64_t  oldId = 0;
  void *   pMetaRow = NULL;
  int      total_size = 0;
  int      real_size = 0;

  oldId = pTable->id;
  if (sdbOpenSdbFile(pTable) < 0) return -1;

  total_size = sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM);
  SRowHead *rowHead = (SRowHead *)malloc(total_size); //动态分配行头内存
  if (rowHead == NULL) {
    sdbError("failed to allocate row head memory, sdb: %s", pTable->name);
    return -1;
  }

  // Loop to read sdb file row by row
  while (1) {
    memset(rowHead, 0, total_size);

    bytes = read(pTable->fd, rowHead, sizeof(SRowHead));  //从已打开的系统数据表文件读取 行头 个字节到内存中
    if (bytes < 0) {
      sdbError("failed to read sdb file: %s", pTable->fn);
      goto sdb_exit1;
    }

    if (bytes == 0) break;

    if (bytes < sizeof(SRowHead) || rowHead->delimiter != SDB_DELIMITER) {
      pTable->size++;
      lseek(pTable->fd, -(bytes - 1), SEEK_CUR);
      continue;
    }

    if (rowHead->rowSize < 0 || rowHead->rowSize > pTable->maxRowSize) {
      sdbError("error row size in sdb file: %s  rowSize: %d  maxRowSize: %d", pTable->fn, rowHead->rowSize,
               pTable->maxRowSize);
      pTable->size += sizeof(SRowHead);
      continue;
    }

    // sdbTrace("%s id:%ld rowSize:%d", pTable->name, rowHead->id,
    // rowHead->rowSize);

    bytes = read(pTable->fd, rowHead->data, rowHead->rowSize + sizeof(TSCKSUM));
    if (bytes < rowHead->rowSize + sizeof(TSCKSUM)) {
      // TODO: Here may cause pTable->size not end of the file
      sdbError("failed to read sdb file: %s  id: %d  rowSize: %d", pTable->fn, rowHead->id, rowHead->rowSize);
      break;
    }

    real_size = sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM);
    if (!taosCheckChecksumWhole((uint8_t *)rowHead, real_size)) {
      sdbError("error sdb checksum, sdb: %s  id: %d, skip", pTable->name, rowHead->id);
      pTable->size += real_size;
      continue;
    }

    // Check if the the object exists already

    pMetaRow = sdbGetRow(pTable, rowHead->data);
    if (pMetaRow == NULL) {  // New object
      if (rowHead->id < 0) {
        /* assert(0); */
        sdbError("error sdb negative id: %d, sdb: %s, skip", rowHead->id, pTable->name);
      } else {
        rowMeta.id = rowHead->id;
        // TODO: Get rid of the rowMeta.offset and rowSize
        rowMeta.offset = pTable->size;
        rowMeta.rowSize = rowHead->rowSize;
        rowMeta.row = (*(pTable->appTool))(SDB_TYPE_DECODE, NULL, rowHead->data, rowHead->rowSize, NULL);
        (*sdbAddIndexFp[pTable->keyType])(pTable->iHandle, rowMeta.row, &rowMeta);
        if (pTable->keyType == SDB_KEYTYPE_AUTO) pTable->autoIndex++;
        pTable->numOfRows++;
      }
    } else {                  // already exists
      if (rowHead->id < 0) {  // Delete the object
        (*sdbDeleteIndexFp[pTable->keyType])(pTable->iHandle, rowHead->data);
        (*(pTable->appTool))(SDB_TYPE_DESTROY, pMetaRow, NULL, 0, NULL);
        pTable->numOfRows--;
        numOfDels++;
      } else {  // Reset the object TODO: is it possible to merge reset and
                // update ??
        (*(pTable->appTool))(SDB_TYPE_RESET, pMetaRow, rowHead->data, rowHead->rowSize, NULL);
      }
      numOfDels++;
    }

    pTable->size += real_size;
    if (pTable->id < abs(rowHead->id)) pTable->id = abs(rowHead->id);
  }

  sdbVersion += (pTable->id - oldId);
  if (numOfDels > pTable->maxRows / 4) sdbSaveSnapShot(pTable);

  pTable->numOfUpdates = 0;
  pTable->updatePos = 0;

  tfree(rowHead);
  return 0;

sdb_exit1:
  tfree(rowHead);
  return -1;
}

/*
 * 系统数据库打开系统表
 * maxRows：最大行数
 * maxRowSize：最大行数大小
 * *name：系统表名称，有“db”，“meters”，“user”，“vgroups”
 * keyType：key类型：
 * *directory：系统表存储目录
 * *(*appTool):指针指向系统表操作处理函数
 * 返回：系统数据表指针
 */
void *sdbOpenTable(int maxRows, int32_t maxRowSize, char *name, char keyType, char *directory,
                   void *(*appTool)(char, void *, char *, int, int *)) {
  SSdbTable *pTable = (SSdbTable *)malloc(sizeof(SSdbTable));
  if (pTable == NULL) return NULL;
  memset(pTable, 0, sizeof(SSdbTable));

  int size = sizeof(SSdbUpdate) * maxRows;
  pTable->update = (SSdbUpdate *)malloc(size);
  if (pTable->update == NULL) {
    free(pTable);
    return NULL;
  };
  memset(pTable->update, 0, size);

  strcpy(pTable->name, name);
  pTable->keyType = keyType;
  pTable->maxRows = maxRows;
  pTable->maxRowSize = maxRowSize;
  pTable->appTool = appTool;    //指针指向系统数据表操作函数指针
  sprintf(pTable->fn, "%s/%s.db", directory, pTable->name);

  //根据keyType初始化索引
  if (sdbInitIndexFp[keyType] != NULL) pTable->iHandle = (*sdbInitIndexFp[keyType])(maxRows, sizeof(SRowMeta));

  pthread_mutex_init(&pTable->mutex, NULL);   //初始化系统数据表对象的线程互斥锁

  if (sdbInitTableByFile(pTable) < 0) return NULL;

  pTable->dbId = sdbNumOfTables++;
  tableList[pTable->dbId] = pTable;

  sdbTrace("table:%s is initialized, numOfRows:%d, numOfTables:%d", pTable->name, pTable->numOfRows, sdbNumOfTables);

  return pTable;
}

/*
 * 根据key获取系统数据表的一行
 * *handle：指针，指向系统数据表
 * *key：指针，指向系统数据表的key信息
 * 返回：指针，指向系统数据表的一行对象
 */
SRowMeta *sdbGetRowMeta(void *handle, void *key) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta;

  if (handle == NULL) return NULL;

  pMeta = (*sdbGetIndexFp[pTable->keyType])(pTable->iHandle, key);

  return pMeta;
}

/*
 * 获取系统数据库表的行的元数据
 * *handle：入参指针，指向SdbTable（系统数据库表）对象
 * *key：入参指针，vgroup id，DB name
 */
void *sdbGetRow(void *handle, void *key) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta;

  if (handle == NULL) return NULL;

  pthread_mutex_lock(&pTable->mutex);
  pMeta = (*sdbGetIndexFp[pTable->keyType])(pTable->iHandle, key);    //根据keyType调用不同指针函数，返回指针，指向系统表的行的元数据
  pthread_mutex_unlock(&pTable->mutex);

  if (pMeta == NULL) return NULL;

  return pMeta->row;
}

/*
 * 在系统数据库表（sdbTable）中插入一行（DB对象）
 * *handle：指针，指向系统数据表对象
 * *row：指针指向DBobj对象
 * rowSize：int型，行大小
 * 返回值：int64，返回码，0为成功
 * row here must be encoded string (rowSize > 0) or the object it self (rowSize
 * = 0)
 */
int64_t sdbInsertRow(void *handle, void *row, int rowSize) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta   rowMeta;
  int64_t    id = -1;
  void *     pObj = NULL;
  int        total_size = 0;
  int        real_size = 0;
  /* char       action = SDB_TYPE_INSERT; */

  if (pTable == NULL) return -1;

  if ((pTable->keyType != SDB_KEYTYPE_AUTO) || *((int64_t *)row))
    if (sdbGetRow(handle, row)) return -1;

  total_size = sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM);
  SRowHead *rowHead = (SRowHead *)malloc(total_size);
  if (rowHead == NULL) {
    sdbError("failed to allocate row head memory, sdb: %s", pTable->name);
    return -1;
  }
  memset(rowHead, 0, total_size);

  if (rowSize == 0) {  // object is created already
    pObj = row;
  } else {  // encoded string, to create object
    pObj = (*(pTable->appTool))(SDB_TYPE_DECODE, NULL, row, rowSize, NULL);
  }
  (*(pTable->appTool))(SDB_TYPE_ENCODE, pObj, rowHead->data, pTable->maxRowSize, &(rowHead->rowSize));
  assert(rowHead->rowSize > 0 && rowHead->rowSize <= pTable->maxRowSize);

  pthread_mutex_lock(&pTable->mutex);

  pTable->id++;
  sdbVersion++;
  if (pTable->keyType == SDB_KEYTYPE_AUTO) {
    // TODO: here need to change
    *((uint32_t *)pObj) = ++pTable->autoIndex;
    (*(pTable->appTool))(SDB_TYPE_ENCODE, pObj, rowHead->data, pTable->maxRowSize, &(rowHead->rowSize));
  }

  real_size = sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM);

  rowHead->delimiter = SDB_DELIMITER;
  rowHead->id = pTable->id;
  if (taosCalcChecksumAppend(0, (uint8_t *)rowHead, real_size) < 0) {
    sdbError("failed to get checksum while inserting, sdb: %s", pTable->name);
    pthread_mutex_unlock(&pTable->mutex);
    tfree(rowHead);
    return -1;
  }

  // update in SDB layer
  rowMeta.id = pTable->id;
  rowMeta.offset = pTable->size;
  rowMeta.rowSize = rowHead->rowSize;
  rowMeta.row = pObj;
  (*sdbAddIndexFp[pTable->keyType])(pTable->iHandle, pObj, &rowMeta);

  /* Update the disk content */
  /* write(pTable->fd, &action, sizeof(action)); */
  /* pTable->size += sizeof(action); */
  twrite(pTable->fd, rowHead, real_size);
  pTable->size += real_size;
  sdbFinishCommit(pTable);

  sdbAddIntoUpdateList(pTable, SDB_TYPE_INSERT, rowMeta.row);

  pTable->numOfRows++;
  switch (pTable->keyType) {
    case SDB_KEYTYPE_STRING:
      sdbTrace("table:%s, a record is inserted:%s, sdbVersion:%ld id:%ld rowSize:%d numOfRows:%d fileSize:%ld",
               pTable->name, (char *)row, sdbVersion, rowHead->id, rowHead->rowSize, pTable->numOfRows, pTable->size);
      break;
    case SDB_KEYTYPE_UINT32:
    case SDB_KEYTYPE_AUTO:
      sdbTrace("table:%s, a record is inserted:%d, sdbVersion:%ld id:%ld rowSize:%d numOfRows:%d fileSize:%ld",
               pTable->name, *(int32_t *)row, sdbVersion, rowHead->id, rowHead->rowSize, pTable->numOfRows, pTable->size);
      break;
    default:
      sdbTrace("table:%s, a record is inserted, sdbVersion:%ld id:%ld rowSize:%d numOfRows:%d fileSize:%ld",
               pTable->name, sdbVersion, rowHead->id, rowHead->rowSize, pTable->numOfRows, pTable->size);
      break;
  }

  id = rowMeta.id;

  tfree(rowHead);

  pthread_mutex_unlock(&pTable->mutex);

  /* callback function to update the MGMT layer */
  if (id >= 0 && pTable->appTool) (*pTable->appTool)(SDB_TYPE_INSERT, pObj, NULL, 0, NULL);

  return id;
}

// row here can be object or null-terminated string
int sdbDeleteRow(void *handle, void *row) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta = NULL;
  int        code = -1;
  void *     pMetaRow = NULL;
  SRowHead * rowHead = NULL;
  int        rowSize = 0;
  int        total_size = 0;
  /* char       action     = SDB_TYPE_DELETE; */

  if (pTable == NULL) return -1;

  pMeta = sdbGetRowMeta(handle, row);
  if (pMeta == NULL) {
    sdbTrace("table:%s, record is not there, delete failed", pTable->name);
    return -1;
  }

  pMetaRow = pMeta->row;
  assert(pMetaRow != NULL);

  switch (pTable->keyType) {
    case SDB_KEYTYPE_STRING:
      rowSize = strlen((char *)row) + 1;
      break;
    case SDB_KEYTYPE_UINT32:
      rowSize = sizeof(uint32_t);
      break;
    case SDB_KEYTYPE_AUTO:
      rowSize = sizeof(uint64_t);
      break;
    default:
      return -1;
  }

  total_size = sizeof(SRowHead) + rowSize + sizeof(TSCKSUM);
  rowHead = (SRowHead *)malloc(total_size);
  if (rowHead == NULL) {
    sdbError("failed to allocate row head memory, sdb: %s", pTable->name);
    return -1;
  }
  memset(rowHead, 0, total_size);

  pthread_mutex_lock(&pTable->mutex);

  pTable->id++;
  sdbVersion++;

  rowHead->delimiter = SDB_DELIMITER;
  rowHead->rowSize = rowSize;
  rowHead->id = -(pTable->id);
  memcpy(rowHead->data, row, rowSize);
  if (taosCalcChecksumAppend(0, (uint8_t *)rowHead, total_size) < 0) {
    sdbError("failed to get checksum while inserting, sdb: %s", pTable->name);
    pthread_mutex_unlock(&pTable->mutex);
    tfree(rowHead);
    return -1;
  }
  /* write(pTable->fd, &action, sizeof(action)); */
  /* pTable->size += sizeof(action); */
  twrite(pTable->fd, rowHead, total_size);
  pTable->size += total_size;
  sdbFinishCommit(pTable);

  pTable->numOfRows--;
  // TODO: Change the update list here
  sdbAddIntoUpdateList(pTable, SDB_TYPE_DELETE, pMetaRow);
  switch (pTable->keyType) {
    case SDB_KEYTYPE_STRING:
      sdbTrace("table:%s, a record is deleted:%s, sdbVersion:%ld id:%ld numOfRows:%d",
               pTable->name, (char *)row, sdbVersion, pTable->id, pTable->numOfRows);
      break;
    case SDB_KEYTYPE_UINT32:
    case SDB_KEYTYPE_AUTO:
      sdbTrace("table:%s, a record is deleted:%d, sdbVersion:%ld id:%ld numOfRows:%d",
               pTable->name, *(int32_t *)row, sdbVersion, pTable->id, pTable->numOfRows);
      break;
    default:
      sdbTrace("table:%s, a record is deleted, sdbVersion:%ld id:%ld numOfRows:%d", pTable->name, sdbVersion,
               pTable->id, pTable->numOfRows);
      break;
  }

  // Delete from current layer
  (*sdbDeleteIndexFp[pTable->keyType])(pTable->iHandle, row);

  code = 0;

  pthread_mutex_unlock(&pTable->mutex);

  tfree(rowHead);

  // callback function of the delete
  if (code == 0 && pTable->appTool) (*pTable->appTool)(SDB_TYPE_DELETE, pMetaRow, NULL, 0, NULL);

  return code;
}

// row here can be the object or the string info (encoded string)
int sdbUpdateRow(void *handle, void *row, int updateSize, char isUpdated) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta = NULL;
  int        code = -1;
  int        total_size = 0;
  int        real_size = 0;
  /* char       action     = SDB_TYPE_UPDATE; */

  if (pTable == NULL || row == NULL) return -1;
  pMeta = sdbGetRowMeta(handle, row);
  if (pMeta == NULL) {
    sdbTrace("table:%s, record is not there, update failed", pTable->name);
    return -1;
  }

  void *pMetaRow = pMeta->row;
  assert(pMetaRow != NULL);

  total_size = sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM);
  SRowHead *rowHead = (SRowHead *)malloc(total_size);
  if (rowHead == NULL) {
    sdbError("failed to allocate row head memory, sdb: %s", pTable->name);
    return -1;
  }
  memset(rowHead, 0, total_size);

  if (!isUpdated) {
    (*(pTable->appTool))(SDB_TYPE_UPDATE, pMetaRow, row, updateSize, NULL);  // update in upper layer
  }

  if (pMetaRow != row) {
    memcpy(rowHead->data, row, updateSize);
    rowHead->rowSize = updateSize;
  } else {
    (*(pTable->appTool))(SDB_TYPE_ENCODE, pMetaRow, rowHead->data, pTable->maxRowSize, &(rowHead->rowSize));
  }

  real_size = sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM);
  ;

  pthread_mutex_lock(&pTable->mutex);

  pTable->id++;
  sdbVersion++;

  // write to the new position
  rowHead->delimiter = SDB_DELIMITER;
  rowHead->id = pTable->id;
  if (taosCalcChecksumAppend(0, (uint8_t *)rowHead, real_size) < 0) {
    sdbError("failed to get checksum, sdb: %s id: %d", pTable->name, rowHead->id);
    pthread_mutex_unlock(&pTable->mutex);
    tfree(rowHead);
    return -1;
  }
  /* write(pTable->fd, &action, sizeof(action)); */
  /* pTable->size += sizeof(action); */
  twrite(pTable->fd, rowHead, real_size);

  pMeta->id = pTable->id;
  pMeta->offset = pTable->size;
  pMeta->rowSize = rowHead->rowSize;
  pTable->size += real_size;

  sdbFinishCommit(pTable);

  switch (pTable->keyType) {
    case SDB_KEYTYPE_STRING:
      sdbTrace("table:%s, a record is updated:%s, sdbVersion:%ld id:%ld numOfRows:%d",
               pTable->name, (char *)row, sdbVersion, pTable->id, pTable->numOfRows);
      break;
    case SDB_KEYTYPE_UINT32:
    case SDB_KEYTYPE_AUTO:
      sdbTrace("table:%s, a record is updated:%d, sdbVersion:%ld id:%ld numOfRows:%d",
               pTable->name, *(int32_t *)row, sdbVersion, pTable->id, pTable->numOfRows);
      break;
    default:
      sdbTrace("table:%s, a record is updated, sdbVersion:%ld id:%ld numOfRows:%d", pTable->name, sdbVersion,
               pTable->id, pTable->numOfRows);
      break;
  }

  sdbAddIntoUpdateList(pTable, SDB_TYPE_UPDATE, pMetaRow);
  code = 0;

  pthread_mutex_unlock(&pTable->mutex);

  tfree(rowHead);

  return code;
}

// row here must be the instruction string
int sdbBatchUpdateRow(void *handle, void *row, int rowSize) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta = NULL;
  int        total_size = 0;
  /* char        action = SDB_TYPE_BATCH_UPDATE; */

  if (pTable == NULL || row == NULL || rowSize <= 0) return -1;
  pMeta = sdbGetRowMeta(handle, row);
  if (pMeta == NULL) {
    sdbTrace("table: %s, record is not there, batch update failed", pTable->name);
    return -1;
  }

  void *pMetaRow = pMeta->row;
  assert(pMetaRow != NULL);

  total_size = sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM);
  SRowHead *rowHead = (SRowHead *)malloc(total_size);
  if (rowHead == NULL) {
    sdbError("failed to allocate row head memory, sdb: %s", pTable->name);
    return -1;
  }

  pthread_mutex_lock(&pTable->mutex);

  (*(pTable->appTool))(SDB_TYPE_BEFORE_BATCH_UPDATE, pMetaRow, NULL, 0, NULL);

  void *next_row = pMetaRow;
  while (next_row != NULL) {
    pTable->id++;
    sdbVersion++;

    void *last_row = next_row;
    next_row = (*(pTable->appTool))(SDB_TYPE_BATCH_UPDATE, last_row, (char *)row, rowSize, 0);
    memset(rowHead, 0, sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM));

    // update in current layer
    pMeta->id = pTable->id;
    pMeta->offset = pTable->size;

    // write to disk
    rowHead->delimiter = SDB_DELIMITER;
    rowHead->id = pMeta->id;
    (*(pTable->appTool))(SDB_TYPE_ENCODE, last_row, rowHead->data, pTable->maxRowSize, &(rowHead->rowSize));
    taosCalcChecksumAppend(0, (uint8_t *)rowHead, sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM));
    pMeta->rowSize = rowHead->rowSize;
    lseek(pTable->fd, pTable->size, SEEK_SET);
    twrite(pTable->fd, rowHead, sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM));
    pTable->size += (sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM));

    sdbAddIntoUpdateList(pTable, SDB_TYPE_UPDATE, last_row);

    if (next_row != NULL) {
      pMeta = sdbGetRowMeta(handle, next_row);
    }
  }

  sdbFinishCommit(pTable);

  (*(pTable->appTool))(SDB_TYPE_AFTER_BATCH_UPDATE, pMetaRow, NULL, 0, NULL);

  pthread_mutex_unlock(&pTable->mutex);

  tfree(rowHead);

  return 0;
}

/*
 * 关闭系统数据表
 * *handle：指针变量，指向要关闭的系统数据表对象
 */
void sdbCloseTable(void *handle) {
  SSdbTable *pTable = (SSdbTable *)handle;
  void *     pNode = NULL;
  void *     row;

  if (pTable == NULL) return;

  while (1) {
    pNode = sdbFetchRow(handle, pNode, &row);
    if (row == NULL) break;
    (*(pTable->appTool))(SDB_TYPE_DESTROY, row, NULL, 0, NULL);     //操作释放系统数据表
  }

  if (sdbCleanUpIndexFp[pTable->keyType]) (*sdbCleanUpIndexFp[pTable->keyType])(pTable->iHandle);

  if (pTable->fd) tclose(pTable->fd);     //关闭系统数据表文件句柄

  pthread_mutex_destroy(&pTable->mutex);  //系统数据表线程互斥锁销毁

  sdbNumOfTables--;     //系统数据表统计计数
  sdbTrace("table:%s is closed, id:%ld numOfTables:%d", pTable->name, pTable->id, sdbNumOfTables);

  tfree(pTable->update);  //释放内存
  tfree(pTable);
}

void sdbResetTable(SSdbTable *pTable) {
  /* SRowHead rowHead; */
  SRowMeta  rowMeta;
  int       bytes;
  int       total_size = 0;
  int       real_size = 0;
  int64_t   oldId;
  SRowHead *rowHead = NULL;
  void *    pMetaRow = NULL;

  oldId = pTable->id;
  if (sdbOpenSdbFile(pTable) < 0) return;

  total_size = sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM);
  rowHead = (SRowHead *)malloc(total_size);
  if (rowHead == NULL) {
    return;
  }

  while (1) {
    memset(rowHead, 0, total_size);

    bytes = read(pTable->fd, rowHead, sizeof(SRowHead));
    if (bytes < 0) {
      sdbError("failed to read sdb file: %s", pTable->fn);
      tfree(rowHead);
      return;
    }

    if (bytes == 0) break;

    if (bytes < sizeof(SRowHead) || rowHead->delimiter != SDB_DELIMITER) {
      pTable->size++;
      lseek(pTable->fd, -(bytes - 1), SEEK_CUR);
      continue;
    }

    if (rowHead->rowSize < 0 || rowHead->rowSize > pTable->maxRowSize) {
      sdbError("error row size in sdb file: %s  rowSize: %d  maxRowSize: %d", pTable->fn, rowHead->rowSize,
               pTable->maxRowSize);
      pTable->size += sizeof(SRowHead);
      continue;
    }

    bytes = read(pTable->fd, rowHead->data, rowHead->rowSize + sizeof(TSCKSUM));
    if (bytes < rowHead->rowSize + sizeof(TSCKSUM)) {
      sdbError("failed to read sdb file: %s  id: %d  rowSize: %d", pTable->fn, rowHead->id, rowHead->rowSize);
      break;
    }

    real_size = sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM);
    if (!taosCheckChecksumWhole((uint8_t *)rowHead, real_size)) {
      sdbError("error sdb checksum, sdb: %s  id: %d, skip", pTable->name, rowHead->id);
      pTable->size += real_size;
      continue;
    }

    if (abs(rowHead->id) > oldId) {  // not operated
      pMetaRow = sdbGetRow(pTable, rowHead->data);
      if (pMetaRow == NULL) {  // New object
        if (rowHead->id < 0) {
          sdbError("error sdb negative id: %d, sdb: %s, skip", rowHead->id, pTable->name);
        } else {
          rowMeta.id = rowHead->id;
          // TODO: Get rid of the rowMeta.offset and rowSize
          rowMeta.offset = pTable->size;
          rowMeta.rowSize = rowHead->rowSize;
          rowMeta.row = (*(pTable->appTool))(SDB_TYPE_DECODE, NULL, rowHead->data, rowHead->rowSize, NULL);
          (*sdbAddIndexFp[pTable->keyType])(pTable->iHandle, rowMeta.row, &rowMeta);
          pTable->numOfRows++;

          (*pTable->appTool)(SDB_TYPE_INSERT, rowMeta.row, NULL, 0, NULL);
        }
      } else {                  // already exists
        if (rowHead->id < 0) {  // Delete the object
          (*sdbDeleteIndexFp[pTable->keyType])(pTable->iHandle, rowHead->data);
          (*(pTable->appTool))(SDB_TYPE_DESTROY, pMetaRow, NULL, 0, NULL);
          pTable->numOfRows--;
        } else {  // update the object
          (*(pTable->appTool))(SDB_TYPE_UPDATE, pMetaRow, rowHead->data, rowHead->rowSize, NULL);
        }
      }
    }

    pTable->size += real_size;
    if (pTable->id < abs(rowHead->id)) pTable->id = abs(rowHead->id);
  }

  sdbVersion += (pTable->id - oldId);
  pTable->numOfUpdates = 0;
  pTable->updatePos = 0;

  tfree(rowHead);

  sdbTrace("table:%s is updated, sdbVerion:%ld id:%ld", pTable->name, sdbVersion, pTable->id);
}

/*
 *  系统数据表保存快照
 *  *handle：系统数据表对象
 *  TODO: A problem here : use snapshot file to sync another node will cause problem
 */
void sdbSaveSnapShot(void *handle) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta;
  void *     pNode = NULL;
  int        total_size = 0;
  int        real_size = 0;
  int        size = 0;
  int        numOfRows = 0;
  uint32_t   sdbEcommit = SDB_ENDCOMMIT;
  char *     dirc = NULL;
  char *     basec = NULL;
  /* char       action = SDB_TYPE_INSERT; */

  if (pTable == NULL) return;

  sdbTrace("Table:%s, save the snapshop", pTable->name);

  char fn[128] = "\0";
  dirc = strdup(pTable->fn);
  basec = strdup(pTable->fn);
  sprintf(fn, "%s/.%s", dirname(dirc), basename(basec));
  int fd = open(fn, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
  tfree(dirc);
  tfree(basec);

  total_size = sizeof(SRowHead) + pTable->maxRowSize + sizeof(TSCKSUM);
  SRowHead *rowHead = (SRowHead *)malloc(total_size);
  if (rowHead == NULL) {
    sdbError("failed to allocate memory while saving SDB snapshot, sdb: %s", pTable->name);
    return;
  }
  memset(rowHead, 0, size);

  // Write the header
  twrite(fd, &(pTable->header), sizeof(SSdbHeader));
  size += sizeof(SSdbHeader);
  twrite(fd, &sdbEcommit, sizeof(sdbEcommit));
  size += sizeof(sdbEcommit);

  while (1) {
    pNode = (*sdbFetchRowFp[pTable->keyType])(pTable->iHandle, pNode, (void **)&pMeta);
    if (pMeta == NULL) break;

    rowHead->delimiter = SDB_DELIMITER;
    rowHead->id = pMeta->id;
    (*(pTable->appTool))(SDB_TYPE_ENCODE, pMeta->row, rowHead->data, pTable->maxRowSize, &(rowHead->rowSize));
    real_size = sizeof(SRowHead) + rowHead->rowSize + sizeof(TSCKSUM);
    if (taosCalcChecksumAppend(0, (uint8_t *)rowHead, real_size) < 0) {
      sdbError("failed to get checksum while save sdb %s snapshot", pTable->name);
      tfree(rowHead);
      return;
    }

    /* write(fd, &action, sizeof(action)); */
    /* size += sizeof(action); */
    twrite(fd, rowHead, real_size);
    size += real_size;
    twrite(fd, &sdbEcommit, sizeof(sdbEcommit));
    size += sizeof(sdbEcommit);
    numOfRows++;
  }

  tfree(rowHead);

  // Remove the old file
  tclose(pTable->fd);
  remove(pTable->fn);
  // Rename the .sdb.db file to sdb.db file
  rename(fn, pTable->fn);
  pTable->fd = fd;
  pTable->size = size;
  pTable->numOfRows = numOfRows;

  fdatasync(pTable->fd);
}

/*
 * 系统数据库表匹配到具体的行（DB数据库）
 * *handle：指针指向系统数据库表对象
 * *pNode：
 * **PPRow：指针变量，指针匹配的DB数据库的指针
 */
void *sdbFetchRow(void *handle, void *pNode, void **ppRow) {
  SSdbTable *pTable = (SSdbTable *)handle;
  SRowMeta * pMeta;

  *ppRow = NULL;
  if (pTable == NULL) return NULL;

  //按照keyType匹配系统数据库表的行
  pNode = (*sdbFetchRowFp[pTable->keyType])(pTable->iHandle, pNode, (void **)&pMeta);
  if (pMeta == NULL) return NULL;

  *ppRow = pMeta->row;  //匹配到的系统数据库表的行（业务DB数据库）

  return pNode;
}

int64_t sdbGetId(void *handle) { return ((SSdbTable *)handle)->id; }

/*
 * 从sdbTable获取行数，表示Db计数
 * *handle：指向sdbTable对象
 */
int64_t sdbGetNumOfRows(void *handle) { return ((SSdbTable *)handle)->numOfRows; }
