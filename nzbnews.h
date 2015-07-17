#ifndef NZBNEWS_H
#define NZBNEWS_H

#define DECODE_CMD "nice -n 10 uudeview -i -a -m -d -s -s -q " 

#define NNTP_HELP_OK            100
#define NNTP_READY              200
#define NNTP_READY_NO_POSTING   201
#define NNTP_QUIT_OK            205
#define NNTP_GROUP_OK           211
#define NNTP_LIST_OK            215
#define NNTP_ARTICLE_OK         220
#define NNTP_HEAD_OK            221
#define NNTP_BODY_OK            222
#define NNTP_STAT_OK            223
#define NNTP_AUTHINFO_OK        250
#define NNTP_AUTHINFO_OK2       281
#define NNTP_AUTHINFO_CONTINUE  381
#define NNTP_DISCONTINUED       400
#define NNTP_NO_SUCH_GROUP      411
#define NNTP_NO_GROUP_SELECTED  412
#define NNTP_NO_ARTICLE_SELECTED    420
#define NNTP_NO_SUCH_ARTICLE    430
#define NNTP_AUTH_REQUIRED      450
#define NNTP_AUTH_REJECTED      452
#define NNTP_NOT_RECOGNIZED     500
#define NNTP_SYNTAX             501
#define NNTP_ACCESS             502
#define NNTP_ERROR              503

typedef struct _chunk {
	struct _chunk* next;
	unsigned char* data;
} chunk;

typedef struct _segment_node {
	struct _segment_node*	next;
	unsigned int	bytes;
	unsigned int	number;
	char	        msgid[512];
	short			done;
	chunk*			chunks;
} segment_node;

typedef struct _file_node {
	struct _file_node*	next;
	char	poster[256];
	char	group[256];
	time_t			date;
	char	subject[256];
	char	filename[128];
	short			done;
	segment_node* 	segments;
} file_node;

/* nzbnews.c */
/* nzbnews.c */
int send_msg(int sock, char *buf, int len, int timeout);
int recv_msg(int sock, char *buf, int len, int timeout);
char *remove_dangerous_shell_chars(char *buf, size_t len);
file_node *parse_nzb(char *nzbfile);
file_node *get_file_list(char *file);
int del_file_list(file_node *list);
int decode_file(file_node *file);
int server_login(int sock, char *username, char *password);
int server_set_mode_reader(int sock);
int server_connect(int retries);
int server_disconnect(int* sock);
int remove_dots(char *src, size_t srclen, char *dst, int dstlen);
int get_segment(int* sock, file_node *file, segment_node *segment);
int set_group(int* sock, char *group);
int get_file(int* sock, file_node *file);
void print_usage(void);
int init(int argc, char *argv[]);
int check_response_status(char *response);
void signal_handler(int sig);

#endif
