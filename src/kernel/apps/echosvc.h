#ifndef APPS_ECHOSVC_H
#define APPS_ECHOSVC_H

void echosvc_entry(void);
/* 请求当前 echo 服务退出（supervisor 重启测试用）。 */
void echosvc_request_stop(void);

#endif
