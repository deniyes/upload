object := main.o rbtree.o
objs := upload
cc=gcc
MAKE=make
flags := -O2 -Wall -g 

main:$(object)
	$(cc) $(flags) $(object) -o $(objs)
$(object):%o:%c
	$(cc) $(flags) -c $< -o $@
.PHONE:clean

clean:
	-rm *.o $(objs)
