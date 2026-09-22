.PHONY: run compile all

compile:
	gcc main.c -o sessions -Wall -Wextra

all: compile run

run: compile
	./sessions
