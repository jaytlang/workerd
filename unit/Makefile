TESTPROGNAME=	test
TESTDIRS!=	find . -type d -name "t_*"

.PHONY: build_all clean_all image_clean
build_all: image_clean
	make -C ../src clean
.for TESTDIR in ${TESTDIRS}
	make -C ${TESTDIR}
.endfor

clean_all: clean image_clean
.for TESTDIR in ${TESTDIRS}
	make -C ${TESTDIR} clean
.endfor

image_clean:
	doas rm -f /var/workerd/disks/*

REGRESS_SETUP_ONCE= build_all
REGRESS_CLEANUP= clean_all

.for TESTDIR in ${TESTDIRS}
test_${TESTDIR:S/.\/t_//}:
	make -C ${TESTDIR} run

REGRESS_TARGETS += test_${TESTDIR:S/.\/t_//}
.endfor

.include <bsd.regress.mk>
