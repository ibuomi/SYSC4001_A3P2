# SYSC4001_A3P2
This project simulates TAs grading student exams and comprises of two parts.

Part 2a - TAs can write to the files at the same time
Part 2b - TAs connot write to the file at the same time(fixed using semaphores)

COMPILATION
For part 2a, run in your terminal:

gcc -o part2a part2a_101287549_101302779.c -std=c11

For part 2b, run in your terminal:

gcc -o part2b part2b_101287549_101302779.c -std=c11

RUNNING YOUR PROGRAM
For part 2a, run in your terminal

./part2a <num_TAs> <num_exams>
# Example: ./part2a 4 20

For part 2b, run in your terminal

./part2b <num_TAs> <num_exams>
# Example: ./part2b 4 20



