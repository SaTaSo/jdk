

# Welcome to the JDK!

For build instructions please see the
[online documentation](https://openjdk.org/groups/build/doc/building.html),
or either of these files:

- [doc/building.html](doc/building.html) (html version)
- [doc/building.md](doc/building.md) (markdown version)

See <https://openjdk.org/> for more information about the OpenJDK
Community and the JDK and see <https://bugs.openjdk.org> for JDK issue
tracking.


findMinHeap.sh:
It finds the minimum heap size used by the given benchmarks, while no allocation/relocation stalls and OOM errors 
and exceptions are found with the selected heap size.

Open the file, and set the following variables:

reults_dir -> the directory in which ZGC log files are inserted for each benchmark and heap size
jdk_dir -> jdk directory on your machine
chopin_jar -> the path to DaCapo Chopin jar file
