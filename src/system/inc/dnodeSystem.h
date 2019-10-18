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

#ifndef TDENGINE_DNODESYSTEM_H
#define TDENGINE_DNODESYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

enum _module { TSDB_MOD_HTTP, TSDB_MOD_MONITOR, TSDB_MOD_MAX };   //不同模块的枚举

/*
* 模块对象
*/
typedef struct {
  char  *name;              //模块名称
  int  (*initFp)();         //模块初始化函数指针
  void (*cleanUpFp)();      //模块清除函数指针
  int  (*startFp)();        //模块启动函数指针
  void (*stopFp)();         //模块停止函数指针
  int    num;
  int    curNum;
  int    equalVnodeNum;
} SModule;

extern uint32_t        tsModuleStatus;
extern SModule         tsModule[];
extern pthread_mutex_t dmutex;

void dnodeCleanUpSystem();
int  dnodeInitSystem();

int vnodeInitSystem();

int  mgmtInitSystem();
void mgmtCleanUpSystem();

#ifdef __cplusplus
}
#endif

#endif
