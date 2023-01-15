#ifndef __UI_HTTP_SERVER_H__
#define __UI_HTTP_SERVER_H__

void init_http_server_task(void);

typedef struct {
  char str_value[4];
  long long_value;
} URL_t;

#endif  // __UI_HTTP_SERVER_H__
