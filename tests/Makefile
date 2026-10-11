CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra

run: link_sim
	./link_sim

link_sim: link_sim.cpp ../receiver_esp32/LiFiDecoder.h
	$(CXX) $(CXXFLAGS) link_sim.cpp -o $@

clean:
	rm -f link_sim

.PHONY: run clean
