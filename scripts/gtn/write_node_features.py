import torch
import numpy as np
import pickle

with open('node_features.pkl', 'rb') as f:
    node_features = pickle.load(f)

with open('ACM_node_features.csv', 'w') as f:
    for item in node_features:
        for i in item:
            print(i, end=' ')
        print("\n", end=' ')
