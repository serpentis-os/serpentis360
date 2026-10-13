#!/bin/sh
# builds the selftest kernel, boots it on qemu powernv and checks what it printed
cd "$(dirname "$0")"
export PATH=/usr/local/xenon/bin:$PATH
make -s EXTRA=-DSELFTEST BUILD=build/test || exit 1
QEMU=${QEMU:-qemu-system-ppc64}
timeout 90 "$QEMU" -M powernv8 -cpu power8 -m 1G -nographic -display none -serial mon:stdio \
	-kernel build/test/kernel.elf > build/test/boot.log 2>&1
sed -n '/serpentis360/,$p' build/test/boot.log | tr -d '\r' > build/test/kernel.log
fail=0
for want in "selftest ok" "illegal instruction at" "panic: illegal instruction" "Shutdown request"; do
	grep -q "$want" build/test/kernel.log build/test/boot.log || { echo "missing: $want"; fail=1; }
done
grep "FAIL" build/test/kernel.log && fail=1
[ $fail = 0 ] && echo "pass" || { cat build/test/kernel.log; echo "fail"; }
exit $fail
