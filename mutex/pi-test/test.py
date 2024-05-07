#!/usr/bin/env python3

import os

binary = 'build/pi-test'

total = 10
good = 0
for i in range(0, total):
    seq = os.popen(f"sudo taskset -c 1 ./{binary}").read().strip()
    print(seq)
    if seq == "h":
        good += 1

print("result = ", good/total)
