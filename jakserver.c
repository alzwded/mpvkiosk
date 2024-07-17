// Copyright 2024 Vlad Mesco
// 
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// -std=gnu99 would set this to >= 600
#ifndef _XOPEN_SOURCE
# define _XOPEN_SOURCE 600
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <err.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <netinet/in.h>
#include <arpa/inet.h>

// don't bother with POST requests bigger than 1MB
#ifndef CONTENT_LENGTH_LIMIT
# define CONTENT_LENGTH_LIMIT (1 * 1024 * 1024)
#endif

// don't bother with clients that take this long to say anything.
// since this runs on lan, it might as well be <5...
#ifndef TIMEOUT_LIMIT
# define TIMEOUT_LIMIT 30
#endif

// safety precaution for the handler script;
// makes sure to smother it if it goes haywire, but also
// don't bother with clients that take too long to accept
// the response.
// define to <= 0 to disable and handle timeouts in the
// handler script itself
#ifndef HANDLER_TIMEOUT_LIMIT
# define HANDLER_TIMEOUT_LIMIT TIMEOUT_LIMIT
#endif

// see listen(3), backlog
#ifndef MAX_BACKLOG
# define MAX_BACKLOG 10
#endif

// server socket; needs to be closed by child processes, or self
int gsock = 0;

// path to shell script handling requests
char* handler = NULL;
// interface to bind; ipv4
unsigned iface = INADDR_ANY;
// port to bind
unsigned port = 8080;
// be more chatty
int verbose = 1;

// passed to atexit(3)
void myatexit()
{
    if(gsock) close(gsock);
}

// used by parser
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

// parser parsing state
enum estate {
    INIT = 0,
    PROTO,
    HEADERS,
    BODY
};

// parser state
struct parser {
    // internal parser state, for resume in case it needed more input
    enum estate state;
    // internal parser state, for resume in case it needed more input
    size_t ip;
    // HTTP method
    char* method;
    // HTTP path
    char* path;
    // Content-Length header value
    size_t contentLength;
    // Pointer to CRLF delimited header entries (raw)
    char* headers;
    // Pointer to body (raw)
    char* body;
};

enum parse_return {
    ERROR = -1,
    DONE = 0,
    MORE = 1
};

// initialize parser once, then keep passing that state in
// if it returns MORE, it expects the caller to read more into
// buf; it expects buf is realloc(3)'d or something like that, i.e.
// preceding data is still there when called later.
// DONE means you can pass this to execute()
// ERROR means we don't want to talk to the client anymore
int parse(struct parser* parser, char* buf, size_t sbuf)
{
    char* end = buf + sbuf;
    // work pointers
    char* p1, *p2, *p3;
    // set if headers were properly CRLF delimited; unset if nonstandard?
    int p2WasCRLF = 0;
    switch(parser->state) {
        case INIT:
            parser->ip = 0;
            parser->state = PROTO;
            /*fallthrough*/
        case PROTO:
            // parse status / protocol / request line (whatever the first line is called)
            // start from the begining
            p1 = buf;
            // find the end of the request line
            while(p1 < end && *p1 != '\n') ++p1;
            if(p1 >= end) {
                // if it's not a line, ignore the client
                return ERROR;
            }
            *p1 = '\0';
            // check for known request methods;
            // p2 will point to after the method
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
            // if none found, ignore client
            if(parser->method == NULL) return ERROR;
            // p2 points past the method

            // skip whitespace
            while(isspace(*p2) && p2 < p1) ++p2;
            if(p2 >= p1) return ERROR;

            // there should be a space after the path, followed by HTTP/1.1
            p3 = p2;
            while(!isspace(*p3) && p3 < p1) ++p3;
            if(p3 >= p1) return ERROR;

            *p3 = '\0';
            p3++;
            // p2 should be our path now.
            parser->path = strdup(p2); // buf may be realloc'd, so strdup

            // check for HTTP/1.1
            while(isspace(*p3) && p3 < p1) ++p3;
            if(strncmp(p3, "HTTP/1.1", 8) != 0) {
                return ERROR;
            }

            parser->ip = (p1 - buf) + 1;
            /*falthrough*/
        case HEADERS:
            // parse headers, should end with CLRF

            // p1 will point to the begining of a header line, of the form
            // H: v\r\n
            p1 = buf + parser->ip;
            while(p1 < end) {
                // advance p2 to CRLF
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
                // check if we actually hit CRLFCRLF, i.e. end of headers
                if(*p1 == '\0') {
                    parser->state = BODY;
                    p1 = p2 + 1;
                    break;
                }
                // else, find the colon
                p3 = strchr(p1, ':');
                if(p3 == NULL) goto undop2;
                *p3 = '\0';
                ++p3;

                // headers are case insitive
                for(char* pp = p1; pp < p3; ++pp) {
                    *pp = tolower(*pp);
                }

                // skip leading whitespace
                while(p1 < end && isspace(*p1) && *p1) p1++;

                // parse content-length, we need that to be able to
                // parse the body. Only Content-Type/Content-Length single
                // file disposition is supported
                if(strncmp(p1, "content-length", strlen("content-length")) == 0) {
                    parser->contentLength = atoi(p3);
                }

                // undo nullifications to allow someone else to read this garbage
undop3:
                p3[-1] = ':';
undop2:
                *p2 = '\n';
                if(p2WasCRLF) p2[-1] = '\r';
nextheader:

                // p1 now points to the next header
                p1 = p2 + 1;
            }
            parser->headers = strdup(buf + parser->ip); // dup headers, because buf may be reallocated for larger requests
            parser->ip = p1 - buf;
            parser->state = BODY;
            /*fallthrough*/
        case BODY:
            // if we don't have content-length, we're done; no body
            if(parser->contentLength == 0) {
                parser->body = NULL;
                return DONE;
            } else {
                if(parser->contentLength < 0) return ERROR;
                // if trying to be given more content than we want, ignore the client
                if(parser->contentLength > CONTENT_LENGTH_LIMIT) return ERROR;
                // if the buffer doesn't contain all the data we need, tell
                // the caller we want more
                if(sbuf - parser->ip < parser->contentLength) {
                    return MORE;
                } else {
                    // else, we're done; pass the parser to execute()
                    parser->body = buf + parser->ip;
                    return DONE;
                }
            }
    }

    return ERROR;
}

// runs in child only
// passes off the request to the handler script
void execute(int conn, struct parser* parser)
{
    // make the socket be the process's stdout
    dup2(conn, STDOUT_FILENO);
    close(conn);
    if(verbose) fprintf(stderr, "%d: Executing %s %s\n", conn, parser->method, parser->path);
    // set headers and body env vars
    setenv("REQHEADERS", parser->headers, 1);
    if(parser->body) setenv("REQBODY", parser->body, 1);
#if HANDLER_TIMEOUT_LIMIT > 0
    // set an alarm for the handler script
    alarm(HANDLER_TIMEOUT_LIMIT);
#endif
    // exec to the handler script
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

void sighandler(int _ignored)
{
    (void)_ignored; // keep some compilers happy
    exit(0);
}

// in-process, quick response function for clients, in case parsing failed
void send_message(int conn, int code, const char* msg)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.1 %d\r\nContent-Type: text/plain\r\nContent-Length: %zd\r\n\r\n%s\r\n",
            code, strlen(msg) + /*len(CRLF)*/2, msg);

    int n = 0;

    // try to talk back for 3s, then give up
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

// handle connection
// spawns child process to do the actual handling
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

    // read up to 1k blocks, and try parsing the request as we go

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

    // establish server

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

    hr = listen(sockfd, MAX_BACKLOG);
    if(-1 == hr)
        err(EXIT_FAILURE, "listen");

    gsock = sockfd;
    atexit(myatexit);

    if(verbose) {
        char* host = inet_ntoa(sockaddr.sin_addr);
        fprintf(stderr, "Listening on %s:%u\n", host, port);
    }

    signal(SIGINT, sighandler);
    signal(SIGQUIT, sighandler);

    /*
    struct sigaction sa;
    sa.sa_handler = sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGCHLD, &sa, NULL) == -1)
        err(EXIT_FAILURE, "sigaction");
        */

    // main loop

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
