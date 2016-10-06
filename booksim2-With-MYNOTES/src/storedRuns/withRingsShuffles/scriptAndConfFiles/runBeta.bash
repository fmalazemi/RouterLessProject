#!/bin/bash
#this script must be in the same directory as src/booksim
#the injection_rate=xx must be at line 44


#read -r -p "Enter config files directory: " config
#if [ ! -d $config ]; then
#   echo "$config is not a directory!"
#   echo "Terminating... BYE!"
#   exit 1
#fi



config="examples/rlBeta"
dir="runs"
if [ -d $dir ]; then
    # Will enter here if $DIRECTORY exists, even if it contains spaces
    #echo "Directory $dir exists!"
    #echo "Do you want to delete all file in $dir?(Y/N)"
    #read -n1 -r -p "`echo $'\n> '`" key
    #if [ "$key" = 'Y' ] || [ "$key" = 'y' ]; then 
    #   echo -e "\nDeleting files in directory runs!"
    #   rm  -rf runs/*
    #else 
    #   echo -e "\nTerminating... BYE!"
    #   exit 1
    #fi 
    echo "Hi"
else
    echo "Directory runs is Created!"
    mkdir runs    
fi


declare -a arr=("0.005" "0.01" "0.015" "0.02" "0.025" "0.03" "0.035" "0.04" "0.045" "0.05" "0.055" "0.06" "0.065" "0.07" "0.075" "0.08" "0.085" "0.09" "0.095" "0.1" "0.105" "0.11" "0.115" "0.12" "0.125" "0.13" "0.135" "0.14" "0.145" "0.15" "0.155" "0.16" "0.165" "0.17" "0.175" "0.18" "0.185" "0.19" "0.195" "0.2" "0.205" "0.21" "0.215" "0.22" "0.225" "0.23" "0.235" "0.24" "0.245" "0.25" "0.255" "0.26" "0.265" "0.27" "0.275" "0.28" "0.285" "0.29" "0.295" "0.3" "0.305" "0.31" "0.315" "0.32" "0.325" "0.33" "0.335" "0.34" "0.345" "0.35" "0.355" "0.36" "0.365" "0.37" "0.375" "0.38" "0.385" "0.39" "0.395" "0.4" "0.405" "0.41" "0.415" "0.42" "0.425" "0.43" "0.435" "0.44" "0.445" "0.45" "0.455" "0.46" "0.465" "0.47" "0.475" "0.48" "0.485" "0.49" "0.495" "0.5" "0.505" "0.51" "0.515" "0.52" "0.525" "0.53" "0.535" "0.54" "0.545" "0.55" "0.555" "0.56" "0.565" "0.57" "0.575" "0.58" "0.585" "0.59" "0.595" "0.6" "0.605" "0.61" "0.615" "0.62" "0.625" "0.63" "0.635" "0.64" "0.645" "0.65" "0.655" "0.66" "0.665" "0.67" "0.675" "0.68" "0.685" "0.69" "0.695" "0.7" "0.705" "0.71" "0.715" "0.72" "0.725" "0.73" "0.735" "0.74" "0.745" "0.75" "0.755" "0.76" "0.765" "0.77" "0.775" "0.78" "0.785" "0.79" "0.795" "0.8" "0.805" "0.81" "0.815" "0.82" "0.825" "0.83" "0.835" "0.84" "0.845" "0.85" "0.855" "0.86" "0.865" "0.87" "0.875" "0.88" "0.885" "0.89" "0.895" "0.9" "0.905" "0.91" "0.915" "0.92" "0.925" "0.93" "0.935" "0.94" "0.945" "0.95" "0.955" "0.96" "0.965" "0.97" "0.975" "0.98" "0.985" "0.99" "0.995" "1.0")
#declare -a strArr=("rlAlphaBitrev" "rlBetaBitrev" "rlAlphaTornado" "rlBetaBitTornado" "imrUniform" "imrBitrev" "imrTornado" "imrTranspose" "imrBitcomp")
declare -a strArr=("rlBetaTornado" "rlBetaTranspose" "rlBetaUniform" "rlBetaBitComp" "rlBetaBitrev" "rlBetaShuffle")

for c in "${strArr[@]}" 
do 
	config_file=$config"/"$c
	outputFile=$dir"/"$c"Output"
	echo "./booksim $config_file -> $outputFile."

	for i in "${arr[@]}"
	do 
  		j="injection_rate = $i;" 
		echo "Run at $j"
   		perl -i -pe "s/.*/$j / if $.==44" $config_file
   		# or do whatever with individual element of the array
   		./booksim $config_file >/dev/null 
   		x=`awk '/./{line=$0} END{print line}' $outputFile | awk '{print $1;}'`
   		x=`printf "%.0f" $x`
   		if [ $x -gt "200" ]
  		then 
			echo "$config_file exited at $i!"
   			break
  		fi
	done
done
echo "Program Terminating... BYE!"

