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
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "tsdb.h"
#include "tsocket.h"
#include "vnode.h"

/* internal global, not configurable
* 内部全局变量，不是来自配置
*/
void *   vnodeTmrCtrl;    //vnode定时器控制对象指针
void **  rpcQhandle;      //全局指针，指向donde的rpc消息处理任务队列
void *   dmQhandle;       //全局指针，指向dnode管理的mgmt任务队列
void *   queryQhandle;    //全局指针，指向query任务队列
int      tsMaxQueues;     //最大rpc任务任务队列数
uint32_t tsRebootTime;    //系统启动时间戳

/*
* vnode节点初始化系统
*/
int vnodeInitSystem() {
  int numOfThreads;

  //线程数 = tsRatioOfQueryThreads * 系统核数（来自系统信息） * 每核线程数（来自配置信息）
  numOfThreads = tsRatioOfQueryThreads * tsNumOfCores * tsNumOfThreadsPerCore;  
  if (numOfThreads < 1) numOfThreads = 1;
  
  /*
  * 初始化query任务队列，返回任务队列的地址
  * 任务队列大小=每核vnode数 * 系统核数 * 每vnode的会话数
  */
  queryQhandle = taosInitScheduler(tsNumOfVnodesPerCore * tsNumOfCores * tsSessionsPerVnode, numOfThreads, "query");

  tsMaxQueues = (1.0 - tsRatioOfQueryThreads) * tsNumOfCores * tsNumOfThreadsPerCore / 2.0;
  if (tsMaxQueues < 1) tsMaxQueues = 1;

  //rpc任务处理队列动态内存分配
  rpcQhandle = malloc(tsMaxQueues*sizeof(void *));
  for (int i = 0; i < tsMaxQueues; i++)
    //dnode任务队列初始化，任务队列大小是每vnode的会话数，1个线程
    rpcQhandle[i] = taosInitScheduler(tsSessionsPerVnode, 1, "dnode");    

  //mgmt任务队列初始化，任务队列大小是每vnode的会话数，1个线程
  dmQhandle = taosInitScheduler(tsSessionsPerVnode, 1, "mgmt");       

  //DND-vnode定时器控制单元初始化，会话数+1000个定时器，定时器精度200毫秒，最大60000毫秒，返回vnode定时器控制对象指针
  vnodeTmrCtrl = taosTmrInit(tsSessionsPerVnode + 1000, 200, 60000, "DND-vnode");   
  if (vnodeTmrCtrl == NULL) {
    dError("failed to init timer, exit");
    return -1;
  }

  if (vnodeInitStore() < 0) {
    dError("failed to init vnode storage");
    return -1;
  }

  if (vnodeInitShell() < 0) {
    dError("failed to init communication to shell");
    return -1;
  }

  if (vnodeInitVnodes() < 0) {
    dError("failed to init store");
    return -1;
  }

  dPrint("vnode is initialized successfully");

  return 0;
}
