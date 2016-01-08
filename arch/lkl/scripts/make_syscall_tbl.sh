#!/bin/bash

#set -x

# requires: bash-4.0 (hashtable), cpp, grep, sed, cat

##
## Usage: $0
## ARGV[0]: (OPT: "systbl" oe "sysdef")
## ARGV[1]: (IN: asm/unistd.h)
## ARGV[2]: (OUT: syscall_table.h(systbl) or syscall_defs.h (sysdef))
##

mode=$1
in=$2
out=$3

if [ "$mode" != "systbl" ] && [ "$mode" != "sysdef" ] ; then
	echo "Usage: $0 [systbl|sysdef] (unistd.h) (output file)"
	exit
fi

echo "" > $out

# obtain the blacklist of syscall
declare -A ignore_syscall

##
## step 1: create ignored syscall table entries based on
##
while read def nr ; do
	name=$(echo $nr | sed "s/__IGNORE_/__NR_/")
	ignore_syscall[$name]="ignored"

if [ $mode = "systbl" ] ; then
	cat << EOF  >> $out
#define __SYSCALL_STE_${name}(nr,sym) [nr] = (syscall_handler_t)sys_ni_syscall,
__SYSCALL_STE_${name}(${name},dummy)
EOF
fi
done < <(${CROSS_COMPILE}cpp ${LINUXINCLUDE} -dM -fdirectives-only $in | grep '__IGNORE_')

##
## stop if systbl is specified
##
if [ $mode = "systbl" ] ; then
	exit
fi


##
## step 2-1: grep SYSCALL_DEFINE in whole tree with specific
## directories ($SYSCALL_DIRS)
##
SYSCALL_DIRS="block drivers/pci drivers/char fs init ipc kernel mm net"
SYSCALL_FILES=$(find ${SYSCALL_DIRS}  -name \*.c | \
		       xargs grep -E "^SYSCALL_DEFINE[0-6]" -l)

##
## step 2-2: dump lkl syscall definitions from preprocessor's ouptut
##
set -f
for file in ${SYSCALL_FILES} ;
do
	while read syscall; do
		func=$(echo $syscall | sed "s/SYSCALL_DEFINE.*(\([^,]*\).*/\1/")
		#echo "syscall= "$syscall
		#echo "func= "$func

		# 0) pull the system call name
		funcname=$(echo $func | sed "s/(.*//" | sed "s/long sys_//")
		#echo "funcname ="$funcname

		# 1) skip if the syscall is unsupported
		if [ "${ignore_syscall[__NR_$funcname]}" = "ignored" ] ; then
		#echo $funcname " ignore= " "${ignore_syscall[__NR_$funcname]}"
			continue
		fi

		# 2) count the number of argument
		func_l=/$func/
		IFS=','
		set -- $func_l
		argc=0
		if [ $(echo $func | sed "s/.*(//") != "void)" ] ; then
			argc=`expr $(($#-1)) + 1`
		fi
		#echo "argc = $argc"
		IFS=' '

		# 3) pull arg name template
		args=$(echo $func | sed "s/.*(/(/" | sed "s/__lkl__kernel_//g"\
			      | sed "s/lkl_//g" | sed "s/[()]//g")
		OIFS="$IFS"; IFS=','
		arg=($args); IFS="$OIFS"
		#echo "arg ="$arg > /dev/stdout

		param=""
		for i in {1..6} ; do
			if [ $i -gt ${argc} ] ; then
				break
			fi
			if [ $i -ne 1 ] ; then
				param="${param}, "
			fi
			nparam=$(echo ${arg[$((i-1))]} | \
					sed "s/\(.*\) \(.*\)$/\1, \2/")
			param="${param}${nparam}"
		done
		#echo ${param} > /dev/stdout
		IFS=' '

		# 4) give a template based on funcname, args
		cat << EOF >> $out

#ifdef __NR_$funcname
SYSCALL_DEFINE$argc(_$funcname, $param);
#endif /* __NR_$funcname */
EOF
	done < <(${CROSS_COMPILE}cpp -P ${LINUXINCLUDE} $file 2>/dev/null \
			| grep "long sys_" | sed "s/^; / /" \
			| sed "s/\(.*\) __attribute.*/\1/" | grep -vE ";|,$")
done
