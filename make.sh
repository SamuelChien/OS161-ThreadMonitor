 #!/bin/bash

cd a2/src
./configure

cd kern/conf
./config ASST2

cd ../compile/ASST2
bmake depend
bmake

bmake install

cd ../../..
bmake
bmake install

