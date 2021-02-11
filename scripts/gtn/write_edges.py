import torch
import numpy as np
import pickle

with open('edges.pkl', 'rb') as f:
    edges = pickle.load(f)

Z = edges[0].todok().keys();
with open('ACM_edges.csv', 'w') as f:
    for key in edges[0].todok().keys():
        s = "%d %d 0" % (key[0], key[1])
        print(s, file=f)

Z = edges[1].todok().keys();
with open('ACM_edges.csv', 'a') as f:
    for key in edges[1].todok().keys():
        s = "%d %d 1" % (key[0], key[1])
        print(s, file=f)

Z = edges[2].todok().keys();
with open('ACM_edges.csv', 'a') as f:
    for key in edges[2].todok().keys():
        s = "%d %d 2" % (key[0], key[1])
        print(s, file=f)

Z = edges[3].todok().keys();
with open('ACM_edges.csv', 'a') as f:
    for key in edges[3].todok().keys():
        s = "%d %d 3" % (key[0], key[1])
        print(s, file=f)


