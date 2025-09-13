//RECHECK FOR SPACING ISSUES // MIGHT NEED TO TRUNCATE STUFF LATER

//RUN ON LINUX AND CHANGE THE MAKEFILE !!!!!
//Foreground jobs don't need to be in job table

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

//excecvp finds the path automatically and then replaces the current image with the one that replaces

//Use forks to make sure the current image doesn't replace the actual shell

//If the process is a child -> The fork results in a PID of 0
    // Then you need to execvp for each of the child processes.

//Look at dup2 for doing all the file redirections

//Struct for Command: 
typedef struct {
    char** argVector;  // argv[0] = commandName, argv[1..] = args 
    char* in_arg; // Optional -> Set to null if not used
    char* out_arg; // Optional 
    char* err_arg; //Optional
} Command;

typedef struct {
    Command *left;
    Command *right;
} Pipe;

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
    char *cmd;   // heap copy of original line
} job_t;

static job_t jobs[MAX_JOBS];
static int highest_job_id = 0;   // for numbering policy


static void sigint_handler(int);
static void sigtstp_handler(int);
static void sigchld_handler(int);
static void install_signal_handlers(void);
static void reap_children_and_print_done(void);
static int apply_redirections(const Command *cmd);



static pid_t shell_pgid;                 // shell’s process group id --> return terminal control to the shell after a job finishes/stops
static volatile sig_atomic_t fg_pgid = 0;// foreground job’s PGID (0 if none) --> signal handlers know which group to forward signals to
static volatile sig_atomic_t sigchld_flag = 0;

//Need to use readline() and then need to use free line right after it



static int most_recent_job_index(void) {
    int best = -1, best_id = -1;
    for (int i=0;i<MAX_JOBS;i++)
        if (jobs[i].used && jobs[i].job_id > best_id) { best_id = jobs[i].job_id; best = i; }
    return best;
}

static int find_job_by_pgid(pid_t pg) {
    for (int i=0;i<MAX_JOBS;i++) if (jobs[i].used && jobs[i].pgid==pg) return i;
    return -1;
}

static int find_most_recent_stopped(void) {
    int idx = -1, best_id = -1;
    for (int i=0;i<MAX_JOBS;i++)
        if (jobs[i].used && jobs[i].status==JOB_STOPPED && jobs[i].job_id > best_id) { best_id=jobs[i].job_id; idx=i; }
    return idx;
}

static int alloc_job_slot(void) {
    for (int i=0;i<MAX_JOBS;i++) if (!jobs[i].used) return i;
    return -1;
}

static int add_job(pid_t pgid, job_status_t st, const char *cmdline) {
    int slot = alloc_job_slot(); if (slot<0) return -1;
    jobs[slot].used   = 1;
    jobs[slot].pgid   = pgid;
    jobs[slot].status = st;
    jobs[slot].job_id = ++highest_job_id;
    jobs[slot].cmd    = strdup(cmdline ? cmdline : "");
    return slot;
}

static void remove_job_index(int i) {
    if (!jobs[i].used) return;
    free(jobs[i].cmd);
    memset(&jobs[i], 0, sizeof(jobs[i]));
}



static void give_terminal_to(pid_t pgid) {
    // Best effort; ignore errors if no tty
    tcsetpgrp(STDIN_FILENO, pgid);
}

static void take_terminal_back(void) {
    tcsetpgrp(STDIN_FILENO, shell_pgid);
}

/**
 * Wait for the entire foreground process group.
 * Returns 1 if the group stopped (WIFSTOPPED), 0 if it exited/was signaled.
 */
// static int wait_foreground_job(pid_t pgid) {
//     int st;
//     pid_t w;
//     for (;;) {
//         int st; pid_t pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED);
//         if (pid <= 0) break;
//         pid_t pg = getpgid(pid);
//         if (pg < 0) continue;
//         int j = find_job_by_pgid(pg);
//         if (j < 0) continue;

//         if (WIFSTOPPED(st)) {
//             jobs[j].status = JOB_STOPPED;
//         } else if (WIFCONTINUED(st)) {
//             jobs[j].status = JOB_RUNNING;
//         } else if (WIFEXITED(st) || WIFSIGNALED(st)) {
//             jobs[j].status = JOB_DONE_PENDING;
//             // also push to done_q if you want; or just rely on status here
//         }
//     }

//     // Now emit deferred Done lines and remove them
//     for (int i=0;i<MAX_JOBS;i++) if (jobs[i].used && jobs[i].status==JOB_DONE_PENDING) {
//         printf("Done       %s\n", jobs[i].cmd);
//         remove_job_index(i);
//     }

// }
static int wait_foreground_job(pid_t pgid) {
    int st; pid_t w;
    while(1) {
        w = waitpid(-pgid, &st, WUNTRACED);
        if (w < 0) {
            if (errno == ECHILD) return 0; // no more members → exited/signaled
            return 0;
        }
        if (WIFSTOPPED(st)) return 1;     // stopped by SIGTSTP
        // else a member exited; keep looping until group gone
    }
}



static void sigchld_handler(int signo) {

//     Whenever a child changes state (dies, stops, continues), the kernel sends the parent a signal: SIGCHLD.
// This is like the kernel tapping the parent on the shoulder:
// “Hey, something happened to your kid. You might want to call waitpid().”
    //Just set the flag and then wait_pid in the main code since this can be affected by synchronous signal functions
    sigchld_flag = 1;
}

// REPLACE the whole function:
static void reap_children_and_print_done(void) {
    int saw_any = 0;

    while (1) {
        int st;

        //wait pid waits for any child :
        // When it returns:
        // The kernel gives you the PID of the finished child.
        // It removes the zombie entry.
        // You now know why/how the child finished.
        pid_t pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break;
        saw_any = 1;

        pid_t pg = getpgid(pid);
        if (pg < 0) pg = pid;                  // fallback for exited child
        int j = find_job_by_pgid(pg);
        if (j < 0) continue;

        if (WIFSTOPPED(st))      jobs[j].status = JOB_STOPPED;
        else if (WIFCONTINUED(st)) jobs[j].status = JOB_RUNNING;
        else                      jobs[j].status = JOB_DONE_PENDING;
    }

    if (!saw_any) return;

    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used && jobs[i].status == JOB_DONE_PENDING) {
            printf("Done       %s\n", jobs[i].cmd);
            fflush(stdout);
            remove_job_index(i);
        }
    }
}


static void run_single_command_background(Command *cmd, const char *cmdline) {
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0,0);
        signal(SIGINT,SIG_DFL); signal(SIGTSTP,SIG_DFL);
        signal(SIGTTIN,SIG_DFL); signal(SIGTTOU,SIG_DFL);
        signal(SIGCHLD,SIG_DFL);
        if (apply_redirections(cmd) < 0) _exit(127);
        execvp(cmd->argVector[0], cmd->argVector);
        _exit(127);
    } else if (pid > 0) {
        setpgid(pid, pid);
        // record job as RUNNING
        (void)add_job(pid, JOB_RUNNING, cmdline);
    }
}



static void init_shell(void) {
    // Put shell in its own process group
    shell_pgid = getpid();
   // setpgid(pid, pgid) changes the process group of pid.
    setpgid(shell_pgid, shell_pgid);        // make the shell its own group leader of the terminal; 
    //puts the shell into a new process group whose ID is its own PID.
    
    
    tcsetpgrp(STDIN_FILENO, shell_pgid);    // shell owns the terminal
//From now on, when the user types Ctrl-C (SIGINT) or Ctrl-Z (SIGTSTP), the kernel will deliver those signals to the 
//foreground process group (initially the shell; later you’ll switch foreground to the running job)
//Why: an interactive shell should be the leader of its own group so it can later move jobs (child processes) between foreground/background cleanly.

    // The shell should not be suspended by tty job-control signals

    //if my shell tries to do terminal I/O while not in the foreground, don’t stop it 

  //  Prevents the shell from being auto-stopped by the kernel during terminal handoffs.
    signal(SIGTTOU, SIG_IGN); // Signal out // Ignore // sent to a background process group that writes to the terminal.
    signal(SIGTTIN, SIG_IGN); // Signal in // Ignore // sent to a background process group that reads from the terminal.

    //ensure the shell doesn’t get stopped during the brief window before our handlers are in place
    signal(SIGTSTP, SIG_IGN); // Shell itself ignores Ctrl-Z

    // A shell sometimes has to do terminal work (e.g., tcsetpgrp) even while it’s technically in the background during handoffs. 
    // Ignoring these prevents the shell from being accidentally suspended by the kernel while doing legitimate control operations.

    // Install Ctrl-C / Ctrl-Z forwarding handlers
    install_signal_handlers();
}

//Need custom ways to handle both Ctrl-Z and Ctrl-C
//Instead of the default actions (e.g., terminate on SIGINT), we want the shell to catch signals and decide what to do 
//(usually: forward them to the current foreground job).
static void install_signal_handlers(void) {

    //Clears the struct that we use for each signal
    struct sigaction sa = {0};

    //For the interrupting ones:

    // sa_handler is a function pointer for the handler.
    // Here we say: when a SIGINT (Ctrl-C) arrives, call our function sigint_handler().
    
    //When SIGINT comes, use sigint_handler
    sa.sa_handler = sigint_handler;

    //Don't block extra signals when this handler is running
    sigemptyset(&sa.sa_mask);

    //Without this, some syscalls (like read()Normally, if a signal interrupts a blocking system call (read(), wait(), etc.), the syscall fails with error EINTR.
//With SA_RESTART, the kernel retries the syscall automatically, so your shell loop doesn’t break every time the user hits Ctrl-C.) 
//would fail with EINTR if a signal interrupted them.
    //With SA_RESTART, the kernel automatically restarts those syscalls.

    //
    sa.sa_flags = SA_RESTART;
    //Actually installs the handler for SIGINT

    // This tells the kernel: “For SIGINT, use the rules in sa.”
    // From now on, when the user presses Ctrl-C, the shell doesn’t die—it calls sigint_handler() instead.
    sigaction(SIGINT, &sa, NULL);

    //Same stuff but for stop
    sa.sa_handler = sigtstp_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa, NULL);

    sa.sa_handler = sigchld_handler; 
    sigaction(SIGCHLD, &sa, NULL);
}

static void sigint_handler(int signo)  { 
    if (fg_pgid) 
        kill(-fg_pgid, SIGINT);  
}

static void sigtstp_handler(int signo) { 
    if (fg_pgid) 
        kill(-fg_pgid, SIGTSTP); 
}

static void create_command(Command *cmd) {
    cmd->argVector = NULL;
    cmd->in_arg  = NULL;
    cmd->out_arg = NULL;
    cmd->err_arg = NULL;
}

static void free_command(Command *cmd) {
    if (!cmd) 
    {
        return;
    }
    free(cmd->argVector);      // argv entries point inside the line buffer; don't free them
    // in_arg/out_arg/err_arg also point into the same buffer; don't free them
    memset(cmd, 0, sizeof(*cmd)); // What's the point if this?
}

void free_pipe(Pipe *p) {
    if (!p) 
    {
        return;
    }
    if (p->left)  
    { 
        free_command(p->left);  
        free(p->left);  
    }
    if (p->right) 
    { 
        free_command(p->right); 
        free(p->right); 
    }
}
//argv: A pointer to the argv array of char** and appends one to it
//size: Size of the actual array that's been malloc
//count: Number of elements filled in the array

static int argv_push(char ***argv, size_t *size, size_t *count, char *tok) {
    
    
    //First check if the argv has any space yet:
    if( *argv == NULL)
    {
        
        *size  = 4; //Starting size of argument vector
        *argv = malloc((*size) * sizeof(char*)); // 4*8 = 32, 
        //Maybe add if a malloc fails here?
    }

    //Check if need to realloc
    if (*count+1>=*size)
    {
        *size *=2 ; // Double the size
        *argv = realloc(*argv, (*size) * sizeof(char*));

        //Check if the malloc returns null?
    }

    (*argv)[*count] = tok; // Store the token pointer

    (*count)++;

    (*argv)[*count] = NULL;  // null terminate just in case

    return 0;
    
}

int parse_command(char* side, Command* output)
{
    //Initialize the command:
    create_command(output);

    size_t argCount = 0;
    size_t argSizeAllocced = 0;


    char *save = NULL;
    char *tok  = strtok_r(side, " \t", &save);

    
    int state = 0; // 0 is for Normal, 1 is for File In, 2 is for File Out, 3 is for File Err
    
    while(tok)
    {

        // if (strcmp(tok, "&") == 0) {        // if you kept '&' on this side, just skip it
        //     tok = strtok_r(NULL, " \t", &save);
        //     continue;
        // }

        //In the normal state
        if (state == 0) {
            if (strcmp(tok, "<") == 0) {
                state = 1;
            } else if (strcmp(tok, ">") == 0) {
                state = 2;
            } else if (strcmp(tok, "2>") == 0) {
                state = 3;
            } else {
                if (argv_push(&output->argVector, &argSizeAllocced, &argCount, tok) != 0) return -1;
            }
        }

        //State is one of the file ones
        else{

            //Has to be a file name
            if (*tok == '\0'){
                return -1; //Return an error
            }

            if (state == 1)
            {
                // Can only have one in_arg for each command
                if (output->in_arg)
                {
                    return -1;
                }
                output->in_arg = tok;
            }
            else if (state == 2)
            {
                // Can only have one out_arg for each command
                if (output -> out_arg)
                {
                    return -1;
                }
                output->out_arg = tok;
            }
            else if(state == 3)
            {
                // Can only have one err_arg for each command
                if (output -> err_arg)
                {
                    return -1;
                }
                output->err_arg = tok;
            }
            //Reset the state
            state = 0;
        }

        //Go on to the next token
        tok = strtok_r(NULL, " \t", &save);
    }

    //Has to be set to normal or it's not a valid command, since it's expecting a file name or something
    if (state != 0) 
    {
        return -1;
    }
    //Need to check if the file redirection thing has been set:

    //Need to make sure argVector is not null and that there's at least one command in the argVector
    if (!output->argVector || !output->argVector[0]) 
    {
        return -1;
    }

    //Successfully have parsed the command
    return 0; 
}

int parsePipe(char *line, Pipe *outPipe)
{
    //Initialize both of the commands
    outPipe->left = NULL;
    outPipe->right = NULL;

    char* leftSidePipe = line; 
    char* pipeIndex = strchr(line, '|');

    if (!pipeIndex){
        return -1;
    } 
    //Close off one of the sides (where the pipe is):
    *pipeIndex = '\0';
    char *rightSidePipe = pipeIndex + 1;   // just advance the pointer

    //Just in case trim any of the white spaces:
    while (*rightSidePipe == ' ' || *rightSidePipe == '\t')
    {
        rightSidePipe++;
    } 

    //There are multiple pipes: 
    //This is invalid
    if (strchr(rightSidePipe, '|')) 
    {
        return -1;
    }

    //Initialize both left and right to all 0
    outPipe->left  = calloc(1, sizeof(Command));
    outPipe->right = calloc(1, sizeof(Command));

    //Can probably comment these out:
    if (!outPipe->left || !outPipe->right)
    {
        if (outPipe->left)  { 
            free_command(outPipe->left);  
            free(outPipe->left);  
            outPipe->left  = NULL; 
        }
        if (outPipe->right) { 
            free_command(outPipe->right); 
            free(outPipe->right); 
            outPipe->right = NULL; 
        }
        return -1;
    } 

    if (parse_command(leftSidePipe,  outPipe->left) != 0) 
    {
        if (outPipe->left)  { 
            free_command(outPipe->left);  
            free(outPipe->left);  
            outPipe->left  = NULL; 
        }
        if (outPipe->right) { 
            free_command(outPipe->right); 
            free(outPipe->right); 
            outPipe->right = NULL; 
        }
        return -1; 
    }
    if (parse_command(rightSidePipe, outPipe->right) != 0) 
    {
        if (outPipe->left)  { 
            free_command(outPipe->left);  
            free(outPipe->left);  
            outPipe->left  = NULL; 
        }
        if (outPipe->right) { 
            free_command(outPipe->right); 
            free(outPipe->right); 
            outPipe->right = NULL; 
        }
        return -1;
    }
    //This returned as successful
    return 0;

}


///////////////
static int apply_redirections(const Command *cmd) {
    // input: must exist
    if (cmd->in_arg) {
        int fd = open(cmd->in_arg, O_RDONLY);
        if (fd < 0) { // On success, nonnegative integer
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
        if (fd < 0)
        {
            return -1;
        } 
        if (dup2(fd, STDOUT_FILENO) < 0) 
        { 
            close(fd); 
            return -1; 
        }
        close(fd);
    }
    // stderr: same as stdout but onto STDERR_FILENO
    if (cmd->err_arg) {
        int fd = open(cmd->err_arg, O_WRONLY|O_CREAT|O_TRUNC,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        if (fd < 0){
            return -1;
        }
        if (dup2(fd, STDERR_FILENO) < 0) { 
            close(fd); 
            return -1; 
        }
        close(fd);
    }
    return 0;
}

static void run_single_command_foreground(Command *cmd, const char *cmdline) {

    //Create a new process
    pid_t pid = fork();
    //Returns the child pid for the parent
    //Returns 0 for the child

    //After splitting the processes, the child comes here, and the parent goes to the other loop
    if (pid == 0) {
        // child: its own process group

        //Put the child into a new process group whose PGID equals its own PID (the first 0 means 
        //“this process”, the second 0 means “use my PID as PGID”).
        //Why: for job control. Each job (pipeline) lives in its own process group so the shell can 
        //later give/restore terminal control, send signals to the whole job, etc.
        setpgid(0, 0);

        // APPLY FILE REDIRECTIONS HERE
        if (apply_redirections(cmd) < 0){ 
            _exit(127);
        }
        // Replace the child’s image with the target program.
        // execvp looks up argVector[0] in PATH.
        // cmd->argVector must be a NULL-terminated char*[].
        // On success, this never returns. On failure, it returns -1 and sets errno.
        // CHILD ONLY (just after fork, before exec)
        signal(SIGINT,  SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        execvp(cmd->argVector[0], cmd->argVector);
        _exit(127); // exec failed with the error code: 127
    } else if (pid > 0) {
        // parent: put child in its own pgid and wait (foreground only)
        setpgid(pid, pid);
        // (you'll add tcsetpgrp/waitpid with WUNTRACED later; basic wait is fine now)
        // int st; (void)st;
        // //Wait until specific child finishes 
        // waitpid(pid, &st, 0);

        setpgid(pid, pid);               // ensure group set
        fg_pgid = pid;
        give_terminal_to(pid);

        int stopped = wait_foreground_job(pid);  // you already call this
        take_terminal_back();
        fg_pgid = 0;

        if (stopped) {
            // If not already tracked, add as STOPPED so it becomes most recent via new job_id
                // You need the original command string to store here.
                // Easiest: pass it in as an argument to this function, like you did for background.
                // For now, you can skip making it "most recent" if you don't have `orig` here.
                // (But to be perfect: change signature to run_single_command_foreground(Command *cmd, const char *cmdline))
                if (find_job_by_pgid(pid) == -1) add_job(pid, JOB_STOPPED, cmdline);

            
        }

    }
}

static void run_pipeline_foreground(Command *left, Command *right) {
    int fds[2];
    if (pipe(fds) < 0) return;

    pid_t left_pid = fork();
    if (left_pid == 0) {
        // left child
        setpgid(0, 0);                       // leader pgid = left child's pid
        // wire stdout -> pipe write end
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);

        // APPLY FILE REDIRECTIONS FOR LEFT AFTER WIRING PIPE
        if (apply_redirections(left) < 0) _exit(127);

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
    setpgid(left_pid, left_pid);

    pid_t right_pid = fork();
    if (right_pid == 0) {
        // right child
        setpgid(0, left_pid);                // join left's pgid
        //setpgid(pid, pgid)
        // wire stdin <- pipe read end
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]); close(fds[1]);

        // APPLY FILE REDIRECTIONS FOR RIGHT AFTER WIRING PIPE
        if (apply_redirections(right) < 0) _exit(127);

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
    setpgid(right_pid, left_pid);
    close(fds[0]); close(fds[1]);

    // wait for both (foreground)
    // int st;
    // waitpid(left_pid,  &st, 0);
    // waitpid(right_pid, &st, 0);


    //Waiting for the entire group since the left one is the group leader
    fg_pgid = left_pid;
    give_terminal_to(left_pid);

    (void)wait_foreground_job(left_pid);     // waits the whole group

    take_terminal_back();
    fg_pgid = 0;
}



int main(void) {
    init_shell();

    while(1){
        reap_children_and_print_done();

        char *line = readline("# ");
        reap_children_and_print_done();
        
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
        Line *lineStruct = calloc(1, sizeof(Line));
        //Can I just delete this line: 
        if (!lineStruct) { perror("calloc"); free(orig); free(line); continue; }


        // Background?
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '&') {
            lineStruct->isBackground = 1;
            //Put a null pointer at the end
            line[len - 1] = '\0';
            // retrim
            for (size_t L = strlen(line); L && (line[L-1]==' ' || line[L-1]=='\t'); --L) {
                line[L-1] = '\0';
            }
            //This is to prevent just the & being passed in
            if (line[0] == '\0') {   // "&" alone is invalid per our simplified rules
                putchar('\n');
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
                putchar('\n'); // Create a new line
                free(lineStruct);
                free(orig);
                free(line);
                continue;
            }


            lineStruct->linePipe = malloc(sizeof(Pipe));
            if (!lineStruct->linePipe) { perror("malloc"); free(lineStruct); free(orig); free(line); continue; }


            if (parsePipe(line, lineStruct->linePipe) != 0) {
                putchar('\n');
                free_pipe(lineStruct->linePipe);
                free(lineStruct->linePipe);
                free(lineStruct);
                free(orig);
                free(line);
                continue;
            }


            // At this point the left/right Command argVector pointers point into `line`.
            // Run the pipeline in the foreground (spec only requires single '|')
            run_pipeline_foreground(lineStruct->linePipe->left, lineStruct->linePipe->right);

            // Free parsed structs (safe: children got their own copy-on-write memory)
            free_pipe(lineStruct->linePipe);
            free(lineStruct->linePipe);
            free(lineStruct);
            free(orig);
            free(line);
            continue;

        }

        // Case 2: single command (no pipe)
        lineStruct->lineCommand = malloc(sizeof(Command));
        if (!lineStruct->lineCommand) { perror("malloc"); free(lineStruct); free(orig); free(line); continue; }

        //Something wrong with the command:
        if (parse_command(line, lineStruct->lineCommand) != 0) {
            putchar('\n');
            free_command(lineStruct->lineCommand);
            free(lineStruct->lineCommand);
            free(lineStruct);
            free(orig);
            free(line);
            continue;
        }


        // Built-ins: jobs, fg, bg (no args for this lab)
        Command *c = lineStruct->lineCommand;
        if (c->argVector && c->argVector[0]) {
            if (strcmp(c->argVector[0], "jobs") == 0) {
                // print table
                int mr = most_recent_job_index();
                for (int i=0;i<MAX_JOBS;i++) if (jobs[i].used) {
                    char mark = (i==mr)?'+':'-';
                    const char *st = (jobs[i].status==JOB_RUNNING)?"Running":"Stopped";
                    printf("[%d] %c   %s       %s\n",
                        jobs[i].job_id, mark, st, jobs[i].cmd);
                }
                putchar('\n');
                // cleanup structs and continue loop
                free_command(c); free(c); free(lineStruct); free(orig); free(line); continue;
            }
            if (strcmp(c->argVector[0], "fg") == 0) {
                int mr = most_recent_job_index();
                if (mr != -1) {
                    pid_t pg = jobs[mr].pgid;
                    // print original command (like bash)
                    printf("%s\n", jobs[mr].cmd);
                    fflush(stdout);

                    // continue and wait in foreground
                    kill(-pg, SIGCONT);
                    fg_pgid = pg; give_terminal_to(pg);
                    int stopped = wait_foreground_job(pg);
                    take_terminal_back(); fg_pgid = 0;

                    if (kill(-pg, 0) == -1 && errno == ESRCH) {
                        // fully exited
                        remove_job_index(mr);
                    } else if (stopped) {
                        jobs[mr].status = JOB_STOPPED;
                    } else {
                        // still alive but not stopped is unlikely here; keep as RUNNING
                        jobs[mr].status = JOB_RUNNING;
                    }
                }
                putchar('\n');
                free_command(c); free(c); free(lineStruct); free(orig); free(line);
                continue;
            }

            if (strcmp(c->argVector[0], "bg") == 0) {
                int sj = find_most_recent_stopped();
                if (sj != -1) {
                    jobs[sj].status = JOB_RUNNING;
                    kill(-jobs[sj].pgid, SIGCONT);
                    int mr = most_recent_job_index();
                    char mark = (sj==mr)?'+':'-';
                    printf("[%d] %c   Running       %s &\n",
                        jobs[sj].job_id, mark, jobs[sj].cmd);
                }
                putchar('\n');
                free_command(c); free(c); free(lineStruct); free(orig); free(line);
                continue;
            }

        }


        // Dispatch foreground/background
        if (lineStruct->isBackground) {
            run_single_command_background(lineStruct->lineCommand, orig);
        } else {
            run_single_command_foreground(lineStruct->lineCommand, orig);
        }

        // Cleanup (safe after forks/execs)
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


//Check the slides for the full code in parsing the strings

//