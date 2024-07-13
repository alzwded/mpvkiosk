// Copyright 2024 Vlad Mesco
// 
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <err.h>
#include <ctype.h>
#include <getopt.h>
#include <arpa/inet.h>

#define CONTENT_LENGTH_LIMIT (1 * 1024 * 1024)
#define TIMEOUT_LIMIT 30

int gsock = 0;

char* handler = NULL;
unsigned iface = INADDR_ANY;
unsigned port = 8080;
int verbose = 1;

static const char* KNOWN_METHODS[] = {
    "GET ",
    "POST ",
    "PUT ",
    "DELETE ",
    "HEAD ",
    "PATCH ",
    "OPTIONS ",
    "CONNECT ",
    "TRACE ",
    NULL
};

void myatexit()
{
    if(gsock) close(gsock);
}

enum estate {
    INIT = 0,
    PROTO,
    HEADERS,
    BODY
};

struct parser {
    ssize_t n;
    enum estate state;
    char* method;
    char* path;
    size_t headerLength;
    size_t contentLength;
    char* headers;
    char* body;
    size_t ip;
};

enum parse_return {
    ERROR = -1,
    DONE = 0,
    MORE = 1
};

int parse(struct parser* parser, char* buf, size_t sbuf)
{
    char* end = buf + sbuf;
    char* p1, *p2, *p3;
    int p2WasCRLF = 0;
    switch(parser->state) {
        case INIT:
            parser->ip = 0;
            parser->state = PROTO;
            /*fallthrough*/
        case PROTO:
            p1 = buf;
            while(p1 < end && *p1 != '\n') ++p1;
            if(p1 >= end) {
                return ERROR;
            }
            *p1 = '\0';
            for(const char** p = KNOWN_METHODS; *p; ++p) {
                size_t l = strlen(*p);
                if(sbuf < l) return ERROR;
                if(strncmp(buf, *p, l) == 0) {
                    parser->method = strdup(*p);
                    parser->method[l - 1] = '\0';
                    p2 = buf + l;
                    break;
                }
            }
            if(parser->method == NULL) return ERROR;

            while(isspace(*p2) && p2 < p1) ++p2;
            if(p2 >= p1) return ERROR;

            p3 = p2;
            while(!isspace(*p3) && p3 < p1) ++p3;
            if(p3 >= p1) return ERROR;

            *p3 = '\0';
            p3++;
            parser->path = strdup(p2); // buf may be realloc'd, so strdup

            while(isspace(*p3) && p3 < p1) ++p3;
            if(strncmp(p3, "HTTP/1.1", 8) != 0) {
                return ERROR;
            }

            parser->ip = (p1 - buf) + 1;
            /*falthrough*/
        case HEADERS:
            parser->headers = strdup(buf + parser->ip);
            p1 = buf + parser->ip;
            while(p1 < end) {
                p2 = p1;
                while(p2 < end && *p2 != '\n') ++p2;
                if(p2 >= end) {
                    return ERROR;
                }
                *p2 = '\0';
                if(p2 > p1 && p2[-1] == '\r') {
                    p2WasCRLF = 1;
                    p2[-1] = '\0';
                } else {
                    p2WasCRLF = 0;
                }
                if(*p1 == '\0') {
                    parser->state = BODY;
                    p1 = p2 + 1;
                    break;
                }
                p3 = strchr(p1, ':');
                if(p3 == NULL) continue;
                *p3 = '\0';
                ++p3;

                for(char* pp = p1; pp < p3; ++pp) {
                    *pp = tolower(*pp);
                }

                while(p1 < end && isspace(*p1) && *p1) p1++;

                if(strncmp(p1, "content-length", strlen("content-length")) == 0) {
                    parser->contentLength = atoi(p3);
                }

                // undo nullifications to allow someone else to read this garbage
                *p2 = '\n';
                if(p2WasCRLF) p2[-1] = '\r';
                p3[-1] = ':';

                p1 = p2 + 1;
            }
            parser->ip = p1 - buf;
            parser->state = BODY;
            /*fallthrough*/
        case BODY:
            if(parser->contentLength == 0) {
                parser->body = NULL;
                return DONE;
            } else {
                if(parser->contentLength < 0) return ERROR;
                if(parser->contentLength > CONTENT_LENGTH_LIMIT) return ERROR;
                if(sbuf - parser->ip < parser->contentLength) {
                    return MORE;
                } else {
                    parser->body = buf + parser->ip;
                    return DONE;
                }
            }
    }

    return ERROR;
}

// runs in child only
void execute(int conn, struct parser* parser)
{
    char buf[32];
    sprintf(buf, "%d", conn);
    dup2(conn, STDOUT_FILENO);
    close(conn);
    if(verbose) fprintf(stderr, "%d: Executing %s %s\n", conn, parser->method, parser->path);
    setenv("REQHEADERS", parser->headers, 1);
    if(parser->body) setenv("REQBODY", parser->body, 1);
    int hr = execlp(handler, handler, parser->method, parser->path, NULL);
    if(hr == -1)
        err(EXIT_FAILURE, "execlp");
}

/*
   // disabled since unnecessary
   // but if you want to enable it, compile with -std=gnu99
void sigchld(int)
{
    int wstatus;
    pid_t pid;
    while((pid = waitpid(-1, &wstatus, WNOHANG)) > 0)
        ;
}
*/

void send_message(int conn, int code, const char* msg)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.1 %d\r\nContent-Type: text/plain\r\nContent-Length: %zd\r\n\r\n%s\r\n",
            code, strlen(msg) + /*len(CRLF)*/2, msg);

    int n = 0;

    while(n++ < 3) {
        int hr = send(conn, buf, strlen(buf), MSG_DONTWAIT);
        if(hr == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                sleep(1);
                continue;
            }
            err(EXIT_FAILURE, "send");
        }
        break;
    }

    close(conn);
    exit(0);
}

void send_error(int conn)
{
    if(verbose) fprintf(stderr, "%d: rejected\n", conn);
    send_message(conn, 500, "Error");
}

void send_bad_request(int conn, const char* msg)
{
    if(verbose) fprintf(stderr, "%d: rejected\n", conn);
    send_message(conn, 400, msg);
}

void send_done(int conn)
{
    send_message(conn, 200, "OK");
}

void handle(int conn)
{
    pid_t newpid = fork();
    if(-1 == newpid) {
        close(conn);
        fprintf(stderr, "Failed to fork: %d (%s)", errno, strerror(errno));
        errno = 0;
        return;
    }

    if(newpid > 0) {
        // parent
        close(conn);
        return;
    } else {
        // runs in child which:
        // - execs bash
        // - exits
        // so no need to worry about free
        close(gsock);
        gsock = 0;
    }


    fd_set rfds;
    struct timeval tv;
    int retval;

    char* buf = malloc(1025);
    char* pbuf = buf;
    ssize_t sbuf = 0;
    buf[1024] = '\0';

    struct parser parser;
    memset(&parser, 0, sizeof(struct parser));

    while(1) {
        FD_ZERO(&rfds);
        FD_SET(conn, &rfds);

        memset(&tv, 0, sizeof(struct timeval));
        tv.tv_sec = TIMEOUT_LIMIT;
        tv.tv_usec = 0;

        retval = select(conn+1, &rfds, NULL, NULL, &tv);
        if(-1 == retval)
            err(EXIT_FAILURE, "select");
        if(0 == retval) exit(1); // client didn't want to write to us, ignore

        ssize_t bytes = recv(conn, pbuf, 1024, 0);
        if(bytes == -1) {
            if(errno == EAGAIN) continue;
            err(EXIT_FAILURE, "recv");
        }
        sbuf += bytes;
        pbuf[bytes] = '\0';

        int what = parse(&parser, buf, sbuf);
        if(what == MORE) {
            buf = realloc(buf, sbuf + 1025);
            pbuf = buf + sbuf;
            if(bytes == 0) {
                // EOF
                send_bad_request(conn, "Expected more data");
            }
            continue;
        } else if(what == DONE) {
            execute(conn, &parser);
            send_done(conn);
        } else {
            send_bad_request(conn, "Bad request, or inernal bug");
        }
    }
}

void help(const char* argv0)
{
    printf("Usage: %s -x handler_script [-H ip4] [-p port] [-q]\n"
            "Version %s\n"
            "by Vlad Mesco\n\n"
            "\t-h                 print this message\n"
            "\t-q                 quiet, turn off verbose logging\n"
            "\t-H ip4             interface to bind; default 0.0.0.0\n"
            "\t-p port            which port to bind; default 8080\n"
            "\t-x handler_script  path to an executable script to handle requests\n"
            "\n"
            "The handler_script will receive 2 or 3 arguments:\n"
            "  o the request method\n"
            "  o the request path\n"
            "\n"
            "The headers are passed through the REQHEADERS environment variable\n"
            "The body is passed through the REQBODY environment variable\n"
            "\n"
            "Log is on STDERR\n"
            ,
            argv0,
            VERSION);
    exit(2);
}

int main(int argc, char* argv[])
{
    int opt;
    while((opt = getopt(argc, argv, "H:p:x:hq")) != -1) {
        switch(opt) {
            case 'h': help(argv[0]); return 2;
            case 'x': handler = strdup(optarg); break;
            case 'H': iface = inet_addr(optarg); break;
            case 'p': port = atoi(optarg); break;
            case 'q': verbose = 0; break;
            default:
                      fprintf(stderr, "Unknown flag %c\n", opt);
                      help(argv[0]);
                      return 2;
        }
    }

    if(!handler) {
        fprintf(stderr, "You must specify -x handler_script\n");
        exit(2);
    }
    if(iface == INADDR_NONE) {
        fprintf(stderr, "Invalid IP passed to -H\n");
        exit(2);
    }
    if(0 != access(handler, R_OK|X_OK))
        err(EXIT_FAILURE, "access(handler_script)");

    if(port == 0) {
        fprintf(stderr, "Port must be > 0\n");
        exit(2);
    }

    int hr = 0;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(-1 == sockfd)
        err(EXIT_FAILURE, "socket");

    int nnn = 1;
    if(-1 == setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &nnn, sizeof(int)))
        err(EXIT_FAILURE, "setsockopt(SO_REUSEADDR)");

    struct sockaddr_in sockaddr = {
        AF_INET,
        htons(8080),
        { iface }
    };
    hr = bind(sockfd, (struct sockaddr*)&sockaddr, sizeof(sockaddr));
    if(-1 == hr)
        err(EXIT_FAILURE, "bind");

    hr = listen(sockfd, 10);
    if(-1 == hr)
        err(EXIT_FAILURE, "listen");

    gsock = sockfd;
    atexit(myatexit);

    if(verbose) {
        char* host = inet_ntoa(sockaddr.sin_addr);
        fprintf(stderr, "Listening on %s:%u\n", host, port);
    }

    /*
    struct sigaction sa;
    sa.sa_handler = sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGCHLD, &sa, NULL) == -1)
        err(EXIT_FAILURE, "sigaction");
        */

    while(1) {
        struct sockaddr_in client;
        socklen_t client_size = sizeof(struct sockaddr_in);
        memset(&client, 0, sizeof(struct sockaddr_in));
        int conn = accept(sockfd, (struct sockaddr*)&client, &client_size);
        if(-1 == conn) {
            fprintf(stderr, "Failed to accept connection: %d (%s)\n", errno, strerror(errno));
            errno = 0;
            continue;
        }

        if(verbose) {
            unsigned int aaa = *((unsigned int*)&client.sin_addr.s_addr);
            unsigned bits[] = {
                (aaa >> 0)  & 0xFF,
                (aaa >> 8)  & 0xFF,
                (aaa >> 16) & 0xFF,
                (aaa >> 24) & 0xFF,
            };
            fprintf(stderr, "Got a connection %d from %s\n", conn, inet_ntoa(client.sin_addr));
        }

        handle(conn);
    }
}
