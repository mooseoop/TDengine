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

#define _GNU_SOURCE /* See feature_test_macros(7) */
#include <fcntl.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tsdb.h"
#include "vnode.h"
#include "vnodeUtil.h"

/*
 * commit头信息
 */
typedef struct {
  int  sversion;  //version信息
  int  sid;       //session id信息
  int  contLen;   //commit内容长度
  int  action:8;  //commit的操作信息，占8个位域，1个字节
  int  simpleCheck:24;  //简单校验信息（head信息的求和然后和0xFF相与），占24个位域，3个字节
} SCommitHead;

int vnodeOpenCommitLog(int vnode, uint64_t firstV) {
  SVnodeObj *pVnode = vnodeList + vnode;
  char *     fileName = pVnode->logFn;

  pVnode->logFd = open(fileName, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
  if (pVnode->logFd < 0) {
    dError("vid:%d, failed to open file:%s, reason:%s", vnode, fileName, strerror(errno));
    return -1;
  }

  dTrace("vid:%d, logfd:%d, open file:%s success", vnode, pVnode->logFd, fileName);
  if (posix_fallocate64(pVnode->logFd, 0, pVnode->mappingSize) != 0) {
    dError("vid:%d, logfd:%d, failed to alloc file size:%d reason:%s", vnode, pVnode->logFd, pVnode->mappingSize, strerror(errno));
    perror("fallocate failed");
    goto _err_log_open;
  }

  struct stat statbuf;
  stat(fileName, &statbuf);
  int64_t length = statbuf.st_size;

  if (length != pVnode->mappingSize) {
    dError("vid:%d, logfd:%d, alloc file size:%ld not equal to mapping size:%ld", vnode, pVnode->logFd, length,
           pVnode->mappingSize);
    goto _err_log_open;
  }

  pVnode->pMem = mmap(0, pVnode->mappingSize, PROT_WRITE | PROT_READ, MAP_SHARED, pVnode->logFd, 0);
  if (pVnode->pMem == MAP_FAILED) {
    dError("vid:%d, logfd:%d, failed to map file, reason:%s", vnode, pVnode->logFd, strerror(errno));
    goto _err_log_open;
  }

  pVnode->pWrite = pVnode->pMem;
  memcpy(pVnode->pWrite, &(firstV), sizeof(firstV));
  pVnode->pWrite += sizeof(firstV);

  return pVnode->logFd;

  _err_log_open:
  close(pVnode->logFd);
  remove(fileName);
  pVnode->logFd = -1;
  return -1;
}

int vnodeRenewCommitLog(int vnode) {
  SVnodeObj *pVnode = vnodeList + vnode;
  char *     fileName = pVnode->logFn;
  char *     oldName = pVnode->logOFn;

  pthread_mutex_lock(&(pVnode->logMutex));

  if (VALIDFD(pVnode->logFd)) {
    munmap(pVnode->pMem, pVnode->mappingSize);
    close(pVnode->logFd);
    rename(fileName, oldName);
  }

  if (pVnode->cfg.commitLog) vnodeOpenCommitLog(vnode, vnodeList[vnode].version);

  pthread_mutex_unlock(&(pVnode->logMutex));

  return pVnode->logFd;
}

void vnodeRemoveCommitLog(int vnode) { remove(vnodeList[vnode].logOFn); }

/*
 * 从submit olog文件中恢复vnode的commit数据
 * vnode：vnode 节点id
 * *fileNmae：指针变量，指向submit log文件
 * *firstV：
 */
size_t vnodeRestoreDataFromLog(int vnode, char *fileName, uint64_t *firstV) {
  int    fd, ret;
  char * cont = NULL;
  size_t totalLen = 0;
  int    actions = 0;

  SVnodeObj *pVnode = vnodeList + vnode;    //指针变量*pVnode，指向vnodelist列表对象的指定vnode节点
  if (pVnode->meterList == NULL) {
    dError("vid:%d, vnode is not initialized!!!", vnode);
    return 0;
  }

  struct stat fstat;
  if (stat(fileName, &fstat) < 0) {   //获取fileNmae的文件状态到&fstat，是否失败
    dTrace("vid:%d, no log file:%s", vnode, fileName);
    return 0;
  }

  dTrace("vid:%d, uncommitted data in file:%s, restore them ...", vnode, fileName);

  fd = open(fileName, O_RDWR);    //打开submit olog为提交的数据文件，以读写方式打开
  if (fd < 0) {
    dError("vid:%d, failed to open:%s, reason:%s", vnode, fileName, strerror(errno));
    goto _error;
  }

  ret = read(fd, firstV, sizeof(pVnode->version));    //从submit olog文件中读取vnode的version信息
  if (ret <= 0) {
    dError("vid:%d, failed to read version", vnode);
    goto _error;
  }
  pVnode->version = *firstV;    //把从文件中读取的值设置为version。

  int32_t bufLen = TSDB_PAYLOAD_SIZE;   
  cont = calloc(1, bufLen);     //分配并初始化为0内存，1个长度bufLen
  if (cont == NULL) {
    dError("vid:%d, out of memory", vnode);
    goto _error;
  }

  TSKEY now = taosGetTimestamp(pVnode->cfg.precision);
  SCommitHead head;
  int simpleCheck = 0;
  while (1) {
    ret = read(fd, &head, sizeof(head));  //从submit olog文件中读取commit head信息
    if (ret < 0) goto _error;
    if (ret == 0) break;    //没有读到内容，退出读文件循环
    
    //读取的commit head信息求和，然后和0xFF相与，不相等则退出，判断文件信息是否被非法修改
    if (((head.sversion+head.sid+head.contLen+head.action) & 0xFFFFFF) != head.simpleCheck) break;    
    simpleCheck = head.simpleCheck;

    // head.contLen validation is removed
    if (head.sid >= pVnode->cfg.maxSessions || head.sid < 0 || head.action >= TSDB_ACTION_MAX) {
      //文件读取的session id大于最大会话，或者sid<0，或者文件读取的action >= Action最大值，非法值
      dError("vid, invalid commit head, sid:%d contLen:%d action:%d", head.sid, head.contLen, head.action);
    } else {
      if (head.contLen > 0) {
        if (bufLen < head.contLen+sizeof(simpleCheck)) {  // pre-allocated buffer is not enough
          cont = realloc(cont, head.contLen+sizeof(simpleCheck));
          bufLen = head.contLen+sizeof(simpleCheck);
        }

        if (read(fd, cont, head.contLen+sizeof(simpleCheck)) < 0) goto _error;    //继续读取submit olog文件，长度为commit内容长度+简单校验长度
        if (*(int *)(cont+head.contLen) != simpleCheck) break;
        SMeterObj *pObj = pVnode->meterList[head.sid];    //指针变量指向vnode节点测量对象列表的指定sid的测量对象
        if (pObj == NULL) {
          dError("vid:%d, sid:%d not exists, ignore data in commit log, contLen:%d action:%d",  //从commit log获取的sid无效，跳过
              vnode, head.sid, head.contLen, head.action);
          continue;
        }

        if (vnodeIsMeterState(pObj, TSDB_METER_STATE_DELETING)) { //测量任务对象的状态时deleting 或者 deleted
          dWarn("vid:%d sid:%d id:%s, meter is dropped, ignore data in commit log, contLen:%d action:%d",
                 vnode, head.sid, head.contLen, head.action);
          continue;
        }

        int32_t numOfPoints = 0;
        (*vnodeProcessAction[head.action])(pObj, cont, head.contLen, TSDB_DATA_SOURCE_LOG, NULL, head.sversion,
                                           &numOfPoints, now);
        actions++;
      } else {
        break;
      }
    }

    totalLen += sizeof(head) + head.contLen + sizeof(simpleCheck);
  }

  tclose(fd);
  tfree(cont);
  dTrace("vid:%d, %d pieces of uncommitted data are restored", vnode, actions);

  return totalLen;

_error:
  tclose(fd);
  tfree(cont);
  dError("vid:%d, failed to restore %s, remove this node...", vnode, fileName);

  // rename to error file for future process
  char *f = NULL;
  taosFileRename(fileName, "error", '/', &f);
  free(f);

  return -1;
}

/*
 * 初始化vnode节点Commit
 */
int vnodeInitCommit(int vnode) {
  size_t     size = 0;
  uint64_t   firstV = 0;
  SVnodeObj *pVnode = vnodeList + vnode;

  pthread_mutex_init(&(pVnode->logMutex), NULL);    //vnode的线程互斥锁初始化

  sprintf(pVnode->logFn, "%s/vnode%d/db/submit%d.log", tsDirectory, vnode, vnode);  //submit日志文件，/var/lib/taos/vnode1/db/submit1.log
  sprintf(pVnode->logOFn, "%s/vnode%d/db/submit%d.olog", tsDirectory, vnode, vnode);  //submit old日志文件，/var/lib/taos/vnode1/db/submit1.olog
  pVnode->mappingSize = ((int64_t)pVnode->cfg.cacheBlockSize) * pVnode->cfg.cacheNumOfBlocks.totalBlocks * 1.5;   //vnode节点缓存总大小
  pVnode->mappingThreshold = pVnode->mappingSize * 0.7;   //vnode节点缓存门槛大小，比如超过门槛值进行告警等

  // restore from .olog file and commit to file
  size = vnodeRestoreDataFromLog(vnode, pVnode->logOFn, &firstV);
  if (size < 0) return -1;
  if (size > 0) {
    if (pVnode->commitInProcess == 0) vnodeCommitToFile(pVnode);
    remove(pVnode->logOFn);
  }

  // restore from .log file to cache
  size = vnodeRestoreDataFromLog(vnode, pVnode->logFn, &firstV);
  if (size < 0) return -1;

  if (pVnode->cfg.commitLog == 0) return 0;

  if (size == 0) firstV = pVnode->version;
  if (vnodeOpenCommitLog(vnode, firstV) < 0) {
    dError("vid:%d, commit log init failed", vnode);
    return -1;
  }

  pVnode->pWrite += size;
  dTrace("vid:%d, commit log is initialized", vnode);

  return 0;
}

void vnodeCleanUpCommit(int vnode) {
  SVnodeObj *pVnode = vnodeList + vnode;

  if (VALIDFD(pVnode->logFd)) close(pVnode->logFd);

  if (pVnode->cfg.commitLog && (pVnode->logFd > 0 && remove(pVnode->logFn) < 0)) {
    dError("vid:%d, failed to remove:%s", vnode, pVnode->logFn);
    taosLogError("vid:%d, failed to remove:%s", vnode, pVnode->logFn);
  }

  pthread_mutex_destroy(&(pVnode->logMutex));
}

/*
* vnode记录commit日志
*/
int vnodeWriteToCommitLog(SMeterObj *pObj, char action, char *cont, int contLen, int sverion) {
  SVnodeObj *pVnode = vnodeList + pObj->vnode;
  if (pVnode->pWrite == NULL) return 0;

  SCommitHead head;
  head.sid = pObj->sid;
  head.action = action;
  head.sversion = pObj->sversion;
  head.contLen = contLen;
  head.simpleCheck = (head.sversion+head.sid+head.contLen+head.action) & 0xFFFFFF;
  int simpleCheck = head.simpleCheck;

  pthread_mutex_lock(&(pVnode->logMutex));
  // 100 bytes redundant mem space
  if (pVnode->mappingSize - (pVnode->pWrite - pVnode->pMem) < contLen + sizeof(SCommitHead) + sizeof(simpleCheck) + 100) {
    pthread_mutex_unlock(&(pVnode->logMutex));
    dTrace("vid:%d, mem mapping space is not enough, wait for commit", pObj->vnode);
    vnodeProcessCommitTimer(pVnode, NULL);
    return TSDB_CODE_ACTION_IN_PROGRESS;
  }
  char *pWrite = pVnode->pWrite;
  pVnode->pWrite += sizeof(head) + contLen + sizeof(simpleCheck);
  memcpy(pWrite, (char *)&head, sizeof(head));
  memcpy(pWrite + sizeof(head), cont, contLen);
  memcpy(pWrite + sizeof(head) + contLen, &simpleCheck, sizeof(simpleCheck));
  pthread_mutex_unlock(&(pVnode->logMutex));

  if (pVnode->pWrite - pVnode->pMem > pVnode->mappingThreshold) {
    dTrace("vid:%d, mem mapping is close to limit, commit", pObj->vnode);
    vnodeProcessCommitTimer(pVnode, NULL);
  }

  dTrace("vid:%d sid:%d, data is written to commit log", pObj->vnode, pObj->sid);

  return 0;
}
