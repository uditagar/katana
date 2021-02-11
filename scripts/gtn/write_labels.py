import torch
import numpy as np
import pickle

with open('labels.pkl', 'rb') as f:
    labels = pickle.load(f)

with open('ACM_labels0.csv', 'w') as f:
    for key in labels[0]:
        s = "%d %d" % (key[0], key[1])
        print(s, file=f)

with open('ACM_labels1.csv', 'w') as f:
    for key in labels[1]:
        s = "%d %d" % (key[0], key[1])
        print(s, file=f)

with open('ACM_labels2.csv', 'w') as f:
    for key in labels[2]:
        s = "%d %d" % (key[0], key[1])
        print(s, file=f)
