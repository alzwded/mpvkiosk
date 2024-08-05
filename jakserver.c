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
#include <stdint.h>
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
#include <sys/stat.h>

#include <netinet/in.h>
#include <arpa/inet.h>

// don't bother with POST requests bigger than 1MB
#ifndef REQUEST_SIZE_LIMIT
# define REQUEST_SIZE_LIMIT (1 * 1024 * 1024)
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
// use temporary files to pass BODY through handler's stdin
// tmpfs(5) or mfs(8) is what's intended to be used
char* payloadPath = NULL;
// what's my pid again?
pid_t myPid = -1;

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
#define CHUNKED_MAGIC ((size_t)-1)
    // Content-Length header value
    size_t contentLength;
    // Pointer to CRLF delimited header entries (raw)
    char* headers;
    // Pointer to body (raw)
    char* body;
};

enum parse_return {
    NOT_IMPLEMENTED = -2,
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
//
// called in child process
int parse(struct parser* parser, char* buf, size_t sbuf)
{
    if(verbose >= 2) {
        fprintf(stderr, "%jd: entered parser, state %d sbuf %zd\n", (intmax_t)myPid, parser->state, sbuf);
    }

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
                return MORE;
            }
            *p1 = '\0';
            p2 = p1;
            // check for known request methods;
            // p2 will point to after the method
            for(const char** p = KNOWN_METHODS; *p; ++p) {
                size_t l = strlen(*p);
                if(sbuf < l) return ERROR;
                if(strncmp(buf, *p, l) == 0) {
                    parser->method = strdup(*p);
                    parser->method[l - 1] = '\0'; // KNOWN_METHODS contain one space, null it out on the dup
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

            // check for HTTP/1.1 or HTTP/1.0
            while(isspace(*p3) && p3 < p1) ++p3;
            if(strncmp(p3, "HTTP/1.1", 8) != 0
                    && strncmp(p3, "HTTP/1.0", 8) != 0) {
                return ERROR;
            }

            parser->ip = (p1 - buf) + 1;
            parser->state = HEADERS;
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
                    return MORE;
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
                    if(parser->contentLength > 0) {
                        if(verbose >= 2) fprintf(stderr, "%jd: content-length and/or transfer-encoding specified multiple times\n", (intmax_t)myPid);
                        return ERROR;
                    }
                    parser->contentLength = atoi(p3);
                } else if(strncmp(p1, "transfer-encoding", strlen("tranfer-encoding")) == 0) {
                    if(parser->contentLength > 0) {
                        if(verbose >= 2) fprintf(stderr, "%jd: content-length and/or transfer-encoding specified multiple times\n", (intmax_t)myPid);
                        return ERROR;
                    }
                    parser->contentLength = CHUNKED_MAGIC;
                }

                // undo nullifications to allow someone else to read this garbage
//undop3:
                p3[-1] = ':';
undop2:
                *p2 = '\n';
                if(p2WasCRLF) p2[-1] = '\r';
//nextheader:

                // p1 now points to the next header
                p1 = p2 + 1;
            }
            if(parser->state == HEADERS) return MORE;

            parser->headers = strdup(buf + parser->ip); // dup headers, because buf may be reallocated for larger requests
            parser->ip = p1 - buf;
            parser->state = BODY;
            /*fallthrough*/
        case BODY:
            // if we don't have content-length, we're done; no body
            if(parser->contentLength == CHUNKED_MAGIC) {
                // Still considering if to add this or not.
                //
                // It should do something like:
                // - Parse [A-Fa-f0-9]\+\r\n
                // - decode to decimal -> ChLen
                // - if ChLen == 0
                //   + read \r\n or stop (don't ask for MORE for two terminator bytes)
                // - Read ChLen bytes (or ask MORE)
                // - Read \r\n (or ask MORE)
                // - loop
                //
                // The problem is, a bad request will only be handled by
                // the per request timeout, which is annoying. It also
                // complicates this parser a lot. And it probably won't
                // be all that well tested either.
                //
                // Also, there was a mention somewhere about sending
                // headers after the body; unclear if that referred solely
                // to the overall Content-Length or what, but it sounds
                // like a potential headache.
                return NOT_IMPLEMENTED;
            } else if(parser->contentLength == 0) {
                parser->body = NULL;
                return DONE;
            } else {
                if(parser->contentLength < 0) return ERROR; // FIXME it's currently unsigned...
                // Sanity check: if the content length itself is bigger than
                // our limit, exit early; otherwise the caller will error
                // out if the overall request size is > REQUEST_SIZE_LIMIT
                if(parser->contentLength > REQUEST_SIZE_LIMIT) return ERROR;
                // if the buffer doesn't contain all the data we need, tell
                // the caller we want more
                if(sbuf - parser->ip < parser->contentLength) {
                    return MORE;
                } else {
                    // else, we're done; pass the parser to execute()
                    parser->body = buf + parser->ip;
                    // body[contentLength] should not be out of bounds, we shoul dhave
                    // overallocated by a byte for this purpose specifically
                    parser->body[parser->contentLength] = '\0';
                    return DONE;
                }
            }
    }

    return ERROR;
}

// in-process, quick response function for clients, in case parsing failed
//
// called in child process
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
    if(verbose) fprintf(stderr, "%jd: rejected\n", (intmax_t)myPid);
    send_message(conn, 500, "Error");
}

void send_bad_request(int conn, const char* msg)
{
    if(verbose) fprintf(stderr, "%jd: rejected\n", (intmax_t)myPid);
    send_message(conn, 400, msg);
}

void send_done(int conn)
{
    send_message(conn, 200, "OK");
}


// runs in child only
// passes off the request to the handler script
//
// if -0 was specified, the handler will get the request body on stdin;
// otherwise, it's in an env var; the latter is leaner, but you only get
// some amount of KBs available for one request
//
// called in child process
void execute(int conn, struct parser* parser)
{
    // before closing conn...
    // ...check if we need to pass a body, and how
    if(parser->body) {
        if(!payloadPath) {
            // by env var, close stdin
            setenv("REQBODY", parser->body, 1);
            close(STDIN_FILENO);
        } else {
            // set up temp buffer
            int fd = mkstemp(payloadPath);
            if(fd == -1) {
                fprintf(stderr, "%jd: Failed to open %s, reason: %s\n",
                        (intmax_t)myPid, payloadPath, strerror(errno));
                send_error(conn); // exits
            }
            unlink(payloadPath);

            // fill up buffer
            size_t written = 0;
            // will break on:
            // - error
            // - written == parser->contentLength
            while(1) {
                ssize_t wrote = write(fd, parser->body + written, parser->contentLength - written);
                if(verbose >= 2) fprintf(stderr, "%jd: write() = %zd\n", (intmax_t)myPid, wrote);
                if(wrote == -1) {
                    if(errno == EAGAIN) {
                        errno = 0;
                        continue;
                    }
                    fprintf(stderr, "%jd: Failed to write to %s, reason: %s\n",
                            (intmax_t)myPid, payloadPath, strerror(errno));
                    send_error(conn);
                } else if(wrote == 0) {
                    fprintf(stderr, "%jd: failed to write to %s, reason: gave up\n",
                            (intmax_t)myPid, payloadPath);
                    send_error(conn); // exits
                }
                written += wrote;
                if(written == parser->contentLength) break;
            }

            // "pass" buffer to child process as its stdin
            lseek(fd, 0, SEEK_SET);
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
    } else {
        // no body, close stdin
        close(STDIN_FILENO);
    }

    // make the socket be the process's stdout
    dup2(conn, STDOUT_FILENO);
    // get rid of our copy
    close(conn);

    // set headers env vars
    setenv("REQHEADERS", parser->headers, 1);

    if(verbose) fprintf(stderr, "%jd: Executing %s %s\n", (intmax_t)myPid, parser->method, parser->path);
    // exec to the handler script
    int hr = execlp(handler, handler, parser->method, parser->path, NULL);
    if(hr == -1)
        err(EXIT_FAILURE, "execlp");
}

/*
   // leaving this around in case I want to log children exiting
void sigchld(int _ignored)
{
    (void)_ignored;
    int wstatus;
    pid_t pid;
    while((pid = waitpid(-1, &wstatus, WNOHANG)) > 0)
        ;
}
*/

void sighandler(int _ignored)
{
    (void)_ignored;
    exit(0);
}

void handler_timedout(int _ignored)
{
    (void)_ignored;
    //                      0  1  2  3  4  5  6  7  8  9 10 11 12 13  14   15
    static char buf[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', ' ' };
    if(verbose) {
        int pid = myPid;
        int p = 14 /* pos of ':' */;
        // p will point to a string containing "<pid>: ", where <pid> is myPid stringified;
        // this does not allocate new memory, since signal handler
        while(pid > 0) {
            buf[--p] = pid % 10 + '0';
            pid /= 10;
        }
        // we're supposed to exit asap, so ignore rval of write
        int _ignore;
        _ignore = write(fileno(stderr), &buf[p], 16 - p);
        (void) _ignore;
        _ignore = write(fileno(stderr), "timed out\n", strlen("timed out\n"));
        (void) _ignore;
        // I really don't understand why they need to insist so much on the return value
        // of write; this is literally the process's last dying breath, it's really not
        // the time to deal with bureaucracy. I hope the compiler doesn't learn to interpret
        // the above lines as "discarding the value" or I'm going to disable that
        // warning entirely, and then it helps noone.
    }
    _exit(1);
}

// handle connection
// spawns child process to do the actual handling
//
// called in parent and child processes, parent returns early after setting up child
void handle(int conn, struct in_addr client_addr)
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
        // child; save own pid to not call getpid() too much
        myPid = newpid;
    }
    // runs in child which:
    // - execs bash
    // - exits
    // so no need to worry about free

    if(verbose) fprintf(stderr, "%jd: Handling request from %s\n", (intmax_t)myPid, inet_ntoa(client_addr));

    // close the listen() socket, not needed here
    close(gsock);
    gsock = 0;

    // set timer now to not deal with timeouts in select
#if HANDLER_TIMEOUT_LIMIT > 0
    // set an alarm for the handler script
    alarm(HANDLER_TIMEOUT_LIMIT);
    signal(SIGALRM, handler_timedout);
#endif

    fd_set rfds;
    int retval;

    // read up to 1k blocks, and try parsing the request as we go

    char* buf = malloc(1025);
    if(!buf)
        err(EXIT_FAILURE, "malloc");
    char* pbuf = buf;
    ssize_t sbuf = 0;
    buf[1024] = '\0';

    struct parser parser;
    memset(&parser, 0, sizeof(struct parser));

    // will exit on:
    // - error
    // - exec()
    // - SIGALRM
    // - connection opened, but nothing written by client
    while(1) {
        FD_ZERO(&rfds);
        FD_SET(conn, &rfds);

        retval = select(conn+1, &rfds, NULL, NULL, NULL);
        if(-1 == retval)
            err(EXIT_FAILURE, "select");
        if(0 == retval) exit(1); // client didn't want to write to us, ignore

        ssize_t bytes = recv(conn, pbuf, 1024, 0);
        if(bytes == -1) {
            if(errno == EAGAIN) continue;
            err(EXIT_FAILURE, "recv");
        }
        if(bytes > 0) {
            sbuf += bytes;
            pbuf[bytes] = '\0';
        }
        if(verbose >= 2)
            fprintf(stderr, "%jd: DEBUG: bytes %zd buf %s pbuf %s pbuf-buf %zd\n", (intmax_t)myPid, bytes, buf, pbuf, pbuf - buf);

        int what = parse(&parser, buf, sbuf);
        if(what == MORE) {
            if(bytes == 0) {
                // EOF
                send_bad_request(conn, "Expected more data");
            }
            if(sbuf + 1025 > REQUEST_SIZE_LIMIT) {
                // too big
                send_bad_request(conn, "Request too large");
            }
            buf = realloc(buf, sbuf + 1025);
            if(!buf)
                err(EXIT_FAILURE, "realloc");
            pbuf = buf + sbuf;
            continue;
        } else if(what == DONE) {
            execute(conn, &parser);
            send_done(conn);
        } else if(what == NOT_IMPLEMENTED) {
            send_message(conn, 501, "Not implemented");
        } else {
            send_bad_request(conn, "Bad request, or inernal bug");
        }
    }
}

void help(const char* argv0)
{
    printf("Usage: %s -x handler_script [-H ip4] [-p port] [-q] [-v]\n"
            "Version %s\n"
            "by Vlad Mesco\n\n"
            "\t-h                 print this message\n"
            "\t-q                 quiet, prints less\n"
            "\t-v                 verbose; may be repeated\n"
            "\t-H ip4             interface to bind; default 0.0.0.0\n"
            "\t-p port            which port to bind; default 8080\n"
            "\t-x handler_script  path to an executable script to handle requests\n"
            "\t-0 /dev/shm        sends request body to handler_script via its stdin\n"
            "\t                   expects a writable path, like /dev/shm or /tmp\n"
            "\n"
            "The handler_script will receive 2 or 3 arguments:\n"
            "  o the request method\n"
            "  o the request path\n"
            "\n"
            "The headers are passed through the REQHEADERS environment variable.\n"
            "Unless -0 path is specified, request body is passed through the\n"
            "REQBODY environment variable.\n"
            "If -0 dirpath is specified, that location will be used to buffer\n"
            "request bodies. The handler_script may read the body from its stdin\n"
            "\n"
            "Log is on STDERR\n"
            ,
            argv0,
            VERSION);

    printf("\nCompilation options:\n"
            "MAX_BACKLOG=%d\n"
            "REQUEST_SIZE_LIMIT=%d\n"
            "TIMEOUT_LIMIT=%d\n"
            "HANDLER_TIMEOUT_LIMIT=%d\n"
            ,
            MAX_BACKLOG,
            REQUEST_SIZE_LIMIT,
            TIMEOUT_LIMIT,
            HANDLER_TIMEOUT_LIMIT);

    exit(2);
}

int main(int argc, char* argv[])
{
    int opt;
    while((opt = getopt(argc, argv, "H:p:x:hqv0:")) != -1) {
        switch(opt) {
            case 'h': help(argv[0]); return 2;
            case 'x': free(handler); handler = strdup(optarg); break;
            case 'H': iface = inet_addr(optarg); break;
            case 'p': port = atoi(optarg); break;
            case 'q': verbose--; break;
            case 'v': verbose++; break;
            case '0': free(payloadPath); payloadPath = strdup(optarg); break;
            default:
                      fprintf(stderr, "Unknown flag %c\n", opt);
                      help(argv[0]);
                      return 2;
        }
    }

    if(!handler) {
        fprintf(stderr, "You must specify -x handler_script\n");
        exit(2);
    } else {
        handler = strdup(handler);
    }
    if(iface == INADDR_NONE) {
        fprintf(stderr, "Invalid IP passed to -H\n");
        exit(2);
    }
    if(0 != access(handler, R_OK|X_OK))
        err(EXIT_FAILURE, "access(handler_script, r-x)");

    if(port == 0) {
        fprintf(stderr, "Port must be > 0\n");
        exit(2);
    }

    if(payloadPath) {
        struct stat sb;
        if(0 != stat(payloadPath, &sb)) {
            err(EXIT_FAILURE, "stat -0 path");
        } else if(!S_ISDIR(sb.st_mode)) {
            fprintf(stderr, "%s is not a directory\n", payloadPath);
            exit(1);
        } else if(0 != access(payloadPath, R_OK|W_OK|X_OK)) {
            err(EXIT_FAILURE, "access(-0 path, rwx)");
        }
        char* dir = payloadPath;
#define TEMPLATE_TAIL "/jakXXXXXX"
        size_t fullSize = strlen(payloadPath) + strlen(TEMPLATE_TAIL) + 1;
        payloadPath = malloc(fullSize);
        snprintf(payloadPath, fullSize, "%s%s", dir, TEMPLATE_TAIL);
        payloadPath[fullSize - 1] = '\0';
        free(dir);
#undef TEMPLATE_TAIL
    }

    myPid = getpid();

    // establish server

    int hr = 0;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(-1 == sockfd)
        err(EXIT_FAILURE, "socket");

    int nnn = 1;
    if(-1 == setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &nnn, sizeof(int)))
        err(EXIT_FAILURE, "setsockopt(SO_REUSEADDR)");

    struct sockaddr_in sockaddr = {
#ifdef __OpenBSD__
        sizeof(struct sockaddr_in),
#endif
        AF_INET,
        htons(port),
        { iface }
    };
    hr = bind(sockfd, (struct sockaddr*)&sockaddr, sizeof(sockaddr));
    if(-1 == hr)
        err(EXIT_FAILURE, "bind");

    hr = listen(sockfd, MAX_BACKLOG);
    if(-1 == hr)
        err(EXIT_FAILURE, "listen");

    gsock = sockfd;

    if(verbose) {
        char* host = inet_ntoa(sockaddr.sin_addr);
        fprintf(stderr, "Listening on %s:%u\n", host, port);
    }

    signal(SIGINT, sighandler);
    signal(SIGQUIT, sighandler);

    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    //sa.sa_handler = sigchld;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    // if SA_RESTART isn't set, it interrupts accept(3) once, then
    // we never get another SIGCHLD ever again.
    // SA_NOCLDWAIT with SIG_DFL is functinoally equivalent to reaping children,
    // so skip having to handle SIGCHLD
    sa.sa_flags = SA_RESTART|SA_NOCLDWAIT;
    if(sigaction(SIGCHLD, &sa, NULL) == -1)
        err(EXIT_FAILURE, "sigaction");

    // main loop

    // exits on signals
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

        handle(conn, client.sin_addr);
    }
}
