CHECKS = check-qdict$(EXESUF)
CHECKS += check-qfloat$(EXESUF)
CHECKS += check-qint$(EXESUF)
CHECKS += check-qjson$(EXESUF)
CHECKS += check-qlist$(EXESUF)
CHECKS += check-qstring$(EXESUF)
CHECKS += test-coroutine$(EXESUF)
CHECKS += test-qmp-input-visitor$(EXESUF)
CHECKS += test-qmp-output-visitor$(EXESUF)
CHECKS += test-string-input-visitor$(EXESUF)
CHECKS += test-string-output-visitor$(EXESUF)

check-qint.o check-qstring.o check-qdict.o check-qlist.o check-qfloat.o check-qjson.o test-coroutine.o: $(GENERATED_HEADERS)

check-qint$(EXESUF): check-qint.o qint.o $(tools-obj-y)
check-qstring$(EXESUF): check-qstring.o qstring.o $(tools-obj-y)
check-qdict$(EXESUF): check-qdict.o qdict.o qfloat.o qint.o qstring.o qbool.o qlist.o $(tools-obj-y)
check-qlist$(EXESUF): check-qlist.o qlist.o qint.o $(tools-obj-y)
check-qfloat$(EXESUF): check-qfloat.o qfloat.o $(tools-obj-y)
check-qjson$(EXESUF): check-qjson.o $(qobject-obj-y) $(tools-obj-y)
test-coroutine$(EXESUF): test-coroutine.o qemu-timer-common.o async.o $(coroutine-obj-y) $(tools-obj-y)

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
test-string-output-visitor$(EXESUF): test-string-output-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-string-input-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-string-input-visitor$(EXESUF): test-string-input-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-qmp-output-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-qmp-output-visitor$(EXESUF): test-qmp-output-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-qmp-input-visitor.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h) $(qapi-obj-y)
test-qmp-input-visitor$(EXESUF): test-qmp-input-visitor.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o

test-qmp-commands.o: $(addprefix $(qapi-dir)/, test-qapi-types.c test-qapi-types.h test-qapi-visit.c test-qapi-visit.h test-qmp-marshal.c test-qmp-commands.h) $(qapi-obj-y)
test-qmp-commands$(EXESUF): test-qmp-commands.o $(qobject-obj-y) $(qapi-obj-y) $(tools-obj-y) $(qapi-dir)/test-qapi-visit.o $(qapi-dir)/test-qapi-types.o $(qapi-dir)/test-qmp-marshal.o module.o

.PHONY: check
check: $(CHECKS)
	$(call quiet-command, gtester $(CHECKS), "  CHECK")
