
#include <stdio.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <iostream>

#include "tokenize.h"
#include "tcp-utils.h" 

#define MAX_BUFFER_SIZE 1024

/*
To implement the shell in CPP language, we have refered to this links for refrence.
And also used ChatGPT for programming syntax, Sockets and for debugging purpose.
*/

/*
 * Global configuration variables.
 */
const char* path[] = {"/bin","/usr/bin",0}; // path, null terminated (static)
const char* prompt = "sshell> ";            // prompt
const char* config = "shconfig";            // configuration file

/*
 * Typical reaper.
 */
void zombie_reaper (int signal) {
    int ignore;
    while (waitpid(-1, &ignore, WNOHANG) >= 0)
        /* NOP */;
}

/*
 * Dummy reaper, set as signal handler in those cases when we want
 * really to wait for children.  Used by run_it().
 *
 * Note: we do need a function (that does nothing) for all of this, we
 * cannot just use SIG_IGN since this is not guaranteed to work
 * according to the POSIX standard on the matter.
 */
void block_zombie_reaper (int signal) {
    /* NOP */
}

/*
 * run_it(c, a, e, p) executes the command c with arguments a and
 * within environment p just like execve.  In addition, run_it also
 * awaits for the completion of c and returns a status integer (which
 * has the same structure as the integer returned by waitpid). If c
 * cannot be executed, then run_it attempts to prefix c successively
 * with the values in p (the path, null terminated) and execute the
 * result.  The function stops at the first match, and so it executes
 * at most one external command.
 */
int run_it (const char* command, char* const argv[], char* const envp[], const char** path) {

    // we really want to wait for children so we inhibit the normal
    // handling of SIGCHLD
    signal(SIGCHLD, block_zombie_reaper);

    int childp = fork();
    int status = 0;

    if (childp == 0) { // child does execve
#ifdef DEBUG
        fprintf(stderr, "%s: attempting to run (%s)", __FILE__, command);
        for (int i = 1; argv[i] != 0; i++)
            fprintf(stderr, " (%s)", argv[i]);
        fprintf(stderr, "\n");
#endif
        execve(command, argv, envp);     // attempt to execute with no path prefix...
        for (size_t i = 0; path[i] != 0; i++) { // ... then try the path prefixes
            char* cp = new char [strlen(path[i]) + strlen(command) + 2];
            sprintf(cp, "%s/%s", path[i], command);
#ifdef DEBUG
            fprintf(stderr, "%s: attempting to run (%s)", __FILE__, cp);
            for (int i = 1; argv[i] != 0; i++)
                fprintf(stderr, " (%s)", argv[i]);
            fprintf(stderr, "\n");
#endif
            execve(cp, argv, envp);
            delete [] cp;
        }

        // If we get here then all execve calls failed and errno is set
        char* message = new char [strlen(command)+10];
        sprintf(message, "exec %s", command);
        perror(message);
        delete [] message;
        exit(errno);   // crucial to exit so that the function does not
                       // return twice!
    }

    else { // parent just waits for child completion
        waitpid(childp, &status, 0);
        // we restore the signal handler now that our baby answered
        signal(SIGCHLD, zombie_reaper);
        return status;
    }
}

/*
 * Implements the internal command `more'.  In addition to the file
 * whose content is to be displayed, the function also receives the
 * terminal dimensions.
 */
void do_more(const char* filename, const size_t hsize, const size_t vsize) {
    const size_t maxline = hsize + 256;
    char* line = new char [maxline + 1];
    line[maxline] = '\0';

    // Print some header (useful when we more more than one file)
    printf("--- more: %s ---\n", filename);

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        sprintf(line, "more: %s", filename);
        perror(line);
        delete [] line;
        return;
    }

    // main loop
    while(1) {
        for (size_t i = 0; i < vsize; i++) {
            int n = readline(fd, line, maxline);
            if (n < 0) {
                if (n != recv_nodata) {  // error condition
                    sprintf(line, "more: %s", filename);
                    perror(line);
                }
                // End of file
                close(fd);
                delete [] line;
                return;
            }
            line[hsize] = '\0';  // trim longer lines
            printf("%s\n", line);
        }
        // next page...
        printf(":");
        fflush(stdout);
        fgets(line, 10, stdin);
        if (line[0] != ' ') {
            close(fd);
            delete [] line;
            return;
        }
    }
    delete [] line;
}

int main (int argc, char** argv, char** envp) {
    size_t hsize = 0, vsize = 0;  // terminal dimensions, read from
                                  // the config file
    int rport = 0;
    char rhost[1024];
    char command[129];   // buffer for commands
    command[128] = '\0';
    char ip[129];   // buffer for commands
    ip[128] = '\0';
    char* com_tok[129];  // buffer for the tokenized commands
    size_t num_tok;      // number of tokens
    int rhostFound = 0;
    bool connectionOpen = false;  // Flag to track connection state
    int remoteServerSocket = -1;  // Socket for the remote server
    int keepalive = 0;

    printf("Simple shell v1.0.\n");

    // Config:
    int confd = open(config, O_RDONLY);
    if (confd < 0) {
        perror("config");
        fprintf(stderr, "config: cannot open the configuration file.\n");
        fprintf(stderr, "config: will now attempt to create one.\n");
        confd = open(config, O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR);
        if (confd < 0) {
            // cannot open the file, giving up.
            perror(config);
            fprintf(stderr, "config: giving up...\n");
            return 1;
        }
        close(confd);
        // re-open the file read-only
        confd = open(config, O_RDONLY);
        if (confd < 0) {
            // cannot open the file, we'll probably never reach this
            // one but who knows...
            perror(config);
            fprintf(stderr, "config: giving up...\n");
            return 1;
        }
    }

    // read terminal size
    while (hsize == 0 || vsize == 0 || rhostFound == 0 || rport == 0) {
        int n = readline(confd, command, 128);
        if (n == recv_nodata)
            break;
        if (n < 0) {
            sprintf(command, "config: %s", config);
            perror(command);
            break;
        }
        num_tok = str_tokenize(command, com_tok, strlen(command));
        // printf("com_tok[0]: %s\n", com_tok[0]);
        // note: VSIZE and HSIZE can be present in any order in the
        // configuration file, so we keep reading it until the
        // dimensions are set (or there are no more lines to read)
        if (strcmp(com_tok[0], "VSIZE") == 0 && atol(com_tok[1]) > 0)
            vsize = atol(com_tok[1]);
        else if (strcmp(com_tok[0], "HSIZE") == 0 && atol(com_tok[1]) > 0)
            hsize = atol(com_tok[1]);
        
        else if (strcmp(com_tok[0], "RHOST") == 0){
            strcpy(rhost, com_tok[1]);
            rhostFound = 1;
        }
        else if (strcmp(com_tok[0], "RPORT") == 0 && atol(com_tok[1]) > 0){
            // printf("rhost: %s\n", rhost);
            rport = atol(com_tok[1]);
        }
        // lines that do not make sense are hereby ignored
    }
    close(confd);

    if (hsize <= 0) {
        // invalid horizontal size, will use defaults (and write them
        // in the configuration file)
        hsize = 75;
        confd = open(config, O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR);
        write(confd, "HSIZE 75\n", strlen( "HSIZE 75\n"));
        close(confd);
        fprintf(stderr, "%s: cannot obtain a valid horizontal terminal size, will use the default\n", 
                config);
    }
    if (vsize <= 0) {
        // invalid vertical size, will use defaults (and write them in
        // the configuration file)
        vsize = 40;
        confd = open(config, O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR);
        write(confd, "VSIZE 40\n", strlen( "VSIZE 40\n"));
        close(confd);
        fprintf(stderr, "%s: cannot obtain a valid vertical terminal size, will use the default\n",
                config);
    }

    printf("Terminal set to %ux%u.\n", (unsigned int)hsize, (unsigned int)vsize);
    printf("Remort Host set to %s:%i.\n", rhost, rport);

    // install the typical signal handler for zombie cleanup
    // (we will inhibit it later when we need a different behaviour,
    // see run_it)
    signal(SIGCHLD, zombie_reaper);


    // Command loop:
    while(1) {

        // Read input:
        printf("%s",prompt);
        fflush(stdin);
        if (fgets(command, 128, stdin) == 0) {
            // EOF, will quit
            printf("\nBye\n");
            return 0;
        }
        // fgets includes the newline in the buffer, get rid of it
        if(strlen(command) > 0 && command[strlen(command) - 1] == '\n')
            command[strlen(command) - 1] = '\0';

        // printf("Input: %s\n", command);
        strcpy(ip, command);
        // printf("Input ip: %s\n", ip);
        // Tokenize input:
        char** real_com = com_tok;  // may need to skip the first
                                    // token, so we use a pointer to
                                    // access the tokenized command
        num_tok = str_tokenize(command, real_com, strlen(command));
        real_com[num_tok] = 0;      // null termination for execve

        int bg_local = 0;
        int bg_remote = 0;
        if (strcmp(real_com[0], "!") == 0 && strcmp(real_com[1], "&") == 0) {
            bg_local = 1;
        } else if (strcmp(real_com[0], "&") == 0) {
            bg_remote = 1;
        }

        if (strlen(real_com[0]) == 0) {
            continue;
        } else if (strcmp(real_com[0], "!") == 0 && strcmp(real_com[1], "exit") == 0) {
            printf("Bye\n");
            return 0;
        } else if (strcmp(real_com[0], "!") == 0 && strcmp(real_com[1], "keepalive") == 0) { 
            printf("Keepalive is issued, so trying to open a connection.\n");
            if (!connectionOpen) {
                remoteServerSocket = connectbyportint(rhost, rport);
                if (remoteServerSocket >= 0) {
                    connectionOpen = true;
                    keepalive = 1;
                    printf("Server Connection is established to execute the command.\n");
                } else {
                    printf("Failed to established a connection to the server.\n");
                }
            } else {
                printf("A connection to the server is already open.\n");
            }

        } else if (strcmp(real_com[0], "!") == 0 && strcmp(real_com[1], "close") == 0) { 
            printf("Close is issued, so trying to close the connection.\n");
            if (remoteServerSocket != -1 && connectionOpen) {
                connectionOpen = false;
                close(remoteServerSocket);
                remoteServerSocket = -1;
                keepalive = 0;
                printf("Connection Closed.\n");
            }
            else{
                printf("Connection Already Closed.\n");
            }
        } else if (keepalive){
            printf("keepalive is still ON, So It will be remort cmd\n");
            if (connectionOpen && remoteServerSocket != -1) {
                char response[MAX_BUFFER_SIZE];
                command[strlen(command)] = '\n';
                ssize_t bytes_sent = send(remoteServerSocket, command, strlen(command), 0);

                if (bytes_sent == -1) {
                    perror("Write failed");
                    exit(1);
                }
                while (1) {
                    int bytes_received = recv_nonblock(remoteServerSocket, response, MAX_BUFFER_SIZE, 2000); // Adjust the timeout as needed
                    if (bytes_received == recv_nodata) {
                        break;
                    } else if (bytes_received == 0) {
                        printf("Connection closed by the other end.\n");
                        connectionOpen = false;
                        remoteServerSocket = -1;
                        keepalive = 0;
                        break;
                    } else {
                        fwrite(response, 1, bytes_received, stdout);
                    }
                }
            }

        } else if (strcmp(real_com[0], "!") == 0) {
            if (bg_local) {
                int bgp = fork();
                if (bgp == 0) {
                    int r = run_it(real_com[2], real_com + 2, envp, path);
                    printf("& %s done (%d)\n", real_com[2], WEXITSTATUS(r));
                    if (r != 0) {
                        printf("& %s completed with a non-null exit code\n", real_com[2]);
                    }
                    return 0;
                } else {
                    continue;
                }
            } else {
                int r = run_it(real_com[1], real_com + 1, envp, path);
                if (r != 0) {
                    printf("%s completed with a non-null exit code (%d)\n", real_com[1], WEXITSTATUS(r));
                }
            }
        } else {
            if (bg_remote) {
                strcpy(command, ip + 2);
                if (!connectionOpen) {
                    remoteServerSocket = connectbyportint(rhost, rport);
                    if (remoteServerSocket >= 0) {
                        connectionOpen = true;
                        printf("Server Connection is established to execute the command.\n");
                    } else {
                        std::cerr << "Failed to established a connection to the server.\n";
                    }
                } else {
                    printf("A connection to the server is already open.\n");
                }

                if (connectionOpen && remoteServerSocket != -1) {
                    char response[MAX_BUFFER_SIZE];
                    command[strlen(command)] = '\n';
                    ssize_t bytes_sent = send(remoteServerSocket, command, strlen(command), 0);

                    if (bytes_sent == -1) {
                        perror("Write failed");
                        exit(1);
                    }

                    int bytes_received = recv(remoteServerSocket, response, MAX_BUFFER_SIZE, 0);

                    if (bytes_received == -1) {
                        perror("Error while receiving data");
                    } else if (bytes_received == 0) {
                        printf("Connection closed by the other end.\n");
                        connectionOpen = false;
                        remoteServerSocket = -1;
                        keepalive = 0;
                    } else {
                        fwrite(response, 1, bytes_received, stdout);
                    }
                }
                if (remoteServerSocket != -1) {
                    connectionOpen = false;
                    close(remoteServerSocket);
                    remoteServerSocket = -1;
                    keepalive = 0;
                    printf("Connection Closed.\n");
                }
            } else {
                if (!connectionOpen) {
                    remoteServerSocket = connectbyportint(rhost, rport);
                    if (remoteServerSocket >= 0) {
                        connectionOpen = true;
                        printf("Server Connection is established to execute the command.\n");
                    } else {
                        std::cerr << "Failed to established a connection to the server.\n";
                    }
                } else {
                    printf("A connection to the server is already open.\n");
                }

                if (connectionOpen && remoteServerSocket != -1) {
                    char response[MAX_BUFFER_SIZE];
                    command[strlen(command)] = '\n';
                    ssize_t bytes_sent = send(remoteServerSocket, command, strlen(command), 0);

                    if (bytes_sent == -1) {
                        perror("Write failed");
                        exit(1);
                    }
                    while (1) {
                        int bytes_received = recv_nonblock(remoteServerSocket, response, MAX_BUFFER_SIZE, 2000); // Adjust the timeout as needed

                        if (bytes_received == recv_nodata) {
                            break;
                        } else if (bytes_received == 0) {
                            printf("Connection closed by the other end.\n");
                            connectionOpen = false;
                            remoteServerSocket = -1;
                            keepalive = 0;
                            break;
                        } else {
                            fwrite(response, 1, bytes_received, stdout);
                        }
                    }
                }
                if (remoteServerSocket != -1) {
                    connectionOpen = false;
                    close(remoteServerSocket);
                    remoteServerSocket = -1;
                    keepalive = 0;
                    printf("Connection Closed.\n");
                }
            }
        }
    }
    // Cleanup and close the remote server socket if open
    if (remoteServerSocket != -1) {
        printf("Going to close the connection.\n");
        close(remoteServerSocket);
        remoteServerSocket = -1;
    }

    return 0;
}
