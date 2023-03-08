#!/bin/bash

work="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

reults_dir="$work/results"
jdk_dir="$work/jdk"
chopin_jar="$work/chopin/dacapo-evaluation-git-e5ee303d.jar"


mkdir $reults_dir &> /dev/null

function run {

    >$reults_dir/$1-$4-$3.txt
    $jdk_dir/build/linux-x86_64-server-release/jdk/bin/java -Xmx$2m -Xms16m -Xlog:gc* -XX:+UseZGC -XX:+ShowMessageBoxOnError -jar $chopin_jar $1 -n 3 -s $4 --no-pre-iteration-gc &> $reults_dir/$1-$4-$3.txt
}

function main {
    start_power=10
    current_power=$start_power
    last_power=0
    bm=$1 #benchmark name
    size=$2 #inputsize
    flag=false
    echo $bm
    xmx=$(( 2 ** $start_power ))
    run $bm $xmx $current_power $size
    while [[ "$current_power" -le "$max_power" ]];
    do
        echo "checking for heap size = $(( 2 ** $current_power)) MB"
        check=""
        check=`cat $reults_dir/$bm-$2-$current_power.txt | grep 'Out Of Memory\|Allocation Stall\|Relocation Stall\|Error\|Exception'`
          
        if [[ ! -z "$check" ]]
        then
            
            if [[ "$current_power" -eq "$last_power-1" ]]; then
                echo "$bm-$2 $(( 2 ** $last_power)) $last_power" >> "$work/finalMinHeapSizes.txt"
                break
            fi
                flag=true
                xmx=$((2 ** $current_power+1))
                last_power=$current_power
                current_power=$(($current_power+1))
        else
            
            if [[ "$current_power" -gt 1 ]]; then
                if [[ "$current_power" -eq "$last_power+1" ]]; then
                    echo "$bm-$2 $(( 2 ** $current_power)) $current_power" >> "$work/finalMinHeapSizes.txt"
                    break
                else
                    flag=false
                    xmx=$((2 ** $current_power-1))
                    last_power=$current_power
                    current_power=$(($current_power-1))               
                fi
            fi
        fi
        run $bm $xmx $current_power $size
    done      
}


>"$work/finalMinHeapSizes.txt"
max_power=`free | grep 'Mem' | awk '/Mem:/ { printf "%.3f \n", $2/1024 }' | awk '{print log($1)/log(2)}' | awk -F"." '{print $1}'`
echo "Max Power is : $max_power"
# available BMs: jython spring tradebeans tradesoap h2 lusearch jme tomcat avrora batik biojava eclipse graphchi luindex pmd sunflow xalan jython spring tradebeans tradesoap 
for BENCH in tomcat spring luindex batik 
do
#number of VM instances
for i in {1..3}
do
    main $BENCH "large"
    sleep 10
done
done



