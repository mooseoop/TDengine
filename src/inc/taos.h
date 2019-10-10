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


/*
*TDengine的C语言Connector API头文件
*/

#ifndef TDENGINE_TAOS_H
#define TDENGINE_TAOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAOS void
#define TAOS_ROW void **
#define TAOS_RES void
#define TAOS_SUB void
#define TAOS_STREAM void

#define TSDB_DATA_TYPE_NULL       0
#define TSDB_DATA_TYPE_BOOL       1     // 1 bytes
#define TSDB_DATA_TYPE_TINYINT    2     // 1 byte
#define TSDB_DATA_TYPE_SMALLINT   3     // 2 bytes
#define TSDB_DATA_TYPE_INT        4     // 4 bytes
#define TSDB_DATA_TYPE_BIGINT     5     // 8 bytes
#define TSDB_DATA_TYPE_FLOAT      6     // 4 bytes
#define TSDB_DATA_TYPE_DOUBLE     7     // 8 bytes
#define TSDB_DATA_TYPE_BINARY     8     // string
#define TSDB_DATA_TYPE_TIMESTAMP  9     // 8 bytes
#define TSDB_DATA_TYPE_NCHAR      10    // multibyte string

typedef enum {
  TSDB_OPTION_LOCALE,
  TSDB_OPTION_CHARSET,
  TSDB_OPTION_TIMEZONE,
  TSDB_OPTION_CONFIGDIR,
  TSDB_OPTION_SHELL_ACTIVITY_TIMER,
  TSDB_MAX_OPTIONS
} TSDB_OPTION;

typedef struct taosField {
  char  name[64];
  short bytes;
  char  type;
} TAOS_FIELD;

//初始化环境变量。若没有主动调用，在调用创建数据库连接时会自动调用。
void taos_init();

//设置客户端选项。目前支持时区设置（TSDB_OPTION_TIMEZONE）和编码设置（TSDB_OPTION_LOCALE）；
int taos_options(TSDB_OPTION option, const void *arg, ...);

//创建数据库连接
TAOS *taos_connect(char *ip, char *user, char *pass, char *db, int port);

//关闭数据库连接
void taos_close(TAOS *taos);

//执行SQL语句，返回-1表示失败
int taos_query(TAOS *taos, char *sqlstr);

//选择相应的查询结果集
TAOS_RES *taos_use_result(TAOS *taos);

//按行获取查询结果集中的数据。
TAOS_ROW taos_fetch_row(TAOS_RES *res);

int taos_result_precision(TAOS_RES *res);  // get the time precision of result

//释放查询结果集以及相关的资源，查询完成后务必调用该API释放资源，避免应用内存泄露。
void taos_free_result(TAOS_RES *res);
int taos_field_count(TAOS *taos);

//获取查询结果集中的列数
int taos_num_fields(TAOS_RES *res);
int taos_affected_rows(TAOS *taos);

//获取查询结果集每列数据的属性（数据类型、名字、字节数）
TAOS_FIELD *taos_fetch_fields(TAOS_RES *res);
int taos_select_db(TAOS *taos, char *db);
int taos_print_row(char *str, TAOS_ROW row, TAOS_FIELD *fields, int num_fields);
void taos_stop_query(TAOS_RES *res);

int taos_fetch_block(TAOS_RES *res, TAOS_ROW *rows);
int taos_validate_sql(TAOS *taos, char *sql);

// TAOS_RES   *taos_list_tables(TAOS *mysql, const char *wild);
// TAOS_RES   *taos_list_dbs(TAOS *mysql, const char *wild);

char *taos_get_server_info(TAOS *taos);
char *taos_get_client_info();
char *taos_errstr(TAOS *taos);  //获取最近一次API调用失败的原因，返回值为字符串
int taos_errno(TAOS *taos);     //获取最近一次API调用失败的错误代码。

/*
*异步API函数,异步执行SQL语句
*异步API采用非阻塞调用模式。
*taos：是创建的数据库连接结构体指针
*sqlstr：是需要执行的SQL语句
*fp：是用户定义的回调函数指针；
*    param：是应用提供一个用于回调的参数
*    TAOS_RES:该参数返回查询的结果集指针
*    code：用于指示操作是否成功，0表示成功，负数表示失败（调用taos_errstr获取失败原因）
*/
void taos_query_a(TAOS *taos, char *sqlstr, void (*fp)(void *param, TAOS_RES *, int code), void *param);

/*
*异步API函数，批量获取异步查询的结果集，只能和taos_query_a配合使用
*res：是taos_query_a回调时返回的结果集结构体指针
*fp：回调函数
*     param：用户可定义的传递给回调函数的参数结构体指针
*     numOfRows：表明有fetch数据返回的行数，不是本次查询满足条件的全部元组数量。
*
*在回调函数中，应用可以通过调用taos_fetch_row前向迭代获取批量记录中每一行记录。
*读完一块内的所有记录后，应用需要在回调函数中继续调用taos_fetch_rows_a获取下一批记录进行处理，
*直到返回的记录数（numOfRows）为零（结果返回完成）或记录数为负值（查询出错）。
*/
void taos_fetch_rows_a(TAOS_RES *res, void (*fp)(void *param, TAOS_RES *, int numOfRows), void *param);

/*
*异步获取一条记录
*res：是taos_query_a回调时返回的结果集结构体指针。
*fp：为回调函数。
*     param：是应用提供的一个用于回调的参数。
*     TAOS_ROW：回调时，第三个参数TAOS_ROW指向一行记录。
*/
void taos_fetch_row_a(TAOS_RES *res, void (*fp)(void *param, TAOS_RES *, TAOS_ROW row), void *param);

/*
*数据订阅接口
*/

/*
*该API用来启动订阅
*host：管理主节点IP地址；user：用户名；pass：密码；db：数据库；table：表名
*time：开始订阅消息的时间1970/1/1起的毫秒数
*mseconds：查询数据库更新的时间间隔，毫秒数，建议1000毫秒
*返回值为指向TAOS_SUB结构的指针，返回为空，表示失败。
*/
TAOS_SUB *taos_subscribe(char *host, char *user, char *pass, char *db, char *table, int64_t time, int mseconds);

/*
*该API用来获取订阅的最新消息，应用程序一般会将其置于一个无限循环语句中。
*tsub：是订阅api的返回值
*如果数据库有新的记录，该API将返回，返回值参数是一行记录。
*如果没有新的记录，该API将阻塞。
*如果返回值为空，表示系统出错。
*/
TAOS_ROW taos_consume(TAOS_SUB *tsub);

/*
*该API用来取消订阅
*tsub：订阅的返回值
*/
void taos_unsubscribe(TAOS_SUB *tsub);

/*
*用来获取返回的一排数据中数据的列数
*/
int taos_subfields_count(TAOS_SUB *tsub);

/*
*该API用来获取每列数据的属性（数据类型、名字、字节数）
*/
TAOS_FIELD *taos_fetch_subfields(TAOS_SUB *tsub);

/*
*创建数据流
*   TDengine提供时间驱动的实时流式计算API。
*   可以每隔一个指定时间段，对数据库的表进行各种实时聚合计算操作。
*taos：是创建的数据库连接的结构体指针
*sqlstr：是SQL查询语句，仅能使用查询语句
*fp：是用户定义的回调函数指针，每次流式计算完成后，均回调该函数，用户在该函数内定义业务逻辑
*     param：是应用提供的用于回调的一个参数，回调时，提供给应用。
*     ...
*stime:是流式计算开始的时间，如果是0，表示从现在开始，不为零，表示从指定时间开始计算（1970/1/1起的毫秒数）
*TDengine将查询的结果（TAOS_ROW）、查询状态（TAOS_RES）、用户定义参数（param）传递给回调函数。
*在回调函数内，用户可以使用taos_num_fields获取结果集列数，taos_fetch_fields获取结果集每列数据的类型。
*/
TAOS_STREAM *taos_open_stream(TAOS *taos, char *sqlstr, void (*fp)(void *param, TAOS_RES *, TAOS_ROW row),
                              int64_t stime, void *param, void (*callback)(void *));

/*
*关闭数据流
*tstr：是创建数据流时返回的值
*/
void taos_close_stream(TAOS_STREAM *tstr);

extern char configDir[];  // the path to global configuration

#ifdef __cplusplus
}
#endif

#endif
