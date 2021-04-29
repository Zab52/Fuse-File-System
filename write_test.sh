#!/bin/bash

# Files to store output of write functions.
WRITE_OUT="write.log"
MY_WRITE_OUT="mnt/cs333.txt"


# This function takes a single argument: the file whose existence to test for.
# If the file does exist, it exits the script with a helpful message
test_existence() {
    if [ -f "$1" ]; then
        echo "ERROR: \"$1\" exists. Please clean up before running test."
        exit 0
    fi
}

# Compiling helper c code.
gcc -g -Og testwrite.c -o testwrite

test_existence "${WRITE_OUT}"

# Case 1: Testing append for writes
cat ${MY_WRITE_OUT} > ${WRITE_OUT}
echo "testing" >> ${WRITE_OUT}
echo "testing" >> ${MY_WRITE_OUT}

diff ${WRITE_OUT} ${MY_WRITE_OUT}

test=$?

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for append case."
  exit 1
fi

# Case 2: Testing overwriting the original files
echo "abc" > ${WRITE_OUT}
echo "abc" > ${MY_WRITE_OUT}

diff ${WRITE_OUT} ${MY_WRITE_OUT}

test=$?

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for overwrite case."
  exit 1
fi

# Case 3: Writing a string to the file with offset within the file, but end of
# string outside the size of the original file.
./testwrite ${WRITE_OUT} 2 "??Jkkkk"
./testwrite ${MY_WRITE_OUT} 2 "??Jkkkk"

diff ${WRITE_OUT} ${MY_WRITE_OUT}

test=$?

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match case where offset is within filesize, but
  end of inputted string is outside the size of the file."
  exit 1
fi

# Case 4: Writing to an offset past the end of the file.
./testwrite ${WRITE_OUT} 20 "hello darkness my old friend"
./testwrite ${MY_WRITE_OUT} 20 "hello darkness my old friend"

diff ${WRITE_OUT} ${MY_WRITE_OUT}

test=$?

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match for offset case past end of original file."
  exit 1
fi


# Case 5: Writing within the middle of the file
./testwrite ${WRITE_OUT} 3 "okay"
./testwrite ${MY_WRITE_OUT} 3 "okay"

diff ${WRITE_OUT} ${MY_WRITE_OUT}

test=$?

if [ $test -ne "0" ]; then
  echo "Error: output doesn't match when writing into the middle of the file."
  exit 1
fi

echo "Passed all tests."


rm ${WRITE_OUT}
