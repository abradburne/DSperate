# Mac only: Homebrew's mGBA (brew install mgba) supplies libmgba and its headers.
MGBA ?= $(shell brew --prefix mgba)
mgba-oracle: mgba-oracle.c
	cc -O2 -Wall -o $@ $< -I$(MGBA)/include -L$(MGBA)/lib -lmgba -Wl,-rpath,$(MGBA)/lib
