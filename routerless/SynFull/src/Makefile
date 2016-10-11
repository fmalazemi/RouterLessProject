# Makefile
CPP    = g++ -std=gnu++0x -g
CPPFLAGS = -Wall -Wextra -pedantic -O2 -I. -Inetstream 

PROG = tgen
PROGDIR := 
OBJDIR := obj

#NETSTREAM:= ../booksim_latest/src/obj/socketstream.o ../booksim_latest/src/obj/messages.o

CPP_SRCS:=$(wildcard *.cpp)
NET_SRCS:=$(wildcard netstream/*.cpp)

OBJS :=  \
 $(CPP_SRCS:%.cpp=${OBJDIR}/%.o)\
 $(NET_SRCS:%.cpp=${OBJDIR}/%.o)\

#--- Make rules --- 
all: $(PROG)
 
$(PROG): $(OBJS)
	 $(CPP) $^ -o $(PROGDIR)$@
	 
${OBJDIR}/%.o: %.cpp 
	$(CPP) $(CPPFLAGS) -c $< -o $@
	
clean:
	rm -f $(OBJS) 
	rm -f $(PROGDIR)$(PROG)
	rm -f *~
