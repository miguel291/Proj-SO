CC =gcc
OBJS = projSO.o
PROG = prog

# GENERIC
all:	${PROG}

clean:
	rm ${OBJS} *~ ${PROG}

${PROG}: ${OBJS}
	${CC} ${OBJS} -pthread -Wall -o $@
.c.o:
	${CC} $< -c -o $@
#############################
projSO.o: projSO.c
