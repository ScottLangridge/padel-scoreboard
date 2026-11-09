# Host build of the display code, for checking the port against the reference
# frames:
#
#     make -C test check
#
# -ffp-contract=off keeps the floating point identical to the reference
# JavaScript (no fused multiply-add), which the pixel-exact comparison relies on.

CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -Wall -Wextra -ffp-contract=off -I../main

render_clips: render_clips.cpp ../main/anim.cpp ../main/anim.h ../main/canvas.h ../main/fonts.h ../main/splash_frames.h
	$(CXX) $(CXXFLAGS) -o $@ render_clips.cpp ../main/anim.cpp -lm

score_test: score_test.cpp ../main/score.cpp ../main/score.h
	$(CXX) $(CXXFLAGS) -o $@ score_test.cpp ../main/score.cpp

.PHONY: check clean
check: render_clips score_test
	./score_test
	python3 -I ../tools/check_frames.py ./render_clips ../design/frames

clean:
	rm -f render_clips score_test
