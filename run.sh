#!/bin/bash

#  Philip Ton   03/21/2016

# use predefined variables to access passed arguments
#echo arguments to the shell
#echo $1 $2 $3 ' -> echo $1 $2 $3'

# We can also store arguments from bash command line in special array
args=("$@")
#echo arguments to the shell
#echo ${args[0]} ${args[1]} ${args[2]} ' -> args=("$@"); echo ${args[0]} ${args[1]} ${args[2]}'

#use $@ to print out all arguments at once
#echo $@ ' -> echo $@'

# use $# variable to print out
# number of arguments passed to the bash script
#echo Number of arguments passed: $# ' -> echo Number of arguments passed: $#' 

# get drives list from sg_map command
drive_list=$(sg_map)
echo -e "\0033\0143"
echo
echo " Reconfigure drives:"
echo
skip=1
skip_first_1=1
for item in $drive_list
do
   if [ $skip -eq 1 ]; then
      skip=0
      continue
   else
      skip=1
   fi
   if [ $skip_first_1 -gt 0 ]; then
      ((skip_first_1--))
      continue
   fi
   
   echo -ne "   Module at $item  "
   ./sg_read_SM3252_LED $item
done


