TESTPROGNAME=	test
TESTDIRS!=	find . -type d -name "t_*"

.PHONY: build_all clean_all
build_all:
	make -C ../src clean
.for TESTDIR in ${TESTDIRS}
	make -C ${TESTDIR}
.endfor

clean_all:
.for TESTDIR in ${TESTDIRS}
	make -C ${TESTDIR} clean
.endfor

REGRESS_SETUP_ONCE= build_all
REGRESS_CLEANUP= clean_all

.for TESTDIR in ${TESTDIRS}
test_${TESTDIR:S/.\/t_//}:
	make -C ${TESTDIR} run

REGRESS_TARGETS += test_${TESTDIR:S/.\/t_//}
.endfor

.include <bsd.regress.mk>
