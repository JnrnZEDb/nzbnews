/*  <nzb>
 *      <file>
 *          <groups>
 *              <group></group>
 *              ...
 *          </groups>
 *          <segments>
 *              <segment></segment>
 *              ...
 *          </segments>
 *      </file>
 *  </nzb>
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libxml/parser.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <uudeview.h>

#include "nzbnews.h"

#define NN_OK           0
#define NN_ERROR        -1
#define NN_TIMEOUT      -2
#define NN_UNKNOWN      -3

#define DEBUG   if(g.debug >= 1) printf
#define DEBUG2  if(g.debug >= 2) printf
#define DEBUG3  if(g.debug >= 3) printf
#define DEBUG4  if(g.debug >= 4) printf

static struct _global_t {
    short running;
    short debug;
    short verify;
    short anonymous;
    char *config;
    char *server;
    char *username;
    char *password;
    char *nzbfile;
    char *outdir;
    struct {
        time_t start;
        time_t last;
        unsigned long bytes;
        unsigned long last_bytes;
        float rate;
    } stats;
} g;

int file_exists(char* filename) {
    struct stat finfo;
    if(!filename) {
        return NN_ERROR;
    }
    else {
        if(stat(filename, &finfo) == 0) {
            return 1;
        }
        else {
            return 0;
        }
    }
    return 0;
}

int check_response_status(char* response) {
    int ret;
    int rc;

    if(!response) {
        return NN_ERROR;
    }

    if((rc = sscanf(response, "%d %*s", &ret)) != 1) {
        ret = NN_ERROR;
    }
    return ret;
}

int send_msg(int sock, char* buf, int len, int timeout) {
    int rc;
    fd_set fds;
    struct timeval tv;

    if((rc = send(sock, buf, len, 0)) == -1) {
        if(errno != EAGAIN) {
            perror("send");
            return NN_ERROR;
        }
    }
    else {
        return rc;
    }
    
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    if((rc = select(sock + 1, NULL, &fds, NULL, &tv)) == -1) {
        perror("select");
        return NN_ERROR;
    }
    else if(rc == 0) {
        fprintf(stderr, "%s: timed out sending data\n", __FUNCTION__);
        return NN_TIMEOUT;
    }
    else {
        return send(sock, buf, len, 0);
    }
    return NN_ERROR;
}

int recv_msg(int sock, char* buf, int len, int timeout) {
    int rc;
    fd_set fds;
    struct timeval tv;

    if((rc = recv(sock, buf, len, 0)) == -1) {
        if(errno != EAGAIN) {
            perror("recv");
            return NN_ERROR;
        }
    }
    else {
        g.stats.bytes += rc;
        if((time(NULL) - g.stats.last) >= 1) {
            g.stats.rate = ((g.stats.bytes - g.stats.last_bytes) * (1.0))/ 
                           ((time(NULL) - g.stats.last) * 1.0);
            g.stats.last = time(NULL);
            g.stats.last_bytes = g.stats.bytes;
        }
        return rc;
    }
   
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec = 10;
    tv.tv_usec = 0;

    if((rc = select(sock + 1, &fds, NULL, NULL, &tv)) == -1) {
        perror("select");
        return NN_ERROR;
    }
    else if(rc == 0) {
        fprintf(stderr, "%s: timed out receiving data\n", __FUNCTION__);
        return NN_TIMEOUT;
    }
    else {
        rc = recv(sock, buf, len, 0);
        g.stats.bytes += rc;
        if((time(NULL) - g.stats.last) >= 1) {
            g.stats.rate = ((g.stats.bytes - g.stats.last_bytes) * (1.0))/ 
                           ((time(NULL) - g.stats.last) * 1.0);
            g.stats.last = time(NULL);
            g.stats.last_bytes = g.stats.bytes;
        }
        return rc;
    }
    return -1;
}

/* XXX NOT THREAD SAFE! */
char* remove_dangerous_shell_chars(char* buf, size_t len) {
    static char ret[1024];
    char* p;

    strncpy(ret, buf, len);
    p = ret;
    while(*p) {
        if(*p == '\''   || *p == '"'
        || *p == '`'    || *p == '&'
        || *p == '\n'   || *p == '\r'
        || *p == '#'    || *p == '('
        || *p == ')'    || *p == '['
        || *p == ']') {
            *p = '_';
        }
        p++;
    }
    return ret;
}

/* Parses a .nzb file and returns a linked-list of files, along with their
 * list of segments */
file_node* parse_nzb(char* nzbfile) {
    xmlDocPtr   doc = NULL;
    xmlNodePtr  nzb = NULL;
    xmlNodePtr  file = NULL;
    xmlNodePtr  segments = NULL;
    xmlNodePtr  segment = NULL;
    xmlNodePtr  groups = NULL;
    xmlNodePtr  group = NULL;
    xmlChar*    groupname = NULL;
    xmlChar*    poster = NULL;
    xmlChar*    date = NULL;
    xmlChar*    subject = NULL;
    xmlChar*    bytes = NULL;
    xmlChar*    number = NULL;
    xmlChar*    msgid = NULL;
    char        cmd[512];
    FILE*       fp = NULL;
    char*       p;

    file_node*      file_list = NULL;
    file_node*      fptr = NULL;
    segment_node*   sptr = NULL;
    file_node*      fwalk = NULL;
    segment_node*   swalk = NULL;

    if((doc = xmlParseFile(nzbfile)) != NULL) {
        if((nzb = xmlDocGetRootElement(doc)) != NULL) {
            if(!xmlStrcmp(nzb->name, (xmlChar*)"nzb")) {
                for(file = nzb->xmlChildrenNode; file; file = file->next) {
                    if(!xmlStrcmp(file->name, "file")) {
                        poster = xmlGetProp(file, "poster");
                        date = xmlGetProp(file, "date");
                        subject = xmlGetProp(file, "subject");
                        
                        if((fptr = (file_node*)calloc(1, sizeof(file_node))) == NULL) {
                            perror("calloc");
                            exit(1);
                        }
                        strncpy(fptr->poster, poster, sizeof(fptr->poster));
                        strncpy(fptr->subject, subject, sizeof(fptr->subject));
                        fptr->date = strtoul(date, NULL, 10);
                        snprintf(cmd, sizeof(cmd), "echo \"%s\" | md5sum | awk '{print $1}'",
                            remove_dangerous_shell_chars(fptr->subject, strlen(fptr->subject)));
                        if((fp = popen(cmd, "r")) == NULL) {
                            perror("popen");
                            exit(1);
                        }
                        fgets(fptr->filename, sizeof(fptr->filename), fp);
                        pclose(fp);
            
                        if((p = strchr(fptr->filename, '\n')) != NULL) {
                            *p = '\0';
                        }
                        
                        if(!file_list) {
                            file_list = fptr;
                        }
                        else {
                            fwalk = file_list;
                            while(fwalk->next) {
                                fwalk = fwalk->next;
                            }
                            fwalk->next = fptr;
                        }
            
                        xmlFree(poster);
                        xmlFree(date);
                        xmlFree(subject);
            
                        for(groups = file->xmlChildrenNode; groups; groups = groups->next) {
                            if(!xmlStrcmp(groups->name, "groups")) {
                                for(group = groups->xmlChildrenNode; group; group = group->next) {
                                    if(!xmlStrcmp(group->name, "group")) {
                                        groupname = xmlNodeListGetString(doc, group->xmlChildrenNode, 1);
                                        strncpy(fptr->group, groupname, sizeof(fptr->group));
                                        xmlFree(groupname);
                                    }
                                }
                            }
                        }
                        
                        for(segments = file->xmlChildrenNode; segments; segments = segments->next) {
                            if(!xmlStrcmp(segments->name, "segments")) {
                                for(segment = segments->xmlChildrenNode; segment; segment = segment->next) {
                                    if(!xmlStrcmp(segment->name, "segment")) {
                                        bytes = xmlGetProp(segment, "bytes");
                                        number = xmlGetProp(segment, "number");
                                        msgid = xmlNodeListGetString(doc, segment->xmlChildrenNode, 1);
            
                                        if((sptr = (segment_node*)calloc(1, sizeof(segment_node))) == NULL) {
                                            perror("calloc");
                                            exit(1);
                                        }
                                        sptr->bytes = strtoul(bytes, NULL, 10);
                                        sptr->number = strtoul(number, NULL, 10);
                                        strncpy(sptr->msgid, msgid, sizeof(sptr->msgid));
            
                                        if(!fptr->segments) {
                                            fptr->segments = sptr;
                                        }
                                        else {
                                            swalk = fptr->segments;
                                            while(swalk->next) {
                                                swalk = swalk->next;
                                            }
                                            swalk->next = sptr;
                                        }

                                        xmlFree(bytes);
                                        xmlFree(number);
                                        xmlFree(msgid);
                                    }
                                }
                            }
                        }
                    }
                }           
            }
            else {
                fprintf(stderr, "file doesn't appear to be a nzb file\n");
            }
        }
        else {
            fprintf(stderr, "failed to find root/nzb element\n");
        }
        xmlFreeDoc(doc);
    }
    else {
        fprintf(stderr, "failed to parse file [%s]\n", nzbfile);
    }
    return file_list;
}

/* Get a list of files/segments for download */
file_node* get_file_list(char* file) {
    return parse_nzb(file);
}

/* Delete the list of files/segments when you're done */
int del_file_list(file_node* list) {
    file_node* walk;
    segment_node* segment;

    while(list) {
        walk = list;
        while(list->segments) {
            segment = list->segments;
            list->segments = list->segments->next;
            free(segment);
            segment = NULL;
        }
        list = list->next;
        free(walk);
        walk = NULL;
    }
    return 0;
}

int uu_busy_callback(void *ptr, uuprogress *progress)
{
    fprintf(stderr, "%s: %s %s %d/%d %d\n",
        __FUNCTION__,
        progress->action == UUACT_IDLE ?        "Idle"      :
        progress->action == UUACT_SCANNING ?    "Scanning"  :
        progress->action == UUACT_DECODING ?    "Decoding"  :
        progress->action == UUACT_COPYING ?     "Copying"   :
        progress->action == UUACT_ENCODING ?    "Encoding"  :
        progress->action == UUACT_SCANNING ?    "Scanning"  :
                                                "Unknown",
        progress->curfile,
        progress->partno,
        progress->numparts,
        progress->percent);

    return 0;
}

void uu_msg_callback(void *ptr, char *msg, int level)
{
    fprintf(stderr, "%s: %d:%s\n",
        __FUNCTION__, level, msg);
}

char *uu_fname_filter(void *ptr, char *fname)
{
    static char filtered_filename[1024] = {0};

    snprintf(filtered_filename, sizeof(filtered_filename),
        "%s/%s", g.outdir, fname);
    return filtered_filename;
}

int decode_file(file_node *file)
{
    segment_node *segment = NULL;
    uulist *item = NULL;
    int i;

    UUInitialize();
    UUSetBusyCallback(NULL, uu_busy_callback);
    UUSetMsgCallback(NULL, uu_msg_callback);
    UUSetFNameFilter(NULL, uu_fname_filter);

    for(segment = file->segments; segment; segment = segment->next) {
        char segment_name[1024];

        snprintf(segment_name, sizeof(segment_name),
            "%s/.%s.%d", g.outdir, file->filename, segment->number);
        UULoadFile(segment_name, NULL, 0);
    }

    for(i = 0; (item = UUGetFileListItem(i)) != NULL; i++) {
        UUDecodeFile(item, NULL);
    }

    UUCleanUp();
    
    for(segment = file->segments; segment; segment = segment->next) {
        char segment_name[1024];

        snprintf(segment_name, sizeof(segment_name),
            "%s/.%s.%d", g.outdir, file->filename, segment->number);
        unlink(segment_name);
    }

    return 0;
}

int server_login(int sock, char* username, char* password) {
    char buf[1024];
    int rc;

    snprintf(buf, sizeof(buf), "AUTHINFO USER %s\r\n", username);
    if(send_msg(sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error sending AUTHINFO USER command\n", __FUNCTION__);
        return NN_ERROR;
    }
    if(recv_msg(sock, buf, sizeof(buf), 0) < 0) {
        fprintf(stderr, "%s: error receiving AUTHINFO USER response\n", __FUNCTION__);
        return NN_ERROR;
    }
    if((rc = check_response_status(buf)) != NNTP_AUTHINFO_CONTINUE) {
        return NN_ERROR;
    }
    snprintf(buf, sizeof(buf), "AUTHINFO PASS %s\r\n", password);
    if(send_msg(sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error sending AUTHINFO PASS command\n", __FUNCTION__);
        return NN_ERROR;
    }
    if(recv_msg(sock, buf, sizeof(buf), 0) < 0) {
        fprintf(stderr, "%s: receiving AUTHINFO PASS response\n", __FUNCTION__);
        return NN_ERROR;
    }
    if((rc = check_response_status(buf)) == NNTP_AUTHINFO_OK ||
       (rc = check_response_status(buf)) == NNTP_AUTHINFO_OK2) {
        printf("%s: login successful\n", __FUNCTION__);
        return NN_OK;
    }
    else if(rc == NNTP_AUTH_REJECTED) {
        printf("%s: authentication failed [%s]\n", __FUNCTION__, buf);
        return NN_ERROR;
    }
    else {
        fprintf(stderr, "%s: unexpected login response [%s]\n", __FUNCTION__, buf);
        return NN_ERROR;
    }
    return NN_ERROR;
}

int server_set_mode_reader(int sock) {
    char buf[1024];
    int rc;

    snprintf(buf, sizeof(buf), "MODE READER\r\n");
    if(send_msg(sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error setting MODE READER\n", __FUNCTION__);
        return NN_ERROR;
    }
    if(recv_msg(sock, buf, sizeof(buf), 0) < 0) {
        fprintf(stderr, "%s: error receiving MODE READER response\n", __FUNCTION__);
        return NN_ERROR;
    }
    if((rc = check_response_status(buf)) == NNTP_READY ||
       (rc = check_response_status(buf)) == NNTP_READY_NO_POSTING) {
        printf("%s: mode set successfully\n", __FUNCTION__);
        return NN_OK;
    }
    else {
        fprintf(stderr, "%s: unexpected MODE READER response [%s]\n", __FUNCTION__, buf);
        return NN_ERROR;
    }
    return NN_ERROR;
}

int server_connect(int retries) {
    int sock;
    struct sockaddr_in addr;
    char buf[1024];
    struct hostent* hostinfo = NULL;
    int flags;
    int rc;
    
    if((hostinfo = gethostbyname(g.server)) == NULL) {
        perror("gethostbyname");
        return NN_ERROR;
    }

    if(!retries--) {    /* we recursed until we ran out of retries */
        return NN_ERROR;
    }

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return NN_ERROR;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(119);
    memcpy(&addr.sin_addr.s_addr, hostinfo->h_addr, hostinfo->h_length);
    memset(&addr.sin_zero, 0x0, sizeof(addr.sin_zero));
    if(connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr)) == -1) {
        perror("connect");
        close(sock);
        return NN_ERROR;
    }

    flags = fcntl(sock, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(sock, F_SETFL, flags);
    
    if(recv_msg(sock, buf, sizeof(buf), 0) < 0) {
        fprintf(stderr, "%s: error receiving greeting from server %s\n", __FUNCTION__, g.server);
        server_disconnect(&sock);
        return NN_ERROR;
    }
    rc = check_response_status(buf);
    if(rc == NNTP_READY || rc == NNTP_READY_NO_POSTING) {
        printf("%s: connected to news server\n", __FUNCTION__);
    }
    else if(rc == NNTP_DISCONTINUED) {   /* too many connections */
        fprintf(stderr, "%s: too many connections...sleeping\n", __FUNCTION__);
        sleep(10);
        return server_connect(retries);
    }
    else {
        fprintf(stderr, "%s: unexpected greeting from server [%s]\n", __FUNCTION__, buf);
        server_disconnect(&sock);
        return NN_ERROR;
    }
    if(!g.anonymous) {
        if(server_login(sock, g.username, g.password) < 0) {
            fprintf(stderr, "%s: login failed\n", __FUNCTION__);
            return NN_ERROR;
        }
    }
    if(server_set_mode_reader(sock) < 0) {
        fprintf(stderr, "%s: failed to set reader mode\n", __FUNCTION__);
        return NN_ERROR;
    }
    return sock;
}

int server_disconnect(int* sock) {
    char buf[1024];
    
    snprintf(buf, sizeof(buf), "EXIT\r\n");
    if(send_msg(*sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error logging out\n", __FUNCTION__);
        close(*sock);
        return NN_ERROR;
    }
    close(*sock);
    *sock = -1;
    return NN_OK;
}

int connection_reset(int* sock) {
    server_disconnect(sock);
    *sock = server_connect(3);
    return *sock;
}

int get_segment(int* sock, file_node* file, segment_node* segment) {
    char *buf = NULL;
    char *pbuf = NULL;
    size_t buflen;
    size_t bufleft;
    int ret;
    short done;
    short retries;
    int bytes;
    char filename[256];
    FILE* fp = NULL;
    int rc;

    buflen = segment->bytes * 2;
    bufleft = buflen;
    buf = calloc(1, buflen);
    if(!buf) {
        perror("calloc");
        return NN_ERROR;
    }
    pbuf = buf;

    printf("%s: msgid=%s\n", __FUNCTION__, segment->msgid);

    snprintf(filename, sizeof(filename), "%s/.%s.%u", g.outdir, file->filename, segment->number);

    if(file_exists(filename)) {
        printf("%s: file already exists\n", __FUNCTION__);
        return 0;
    }
    if((fp = fopen(filename, "w")) == NULL) {
        perror("fopen");
        return NN_ERROR;
    }
    
    snprintf(buf, buflen, "BODY <%s>\r\n", segment->msgid);
    if(send_msg(*sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error sending BODY command\n", __FUNCTION__);
        ret = NN_ERROR;
    }
    else {
        if((bytes = recv_msg(*sock, pbuf, bufleft, 0)) > 0) {
            buf[bytes] = '\0';
            if((rc = check_response_status(pbuf)) == NNTP_BODY_OK) {
                ret = 0;
                done = 0;
                retries = 0;
                bufleft -= bytes;
                pbuf += bytes;

                while(!done && g.running) {
                    if((bytes = recv_msg(*sock, pbuf, bufleft, 0)) > 0) {
                        pbuf[bytes] = '\0';
                        printf("%s: %8.2f kB/s %3.0f%%\r", 
                            __FUNCTION__,
                            g.stats.rate/1000,
                            ceilf(((buflen - bufleft) * 100.0)/segment->bytes));
                        if(strstr(pbuf, "\r\n.\r\n")) {
                            done = 1;
                        }
                        bufleft -= bytes;
                        pbuf += bytes;
                        retries = 0;
                    }
                    else if(bytes == NN_TIMEOUT) {
                        if(++retries >= 3) {
                            fprintf(stderr, "%s: retries exhausted waiting for BODY text\n", __FUNCTION__);
                            done = 1;
                        }
                        else {
                            fprintf(stderr, "%s: timed out waiting for BODY text.  Retrying...[%d]\n", __FUNCTION__, retries);
                        }
                    }
                    else if(bytes == -1) {
                        fprintf(stderr, "%s: error receiving BODY text\n", __FUNCTION__);
                        done = 1;
                        ret = NN_ERROR;
                    }
                    else if(bytes == 0) {
                        fprintf(stderr, "%s: remote connection closed while receiving BODY text\n", __FUNCTION__);
                        done = 1;
                        ret = NN_ERROR;
                    }
                }
            }
            else if(rc == NNTP_NO_SUCH_ARTICLE) {
                printf("%s: no such article\n", __FUNCTION__);
                ret = NN_ERROR;
            }
            else {
                printf("%s: unexpected response to BODY command [%.40s]\n", __FUNCTION__, buf);
                ret = NN_ERROR;
            }
        }
        else if(bytes == NN_TIMEOUT) {
            fprintf(stderr, "%s: timed out waiting for BODY response\n", __FUNCTION__);
            ret = NN_ERROR;
        }
        else if(bytes == -1) {
            fprintf(stderr, "%s: error receiving BODY response\n", __FUNCTION__);
            ret = NN_ERROR;
        }
        else if(bytes == 0) {
            fprintf(stderr, "%s: remote connection closed\n", __FUNCTION__);
            ret = NN_ERROR;
        }
        printf("\n");
    
        pbuf = strtok(buf, "\n");
        while(pbuf) {
            if(pbuf[0] == '.') {
                pbuf++;
            }
            fputs(pbuf, fp);
            pbuf = strtok(NULL, "\n");
        }
    }

    fclose(fp);
    
    if(!g.running) {
        unlink(filename);
    }
    
    return ret;
}

int set_group(int* sock, char* group) {
    char buf[1024];
    int rc;
    
    snprintf(buf, sizeof(buf), "GROUP %s\r\n", group);
    if(send_msg(*sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error sending GROUP command\n", __FUNCTION__);
        return NN_ERROR;
    }
    if(recv_msg(*sock, buf, sizeof(buf), 0) < 0) {
        fprintf(stderr, "%s: error receiving GROUP response\n", __FUNCTION__);
        return NN_ERROR;
    }
    if((rc = check_response_status(buf)) == NNTP_GROUP_OK) {
        printf("%s: group successfully changed\n", __FUNCTION__);
        return NN_OK;
    }
    else if(rc == NNTP_NO_SUCH_GROUP) {
        printf("%s: no such group\n", __FUNCTION__);
        return NN_ERROR;
    }
    else {
        fprintf(stderr, "%s: unexpected response to GROUP command [%s]\n", __FUNCTION__, buf);
        return NN_ERROR;
    }
    return NN_ERROR;
}

int stat_msg(int* sock, char* msgid) {
    char buf[256];
    int rc;

    snprintf(buf, sizeof(buf), "STAT <%s>\r\n", msgid);
    if(send_msg(*sock, buf, strlen(buf), 0) < 0) {
        fprintf(stderr, "%s: error sending STAT command\n", __FUNCTION__);
        return NN_ERROR;
    }
    if(recv_msg(*sock, buf, sizeof(buf), 0) < 0) {
        fprintf(stderr, "%s: error receiving STAT response\n", __FUNCTION__);
        return NN_ERROR;
    }
    if((rc = check_response_status(buf)) == NNTP_STAT_OK) {
        return 0;
    }
    else if(rc == NNTP_NO_SUCH_ARTICLE) {
        return 1;
    }
    return 0;
}

int verify_file(int* sock, file_node* file) {
    segment_node* segment = NULL;
    int ret = 0;
    int seg_count = 0;
    int seg_verified = 0;

    segment = file->segments;
    while(segment) {
        seg_count++;
        segment = segment->next;
    }

    printf("[%s]\n", file->subject);
    segment = file->segments;
    while(segment && g.running) {
        if(stat_msg(sock, segment->msgid) != 0) {
            ret = 1;
            fprintf(stderr, "%s: no such article [%s]\n", __FUNCTION__, segment->msgid);
        }
        else {
            seg_verified++;
            //printf("%s: msg OK [%s]\n", __FUNCTION__, segment->msgid);
            printf("\r%s: %d/%d", __FUNCTION__, seg_verified, seg_count);
        }
        segment = segment->next;
    }
    printf("\n");
    return seg_count - seg_verified;
}

int get_file(int* sock, file_node* file) {
    int rc;
    segment_node* segment = NULL;
    char buf[256];
    char statfile[256];
    FILE* fp = NULL;
    struct stat fileinfo;
    
    snprintf(statfile, sizeof(statfile), "%s/.%s.done", g.outdir, file->filename);

    printf("%s: [%s]\n", __FUNCTION__, file->subject);

    if(stat(g.outdir,  &fileinfo) != 0) {
        if(errno == ENOENT) {
            mkdir(g.outdir, 0755);
        }
    }

    /* check if file already done */
    if(stat(statfile, &fileinfo) == 0) {
        printf("%s: file already finished\n", __FUNCTION__);
        return NN_OK;
    }
    
    if(set_group(sock, file->group) < 0) {
        fprintf(stderr, "%s: error changing to group %s\n", __FUNCTION__, file->group);
        return NN_ERROR;
    }

    segment = file->segments;
    while(segment && g.running) {
        if(get_segment(sock, file, segment) < 0) {
            printf("%s: segment download failed [msgid=%s]\n", __FUNCTION__, segment->msgid);
            segment->done = 0;
        }
        else {
            segment->done = 1;
        }
        segment = segment->next;
    }
    
    if(g.running) {
        if((rc = decode_file(file))) {
            printf("%s: %d segments decoded\n", __FUNCTION__, rc);
        }

        if((fp = fopen(statfile, "w")) == NULL) {
            perror("fopen");
        }
        else {
            snprintf(buf, sizeof(buf), "%lu", time(NULL));
            fwrite(buf, 1, strlen(buf), fp);
            fclose(fp);
            fp = NULL;
        }
    }
    return NN_OK;
}

void print_usage() {
    printf("usage: nzbnews [-s server] [-u username] [-p password] [-v] [-o directory] <nzbfile>\n");
    return;
}

void signal_handler(int sig) {
    switch(sig) {
    case SIGINT:
    case SIGTERM:
        g.running = 0;
        break;
    default:
        break;
    }
}

int init(int argc, char* argv[]) {
    int opt;
    char buf[1024];

    if(argc < 2) {
        print_usage();
        exit(1);
    }

    g.server = NULL;
    g.username = NULL;
    g.password = NULL;
    g.nzbfile = NULL;
    g.outdir = NULL;
    g.verify = 0;
    g.anonymous = 0;
    g.stats.start = time(NULL);
    g.stats.last = g.stats.start;
    g.stats.bytes = 0;
    g.stats.last_bytes = 0;
    
    while((opt = getopt(argc, argv, "avhxs:u:p:o:c:")) != EOF) {
        switch(opt) {
        case 'a':
            g.anonymous = 1;
            break;
        case 'x':
            g.debug++;
            break;
        case 'c':
            g.config = strdup(optarg);
            break;
        case 's':
            g.server = strdup(optarg);
            break;
        case 'u':
            g.username = strdup(optarg);
            break;
        case 'p':
            g.password = strdup(optarg);
            break;
        case 'o':
            g.outdir = strdup(optarg);
            break;
        case 'v':
            g.verify = 1;
            break;
        case 'h':
        default:
            print_usage();
            exit(0);
            break;
        }
    }
    g.nzbfile = strdup(argv[optind]);

    // Default values
    if(!g.outdir) {
        if(getcwd(buf, sizeof(buf)) == NULL) {
            perror("getcwd");
            exit(1);
        }
        g.outdir = strdup(buf);
    }
    if(!g.config) {
        char *homedir = getenv("HOME");
        snprintf(buf, sizeof(buf), "%s/.nzbnews/nzbnews.conf", homedir);
        g.config = strdup(buf);
    }

    // Parse config file
{
    struct stat finfo;
    FILE* fp = NULL;
    char *p = NULL;
    char *key = NULL, *val = NULL;

    if(stat(g.config, &finfo) == -1) {
        return NN_ERROR;
    }
    fp = fopen(g.config, "r");
    if(!fp) {
        return NN_ERROR;
    }
    while(fgets(buf, sizeof(buf), fp) != NULL) {
        if((p = strchr(buf, '\n')) != NULL) {
            *p = '\0';
        }
        if((p = strchr(buf, '\r')) != NULL) {
            *p = '\0';
        }
        if((p = strchr(buf, '=')) == NULL) {
            continue;
        }
        else {
            *p = '\0';
            key = buf;
            val = ++p;
            
            while(*key == ' ' || *key == '\t') { key++; }
            while(*val == ' ' || *val == '\t') { val++; }

            if(!strcasecmp(key, "server")) {
                if(g.server) {
                    free(g.server);
                }
                g.server = strdup(val);
            }
            else if(!strcasecmp(key, "username")) {
                if(g.username) {
                    free(g.username);
                }
                g.username = strdup(val);
            }
            else if(!strcasecmp(key, "password")) {
                if(g.password) {
                    free(g.password);
                }
                g.password = strdup(val);
            }
            else {
                fprintf(stderr, "%s: Unknown configuration key [%s = %s]\n",
                    __FUNCTION__, key, val);
            }
        }
    }
    fclose(fp);
}

    setvbuf(stdout, NULL, _IONBF, 0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g.running = 1;

    return NN_OK;
}

void cleanup(void) {
    if(g.config) {
        free(g.config); g.config = NULL;
    }
    if(g.server) {
        free(g.server); g.server = NULL;
    }
    if(g.username) {
        free(g.username); g.username = NULL;
    }
    if(g.password) {
        free(g.password); g.password = NULL;
    }
    if(g.nzbfile) {
        free(g.nzbfile); g.nzbfile = NULL;
    }
    if(g.outdir) {
        free(g.outdir); g.outdir = NULL;
    }
}

int main(int argc, char* argv[])
{
    int sock = -1;
    file_node*  file_list = NULL;
    file_node*  file = NULL;
    char *p = NULL;
    char buf[1024];

    init(argc, argv);

    // get required information from the user
    while(!g.server
        || !g.username
        || !g.password) {
        if(!g.server) {
            printf("You must specify a server: ");
            fgets(buf, sizeof(buf), stdin);
            g.server = strdup(buf);
            //fscanf(stdin, "%s", g.server);
            //exit(1);
        }
        if(!g.username) {
            printf("You must specify a username: ");
            fgets(buf, sizeof(buf), stdin);
            g.username = strdup(buf);
            //fscanf(stdin, "%s", g.username);
            //exit(1);
        }
        if(!g.password) {
            printf("You must specify a password: ");
            fgets(buf, sizeof(buf), stdin);
            g.password = strdup(buf);
            //fscanf(stdin, "%s", g.password);
            //exit(1);
        }
    }
    // remove newlines
    if((p = strchr(g.server, '\n')) != NULL)    { *p = '\0'; }
    if((p = strchr(g.username, '\n')) != NULL)  { *p = '\0'; }
    if((p = strchr(g.password, '\n')) != NULL)  { *p = '\0'; }

    // begin processing
    if((file_list = get_file_list(g.nzbfile)) == NULL) {
        fprintf(stderr, "%s: failed to get file list\n", __FUNCTION__);
        exit(1);
    }
    if((sock = server_connect(3)) == -1) {
        fprintf(stderr, "%s: error connecting to server\n", __FUNCTION__);
        exit(1);
    }
    file = file_list;
    while(file && g.running) {
        if(g.verify) {
            verify_file(&sock, file);   
        }
        else {
            get_file(&sock, file);
        }
        file = file->next;
    }
    if(server_disconnect(&sock) == -1) {
        fprintf(stderr, "%s: error disconnecting from server\n", __FUNCTION__);
    }
    del_file_list(file_list);

    cleanup();

    printf("%.2f MB transferred in %lu seconds (%.2f kB/s)\n",
        g.stats.bytes/(1024.0 * 1024.0),
        time(NULL) - g.stats.start,
        g.stats.rate/1000);
    
    return 0;
}
