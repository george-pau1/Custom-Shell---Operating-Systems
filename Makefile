#Need to fix this before submitting, and run it on the VM Linux machine

#This as a whole automates building the shell with all the proper flags and everything

#This defines the compiler that we are using
CC = clang 

#Flags that are sent to compiler: -g for debugger, -Wall for warnings
CFLAGS = -Wall -g

# Homebrew's path for readline
READLINE_PREFIX := $(shell brew --prefix readline)

#This is the include for the header files for readline
#-I means “add this directory to header search path.”
CFLAGS += -I$(READLINE_PREFIX)/include

#-L… tells the linker where to look for libraries (libreadline.dylib).
#-lreadline links the actual readline library.
LDFLAGS = -L$(READLINE_PREFIX)/lib
LIBS = -lreadline

# Our program
#The tabbed line below compiles the program
yash: main.c
	$(CC) $(CFLAGS) -o yash main.c $(LDFLAGS) $(LIBS)

#With all variables: clang -Wall -g -I/opt/homebrew/opt/readline/include \
      -o yash main.c \
      -L/opt/homebrew/opt/readline/lib -lreadline


#This means: Which means:
# Compile main.c with warnings and debug info.
# Add the Homebrew readline headers and libraries.
# Output an executable called yash.

# Clean up build files

# Defines a clean target.
# If you run make clean, it deletes the compiled binary yash.
# -f means “don’t complain if the file doesn’t exist.”
clean:
	rm -f yash
