#ifndef PARSE_H
#define PARSE_H

#include <sys/types.h>

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

void createCommand(Command *cmd);
void free_command(Command *cmd);
void free_pipe(Pipe *p);
int  parseCommand(char* side, Command* output);
int  parsePipe(char *line, Pipe *outPipe);

#endif
