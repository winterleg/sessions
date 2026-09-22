.PHONY: run compile all

all: compile run

run: compile
	./sessions

compile:
	gcc main.c -o sessions -Wall -Wextra
