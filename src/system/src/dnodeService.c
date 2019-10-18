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

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <unistd.h>
#include <wordexp.h>

#include "dnodeSystem.h"
#include "tglobalcfg.h"
#include "tsdb.h"
#include "vnode.h"

/* Termination handler 
* 终止处理
*/
void signal_handler(int signum, siginfo_t *sigInfo, void *context) {
  if (signum == SIGUSR1) {
    tsCfgDynamicOptions("debugFlag 135");
    return;
  }
  if (signum == SIGUSR2) {
    tsCfgDynamicOptions("resetlog");
    return;
  }
  syslog(LOG_INFO, "Shut down signal is %d", signum);     //日志输出程序终止信号
  syslog(LOG_INFO, "Shutting down TDengine service...");
  // clean the system.
  dPrint("shut down signal is %d, sender PID:%d", signum, sigInfo->si_pid);
  dnodeCleanUpSystem();   //TDengine系统释放
  // close the syslog
  syslog(LOG_INFO, "Shut down TDengine service successfully");
  dPrint("TDengine is shut down!");
  closelog();
  exit(EXIT_SUCCESS);   //终止主程序运行，正常退出
}

int main(int argc, char *argv[]) {
  /* Set global configuration file
  * 设置全局配置文件目录
  */
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-c") == 0) {
      if (i < argc - 1) {
        strcpy(configDir, argv[++i]);     //从启动参数中获取TD配置文件目录，赋值给configDir
      } else {
        printf("'-c' requires a parameter, default:%s\n", configDir);
        exit(EXIT_FAILURE);   //程序异常退出
      }
    } else if (strcmp(argv[i], "-V") == 0) {
      printf("version: %s compatible_version: %s\n", version, compatible_version);
      printf("gitinfo: %s\n", gitinfo);
      printf("buildinfo: %s\n", buildinfo);
      return 0;
    }
  }

  /* Set termination handler. 
  * 设置程序终止处理
  */
  struct sigaction act;   //信号处理方式结构定义
  act.sa_flags = SA_SIGINFO;  //指定信号flags
  act.sa_sigaction = signal_handler;  //指向终止处理函数

  //注册修改指定信号（SIGTERM）相关联的处理动作，参数1：要蒱获的信号类型，参数2：指定新的信号处理方式，参数3：输出先前信号的处理方式
  sigaction(SIGTERM, &act, NULL);   //发送给本程序的终止请求信号
  sigaction(SIGHUP, &act, NULL);    //挂起信号
  sigaction(SIGINT, &act, NULL);    //中断信号，如Ctrl-C
  sigaction(SIGUSR1, &act, NULL);   
  sigaction(SIGUSR2, &act, NULL);
  // sigaction(SIGABRT, &act, NULL);  程序异常终止信号

  // Open /var/log/syslog file to record information.
  openlog("TDengine:", LOG_PID | LOG_CONS | LOG_NDELAY, LOG_LOCAL1);  //创建log文件，记录日志信息
  syslog(LOG_INFO, "Starting TDengine service...");   //启动TDengine 服务，输出日志信息

  /* Initialize the system
  * TDengine系统初始化
  */
  if (dnodeInitSystem() < 0) {
    syslog(LOG_ERR, "Error initialize TDengine system");  //TDengine系统初始化失败
    closelog();

    dnodeCleanUpSystem();   //TDengine系统释放
    exit(EXIT_FAILURE);     //终止主程序执行，异常推出
  }

  syslog(LOG_INFO, "Started TDengine service successfully.");   //TDengine系统启动成功

  while (1) {
    sleep(1000);
  }
}
