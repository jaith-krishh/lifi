CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra
HEADERS = ../receiver_esp32/LiFiDecoder.h ../transmitter_nano/LiFiEncoder.h

run: link_sim profile_check_fast profile_check_safe
	./profile_check_fast
	./profile_check_safe
	./link_sim

link_sim: link_sim.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) link_sim.cpp -o $@

profile_check_fast: profile_check.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) profile_check.cpp -o $@

profile_check_safe: profile_check.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -DPROFILE_LDR_SAFE profile_check.cpp -o $@

clean:
	rm -f link_sim profile_check_fast profile_check_safe

.PHONY: run clean
