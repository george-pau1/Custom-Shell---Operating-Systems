#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "parse.h"

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
    }
    //Storing all the pointers to the strings of the arguments
    (*argv)[*count] = tok; // Store the token pointer

    (*count)++;

    (*argv)[*count] = NULL;  // null terminate just in case

    return 0;
    
}

void createCommand(Command *cmd) {
    cmd->argVector = NULL;
    cmd->in_arg  = NULL;
    cmd->out_arg = NULL;
    cmd->err_arg = NULL;
}

void free_command(Command *cmd) {
    if (!cmd) 
    {
        return;
    }
    free(cmd->argVector);      // argv entries point inside the line buffer; don't free them
    // in_arg/out_arg/err_arg also point into the same buffer; don't free them
    memset(cmd, 0, sizeof(*cmd)); 
}

void free_pipe(Pipe *p) {
    if (!p){ 
        return;
    }
    if (p->left)  { 
        free_command(p->left);  
        free(p->left); 
        p->left  = NULL; 
    }
    if (p->right) { 
        free_command(p->right); 
        free(p->right); 
        p->right = NULL; 
    }
}

int parseCommand(char* side, Command* output)
{
    //Initialize the command:
    createCommand(output);

    size_t argCount = 0;
    size_t argSizeAllocced = 0;


    char *save = NULL;
    char *tok  = strtok_r(side, " \t", &save);

    
    int state = 0; // 0 is for Normal, 1 is for File In, 2 is for File Out, 3 is for File Err
    
    while(tok)
    {
        //In the normal state
        if (state == 0) {
            if (strcmp(tok, "<") == 0) {
                state = 1; // Next one is the file name or whatever
            } else if (strcmp(tok, ">") == 0) {
                state = 2;
            } else if (strcmp(tok, "2>") == 0) {
                state = 3;
            } else {
                if (argv_push(&output->argVector, &argSizeAllocced, &argCount, tok) != 0){ 
                    return -1;
                }
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

    //Make sure left and right side are both valid commands
    if (parseCommand(leftSidePipe,  outPipe->left) != 0) 
    {
        free_pipe(outPipe);
        return -1;
    }
    if (parseCommand(rightSidePipe, outPipe->right) != 0) 
    {
        free_pipe(outPipe);
        return -1;
    }
    //This returned as successful
    return 0;

}


