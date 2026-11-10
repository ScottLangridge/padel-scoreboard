# Host build of the firmware's screen flow, for trying it without flashing:
#
#     make -C emulator               build
#     emulator/emulator              play it live in the terminal
#     make -C emulator check         run the scripted scenarios, compare output
#
# The firmware sources compile unchanged against the stand-in headers in
# shims/; the menuconfig settings come from the project's sdkconfig.
# -ffp-contract=off keeps the animation arithmetic identical to the
# reference, as in test/.

CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -Wall -Wextra -ffp-contract=off
BUILD = build
INCLUDES = -Ishims -I. -I$(BUILD) -I../main -I../components/rx433/include
SOURCES = emulator.cpp ../main/padel-scoreboard.cpp ../main/anim.cpp ../main/score.cpp
HEADERS = emulator.h $(wildcard shims/*.h shims/*/*.h) $(wildcard ../main/*.h) ../components/rx433/include/rx433.h
SCENARIOS = $(wildcard scenarios/*.txt)

emulator: $(SOURCES) $(HEADERS) $(BUILD)/sdkconfig.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SOURCES) -lpthread

$(BUILD)/sdkconfig.h: ../sdkconfig gen_sdkconfig.py
	mkdir -p $(BUILD)
	python3 -I gen_sdkconfig.py $< $@

# Each scenarios/<name>.txt must print exactly scenarios/<name>.expected.
# After an intended change, regenerate with `make bless` and review the diff.
.PHONY: check bless clean
check: emulator
	@for script in $(SCENARIOS); do \
		./emulator --script $$script | diff -u $${script%.txt}.expected - > $(BUILD)/diff.txt \
			&& echo "PASS $$script" || { echo "FAIL $$script"; cat $(BUILD)/diff.txt; exit 1; }; \
	done

bless: emulator
	@for script in $(SCENARIOS); do ./emulator --script $$script > $${script%.txt}.expected; echo "wrote $${script%.txt}.expected"; done

clean:
	rm -rf emulator $(BUILD)
