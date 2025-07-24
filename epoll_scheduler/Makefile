all: sample

sample : sample.cpp
	/opt/compiler/gcc-12/bin/g++ -std=c++20 -o $@ $^
	mkfifo communication.pipe communication2.pipe

clean:
	rm sample communication.pipe communication2.pipe
