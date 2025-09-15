#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <signal.h>
#include <termios.h>

#include <errno.h>
#include "parse.h"

//excecvp finds the path automatically and then replaces the current image with the one that replaces

//Use forks to make sure the current image doesn't replace the actual shell

//If the process is a child -> The fork results in a PID of 0
    // Then you need to execvp for each of the child processes.

//Look at dup2 for doing all the file redirections



typedef struct {
    Pipe *linePipe; // Optional -> Since pointers, can be null to avoid space
    Command *lineCommand; //Optional // Needs to be either Command or Pipe included
    int isBackground; //Is it a background process
} Line; 

#define MAX_JOBS 20

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE_PENDING } job_status_t;

typedef struct {
    int used;
    int job_id;
    pid_t pgid;
    job_status_t status;
    char *cmd;   
} job_t;

static job_t jobs[MAX_JOBS];
static int highest_job_id = 0;   // for numbering policy


static void signalIntHandler();
static void signalStopHandler();
static void signalChildHandler();
static void init_signal_handlers(void);
static void checkChildrenAndPrintDone(void);
static int applyRedirections(const Command *cmd);



static pid_t shell_pgid;                 // shell’s process group id --> return terminal control to the shell after a job finishes/stops
static volatile sig_atomic_t fg_pgid = 0;// foreground job’s PGID (0 if none) --> signal handlers know which group to forward signals to
static volatile sig_atomic_t sigchld_flag = 0;

//Need to use readline() and then need to use free line right after it





static int getJobByPgid(pid_t pg) {
    for (int i=0;i<20;i++) {
        if (jobs[i].used) {
            if (jobs[i].pgid==pg)
            {
                return i;
            }  
        }
    }
    return -1;
}

static int recentStoppedJobInd(void) {
    int highest_idx = -1; 
    int best_job_id = -1;
    for (int i=0;i<20;i++) {
        if (jobs[i].used) { 
            if (jobs[i].status == JOB_STOPPED)
            {
                if (jobs[i].job_id > best_job_id)
                {
                    best_job_id = jobs[i].job_id; 
                    highest_idx = i; 
                }
            }
            
        }
    }
    return highest_idx;
}

static int addJob(pid_t pgid, job_status_t st, const char *cmdline) {
    int slot = -1;


    for (int i=0;i<20;i++) {
        //Find the lowest one that's not used
        if (!jobs[i].used){ 
            slot = i;
        }
    }
    if (slot<0) {
        return -1;
    }
    jobs[slot].used   = 1; // Means this slot is being used
    jobs[slot].pgid   = pgid;
    jobs[slot].status = st;


    highest_job_id++;
    jobs[slot].job_id = highest_job_id;

    if (cmdline) {
    jobs[slot].cmd = strdup(cmdline);
    } else {
        jobs[slot].cmd = strdup("");
    }

    return slot;
}

static void removeJobIndex(int i) {
    if (!jobs[i].used) {
        return;
    }
    free(jobs[i].cmd);
    memset(&jobs[i], 0, sizeof(jobs[i])); // Clear it all and set to 0 for all values
}

static int retMostRecentJobIndex(void) {
    int highestInd = -1;
    
    int highestID = -1;
    for (int i=0;i<20;i++){
        if (jobs[i].used ==1) 
        { 
            if (jobs[i].job_id > highestID)
            {
                highestID = jobs[i].job_id; 
                highestInd = i; 
            }
            
        }
    }
    return highestInd;
}

static void giveTerminalTo(pid_t pgid) {
    // Best effort; ignore errors if no tty
    tcsetpgrp(STDIN_FILENO, pgid);
}

static void takeTerminalBack(void) {
    tcsetpgrp(STDIN_FILENO, shell_pgid);
}


static int waitForegroundJob(pid_t pgid) {
    int st; 
    pid_t w;
    while(1) {
        w = waitpid(-pgid, &st, WUNTRACED); // Wakes when child exits or stopped with WUntraced instead of just exits
        //R
        if (w < 0) {
            return 0; // no more members -> exited/signaled
        }
        if (WIFSTOPPED(st)) return 1;     // stopped by SIGTSTP, so return 1 and 
        // else a member exited; 
        // keep looping until all processes in the group complete
    }
}



static void signalChildHandler() {

//     Whenever a child changes state (dies, stops, continues), the kernel sends the parent a signal: SIGCHLD.
    //Just set the flag and then wait_pid in the main code since this can be affected by synchronous signal functions
    sigchld_flag = 1;
}


static void checkChildrenAndPrintDone(void) {
    int saw_any = 0;

    while (1) {
        int st;

        //wait pid waits for any child :
        // When it returns:
        // The kernel gives the PID of the finished child
        // It removes the zombie entry
        // now know why the child finished

        // Ask the kernel about any child (-1) that has a state change and returns if it does:
        // WNOHANG: don’t block; return immediately if nothing 
        // WUNTRACED: report stopped children (SIGTSTP)
        // WCONTINUED: report children that were continued after SIGCONT
        // On exit/termination, this also reaps the child (clears zombie)
        pid_t pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break; // Comes here if nothing to do
        saw_any = 1; // At least one

        pid_t pg = getpgid(pid); // So we can track which process group it's attached to // Since it's tracked by pgid

        if (pg < 0) pg = pid;                  // fallback for exited child
        int j = getJobByPgid(pg); // Find job associated with pgid.
        if (j < 0) continue;

        //Three cases for the state change --> Based on int
        if (WIFSTOPPED(st))      jobs[j].status = JOB_STOPPED;
        else if (WIFCONTINUED(st)) jobs[j].status = JOB_RUNNING;
        else                      jobs[j].status = JOB_DONE_PENDING;
    }

    if (!saw_any) return;

    for (int i = 0; i < MAX_JOBS; ++i) {
        //Now check all the jobs that are done and then print done
        if (jobs[i].used && jobs[i].status == JOB_DONE_PENDING) {
            printf("Done       %s\n", jobs[i].cmd);
            fflush(stdout);
            removeJobIndex(i);
        }
    }
}


static void runSingleCommandBackground(Command *cmd, const char *cmdline) {
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0,0);
        signal(SIGINT,SIG_DFL); 
        signal(SIGTSTP,SIG_DFL);
        signal(SIGTTIN,SIG_DFL); 
        signal(SIGTTOU,SIG_DFL);
        signal(SIGCHLD,SIG_DFL);
        if (applyRedirections(cmd) < 0) _exit(127);
        execvp(cmd->argVector[0], cmd->argVector);
        _exit(127);
    } else if (pid > 0) {
        setpgid(pid, pid);
        // record job as RUNNING
        addJob(pid, JOB_RUNNING, cmdline);
    }
}


///
static void init_shell(void) {
    // Put shell in its own process group
    shell_pgid = getpid();
   // setpgid(pid, pgid) changes the process group of pid.
    setpgid(shell_pgid, shell_pgid);        // make the shell its own group leader of the terminal; 
    //puts the shell into a new process group whose ID is its own PID.
    
    
    tcsetpgrp(STDIN_FILENO, shell_pgid);    // shell owns the terminal
//From now on, when the user types Ctrl-C (SIGINT) or Ctrl-Z (SIGTSTP), the kernel will deliver those signals to the 
//foreground process group (initially the shell)
// shell needs to  be the leader of its own group so it can later move jobs (child processes) between foreground/background

    // The shell should not be suspended by tty job-control signals

    //if my shell tries to do terminal I/O while not in the foreground, don’t stop it 

  //  Prevents the shell from being auto-stopped by the kernel during terminal handoffs.
    signal(SIGTTOU, SIG_IGN); // Signal out // Ignore // sent to a background process group that writes to the terminal
    signal(SIGTTIN, SIG_IGN); // Signal in // Ignore // sent to a background process group that reads from the terminal

    //ensure the shell doesn’t get stopped during the brief window before our handlers are in place
    signal(SIGTSTP, SIG_IGN); // Shell itself ignores Ctrl-Z

    // Ignoring prevents the shell from being suspended by the kernel while doing control operations

    // Install Ctrl-C / Ctrl-Z forwarding handlers
    init_signal_handlers();
}

//Need custom ways to handle both Ctrl-Z and Ctrl-C
//Instead of the default actions, the shell needs to be able to catch signals and decide what to do 
//(for example, forward them to the current foreground job).
static void init_signal_handlers(void) {

    //Clears the struct that we use for each signal
    struct sigaction sa = {0};

    //For the interrupting ones:

    // sa_handler is a function pointer for the handler.
    //when a SIGINT (Ctrl-C) comes, call handler
    
    sa.sa_handler = signalIntHandler;

    //Don't block extra signals when this handler is running

     
    //With SA_RESTART, the kernel retries the syscall automatically, so the shell loop doesn’t break every time 
    //would fail with EINTR if a signal interrupted them.
    //With SA_RESTART, the kernel automatically restarts those syscalls.

    //
   // if this signal interrupts a blocking system call, automatically restart that call instead of making it fail with -1
    sa.sa_flags = SA_RESTART;
    //Actually installs the handler for SIGINT

    sigaction(SIGINT, &sa, NULL);

    //Same stuff but for stop
    sa.sa_handler = signalStopHandler;

    //Reset the set of signals that the kernel will temporarily block while the handler is running
   // sa.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa, NULL);

    sa.sa_handler = signalChildHandler; 
    sigaction(SIGCHLD, &sa, NULL);
}

static void signalIntHandler()  {  // Ctrl - C
    
    if (fg_pgid != 0)  // Make sure not our own group
        kill(-fg_pgid, SIGINT);  //Send the interrupt to all the processes in this pgid
}

static void signalStopHandler() {  // Ctrl - Z
    if (fg_pgid != 0) //Make sure not our own group
        kill(-fg_pgid, SIGTSTP); 
}

//argv: A pointer to the argv array of char** and appends one to it
//size: Size of the actual array that's been malloc
//count: Number of elements filled in the array


///////////////
static int applyRedirections(const Command *cmd) {


    // stderr: same as stdout but onto STDERR_FILENO
    if (cmd->err_arg) {
        int fd = open(cmd->err_arg, O_WRONLY|O_CREAT|O_TRUNC,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        if (fd < 0){
            return -1;
        }
        printf("Here");
        if (dup2(fd, STDERR_FILENO) < 0) { 
            close(fd); 
            return -1; 
        }
        close(fd);
    }
    
    // input: must exist
    if (cmd->in_arg) {
        int fd = open(cmd->in_arg, O_RDONLY);
        if (fd < 0) { // On fail, negative integer
            perror(cmd->in_arg);
            return -1;
        }
        //Copy contents of fd into File Input param
        if (dup2(fd, STDIN_FILENO) < 0) { 
            //If the dup fails
            close(fd); 
            return -1; 
        }
        close(fd);
    }
    // stdout: create/truncate with lab-specified perms
    if (cmd->out_arg) {
        int fd = open(cmd->out_arg, O_WRONLY|O_CREAT|O_TRUNC,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        // O_WRONLY = open file for write only 
        // O_CREAT =if the file doesn’t exist, create it
        // O_TRUNC = if the file exists -> 0 length (overwrite)

        //S_IRUSR - user has read permission
        // S_IWUSR = user has write permission
        // S_IRGRP = group has read permission
        // S_IWGRP = group has write permission
        // S_IROTH = others have read permission
        if (fd < 0)
        {
            perror(cmd->out_arg);
            return -1;
        } 
        if (dup2(fd, STDOUT_FILENO) < 0) 
        { 
            close(fd); 
            return -1; 
        }
        close(fd);
    }
    return 0; 
}

static void runSingleCommandForeground(Command *cmd, const char *cmdline) {

    //Create a new process
    pid_t pid = fork();
    //Returns the child pid for the parent
    //Returns 0 for the child

    //After splitting the processes, the child comes here, and the parent goes to the other loop
    if (pid == 0) {
        // child: its own process group

        //Put the child into a new process group whose PGID equals its own PID (the first 0 means 
        //“this process” and the second 0 means use my PID as PGID.
        //Reason is for job control. Each job  lives in its own process group so the shell can 
        //later give terminal control and send signals to the whole job
        setpgid(0, 0);

        // APPLY FILE REDIRECTIONS HERE
        if (applyRedirections(cmd) < 0){ 
            _exit(127); // Kills the child process
        }
        // Replace the child’s image with the target program
        // execvp looks up argVector[0] in PATH
        // cmd->argVector must be a NULL-terminated char*[]
        // On success, this never returns. On failure, it returns -1 and sets errno
        // CHILD ONLY (just after fork, before exec)
        signal(SIGINT,  SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        execvp(cmd->argVector[0], cmd->argVector); // Replace the code with the argvector[0]
        _exit(127); //Kill child if fail
    } else if (pid > 0) {
        // parent: put child in its own pgid and wait (foreground only)
    //    setpgid(pid, pid);
        // int st; (void)st;
        // //Wait until specific child finishes 
        // waitpid(pid, &st, 0);

        setpgid(pid, pid);               // ensure group set again
        fg_pgid = pid;
        giveTerminalTo(pid); // Give tgdrp to pid

        int stopped = waitForegroundJob(pid);  
        takeTerminalBack(); // Give terminal back to shell
        fg_pgid = 0; // Shell is foreground

        if (stopped) {
                if (getJobByPgid(pid) == -1)
                { 
                    addJob(pid, JOB_STOPPED, cmdline);
                }   
        }
    }
}

static void runPipelineForeground(Command *left, Command *right) {
    int fds[2];
    if (pipe(fds) < 0) return;
    //fd[0] is the read end of the pipe // Empty for now
    //fd[1] is the write end of the pipe // Empty for now

    pid_t left_pid = fork();
    if (left_pid == 0) {
        // left child
        setpgid(0, 0);                       // leader pgid = left child's pid
        // wire stdout -> pipe write end
        //Only needed the write end here
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); 
        close(fds[1]);

        // APPLY FILE REDIRECTIONS FOR LEFT AFTER WIRING PIPE
        if (applyRedirections(left) < 0) _exit(127);

        // CHILD ONLY (just after fork, before exec)
        signal(SIGINT,  SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);


        execvp(left->argVector[0], left->argVector);
        _exit(127);
    }
    // parent continues
    setpgid(left_pid, left_pid); //just in case

    pid_t right_pid = fork();
    if (right_pid == 0) {
        // right child
        setpgid(0, left_pid);                // join left's pgid
        //setpgid(pid, pgid)
        // wire stdin <- pipe read end
        dup2(fds[0], STDIN_FILENO);
        //Only needed the read end anyways here
        close(fds[0]); 
        close(fds[1]);

        // APPLY FILE REDIRECTIONS FOR RIGHT AFTER WIRING PIPE
        if (applyRedirections(right) < 0) _exit(127);

        // in run_pipeline_foreground(), right child branch, before execvp:
        signal(SIGINT,  SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);


        execvp(right->argVector[0], right->argVector);
        _exit(127);
    }

    // parent
    setpgid(right_pid, left_pid); // Make right join the left
    close(fds[0]); 
    close(fds[1]);

    // wait for both (foreground)
    // int st;
    // waitpid(left_pid,  &st, 0);
    // waitpid(right_pid, &st, 0);


    //Waiting for the entire group since the left one is the group leader
    fg_pgid = left_pid;
    giveTerminalTo(left_pid);

    waitForegroundJob(left_pid);     // waits the whole group

    takeTerminalBack();
    fg_pgid = 0;
}



int main(void) {
    init_shell();

    while(1){

        char *line = readline("# ");
        checkChildrenAndPrintDone(); 
        
        if (line == NULL) {   // Ctrl-D
            break;
        }

        char *orig = strdup(line);

        // Trim trailing spaces/tabs
        for (size_t L = strlen(line); L && (line[L-1]==' ' || line[L-1]=='\t'); --L) {
            line[L-1] = '\0';
        }

        // Empty line? Just continue
        if (line[0] == '\0') {
            free(line);
            free(orig);
            //This restarts it
            continue;
        }

        // Create a container for this line
        Line *lineStruct = calloc(1, sizeof(Line)); // Zero fills all of them
    
        // Background?
        size_t len = strlen(line);
        //Has to be the last line since we trimmed the white spaces
        if (len > 0 && line[len - 1] == '&') {
            lineStruct->isBackground = 1;
            //Put a null pointer at the end
            line[len - 1] = '\0';
            // retrim
            for (size_t L = strlen(line); L && (line[L-1]==' ' || line[L-1]=='\t'); --L) {
                line[L-1] = '\0';
            }
            //This is to prevent just the & being passed in
            if (line[0] == '\0') {   // & alone is invalid 
                printf("\n");
                free(lineStruct);
                free(orig);
                free(line);
                continue;
            }
        } else {
            lineStruct->isBackground = 0;
        }

        // Detect pipe
        char *pipeIndex = strchr(line, '|');

        // Case 1: pipeline present
        if (pipeIndex) {
            // Can't have a pipe and a isBackground
            if (lineStruct->isBackground) {
                printf("\n"); // Create a new line
                free(lineStruct);
                free(orig);
                free(line);
                continue;
            }
            lineStruct->linePipe = malloc(sizeof(Pipe));

            if (parsePipe(line, lineStruct->linePipe) != 0) {
                putchar('\n');
                free_pipe(lineStruct->linePipe);
                free(lineStruct->linePipe);
                free(lineStruct);
                free(orig);
                free(line);
                continue;
            }

            runPipelineForeground(lineStruct->linePipe->left, lineStruct->linePipe->right);

            // Free structs
            free_pipe(lineStruct->linePipe);
            free(lineStruct->linePipe);
            free(lineStruct);
            free(orig);
            free(line);
            continue;

        }

        // Case 2: single command (no pipe)
        lineStruct->lineCommand = malloc(sizeof(Command));

        //Something wrong with the command:
        if (parseCommand(line, lineStruct->lineCommand) != 0) {
            printf("\n");
            free_command(lineStruct->lineCommand);
            free(lineStruct->lineCommand);
            free(lineStruct);
            free(orig);
            free(line);
            continue;
        }


        // jobs, fg, bg
        Command *c = lineStruct->lineCommand;
        if (c->argVector && c->argVector[0]) {
            if (strcmp(c->argVector[0], "jobs") == 0) {
                // print table
                int mostRecent = retMostRecentJobIndex();
                for (int i=0;i<20;i++) 
                    if (jobs[i].used == 1) {
                        char mark;
                        if (i == mostRecent) {
                            mark = '+';
                        } else {
                            mark = '-';
                        }

                        const char *st;
                        if (jobs[i].status == JOB_RUNNING) {
                            st = "Running";
                        } else {
                            st = "Stopped";
                        }

                        printf("[%d] %c   %s       %s\n",
                            jobs[i].job_id, mark, st, jobs[i].cmd);
                }
                printf("\n");
                // cleanup structs and continue loop
                free_command(c); 
                free(c); 
                free(lineStruct); 
                free(orig); 
                free(line); 
                continue;
            }
            if (strcmp(c->argVector[0], "fg") == 0) {
                int mostrecent = retMostRecentJobIndex();
                if (mostrecent != -1) {
                    pid_t pg = jobs[mostrecent].pgid;
                    // print original command 
                    printf("%s\n", jobs[mostrecent].cmd);
                    fflush(stdout);

                    // continue and wait in foreground
                    kill(-pg, SIGCONT);
                    fg_pgid = pg; 
                    giveTerminalTo(pg);
                    int stopped = waitForegroundJob(pg);
                    takeTerminalBack(); 
                    fg_pgid = 0;

                    if (kill(-pg, 0) == -1 && errno == ESRCH) {
                        // fully exited
                        removeJobIndex(mostrecent);
                    } else if (stopped) {
                        jobs[mostrecent].status = JOB_STOPPED;
                    } else {
                        // still alive but not stopped is unlikely here
                        jobs[mostrecent].status = JOB_RUNNING;
                    }
                }
                printf("\n");
                free_command(c); 
                free(c); 
                free(lineStruct); 
                free(orig); 
                free(line);
                continue;
            }

            if (strcmp(c->argVector[0], "bg") == 0) {
                int sj = recentStoppedJobInd();
                if (sj != -1) {
                    jobs[sj].status = JOB_RUNNING;
                    kill(-jobs[sj].pgid, SIGCONT);
                    int mr = retMostRecentJobIndex();
                    char mark;
                    if (sj == mr) {
                        mark = '+';
                    } else {
                        mark = '-';
                    }
                    //Make sure it doesn't print the & twice
                    if (strchr(jobs[sj].cmd, '&') != NULL)
                    {
                        printf("[%d] %c   Running       %s\n",
                        jobs[sj].job_id, mark, jobs[sj].cmd);
                    }
                    else{
                    printf("[%d] %c   Running       %s &\n",
                        jobs[sj].job_id, mark, jobs[sj].cmd);
                    }
                }
                printf("\n");
                free_command(c); 
                free(c); 
                free(lineStruct); 
                free(orig); 
                free(line);
                continue;
            }

        }


        // foreground/background
        if (lineStruct->isBackground) {
            runSingleCommandBackground(lineStruct->lineCommand, orig);
        } else {
            runSingleCommandForeground(lineStruct->lineCommand, orig);
        }

        // Cleanup 
        free_command(lineStruct->lineCommand);
        free(orig);
        free(lineStruct->lineCommand);
        free(lineStruct);
        free(line);
    }

    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used) {
            kill(-jobs[i].pgid, SIGTERM);
        }
    }
    usleep(100*1000); // 100ms grace
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used && kill(-jobs[i].pgid, 0) == 0) {
            kill(-jobs[i].pgid, SIGKILL);
        }
    }


    return 0;
}

//Returns 1 if there's a pipe. Returns 0 if not



//Make sure to use strtok_r(char* str, const char* delim, char** saveptr) // Make sure to save the original. // This is for the input tokens
