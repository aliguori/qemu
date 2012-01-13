CHECKS = check-qdict check-qfloat check-qint check-qstring check-qlist
CHECKS += check-qjson test-qmp-output-visitor test-qmp-input-visitor
CHECKS += test-string-input-visitor test-string-output-visitor test-coroutine

HW_TESTS=

TARGETS=$(patsubst %-softmmu,%, $(filter %-softmmu,$(TARGET_DIRS)))

check-qint.o check-qstring.o check-qdict.o check-qlist.o check-qfloat.o check-qjson.o test-coroutine.o: $(GENERATED_HEADERS)

check-qint: check-qint.o qint.o $(tools-obj-y)
check-qstring: check-qstring.o qstring.o $(tools-obj-y)
check-qdict: check-qdict.o qdict.o qfloat.o qint.o qstring.o qbool.o qlist.o $(tools-obj-y)
check-qlist: check-qlist.o qlist.o qint.o $(tools-obj-y)
check-qfloat: check-qfloat.o qfloat.o $(tools-obj-y)
check-qjson: check-qjson.o $(qobject-obj-y) $(tools-obj-y)
test-coroutine: test-coroutine.o qemu-timer-common.o async.o $(coroutine-obj-y) $(tools-obj-y)

test-qmp-input-visitor.o test-qmp-output-visitor.o \
test-string-input-visitor.o test-string-output-visitor.o \
	test-qmp-commands.o qemu-ga$(EXESUF): QEMU_CFLAGS += -I $(qapi-dir)

$(qapi-dir)/test-qapi-types.c $(qapi-dir)/test-qapi-types.h :\
$(SRC_PATH)/qapi-schema-test.json $(SRC_PATH)/scripts/qapi-types.py
	$(call quiet-command,$(PYTHON) $(SRC_PATH)/scripts/qapi-types.py $(gen-out-type) -o "$(qapi-dir)" -p "test-" < $<, "  GEN   $@")
$(qapi-dir)/test-qapi-visit.c $(qapi-dir)/test-qapi-visit.h :\
$(SRC_PATH)/qapi-schema-test.json $(SRC_PATH)/scripts/qapi-visit.py
	$(call quiet-command,$(PYTHON) $(SRC_PATH)/scripts/qapi-visit.py $(gen-out-type) -o "$(qapi-dir)" -p "test-" < $<, "  GEN   $@")
$(qapi-dir)/test-qmp-commands.h $(qapi-dir)/test-qmp-marshal.c :\
$(SRC_PATH)/qapi-schema-test.json $(SRC_PATH)/scripts/qapi-commands.py
	    $(call quiet-command,$(PYTHON) $(SRC_PATH)/scripts/qapi-commands.py $(gen-out-type) -o "$(qapi-dir)" -p "test-" < $<, "  GEN   $@")


test-string-output-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-string-output-visitor: test-string-output-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-string-input-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-string-input-visitor: test-string-input-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-qmp-output-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-qmp-output-visitor: test-qmp-output-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-qmp-input-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-qmp-input-visitor: test-qmp-input-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-qmp-commands.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h test-qmp-marshal.c test-qmp-commands.h) $(qapi-obj-y)
test-qmp-commands: test-qmp-commands.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o $(qapi-dir)/test-qmp-marshal.o module.o

tests/rtc-test: tests/rtc-test.o tests/libqtest.o

check-help:
	@echo "Regression targets:"
	@echo
	@echo " make check                Run all tests"
	@echo " make check-qtest          Run qtest tests"
	@echo " make check-unit           Run qobject tests"
	@echo " make check-report.html    Generates an HTML test report"
	@echo
	@echo "Please note that HTML reports do not regenerate if the unit tests"
	@echo "has not changed."
	@echo
	@echo "The variable SPEED can be set to control the gtester speed setting"

.SECONDARY:

SPEED ?= quick

# Reports
check-report-qtest-%.log: $(HW_TESTS)
	$(call quiet-command,QTEST_QEMU_BINARY=`basename $@ .log | cut -f4 -d-`-softmmu/qemu-system-`basename $@ .log | cut -f4 -d-` \
	  gtester -k -q -o $@ -m=$(SPEED) $(HW_TESTS),"  TEST   $^ (`basename $@ .log | cut -f4 -d-`)")

check-report-unit.log: $(CHECKS)
	$(call quiet-command,gtester -k -q -m=$(SPEED) -o $@ $^, "  TEST   $^")

check-report.log: check-report-unit.log $(patsubst %,check-report-qtest-%.log, $(TARGETS))
	$(call quiet-command,$(SRC_PATH)/scripts/gtester-cat $^ > $@, "  GEN    $@")

check-report.html: check-report.log
	$(call quiet-command,gtester-report $< > $@, "  GEN    $@")

# Check tests

check-qtest-%: $(HW_TESTS)
	@for test in $^; do \
	    arch=`echo $@ | cut -f3- -d-`; \
	    echo "Running '$$test' with qemu-system-$$arch..."; \
	    $(SRC_PATH)/scripts/qtest $$arch-softmmu/qemu-system-$$arch $$test || exit $$?; \
	done

check-qtest: $(patsubst %,check-qtest-%, $(TARGETS))

check-unit: $(CHECKS)
	@for test in $^; do \
	    echo "Running '$$test'..."; \
	    ./$$test || exit $?; \
	done

check: check-unit check-qtest

.PHONY: check-help check-qtest check-unit check
