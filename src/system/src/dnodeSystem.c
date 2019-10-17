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

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "mgmt.h"
#include "vnode.h"

#include "dnodeSystem.h"
#include "httpSystem.h"
#include "monitorSystem.h"
#include "tcrc32c.h"
#include "tglobalcfg.h"
#include "vnode.h"

SModule         tsModule[TSDB_MOD_MAX];
uint32_t        tsModuleStatus;
pthread_mutex_t dmutex;
extern int      vnodeSelectReqNum;
extern int      vnodeInsertReqNum;
bool            tsDnodeStopping = false;

void dnodeCountRequest(SCountInfo *info);

/*
* 初始化TD系统的模块
* 有http模块/monitor模块
*/
void dnodeInitModules() {
  tsModule[TSDB_MOD_HTTP].name = "http";
  tsModule[TSDB_MOD_HTTP].initFp = httpInitSystem;
  tsModule[TSDB_MOD_HTTP].cleanUpFp = httpCleanUpSystem;
  tsModule[TSDB_MOD_HTTP].startFp = httpStartSystem;
  tsModule[TSDB_MOD_HTTP].stopFp = httpStopSystem;
  tsModule[TSDB_MOD_HTTP].num = (tsEnableHttpModule == 1) ? -1 : 0;
  tsModule[TSDB_MOD_HTTP].curNum = 0;
  tsModule[TSDB_MOD_HTTP].equalVnodeNum = 0;

  tsModule[TSDB_MOD_MONITOR].name = "monitor";
  tsModule[TSDB_MOD_MONITOR].initFp = monitorInitSystem;
  tsModule[TSDB_MOD_MONITOR].cleanUpFp = monitorCleanUpSystem;
  tsModule[TSDB_MOD_MONITOR].startFp = monitorStartSystem;
  tsModule[TSDB_MOD_MONITOR].stopFp = monitorStopSystem;
  tsModule[TSDB_MOD_MONITOR].num = (tsEnableMonitorModule == 1) ? -1 : 0;
  tsModule[TSDB_MOD_MONITOR].curNum = 0;
  tsModule[TSDB_MOD_MONITOR].equalVnodeNum = 0;
}

void dnodeCleanUpSystem() {
  if (tsDnodeStopping) return;
  tsDnodeStopping = true;

  for (int mod = 0; mod < TSDB_MOD_MAX; ++mod) {
    if (tsModule[mod].num != 0 && tsModule[mod].stopFp) (*tsModule[mod].stopFp)();
    if (tsModule[mod].num != 0 && tsModule[mod].cleanUpFp) (*tsModule[mod].cleanUpFp)();
  }

  mgmtCleanUpSystem();
  vnodeCleanUpVnodes();

  taosCloseLogger();
}

/*
* 创建TD的数据文件目录
*/
void taosCreateTierDirectory() {
  char fileName[128];

  sprintf(fileName, "%s/tsdb", tsDirectory);
  mkdir(fileName, 0755);

  sprintf(fileName, "%s/data", tsDirectory);
  mkdir(fileName, 0755);
}

/*
* 创建TD数据库运行文件.running
* 文件为只写模式，建立互斥锁定，限定被一个进程访问
*/
void dnodeCheckDbRunning(const char* dir) {
  char filepath[256] = {0};
  sprintf(filepath, "%s/.running", dir);  //创建数据库运行文件.running
  int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO); //创建只写文件.running
  int ret = flock(fd, LOCK_EX | LOCK_NB);   //建立互斥锁定，一个文件只有一个互斥锁定，被一个进程访问。
  if (ret != 0) {
    dError("failed to lock file:%s ret:%d, database may be running, quit", filepath, ret);
    exit(0);
  }
}

/*
* TDengine系统初始化
*/
int dnodeInitSystem() {
  char        temp[128];
  struct stat dirstat;  //文件状态信息的结构体

  taosResolveCRC();

  tsRebootTime = taosGetTimestampSec();   //获取时间戳，赋值给全局变量，记录系统启动时间。
  tscEmbedded = 1;

  /* Read global configuration.
  * 读取全局log类的配置项
  */
  tsReadGlobalLogConfig();

  /*
  * 检测日志文件的状态信息
  * 文件信息保存在dirstat结构体中
  */
  if (stat(logDir, &dirstat) < 0) mkdir(logDir, 0755);    //创建TD日志目录

  /*
  * 初始化TD的日志文件 
  */
  sprintf(temp, "%s/taosdlog", logDir);
  if (taosInitLog(temp, tsNumOfLogLines, 1) < 0) printf("failed to init log file\n");

  if (!tsReadGlobalConfig()) {  // TODO : Change this function
    tsPrintGlobalConfig();
    dError("TDengine read global config failed");
    return -1;
  }

  strcpy(tsDirectory, dataDir); //把TD的数据目录复制到tsDirectory
  if (stat(dataDir, &dirstat) < 0) {
    mkdir(dataDir, 0755);             //检测数据目录，不存在则创建数据目录
  }

  taosCreateTierDirectory();    //创建TD的数据库文件tsdb和

  sprintf(mgmtDirectory, "%s/mgmt", tsDirectory);   //设置TD管理目录
  sprintf(tsDirectory, "%s/tsdb", dataDir);         
  dnodeCheckDbRunning(dataDir);   //创建TD数据物理节点文件.running并加互斥锁定，限定一个进程访问，.running文件是只写文件

  tsPrintGlobalConfig();
  dPrint("Server IP address is:%s", tsInternalIp);

  signal(SIGPIPE, SIG_IGN);   //系统调用，一种特殊的中断；SIGPIPE：管道破裂，写一个没有读端口的管道。

  dnodeInitModules(); //初始化物理节点的http模块和monitor模块
  pthread_mutex_init(&dmutex, NULL);  //线程互斥锁初始化

  dPrint("starting to initialize TDengine engine ...");

  //循环初始化TD系统的模块http模块和monitor模块
  for (int mod = 0; mod < TSDB_MOD_MAX; ++mod) {
    if (tsModule[mod].num != 0 && tsModule[mod].initFp) {
      if ((*tsModule[mod].initFp)() != 0) {
        dError("TDengine initialization failed");
        return -1;
      }
    }
  }

  if (vnodeInitSystem() != 0) {
    dError("TDengine vnodes initialization failed");
    return -1;
  }

  if (mgmtInitSystem() != 0) {
    dError("TDengine mgmt initialization failed");
    return -1;
  }

  monitorCountReqFp = dnodeCountRequest;

  for (int mod = 0; mod < TSDB_MOD_MAX; ++mod) {
    if (tsModule[mod].num != 0 && tsModule[mod].startFp) {
      if ((*tsModule[mod].startFp)() != 0) {
        dError("failed to start TDengine module:%d", mod);
        return -1;
      }
    }
  }

  dPrint("TDengine is initialized successfully");

  return 0;
}

void dnodeCountRequest(SCountInfo *info) {
  httpGetReqCount(&info->httpReqNum);
  info->selectReqNum = __sync_fetch_and_and(&vnodeSelectReqNum, 0);
  info->insertReqNum = __sync_fetch_and_and(&vnodeInsertReqNum, 0);
}
