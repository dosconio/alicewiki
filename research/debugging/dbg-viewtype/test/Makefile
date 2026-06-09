all:
	gcc c.c -g -o c_static -static
	gcc c.c -g -o c_dynami -no-pie -fno-pic

build:
	cd ../build && rm -rf *
	cd ../build && cmake .. && make

test: c_static c_dynami
	../build/dbg-viewtype c_dynami type.txt vars.txt

allnew: build test
