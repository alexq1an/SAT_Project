#compiler setup
CXX = g++
MPICXX = mpic++
CXXFLAGS = -std=c++17 -O3 

COMMON= core/utils.h core/cxxopts.h core/get_time.h 
SERIAL= SAT_serial
PARALLEL= SAT_parallel
MPI= SAT_mpi
ALL= $(SERIAL) $(PARALLEL) $(MPI)

all : $(ALL)

$(SERIAL): %: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

$(PARALLEL): %: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

$(MPI): %: %.cpp
	$(MPICXX) $(CXXFLAGS) -o $@ $<

.PHONY : clean

clean :
	rm -f *.o *.obj $(ALL)
