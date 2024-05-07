#!/usr/bin/env python3

import numpy as np
import matplotlib.pyplot as plt
import os

def collect(binary):
    t = os.popen(f"build/{binary}").read()
    t = np.fromstring(t, dtype=int, sep=',')
    t = t[:-1] # remove the last seperator
    return t

os.system("make")
t1 = collect("test_linux")
t2 = collect("test_pthread")

x = np.arange(len(t1))

plt.plot()
plt.scatter(x, t1, label="futex")
plt.scatter(x, t2, label="pthread")
plt.legend()
plt.ylabel('Latency(ns)')
plt.xlabel('The i-th Experiment')
plt.show()
