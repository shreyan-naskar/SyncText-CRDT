# Build executable named control

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

control: control.cpp
	$(CXX) $(CXXFLAGS) control.cpp -o control

clean:
	rm -f control
