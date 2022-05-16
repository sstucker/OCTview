## Building OCTview with fbs

OCTview is a Windows desktop application built with Qt 5. 

A lightweight installer delivers OCTview to the user. To create this installer, prepare a Python 3.6.8 environment with the dependencies listed in `requirements.txt` and then run:
```
python -m fbs freeze
python -m fbs installer
```
The resulting installer will be in the `targets` directory.

This repository has an action that releases a new executable automatically when new version tags are pushed to the main branch.
