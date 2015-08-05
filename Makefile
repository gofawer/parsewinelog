parsewinelog: main.cpp
	clang++ -g -std=c++11 -stdlib=libc++ main.cpp -o parsewinelog

release: main.cpp
	clang++ -O3 -std=c++11 -stdlib=libc++ main.cpp -o parsewinelog