#!/bin/bash

TARGET_FILE="mnt/cs333.txt"
COPY_FILE="read_copy.log"

# Files to store output of read functions.
READ_OUT="read.log"
MY_READ_OUT="my_read.log"


# This function takes a single argument: the file whose existence to test for.
# If the file does exist, it exits the script with a helpful message
test_existence() {
    if [ -f "$1" ]; then
        echo "ERROR: \"$1\" exists. Please clean up before running test."
        exit 0
    fi
}

# Compiling helper c code.
gcc -g -Og testread.c -o testread

test_existence "${READ_OUT}"
test_existence "${MY_READ_OUT}"
test_existence "${COPY_FILE}"

# Case 1: Testing read for files
cat ${TARGET_FILE} > ${COPY_FILE}

diff ${COPY_FILE} ${TARGET_FILE}

test=$?

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for cat case."
  exit 1
fi

# Case 2: Testing a read that is smaller than size of file with some offset.
./testread ${TARGET_FILE} 2 3 > ${MY_READ_OUT}
./testread ${COPY_FILE} 2 3 > ${READ_OUT}

diff ${READ_OUT} ${MY_READ_OUT}

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for case where read is some section of the
file."
  exit 1
fi

# Case 3: Testing read that is larger than the size of the file with some offset.
./testread ${TARGET_FILE} 5 100 > ${MY_READ_OUT}
./testread ${COPY_FILE} 5 100 > ${READ_OUT}

diff ${READ_OUT} ${MY_READ_OUT}

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for case where size of read is larger than
  the file."
  exit 1
fi


# Case 4: Testing case where offset of read is larger than the file size.
./testread ${TARGET_FILE} 100 100 > ${MY_READ_OUT}
./testread ${COPY_FILE} 100 100 > ${READ_OUT}

diff ${READ_OUT} ${MY_READ_OUT}

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for case where offset of read is larger
  than file."
  exit 1
fi

echo "Passed all tests."

rm "${READ_OUT}" "${MY_READ_OUT}" "${COPY_FILE}"
