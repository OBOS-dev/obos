#!/bin/sh
out="for (volatile bool b = ($1); b; );"
which -s xsel
if [ $? -eq 0 ]
then
    echo $out | xsel -ibo
else
    echo $out
fi
