#!/bin/sh
gcc ./flac/dr_flac_test_0.c -o ./bin/dr_flac_test_0 -std=c89 -ansi -pedantic -O3 -s -Wall -ldl
gcc ./flac/dr_flac_decoding.c -o ./bin/dr_flac_decoding -std=c89 -ansi -pedantic -Wall -O3 -s -lFLAC -ldl
gcc ./flac/dr_flac_seeking.c -o ./bin/dr_flac_seeking -std=c89 -ansi -pedantic -Wall -O3 -s -lFLAC -ldl
gcc ./flac/dr_flac_mkv.c -o ./bin/dr_flac_mkv -std=c89 -ansi -pedantic -Wall -g -lFLAC -ldl
g++ ./flac/dr_flac_decoding.cpp -o ./bin/dr_flac_decoding_cpp -Wall -Wpedantic -pedantic -O3 -s -lFLAC -ldl